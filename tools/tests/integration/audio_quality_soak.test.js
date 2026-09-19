#!/usr/bin/env node
"use strict";
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { WebSocket, WebSocketServer } = require("ws");
const { cliExecutable, pluginBundle, rootDir } = require("./build_paths");
const duration = Number(process.env.WEBRTC_SOAK_SECONDS || 90);
assert.ok(Number.isFinite(duration) && duration >= 30 && duration <= 14400);
const out = path.join(rootDir, "build/test-tools/audio-soak-" + Date.now());
fs.mkdirSync(out, {recursive:true});
const children = new Set();
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const stream = "ownedsoak" + crypto.randomBytes(8).toString("hex");
const windows = [];
let proxy;
const interruptionTimers = [];
function launch(mode, seconds) {
    const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
    Object.assign(env, {
        WEBRTC_VST_MODE: mode, WEBRTC_VST_STREAM_ID: stream, WEBRTC_VST_PASSWORD: "soak &é+pass",
        WEBRTC_VST_SALT: "soak é🔊", WEBRTC_VST_WEB_BASE_URL: "https://backup.vdo.ninja/",
        WEBRTC_VST_HANDSHAKE_URL: proxy ? `ws://127.0.0.1:${proxy.address().port}/${mode}` : process.env.WEBRTC_TEST_WSS || "wss://apibackup.vdo.ninja/",
        WEBRTC_VST_DISABLE_STUN: "1", WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: String(seconds * 1000),
        WEBRTC_CLI_HOST_TIMEOUT_MS: String((seconds + 10) * 1000), WEBRTC_CLI_HOST_REALTIME_PACING: "1",
        WEBRTC_CLI_HOST_TONE_HZ: "1000", WEBRTC_CLI_HOST_RIGHT_TONE_HZ: "1500",
        WEBRTC_CLI_HOST_QUALITY: "1", WEBRTC_CLI_HOST_MONITOR_OUTPUT: "1"
    });
    if (process.env.WEBRTC_SOAK_TRACE) Object.assign(env, {WEBRTC_VST_LOG_STDOUT:"1", WEBRTC_VST_LOG_SIGNALING:"1"});
    const child = spawn(cliExecutable, [pluginBundle], {env, stdio:["ignore","pipe","pipe"]});
    children.add(child);
    const log = fs.createWriteStream(path.join(out, mode + ".log"));
    let pending = "";
    child.stdout.on("data", data => {
        log.write(data); pending += data;
        const lines = pending.split("\n"); pending = lines.pop();
        if (mode === "play") for (const line of lines) if (line.startsWith("[quality]")) {
            const result = Object.fromEntries([...line.matchAll(/(\w+)=([\d.eE+-]+)/g)].map(([,key,value]) => [key, Number(value)]));
            windows.push(result);
            if (windows.length % 60 === 0) console.log("Progress:", result.seconds.toFixed(1), "seconds, last RMS", result.rms_l, result.rms_r);
        }
    });
    child.stderr.on("data", data => log.write(data));
    const done = once(child, "close").then(([code, signal]) => { children.delete(child); log.end(); return {code, signal}; });
    done.catch(() => {});
    return done;
}
const watchdog = setTimeout(() => { for (const child of children) child.kill(); }, (duration + 30) * 1000);
for (const signal of ["SIGINT","SIGTERM"]) process.on(signal, () => { for (const child of children) child.kill(); process.exitCode = 1; });
(async () => {
    console.log(`Running ${duration}s wall-clock stereo audio soak; ${out}`);
    if (process.env.WEBRTC_SOAK_RECONNECT) {
        proxy = new WebSocketServer({host:"127.0.0.1",port:0});
        await once(proxy,"listening");
        proxy.on("connection",socket => {
            const upstream = new WebSocket(process.env.WEBRTC_TEST_WSS || "wss://apibackup.vdo.ninja/");
            const pending = [];
            socket.on("message",data => {
                if(upstream.readyState === WebSocket.OPEN) upstream.send(data.toString());
                else if(pending.length < 32) pending.push(data.toString());
            });
            upstream.on("open",()=>{for(const data of pending) upstream.send(data); pending.length=0;});
            upstream.on("message",data=>{if(socket.readyState===WebSocket.OPEN) socket.send(data.toString());});
            socket.on("close",()=>upstream.close()); socket.on("error",()=>upstream.close());
            upstream.on("close",()=>socket.close()); upstream.on("error",()=>socket.close());
        });
        for(const seconds of [20,40]) interruptionTimers.push(setTimeout(()=>{
            console.log("Interrupting owned signaling connections at " + seconds + "s; media remains connected");
            for(const socket of proxy.clients) socket.close(1012,"Owned reconnect regression");
        },seconds*1000));
    }
    const seed = launch("seed", duration + 8);
    await pause(5000);
    const play = launch("play", duration);
    const result = await play;
    assert.equal(result.code, 0, JSON.stringify(result));
    assert.equal((await seed).code, 0);
    const steady = windows.filter(row => row.seconds >= 10);
    const summary = {
        seconds: duration, windows: windows.length, steadyWindows: steady.length,
        worstCallbackUs: Math.max(...windows.map(row => row.callback_max_us)),
        silentBlocks: steady.reduce((sum,row) => sum + row.silent_blocks, 0),
        totalBlocks: steady.reduce((sum,row) => sum + row.blocks, 0),
        minRms: Math.min(...steady.flatMap(row => [row.rms_l, row.rms_r])),
        maxFrequencyError: Math.max(...steady.flatMap(row => [Math.abs(row.hz_l - 1000), Math.abs(row.hz_r - 1500)]))
    };
    fs.writeFileSync(path.join(out,"results.json"), JSON.stringify({summary, windows},null,2));
    assert.ok(steady.length >= duration - 15, "Missing sustained quality windows");
    for (const row of steady) {
        assert.equal(row.nonfinite, 0, "Nonfinite output");
        assert.ok(row.rms_l > 0.08 && row.rms_r > 0.08 && row.rms_l < 0.4 && row.rms_r < 0.4, "Level/dropout failure: " + JSON.stringify(row));
        assert.ok(row.silent_blocks <= 4, "More than 2% of a one-second window was silent: " + JSON.stringify(row));
        assert.ok(Math.abs(row.hz_l - 1000) < 40 && Math.abs(row.hz_r - 1500) < 40, "Pitch/channel separation failure: " + JSON.stringify(row));
    }
    if (process.env.WEBRTC_SOAK_STRICT) assert.equal(summary.silentBlocks, 0, "Strict playback gate: silent host blocks after warmup");
    console.log("PASS:", JSON.stringify(summary));
})().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => {
    clearTimeout(watchdog); for (const child of children) child.kill();
    for(const timer of interruptionTimers) clearTimeout(timer);
    if(proxy) {for(const socket of proxy.clients) socket.terminate(); await new Promise(resolve=>proxy.close(resolve));}
});
