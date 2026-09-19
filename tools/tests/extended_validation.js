#!/usr/bin/env node
"use strict";
// Wall-clock repeated randomized-state, host/lifecycle and stress validation.
// Signaling stays on an owned loopback fixture; live audio soak is separate.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { WebSocketServer } = require("ws");
const { rootDir, cliExecutable, pluginBundle } = require("./integration/build_paths");
const seconds = Number(process.env.WEBRTC_EXTENDED_SECONDS || 1800);
assert.ok(Number.isFinite(seconds) && seconds >= 10 && seconds <= 14400);
const directory = path.join(rootDir, "build/test-tools/extended-" + Date.now());
fs.mkdirSync(directory, {recursive:true});
const binary = path.join(pluginBundle, process.platform === "darwin" ? "Contents/MacOS/webrtc_vst" :
    process.platform === "win32" ? "Contents/x86_64-win/webrtc_vst.vst3" : "Contents/x86_64-linux/webrtc_vst.so");
const digest = () => crypto.createHash("sha256").update(fs.readFileSync(binary)).digest("hex");
const originalDigest = digest(), results = [];
let child, server, start, failure, interrupted = false;
function stopChild(signal = "SIGTERM") {
    if (!child) return;
    try { if (process.platform === "win32") child.kill(signal); else process.kill(-child.pid, signal); } catch {}
}
async function run(name, seed) {
    const executable = path.join(path.dirname(cliExecutable), name + (process.platform === "win32" ? ".exe" : ""));
    const target = name === "webrtc_vst_settings_test" && process.env.WEBRTC_SANITIZED_SETTINGS || executable;
    const logPath = path.join(directory, `${results.length}-${name}.log`);
    const log = fs.createWriteStream(logPath);
    const began = Date.now();
    const env = {...process.env, WEBRTC_FUZZ_SEED:String(seed), WEBRTC_VST_DISABLE_STUN:"1",
        WEBRTC_VST_HANDSHAKE_URL:`ws://127.0.0.1:${server.address().port}/`, WEBRTC_VST_LOG_STDOUT:"0"};
    delete env.WEBRTC_VST_LOG_SIGNALING; delete env.WEBRTC_VST_LOG_SIGNALING_RAW;
    const timed = process.platform === "darwin";
    assert.ok(!interrupted, "Validation interrupted");
    child = spawn(timed ? "/usr/bin/time" : target, timed ? ["-l",target,pluginBundle] : [pluginBundle], {env,detached:process.platform !== "win32",stdio:["ignore","pipe","pipe"]});
    let output = "";
    for (const stream of [child.stdout,child.stderr]) stream.on("data",data => {log.write(data); if(output.length < 5000000) output += data;});
    const timeout = setTimeout(() => stopChild("SIGKILL"), 120000);
    const [code,signal] = await once(child,"close");
    child = null; clearTimeout(timeout); log.end();
    const entry = {name,seed,code,signal,seconds:(Date.now()-began)/1000,
        peakRssBytes:Number(output.match(/(\d+)\s+maximum resident set size/)?.[1] || 0),log:logPath};
    results.push(entry);
    fs.writeFileSync(path.join(directory,"results.json"),JSON.stringify({started:new Date(start).toISOString(),elapsedSeconds:(Date.now()-start)/1000,
        requiredSeconds:seconds,pluginSha256:originalDigest,complete:false,results},null,2));
    assert.equal(code,0,`${name} failed with ${signal || code}; ${logPath}`);
    assert.equal(digest(),originalDigest,"Plugin changed during soak; restart validation on the final binary");
}
for (const signal of ["SIGINT","SIGTERM"]) process.on(signal,() => {interrupted=true; stopChild(); process.exitCode=1;});
(async () => {
    server = new WebSocketServer({host:"127.0.0.1",port:0});
    await once(server,"listening");
    server.on("connection",socket => socket.on("error",()=>{}));
    start = Date.now();
    console.log(`Started ${seconds}s wall-clock extended validation: ${directory}; SHA256=${originalDigest}`);
    let cycle=0;
    do {
        const seed = 0x56444f + cycle;
        for (const name of ["webrtc_vst_settings_test","webrtc_vst_integration_test","webrtc_vst_fuzz_test","webrtc_vst_stress_test"]) await run(name,seed);
        console.log(`Cycle ${++cycle}: ${(Date.now()-start)/1000}s, ${results.length} suites passed, seed=${seed}`);
    } while(Date.now()-start < seconds*1000);
})().catch(error => {failure=error.message; console.error(error); process.exitCode=1;}).finally(async () => {
    stopChild();
    if(server) {for(const socket of server.clients) socket.terminate(); await new Promise(resolve => server.close(resolve));}
    if (interrupted && !failure) failure="Validation interrupted";
    const elapsedSeconds=(Date.now()-start)/1000;
    if (elapsedSeconds < seconds && !failure) failure="Requested wall-clock duration was not completed";
    if (failure) process.exitCode=1;
    const summary={started:new Date(start).toISOString(),elapsedSeconds,requiredSeconds:seconds,
        complete:!failure, failure,pluginSha256:originalDigest,results};
    fs.writeFileSync(path.join(directory,"results.json"),JSON.stringify(summary,null,2));
    console.log(`${failure ? "FAIL" : "PASS"}: ${summary.elapsedSeconds}s, ${results.length} suites; ${directory}`);
});
