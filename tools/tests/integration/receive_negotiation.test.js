#!/usr/bin/env node
"use strict";

// Exercise data-channel-first negotiation, then the native app's direct-media
// fallback (send-only tracks, balanced BUNDLE, singular trickled candidates).
// Local signaling and synthetic audio only; no physical inputs or public stream.
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const http = require("node:http");
const dgram = require("node:dgram");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { WebSocketServer } = require("ws");
const wrtc = require("@roamhq/wrtc");
const { cliExecutable, pluginBundle } = require("./build_paths");
const { startTestTone } = require("../helpers/paced_tone");

async function check(name, { videoFirst = false, dataFirst = false, encrypted = false, relay = false, discoveryFailure = false, candidateBundle = false,
    earlyIce = false, omitSession = false, reorder = false, audioFirst = false, sendRecv = false, password = "", failureProbe = false, nativeRouting = false } = {}) {
    const key = crypto.createHash("sha256").update((password || "someEncryptionKey123") + "vdo.ninja").digest();
    const withoutCandidates = description => ({ ...description,
        sdp: description.sdp.split("\r\n").filter(line => !/^a=(candidate:|end-of-candidates)/.test(line)).join("\r\n") });
    function encode(message) {
        if (!encrypted) return message;
        const iv = crypto.randomBytes(16);
        const result = { ...message, vector: iv.toString("hex") };
        for (const field of ["description", "candidate", "candidates"]) {
            if (!message[field]) continue;
            const cipher = crypto.createCipheriv("aes-256-cbc", key, iv);
            result[field] = Buffer.concat([cipher.update(JSON.stringify(message[field])), cipher.final()]).toString("hex");
        }
        return result;
    }
    function decode(raw) {
        const message = JSON.parse(raw);
        if (!message.vector) return message;
        for (const field of ["description", "candidate", "candidates"]) {
            if (typeof message[field] !== "string") continue;
            const cipher = crypto.createDecipheriv("aes-256-cbc", key, Buffer.from(message.vector, "hex"));
            message[field] = JSON.parse(Buffer.concat([cipher.update(Buffer.from(message[field], "hex")), cipher.final()]).toString());
        }
        return message;
    }
    const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    await once(server, "listening");
    let turnProcess, discovery, bridge, publisherBridge, publisherAddress, turnAddress;
    let relayCandidates = 0, discoveryRequests = 0, relayedPackets = 0, relayedMediaPackets = 0;
    if (relay || discoveryFailure) {
        let port;
        if (relay) {
            const executable = path.join(path.dirname(cliExecutable), "webrtc_vst_turn_server_fixture" + (process.platform === "win32" ? ".exe" : ""));
            turnProcess = spawn(executable, [], { stdio: ["pipe", "pipe", "inherit"] });
            port = await new Promise((resolve, reject) => {
                const timer = setTimeout(() => reject(new Error("TURN fixture did not start")), 5000);
                turnProcess.once("error", error => { clearTimeout(timer); reject(error); });
                turnProcess.stdout.once("data", data => { clearTimeout(timer); resolve(Number(String(data).trim())); });
            });
            assert.ok(port > 0 && port < 65536);
        }
        discovery = http.createServer((request, response) => {
            discoveryRequests++;
            assert.ok(new URL(request.url, "http://localhost").searchParams.has("ts"));
            response.writeHead(discoveryFailure ? 503 : 200, { "Content-Type": "application/json" });
            response.end(JSON.stringify({ servers: [{ urls: [`turn:127.0.0.1:${port}`], username: "owned-test", credential: "owned-test-password", udp: true }] }));
        });
        discovery.listen(0, "127.0.0.1");
        await once(discovery, "listening");
    }
    const peer = new wrtc.RTCPeerConnection({ iceServers: [] });
    const source = new wrtc.nonstandard.RTCAudioSource();
    const audio = source.createTrack();
    const videoSource = videoFirst || audioFirst ? new wrtc.nonstandard.RTCVideoSource() : null;
    const video = videoSource?.createTrack();
    const videoTimer = videoSource ? setInterval(() => videoSource.onFrame({
        width: 160, height: 120, data: Buffer.alloc(160 * 120 * 3 / 2, 80)
    }), 100) : null;
    if (videoFirst) peer.addTransceiver(video, { direction: "sendonly" });
    if (!dataFirst) peer.addTransceiver(audio, { direction: sendRecv ? "sendrecv" : "sendonly" });
    if (audioFirst) peer.addTransceiver(video, { direction: "sendonly" });
    const dc = dataFirst || nativeRouting ? peer.createDataChannel("sendChannel") : null;
    const tone = startTestTone(source);
    let child, logs = "", failure, started = false, answerSeen = false, preferencesSeen = false, mediaAnswerSeen = false, pongSeen = false;
    let chain = Promise.resolve();
    let misrouted = 0;
    if (dc) dc.onmessage = ({ data }) => {
        chain = chain.then(async () => {
            const message = decode(data);
            if (typeof message.audio === "boolean") {
                assert.equal(message.audio, true);
                assert.equal(message.video, false);
                preferencesSeen = true;
                dc.send(JSON.stringify({ ping: 123456789 }));
                if (!dataFirst) return;
                assert.equal(peer.getSenders().length, 0, "Media sent before preferences");
                peer.addTransceiver(audio, { direction: "sendonly" });
                const offer = await peer.createOffer();
                await peer.setLocalDescription(offer);
                dc.send(JSON.stringify(encode({ description: relay ? withoutCandidates(offer) : offer })));
            } else if (message.pong === 123456789) {
                pongSeen = true;
            } else if (message.description) {
                await peer.setRemoteDescription(relay ? withoutCandidates(message.description) : message.description);
                mediaAnswerSeen = true;
            } else if (message.candidates) {
                for (const candidate of message.candidates) await peer.addIceCandidate(candidate);
            }
        }).catch(error => { failure ||= error; });
    };
    const candidates = [];
    peer.onicecandidate = ({ candidate }) => { if (candidate) candidates.push(candidate.toJSON()); };
    server.on("connection", socket => {
        const send = payload => socket.send(JSON.stringify(encode({ UUID: "native-publisher", ...(omitSession ? {} : { session: "native-session" }), ...payload })));
        socket.on("message", raw => {
            chain = chain.then(async () => {
                const message = decode(raw);
                if (nativeRouting && message.request !== "play") {
                    // The official relay rewrites UUID to its assigned sender
                    // identity. Android prefers a supplied `from` over UUID;
                    // an invented `from` therefore addresses no native peer.
                    const delivered = { ...message, UUID: "server-assigned-viewer" };
                    const nativePeerKey = delivered.from ?? delivered.UUID;
                    if (nativePeerKey !== "server-assigned-viewer") { misrouted++; return; }
                }
                if (message.request === "play" && !started) {
                    started = true;
                    const offer = await peer.createOffer();
                    if (dataFirst) {
                        assert.match(offer.sdp, /m=application /);
                        assert.doesNotMatch(offer.sdp, /m=(audio|video) /);
                    }
                    await peer.setLocalDescription(offer);
                    // Wait for candidates so their ordering is deterministic.
                    const deadline = Date.now() + 4000;
                    while (peer.iceGatheringState !== "complete") {
                        assert.ok(Date.now() < deadline, "ICE gathering timed out");
                        await new Promise(r => setTimeout(r, 10));
                    }
                    assert.ok(candidates.length > 0);
                    if (earlyIce) {
                        for (const candidate of candidates) send({ candidate, type: "local" });
                        await new Promise(r => setTimeout(r, 150));
                    }
                    const sentOffer = failureProbe === "dtls" ? { ...offer, sdp: offer.sdp.replace(/(a=fingerprint:sha-256 )([0-9A-F]{2})/i,
                        (_, prefix, byte) => prefix + (byte === "00" ? "01" : "00")) } : offer;
                    send({ description: sentOffer }); // original SDP has no candidates
                    if (failureProbe === "ice") {
                        send({ candidate: { candidate: "candidate:1 1 udp 2122260223 127.0.0.1 9 typ host", sdpMid: "0", sdpMLineIndex: 0 }, type: "local" });
                    } else if (reorder) {
                        // Deterministic reordering/duplication plus old-generation
                        // and malformed candidates before valid native-style ones.
                        send({ session: "obsolete-generation", candidate: candidates[0], type: "local" });
                        send({ candidates: [null, 17, {}, { candidate: false }, { candidate: "bad-candidate" }], type: "local" });
                        await new Promise(r => setTimeout(r, 200));
                        for (const candidate of [...candidates].reverse()) {
                            send({ candidate, type: "local" });
                            send({ candidate, type: "local" });
                        }
                    } else if (earlyIce) {
                        // Do not resend: connection depends on the pre-SDP queue.
                    } else if (relay) {
                        const candidate = candidates.find(item => /^candidate:\S+ 1 udp \d+ [\d.]+ /.test(item.candidate));
                        assert.ok(candidate, "No IPv4 UDP publisher candidate");
                        const fields = candidate.candidate.split(" ");
                        publisherAddress = { address: fields[4], port: Number(fields[5]) };
                        publisherBridge = dgram.createSocket("udp4");
                        publisherBridge.on("error", error => { failure ||= error; });
                        publisherBridge.on("message", (packet, from) => {
                            if (turnAddress && from.port === publisherAddress.port) {
                                if (packet.length >= 12 && (packet[0] & 0xc0) === 0x80 && (packet[1] < 192 || packet[1] > 223))
                                    relayedMediaPackets++;
                                bridge.send(packet, turnAddress.port, turnAddress.address);
                            }
                        });
                        publisherBridge.bind(0, publisherAddress.address);
                        await once(publisherBridge, "listening");
                        bridge = dgram.createSocket("udp4");
                        bridge.on("error", error => { failure ||= error; });
                        bridge.on("message", (packet, from) => {
                            if (turnAddress && from.port === turnAddress.port && from.address === turnAddress.address) {
                                relayedPackets++;
                                publisherBridge.send(packet, publisherAddress.port, publisherAddress.address);
                            }
                            // Drop all plugin host/srflx traffic. Both directions
                            // must traverse its real TURN allocation; no OS rules.
                        });
                        bridge.bind(0, "127.0.0.1");
                        await once(bridge, "listening");
                        fields[4] = "127.0.0.1";
                        fields[5] = String(bridge.address().port);
                        send({ candidate: { ...candidate, candidate: fields.join(" ") }, type: "local" });
                    } else if (candidateBundle) {
                        send({ candidates: [null, { candidate: 17 }, { candidate: "malformed" },
                            ...candidates.map(candidate => ({ ...candidate, sdpMid: null }))], type: "local" });
                    } else for (const candidate of candidates) send({ candidate, type: "local" });
                } else if (message.description) {
                    assert.equal(message.description.type, "answer");
                    // Inline answer candidates must not bypass the relay or
                    // malformed-bundle path being tested.
                    await peer.setRemoteDescription(relay || candidateBundle || earlyIce || reorder || failureProbe ? withoutCandidates(message.description) : message.description);
                    answerSeen = true;
                } else if (message.candidates) {
                    if (candidateBundle || earlyIce || reorder || failureProbe === "ice") return; // depend on native candidates, not peer-reflexive recovery
                    for (const candidate of message.candidates) {
                        if (relay && !candidate.candidate.includes(" typ relay ")) continue;
                        if (relay) {
                            if (relayCandidates++) continue;
                            const fields = candidate.candidate.split(" ");
                            turnAddress = { address: fields[4], port: Number(fields[5]) };
                            fields[4] = publisherAddress.address;
                            fields[5] = String(publisherBridge.address().port);
                            candidate.candidate = fields.join(" ");
                        }
                        await peer.addIceCandidate(candidate);
                    }
                }
            }).catch(error => { failure ||= error; });
        });
    });
    try {
        const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
        Object.assign(env, {
            WEBRTC_VST_MODE: "play", WEBRTC_VST_STREAM_ID: "native_owned_test", WEBRTC_VST_PASSWORD: encrypted ? password : "off",
            WEBRTC_VST_HANDSHAKE_URL: `ws://127.0.0.1:${server.address().port}/`, WEBRTC_VST_DISABLE_STUN: "1",
            WEBRTC_VST_LOG_STDOUT: "1", WEBRTC_CLI_HOST_MONITOR_OUTPUT: "1", WEBRTC_CLI_HOST_MONITOR_STATUS: "1",
            WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: failureProbe === "ice" ? "45000" : failureProbe ? "12000" : "7000",
            WEBRTC_CLI_HOST_TIMEOUT_MS: failureProbe ? "55000" : "12000",
            WEBRTC_CLI_HOST_REALTIME_PACING: "1", WEBRTC_CLI_HOST_CONTINUOUS_WARMUP: "1"
        });
        if (discovery) env.WEBRTC_VST_TURN_SERVER_LIST_URL = `http://127.0.0.1:${discovery.address().port}/`;
        child = spawn(cliExecutable, [pluginBundle], { env, stdio: ["ignore", "pipe", "pipe"] });
        for (const pipe of [child.stdout, child.stderr]) pipe.on("data", data => { logs += data; });
        const [code] = await once(child, "close");
        await chain;
        if (failure) throw failure;
        assert.equal(code, 0, logs);
        assert.equal(misrouted, 0, "Native publisher could not route the answer/candidates: invented sender identity");
        assert.ok(answerSeen, "No answer\n" + logs);
        if (failureProbe) {
            assert.match(logs, failureProbe === "ice" ? /Peer connection failed \(ICE connectivity\)/ : /Peer failed after ICE connected \(DTLS\/media\)/);
            assert.equal(Number(logs.match(/output_rms=([\d.]+)/)?.[1]), 0);
            console.log(`PASS: ${name}; diagnosed ${failureProbe} failure, no audio`);
            return;
        }
        if (dataFirst) {
            assert.ok(preferencesSeen, "No data-channel preferences");
            assert.ok(mediaAnswerSeen, "No media answer over data channel");
            assert.ok(pongSeen, "Publisher liveness ping was not answered");
        }
        if (nativeRouting) assert.ok(preferencesSeen && pongSeen, "Native control channel did not open");
        assert.match(logs, /Peer connection connected:/);
        assert.doesNotMatch(logs, /no media transport|Failed to apply remote offer|Peer connection failed:/);
        const rms = Number(logs.match(/output_rms=([\d.]+)/)?.[1]);
        assert.ok(rms > 0.01, "No received tone\n" + logs);
        if (discovery) assert.equal(discoveryRequests, 1);
        if (discoveryFailure) assert.match(logs, /TURN discovery unavailable/);
        if (relay) {
            assert.ok(relayCandidates > 0, "Plugin did not gather a relay candidate");
            assert.ok(relayedPackets > 0 && relayedMediaPackets > 100, "Audio did not traverse the TURN allocation");
            const stats = await peer.getStats();
            const selected = [...stats.values()].find(stat => stat.type === "candidate-pair" && stat.nominated && stat.state === "succeeded");
            assert.equal(stats.get(selected?.remoteCandidateId)?.candidateType, "relay", "Audio bypassed TURN");
        }
        console.log(`PASS: ${name}; received RMS=${rms}`);
    } catch (error) {
        if (relay) console.error({ relayCandidates, relayedPackets, relayedMediaPackets, publisherAddress, turnAddress });
        console.error(logs);
        throw error;
    } finally {
        if (child && child.exitCode === null) { const closed = once(child, "close"); child.kill(); await closed; }
        if (videoTimer) clearInterval(videoTimer);
        tone.stop(); audio.stop(); video?.stop(); peer.close();
        for (const socket of server.clients) socket.terminate();
        await new Promise(resolve => server.close(resolve));
        if (discovery) await new Promise(resolve => discovery.close(resolve));
        if (bridge) bridge.close();
        if (publisherBridge) publisherBridge.close();
        if (turnProcess && turnProcess.exitCode === null) { const closed = once(turnProcess, "close"); turnProcess.stdin.end("stop\n"); await closed; }
    }
}

