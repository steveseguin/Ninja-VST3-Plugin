#!/usr/bin/env node
"use strict";

// Owned loopback VDO control peer + actual WHEP HTTP/RTC endpoint. No SDK,
// public services, camera or microphone. Mirrors webrtc.js control envelopes.
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const http = require("node:http");
const { once } = require("node:events");
const { spawn } = require("node:child_process");
const { WebSocketServer } = require("ws");
const wrtc = require("@roamhq/wrtc");
const { startTestTone } = require("../helpers/paced_tone");
const { cliExecutable, pluginBundle } = require("./build_paths");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const key = crypto.createHash("sha256").update("someEncryptionKey123vdo.ninja").digest();
function encrypt(value, iv) {
    const cipher = crypto.createCipheriv("aes-256-cbc", key, iv);
    return Buffer.concat([cipher.update(JSON.stringify(value)), cipher.final()]).toString("hex");
}
function encode(fields) {
    const iv = crypto.randomBytes(16), result = { ...fields, vector: iv.toString("hex") };
    for (const field of ["description", "candidate", "candidates"]) if (fields[field]) result[field] = encrypt(fields[field], iv);
    return result;
}
function decode(raw) {
    const message = JSON.parse(raw);
    for (const field of ["description", "candidate", "candidates"]) {
        if (!message.vector || typeof message[field] !== "string") continue;
        const cipher = crypto.createDecipheriv("aes-256-cbc", key, Buffer.from(message.vector, "hex"));
        message[field] = JSON.parse(Buffer.concat([cipher.update(Buffer.from(message[field], "hex")), cipher.final()]).toString());
    }
    return message;
}
async function gathered(pc) {
    const end = Date.now() + 5000;
    while (pc.iceGatheringState !== "complete") {
        assert.ok(Date.now() < end, "Fixture ICE gathering timeout");
        await sleep(10);
    }
}

