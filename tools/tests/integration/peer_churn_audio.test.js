#!/usr/bin/env node
"use strict";
// Two actual plugin hosts, with signaling confined to an owned loopback server.
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path");
const {spawn} = require("node:child_process"), {once} = require("node:events");
const {WebSocket, WebSocketServer} = require("ws");
const {rootDir, cliExecutable, pluginBundle} = require("./build_paths");
const {analyzeTone} = require("../audio_click_analysis");
const directory = path.join(rootDir, "build/test-tools/peer-churn-" + Date.now());
fs.mkdirSync(directory, {recursive:true});
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const children = new Set(), sockets = new Map(), logs = {}, windows = [];
const churnSessions = new Set(), recovered = new Set();
let server, failure, started = false, sent = 0, analysis;
function fail(error) { failure ||= error; for (const child of children) child.kill(); }
async function waitFor(predicate, message, milliseconds = 8000) {
    const deadline = Date.now() + milliseconds;
    while (!predicate()) {
        if (failure) throw failure;
        assert.ok(Date.now() < deadline, message);
        await pause(25);
    }
}
function send(socket, message) {
    assert.equal(socket?.readyState, WebSocket.OPEN, "Owned signaling socket closed");
    socket.send(JSON.stringify(message));
}
function launch(mode) {
    const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
    Object.assign(env, {
        WEBRTC_VST_MODE:mode, WEBRTC_VST_STREAM_ID:"owned-churn", WEBRTC_VST_PASSWORD:"off",
        WEBRTC_VST_HANDSHAKE_URL:`ws://127.0.0.1:${server.address().port}/`, WEBRTC_VST_DISABLE_STUN:"1",
        WEBRTC_VST_LOG_STDOUT:"1", WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS:"28000",
        WEBRTC_CLI_HOST_TIMEOUT_MS:"33000", WEBRTC_CLI_HOST_WARMUP_MS:"1",
        WEBRTC_CLI_HOST_REALTIME_PACING:"1", WEBRTC_CLI_HOST_QUALITY:"1",
        WEBRTC_CLI_HOST_TONE_HZ:"1000", WEBRTC_CLI_HOST_RIGHT_TONE_HZ:"1500"
    });
    if (mode === "play") env.WEBRTC_CLI_HOST_CAPTURE = path.join(directory,"play.pcmblocks");
    const child = spawn(cliExecutable, [pluginBundle], {env, stdio:["ignore","pipe","pipe"]});
    children.add(child); logs[mode] = "";
    let pending = "";
    for (const pipe of [child.stdout, child.stderr]) pipe.on("data", data => {logs[mode] += data;});
    child.stdout.on("data", data => {
        pending += data;
        const lines = pending.split("\n"); pending = lines.pop();
        if (mode === "play") for (const line of lines) if (line.startsWith("[quality]"))
            windows.push(Object.fromEntries([...line.matchAll(/(\w+)=([\d.eE+-]+)/g)].map(([,k,v]) => [k,Number(v)])));
    });
    const done = once(child,"close").then(([code]) => {
        children.delete(child);
        assert.equal(code, 0, `${mode} failed; see ${directory}`);
        assert.ok(!/ThreadSanitizer|AddressSanitizer|runtime error:/.test(logs[mode]), "Sanitizer diagnostic");
    });
    done.catch(fail);
    return done;
}
for (const signal of ["SIGINT","SIGTERM"]) process.on(signal, () => fail(new Error(signal)));
const watchdog = setTimeout(() => fail(new Error("Peer churn test timed out")), 40000);
(async () => {
    console.log("Peer churn audio test:", directory);
    server = new WebSocketServer({host:"127.0.0.1",port:0}); await once(server,"listening");
    server.on("connection", socket => {
        let mode;
        socket.on("error", fail);
        socket.on("message", raw => {
            try {
                const message = JSON.parse(raw);
                if (message.request === "seed" || message.request === "play") {
                    mode = message.request; sockets.set(mode,socket);
                    if (!started && sockets.has("seed") && sockets.has("play")) {
                        started = true; send(sockets.get("seed"), {request:"offerSDP",UUID:"owned-play"});
                    }
                } else if (message.UUID === "owned-churn-peer") {
                    if (message.description?.type === "offer") churnSessions.add(message.session);
                } else if (message.UUID?.startsWith("owned-recovery-")) {
                    if (message.description?.type === "offer") recovered.add(message.UUID);
                } else if (message.UUID === (mode === "seed" ? "owned-play" : "owned-seed")) {
                    send(sockets.get(mode === "seed" ? "play" : "seed"), {...message,UUID:"owned-"+mode});
                }
            } catch (error) {fail(error);}
        });
    });
    const seed = launch("seed"), play = launch("play");
    await waitFor(() => windows.some(row => row.seconds >= 6 && row.rms_l > .08 && row.rms_r > .08), "Missing live audio baseline", 12000);
    for (let round = 0; round < 3; ++round) {
        const before = churnSessions.size;
        for (let i=0; i<128; ++i) {
            send(sockets.get("seed"), {request:"offerSDP",UUID:"owned-churn-peer"}); ++sent;
            if (i % 16 === 15) await pause(25);
        }
        await waitFor(() => churnSessions.size >= before + 8, "Churn did not create enough distinct peer generations");
        await pause(500);
        const uuid = "owned-recovery-" + round;
        send(sockets.get("seed"), {request:"offerSDP",UUID:uuid});
        await waitFor(() => recovered.has(uuid), "New peer negotiation did not recover after burst");
        console.log(`Round ${round+1}: ${churnSessions.size-before} peer generations, subsequent offer succeeded`);
        await pause(2000);
    }
    await Promise.all([seed,play]);
    if (failure) throw failure;
    const steady = windows.filter(row => row.seconds >= 6 && row.seconds < 26);
    assert.ok(steady.length >= 18, "Insufficient sustained audio windows");
    for (const row of steady) {
        assert.equal(row.nonfinite,0); assert.equal(row.silent_blocks,0,"Live peer lost audio during another peer's replacement");
        assert.ok(row.rms_l > .08 && row.rms_l < .4 && row.rms_r > .08 && row.rms_r < .4, "Audio level failure");
        assert.ok(Math.abs(row.hz_l-1000) < 40 && Math.abs(row.hz_r-1500) < 40, "Pitch/channel failure");
    }
    const pcm = fs.readFileSync(path.join(directory,"play.pcmblocks"));
    const block = 256, rate = 48000, stride = 8+block*2*4;
    assert.equal(pcm.length % stride,0);
    const first = Math.ceil(6*rate/block), last = Math.min(Math.floor(26*rate/block),pcm.length/stride);
    const channels = [new Float32Array((last-first)*block),new Float32Array((last-first)*block)];
    for (let b=first; b<last; ++b) for (let ch=0; ch<2; ++ch) for (let n=0; n<block; ++n)
        channels[ch][(b-first)*block+n] = pcm.readFloatLE(b*stride+8+(ch*block+n)*4);
    analysis = channels.map((samples,ch) => analyzeTone(samples,rate,ch?1500:1000,block));
    for (const row of analysis) {
        assert.equal(row.nonfinite,0); assert.ok(row.peak < 1);
        assert.equal(row.suspiciousSamples,0,"Tone discontinuity during peer replacement");
    }
    console.log(`PASS: ${sent} replacement requests, ${churnSessions.size} distinct offers, three recovery offers; uninterrupted live audio`);
})().catch(error => {fail(error); console.error(error); process.exitCode=1;}).finally(async () => {
    clearTimeout(watchdog);
    for (const child of children) child.kill();
    if (server) {for (const socket of server.clients) socket.terminate(); await new Promise(resolve => server.close(resolve));}
    for (const [mode,log] of Object.entries(logs)) fs.writeFileSync(path.join(directory,mode+".log"),log);
    fs.writeFileSync(path.join(directory,"results.json"),JSON.stringify({passed:!failure,error:failure?.message,sent,
        peerGenerations:churnSessions.size,recoveryOffers:recovered.size,analysis,windows},null,2));
});
