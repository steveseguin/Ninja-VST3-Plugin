#!/usr/bin/env node
"use strict";
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { WebSocketServer } = require("ws");
const { cliExecutable, pluginBundle } = require("./build_paths");

(async () => {
    const server = new WebSocketServer({host:"127.0.0.1",port:0});
    await once(server,"listening");
    const offered = new Set();
    let child, logs = "", sent = false;
    server.on("connection", socket => socket.on("message", raw => {
        const message = JSON.parse(raw);
        if (message.description) offered.add(message.UUID);
        if (message.request !== "seed" || sent) return;
        sent = true;
        for (const payload of ["{secret-sentinel", "[]", "null", '{"request":"offerSDP","UUID":{}}',
            "[".repeat(1000) + "0" + "]".repeat(1000), JSON.stringify({secret:"secret-sentinel", text:"x".repeat(270000)})]) socket.send(payload);
        for (let i=0; i<17; ++i) socket.send(JSON.stringify({request:"offerSDP",UUID:"owned-peer-"+i}));
        // ICE arriving before an SDP answer must remain bounded.
        for (let i=0; i<1024; ++i) socket.send(JSON.stringify({UUID:"owned-peer-0",candidate:{candidate:`candidate:${i} 1 UDP 1 127.0.0.1 9000 typ host`,sdpMid:"0"}}));
    }));
    try {
        const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
        Object.assign(env, {WEBRTC_VST_MODE:"seed",WEBRTC_VST_STREAM_ID:"ownedlimits",WEBRTC_VST_PASSWORD:"off",
            WEBRTC_VST_HANDSHAKE_URL:`ws://127.0.0.1:${server.address().port}/`,WEBRTC_VST_DISABLE_STUN:"1",
            WEBRTC_VST_LOG_STDOUT:"1",WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS:"6000",WEBRTC_CLI_HOST_TIMEOUT_MS:"30000",
            WEBRTC_CLI_HOST_REALTIME_PACING:"1"});
        child = spawn(cliExecutable,[pluginBundle],{env,stdio:["ignore","pipe","pipe"]});
        console.log("Limits test child pid=" + child.pid);
        child.stdout.on("data",data => {logs += data;}); child.stderr.on("data",data => {logs += data;});
        const [code] = await once(child,"exit");
        assert.equal(code,0,logs);
        assert.match(logs,/Rejected malformed or oversized signaling message/);
        assert.ok(!logs.includes("secret-sentinel"),"Parser diagnostics leaked input values");
        assert.match(logs,/Peer session cap reached \(16\)/);
        assert.equal((logs.match(/Pre-SDP ICE limit reached \(256\)/g)||[]).length, 1, "ICE cap must reject excess with one bounded diagnostic");
        assert.ok(offered.size >= 16,"Expected first 16 peers to be accepted, got " + offered.size);
        assert.ok(!offered.has("owned-peer-16"),"Seventeenth peer bypassed cap");
        console.log("PASS: malformed/deep/oversized signaling rejection, redacted errors, 16/17 peer boundary, pre-SDP ICE flood and clean teardown");
    } finally {
        if(child && child.exitCode === null) child.kill();
        for(const socket of server.clients) socket.terminate();
        await new Promise(resolve => server.close(resolve));
    }
})().catch(error => {console.error(error); process.exitCode=1;});