async function check(name, { wssControl = false, video = false, failureResponse = "", replace = false } = {}) {
    const sessions = [], timers = [], expectedToken = "owned-whep-token";
    let posts = 0, deletes = 0, chain = Promise.resolve(), failure, socket, child, output = "", pong = false;
    const control = new wrtc.RTCPeerConnection({ iceServers: [] });
    const dc = control.createDataChannel("sendChannel");
    const server = http.createServer((request, response) => {
        (async () => {
            assert.equal(request.headers.authorization, "Bearer " + expectedToken);
            if (request.method === "DELETE") { deletes++; response.writeHead(200); response.end(); return; }
            assert.equal(request.method, "POST");
            assert.equal(request.headers["content-type"], "application/sdp");
            posts++;
            let sdp = "";
            for await (const chunk of request) sdp += chunk;
            assert.match(sdp, /m=audio /); assert.match(sdp, /m=video /);
            assert.match(sdp, /a=recvonly/); assert.doesNotMatch(sdp, /a=sendrecv|a=sendonly/);
            assert.match(sdp, /a=candidate:/, "Full ICE candidates must be in the POST");
            if (failureResponse === "unauthorized") { response.writeHead(401); response.end(); return; }
            if (failureResponse === "redirect") { response.writeHead(307, { Location: "http://127.0.0.1:1/never" }); response.end(); return; }
            if (failureResponse === "oversized") { response.writeHead(201, { "Content-Type": "application/sdp" }); response.end("v=0\r\n" + "x".repeat(300000)); return; }
            if (failureResponse === "bad-sdp") { response.writeHead(201, { "Content-Type": "application/sdp", Location: "/session/bad" }); response.end("not an SDP answer"); return; }
            const pc = new wrtc.RTCPeerConnection({ iceServers: [] });
            const source = new wrtc.nonstandard.RTCAudioSource(), track = source.createTrack();
            const videoSource = video ? new wrtc.nonstandard.RTCVideoSource() : null;
            const videoTrack = videoSource?.createTrack();
            const item = { pc, track, videoTrack, tone: startTestTone(source) };
            sessions.push(item);
            await pc.setRemoteDescription({ type: "offer", sdp });
            // Use the offered sections, including video-first ordering.
            for (const transceiver of pc.getTransceivers()) {
                const offeredVideo = transceiver.receiver.track.kind === "video";
                const sendTrack = offeredVideo ? videoTrack : track;
                if (sendTrack) { await transceiver.sender.replaceTrack(sendTrack); transceiver.direction = "sendonly"; }
                else transceiver.direction = "inactive";
            }
            await pc.setLocalDescription(await pc.createAnswer());
            await gathered(pc);
            response.writeHead(201, { "Content-Type": "application/sdp", Location: "/session/" + sessions.length });
            response.end(pc.localDescription.sdp);
            if (videoSource) timers.push(setInterval(() => videoSource.onFrame({ width: 160, height: 120, data: Buffer.alloc(160 * 120 * 3 / 2, 80) }), 100));
        })().catch(error => { failure ||= error; response.writeHead(500); response.end(); });
    });
    server.listen(0, "127.0.0.1"); await once(server, "listening");
    const endpoint = `http://127.0.0.1:${server.address().port}/whep`;
    const signaling = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    await once(signaling, "listening");
    function sendControl(settings, session = "owned-generation", encrypted = true) {
        if (!wssControl) { dc.send(JSON.stringify({ whepSettings: settings, session })); return; }
        const envelope = { whepSettings: settings, session };
        if (encrypted) {
            const iv = crypto.randomBytes(16);
            socket.send(JSON.stringify({ UUID: "owned-publisher", session, vector: iv.toString("hex"), whepSettings: encrypt(envelope, iv) }));
        } else socket.send(JSON.stringify({ UUID: "owned-publisher", ...envelope }));
    }
    dc.onmessage = ({ data }) => {
        chain = chain.then(async () => {
            const message = JSON.parse(data);
            if (message.pong === 777) { pong = true; return; }
            if (message.audio !== true) return;
            assert.equal(message.video, false);
            const valid = { type: "whep", url: endpoint, token: expectedToken };
            // Fixed malformed corpus and stale identities must not cause HTTP.
            for (const malformed of [null, [], 17, {}, { ...valid, token: "bad\r\nInjected: header" },
                { ...valid, url: "file:///etc/passwd" }, { ...valid, url: "http://example.com/whep" },
                { ...valid, url: "https://name:password@example.com/whep" }, { ...valid, media: "screen" }]) sendControl(malformed);
            sendControl(valid, "stale-generation");
            if (wssControl) sendControl(valid, "owned-generation", false); // password-protected WSS requires encryption
            await sleep(250);
            assert.equal(posts, 0, "Invalid/unauthenticated control triggered HTTP");
            sendControl(valid);
            // Duplicate broadcasts must not allocate a second session.
            sendControl(valid);
            dc.send(JSON.stringify({ ping: 777 }));
            if (replace) timers.push(setTimeout(() => sendControl({ ...valid, url: endpoint + "?replacement=1" }), 2500));
        }).catch(error => { failure ||= error; });
    };
    signaling.on("connection", ws => {
        socket = ws;
        let played = false;
        ws.on("message", raw => {
            chain = chain.then(async () => {
                const message = decode(raw);
                if (message.request === "play" && !played) {
                    played = true;
                    await control.setLocalDescription(await control.createOffer());
                    await gathered(control);
                    ws.send(JSON.stringify(encode({ UUID: "owned-publisher", session: "owned-generation", description: control.localDescription })));
                } else if (message.description) await control.setRemoteDescription(message.description);
                else if (message.candidates) for (const candidate of message.candidates) await control.addIceCandidate(candidate);
            }).catch(error => { failure ||= error; });
        });
    });
    try {
        const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
        Object.assign(env, { WEBRTC_VST_MODE: "play", WEBRTC_VST_STREAM_ID: "owned-whep", WEBRTC_VST_PASSWORD: "",
            WEBRTC_VST_HANDSHAKE_URL: `ws://127.0.0.1:${signaling.address().port}/`, WEBRTC_VST_DISABLE_STUN: "1",
            WEBRTC_VST_LOG_STDOUT: "1", WEBRTC_VST_LOG_SIGNALING: "1", WEBRTC_CLI_HOST_MONITOR_OUTPUT: "1",
            WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: "7000", WEBRTC_CLI_HOST_TIMEOUT_MS: "18000",
            WEBRTC_CLI_HOST_REALTIME_PACING: "1", WEBRTC_CLI_HOST_CONTINUOUS_WARMUP: "1" });
        child = spawn(cliExecutable, [pluginBundle], { env, stdio: ["ignore", "pipe", "pipe"] });
        for (const pipe of [child.stdout, child.stderr]) pipe.on("data", data => { output += data; });
        const [code] = await once(child, "close"); await chain;
        if (failure) throw failure;
        assert.equal(code, 0, output);
        assert.equal(posts, replace ? 2 : 1, "Unexpected WHEP allocations");
        assert.ok(pong, "Control peer was lost during WHEP setup");
        assert.ok(!output.includes(expectedToken), "Bearer token leaked into plugin logs");
        const rms = Number(output.match(/output_rms=([\d.]+)/)?.[1] || 0);
        if (failureResponse) {
            assert.equal(rms, 0);
            assert.match(output, /WHEP (HTTP|returned)/);
            assert.equal(deletes, failureResponse === "bad-sdp" ? 1 : 0);
        } else {
            assert.ok(rms > 0.01, "No WHEP audio");
            assert.equal(deletes, posts, "Allocated HTTP resources were not deleted");
            if (video) {
                const stats = await sessions.at(-1).pc.getStats();
                assert.ok([...stats.values()].some(s => s.type === "outbound-rtp" && s.kind === "video" && s.bytesSent > 0), "Fixture did not actually send video");
            }
        }
        console.log(`PASS ${name}: posts=${posts} deletes=${deletes} RMS=${rms}`);
    } catch (error) { console.error(output); throw error; }
    finally {
        for (const timer of timers) clearInterval(timer);
        if (child?.exitCode === null) { const closed = once(child, "close"); child.kill(); await closed; }
        control.close();
        for (const item of sessions) { item.tone.stop(); item.track.stop(); item.videoTrack?.stop(); item.pc.close(); }
        for (const ws of signaling.clients) ws.terminate();
        await new Promise(resolve => signaling.close(resolve));
        server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    }
}

(async () => {
    const cases = [
        ["data-channel WHEP audio", {}],
        ["encrypted WSS WHEP with video", { wssControl: true, video: true }],
        ["WHEP URL replacement", { replace: true, video: true }],
        ...["unauthorized", "redirect", "bad-sdp", "oversized"].map(failureResponse => [failureResponse, { failureResponse }])
    ];
    for (const [name, options] of cases) {
        if (!process.env.WEBRTC_TEST_WHEP_CASE || process.env.WEBRTC_TEST_WHEP_CASE === name) await check(name, options);
    }
})().then(() => process.exit(0), error => { console.error(error); process.exit(1); });