(async () => {
    if (process.argv.includes("--sender-routing")) {
        await check("native microphone publisher with server-assigned sender routing", { encrypted: true, nativeRouting: true });
        return;
    }
    if (process.env.WEBRTC_TEST_FAILURE_DIAGNOSTICS === "1" || process.argv.includes("--failure-diagnostics")) {
        await check("unreachable native ICE candidates", { encrypted: true, failureProbe: "ice" });
        await check("native DTLS fingerprint mismatch", { encrypted: true, failureProbe: "dtls" });
        return;
    }
    if (process.env.WEBRTC_TEST_NATIVE_MATRIX === "1" || process.argv.includes("--native-matrix")) {
        await check("native microphone publisher with server-assigned sender routing", { encrypted: true, nativeRouting: true });
        await check("native microphone-only, custom password, pre-SDP ICE", { encrypted: true, password: "owned-native-password", earlyIce: true });
        await check("legacy native microphone-only without session field", { omitSession: true, earlyIce: true });
        await check("native sendrecv audio, reordered/duplicate/stale ICE", { encrypted: true, sendRecv: true, reorder: true });
        await check("native audio-first A/V offer", { encrypted: true, audioFirst: true, reorder: true });
        await check("native video-first A/V offer with early ICE", { encrypted: true, videoFirst: true, earlyIce: true });
        return;
    }
    if (process.env.WEBRTC_TEST_RELAY_ONLY === "1") {
        await check("native audio through TURN with direct connectivity blocked", { encrypted: true, relay: true });
        return;
    }
    await check("data-channel preferences before media", { dataFirst: true });
    await check("encrypted data-channel preferences before media", { dataFirst: true, encrypted: true });
    await check("native microphone publisher with server-assigned sender routing", { encrypted: true, nativeRouting: true });
    await check("native audio-only");
    await check("valid ICE survives malformed bundle entries and null mids", { encrypted: true, candidateBundle: true });
    await check("encrypted native video-first offer", { videoFirst: true, encrypted: true });
    await check("native audio through TURN with direct connectivity blocked", { encrypted: true, relay: true });
    await check("data-channel-first audio through TURN", { dataFirst: true, encrypted: true, relay: true });
    await check("direct connectivity survives TURN discovery outage", { discoveryFailure: true });
})().then(() => process.exit(0), error => { console.error(error); process.exit(1); });
