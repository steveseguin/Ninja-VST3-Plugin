#!/usr/bin/env node
"use strict";
// Only an owned loopback signaling server; no malformed traffic is sent publicly.
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path");
const {spawn} = require("node:child_process"), {once} = require("node:events");
const {WebSocketServer} = require("ws");
const {rootDir, cliExecutable, pluginBundle} = require("./build_paths");
const output = path.join(rootDir, "build/test-tools/signaling-fuzz-" + Date.now());
fs.mkdirSync(output, {recursive:true});
const initialSeed = Number(process.env.WEBRTC_FUZZ_SEED || 0x56444f) >>> 0;
let randomState = initialSeed;
function random() { randomState ^= randomState << 13; randomState ^= randomState >>> 17; randomState ^= randomState << 5; return randomState >>> 0; }
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function check(mode) {
    const server = new WebSocketServer({host:"127.0.0.1", port:0});
    await once(server,"listening");
    let child, logs = "", announcements = 0, sent = 0, healthyOffer = false, task, failure;
    const corpus = [
        {request:"offerSDP", UUID:"owned-fuzz"},
        {request:"alert", message:"owned alert"},
        {UUID:"owned-fuzz", description:{type:"offer", sdp:"v=0\r\n"}},
        {UUID:"owned-fuzz", description:{type:"answer", sdp:"invalid"}},
        {UUID:"owned-fuzz", candidate:{candidate:"candidate:1 1 UDP 1 127.0.0.1 9 typ host", sdpMid:"0"}},
        {UUID:"owned-fuzz", candidates:[null, {}, 17]},
        {vector:"00", description:"bad cipher text"}
    ];
    const values = [null, [], {}, true, false, 0, -1, 1e100, "", "x", "é🔊", [1,null,{}]];
    server.on("connection", socket => socket.on("message", raw => {
        try {
            const message = JSON.parse(raw);
            if (message.UUID === "owned-healthy" && message.description?.type === "offer") healthyOffer = true;
            if (message.request !== mode) return;
            ++announcements;
            if (announcements > 1) {
                socket.send(JSON.stringify({request:"alert", message: mode === "seed" ? "Stream ID already in use" : "owned recovery confirmed"}));
                return;
            }
            task = (async () => {
                for (let i=0; i<2000; ++i) {
                    let input = structuredClone(corpus[random() % corpus.length]);
                    const keys = ["UUID","uuid","session","request","message","description","candidate","candidates","vector","streamID"];
                    input[keys[random() % keys.length]] = values[random() % values.length];
                    let text = JSON.stringify(input);
                    if (i % 3 === 0) {
                        const at = random() % (text.length + 1);
                        text = text.slice(0,at) + String.fromCharCode(random() % 128) + text.slice(at + (random() % 2));
                    }
                    // Keep well-typed peer identities in one owned slot. The
                    // separate limits test covers exhaustion of the 16-peer cap;
                    // this test needs a free slot to verify subsequent recovery.
                    try {
                        const decoded = JSON.parse(text);
                        if (decoded && typeof decoded === "object" && !Array.isArray(decoded)) {
                            for (const key of ["UUID","uuid"]) if (typeof decoded[key] === "string") decoded[key] = "owned-fuzz";
                            if (typeof decoded.session === "string") decoded.session = "";
                            text = JSON.stringify(decoded);
                        }
                    } catch {}
                    socket.send(text); ++sent;
                    if (i % 40 === 39) await pause(10);
                }
                // Explicit size/depth boundaries and redacted parser errors.
                for (const text of ["[".repeat(33)+"0"+"]".repeat(33), " ".repeat(262145), '{"password":secret-sentinel']) {
                    socket.send(text); ++sent;
                }
                await pause(300);
                if (mode === "seed") {
                    socket.send(JSON.stringify({request:"offerSDP", UUID:"owned-healthy"}));
                    for (let i=0; i<100 && !healthyOffer; ++i) await pause(100);
                    assert.ok(healthyOffer,"Valid offer did not work after malformed signaling");
                }
                // Leave a known parser error visible long enough for UI polling.
                socket.send("{broken-final"); ++sent;
                await pause(400);
                socket.close(1012,"owned fuzz recovery");
            })().catch(error => {failure = error; child?.kill();});
        } catch (error) {failure = error; child?.kill();}
    }));
    try {
        const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
        Object.assign(env, {WEBRTC_VST_MODE:mode, WEBRTC_VST_STREAM_ID:"owned-fuzz", WEBRTC_VST_PASSWORD:"off",
            WEBRTC_VST_HANDSHAKE_URL:`ws://127.0.0.1:${server.address().port}/`, WEBRTC_VST_DISABLE_STUN:"1", WEBRTC_VST_LOG_STDOUT:"1",
            WEBRTC_CLI_HOST_MONITOR_STATUS:"1", WEBRTC_CLI_HOST_QUALITY:"1", WEBRTC_CLI_HOST_TONE_HZ:"1000", WEBRTC_CLI_HOST_WARMUP_MS:"1",
            WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS:"15000", WEBRTC_CLI_HOST_TIMEOUT_MS:"20000", WEBRTC_CLI_HOST_REALTIME_PACING:"1"});
        child = spawn(cliExecutable,[pluginBundle],{env,stdio:["ignore","pipe","pipe"]});
        for (const pipe of [child.stdout, child.stderr]) pipe.on("data", data => {logs += data;});
        const [code] = await once(child,"close");
        await task;
        assert.ok(!/ThreadSanitizer|AddressSanitizer|runtime error:/.test(logs),logs);
        if (failure) throw failure;
        assert.equal(code,0,logs);
        assert.equal(sent,2004);
        assert.ok(announcements >= 2,"No reconnect after fuzzing");
        assert.match(logs,/\[status\] Error: Rejected malformed or oversized signaling message/);
        assert.match(logs,/\[status\] (?:Signaling disconnected; retrying|Peer active; signaling disconnected)/);
        assert.match(logs,mode === "seed" ? /\[status\] Error: Stream ID already in use/ : /\[status\] Alert: owned recovery confirmed/);
        const statuses = logs.split("\n").filter(line=>line.startsWith("[status]"));
        assert.ok(statuses.at(-1).includes(mode === "seed" ? "Error: Stream ID already in use" : "Alert: owned recovery confirmed"),
            "Recovery/error status was overwritten");
        assert.ok(!logs.includes("secret-sentinel"),"Malformed-input diagnostic disclosed payload");
        const quality = logs.split("\n").filter(line => line.startsWith("[quality]"));
        assert.ok(quality.length >= 7);
        assert.ok(quality.every(line => /nonfinite=0(?:\s|$)/.test(line)),"Invalid output during fuzzing");
        if (mode === "seed") assert.ok(quality.every(line=>/silent_blocks=0(?:\s|$)/.test(line)),"Dry Seed audio interrupted during signaling fuzz");
        console.log(`PASS: ${mode}: ${sent} mutations/boundaries, controller error/retry/recovery status, finite audio, normal exit`);
        return {mode, sent, announcements, healthyOffer, qualityWindows:quality.length};
    } finally {
        if (child && child.exitCode === null) child.kill();
        for (const socket of server.clients) socket.terminate();
        await new Promise(resolve => server.close(resolve));
        fs.writeFileSync(path.join(output,mode+".log"),logs);
    }
}
(async () => {
    console.log(`Signaling fuzz seed=${initialSeed}; ${output}`);
    const results = [await check("seed"), await check("play")];
    fs.writeFileSync(path.join(output,"results.json"),JSON.stringify({seed:initialSeed,results},null,2));
})().catch(error => {console.error(error); process.exitCode=1;});
