#!/usr/bin/env node
"use strict";
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { WebSocketServer } = require("ws");
const { cliExecutable, pluginBundle } = require("./build_paths");

async function check(mode) {
    const server = new WebSocketServer({host: "127.0.0.1", port: 0});
    await once(server, "listening");
    let requests = 0, error, timer, child;
    const started = Date.now();
    let logs = "";
    const done = new Promise((resolve, reject) => {
        timer = setTimeout(() => reject(new Error(`${mode}: no recovery within 12 seconds; requests=${requests}\n${logs}`)), 12000);
        server.on("connection", socket => socket.on("message", raw => {
            try {
                const data = JSON.parse(raw);
                assert.equal(data.request, mode === "seed" ? "seed" : "play");
                assert.equal(data.streamID, "recovery_owned_test");
                if (++requests < 3) socket.close(1012, "Owned test: signaling restart");
                else resolve();
            } catch (e) { reject(e); }
        }));
    });
    try {
        const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
        Object.assign(env, {
            WEBRTC_VST_MODE: mode, WEBRTC_VST_STREAM_ID: "recovery-owned-test", WEBRTC_VST_PASSWORD: "off",
            WEBRTC_VST_HANDSHAKE_URL: `ws://127.0.0.1:${server.address().port}/`, WEBRTC_VST_DISABLE_STUN: "1",
            WEBRTC_VST_LOG_STDOUT: "1", WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: "15000",
            WEBRTC_CLI_HOST_TIMEOUT_MS: "18000", WEBRTC_CLI_HOST_BLOCK_SLEEP_MS: "5"
        });
        child = spawn(cliExecutable, [pluginBundle], {env, stdio: ["ignore", "pipe", "pipe"]});
        child.stdout.on("data", data => { logs += data; }); child.stderr.on("data", data => { logs += data; });
        await done;
        assert.ok(Date.now() - started >= 1800, "Repeated closes must back off, not busy-loop");
        console.log(`PASS: ${mode}, ${requests - 1} consecutive reconnects in ${Date.now() - started}ms`);
    } catch (e) { error = e; }
    finally {
        clearTimeout(timer);
        if (child && child.exitCode === null) { const exited = once(child, "exit"); child.kill(); await exited; }
        for (const socket of server.clients) socket.terminate();
        await new Promise(resolve => server.close(resolve));
    }
    if (error) throw error;
}
(async () => { await check("play"); await check("seed"); })().catch(error => { console.error(error); process.exitCode = 1; });
