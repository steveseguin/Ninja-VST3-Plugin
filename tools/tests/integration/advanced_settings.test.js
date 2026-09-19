#!/usr/bin/env node
"use strict";

// Observe the real plugin's outbound requests on a loopback-only signaling
// endpoint. No public server, TLS bypass, or changes to the signaling protocol.
const assert = require("node:assert/strict");
const { createHash } = require("node:crypto");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { WebSocketServer } = require("ws");
const { cliExecutable, pluginBundle } = require("./build_paths");

const hash = (s, length) => createHash("sha256").update(s).digest("hex").slice(0, length);

async function runCase(name, saltOverride, web, expectedSalt, room = "") {
    const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    await once(server, "listening");
    const env = { ...process.env,
        WEBRTC_VST_MODE: "seed", WEBRTC_VST_STREAM_ID: "advancedtest",
        WEBRTC_VST_ROOM_NAME: room, WEBRTC_VST_PASSWORD: "secret",
        WEBRTC_VST_HANDSHAKE_URL: `ws://127.0.0.1:${server.address().port}/custom/socket?test=1`,
        WEBRTC_VST_WEB_BASE_URL: web,
        WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: "1800",
        WEBRTC_CLI_HOST_TIMEOUT_MS: "6000", WEBRTC_CLI_HOST_BLOCK_SLEEP_MS: "5"
    };
    for (const key of Object.keys(env)) {
        if (key.startsWith("WEBRTC_CLI_HOST_") && /MODE|STREAM_ID|ROOM|PASSWORD/.test(key)) delete env[key];
    }
    if (saltOverride === undefined) delete env.WEBRTC_VST_SALT;
    else env.WEBRTC_VST_SALT = saltOverride;
    let child;
    let timer;
    let logs = "";
    const observed = new Promise((resolve, reject) => {
        timer = setTimeout(() => reject(new Error(`${name}: no signaling request\n${logs}`)), 8000);
        server.on("connection", (socket, request) => {
            socket.on("message", (data) => {
                try {
                    assert.equal(request.url, "/custom/socket?test=1");
                    const message = JSON.parse(data);
                    if (room) {
                        assert.equal(message.request, "joinroom");
                        assert.equal(message.roomid, hash(room + "secret" + expectedSalt, 16));
                    } else {
                        assert.equal(message.request, "seed");
                        assert.equal(message.streamID, "advancedtest" + hash("secret" + expectedSalt, 6));
                    }
                    resolve();
                } catch (error) { reject(error); }
            });
        });
    });
    try {
        child = spawn(cliExecutable, [pluginBundle], { env, stdio: ["ignore", "pipe", "pipe"] });
        child.stdout.on("data", data => { logs += data; });
        child.stderr.on("data", data => { logs += data; });
        const exited = once(child, "exit");
        await observed;
        const [code] = await exited;
        assert.equal(code, 0, logs);
        console.log(`PASS: ${name}`);
    } finally {
        clearTimeout(timer);
        if (child && child.exitCode === null) child.kill();
        for (const socket of server.clients) socket.terminate();
        await new Promise(resolve => server.close(resolve));
    }
}

(async () => {
    await runCase("explicit salt overrides web domain", "custom & salt", "https://other.test/", "custom & salt");
    await runCase("automatic salt follows web domain, not WSS", "", "https://studio.example.test/ninja/", "studio.example.test");
    await runCase("official subdomain retains official salt", undefined, "https://beta.vdo.ninja/", "vdo.ninja");
    await runCase("room hashing uses custom salt", "room-salt", "https://other.test/", "room-salt", "testroom");
    await runCase("Unicode salt reaches hashing unchanged", "café + salt", "https://other.test/", "café + salt");
})().catch(error => { console.error(error); process.exitCode = 1; });
