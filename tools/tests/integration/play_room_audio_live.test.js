#!/usr/bin/env node
"use strict";

// Own both endpoints and use a unique encrypted room on the real relay.
// Audio/video preferences belong on sendChannel, not the play request.
const { spawnSync } = require("node:child_process");
const { randomBytes } = require("node:crypto");
const path = require("node:path");
const result = spawnSync(process.execPath, [path.join(__dirname, "cli_loopback.test.js")], {
    stdio: "inherit",
    env: {
        ...process.env,
        WEBRTC_TEST_ROOM_NAME: "vstroom" + randomBytes(8).toString("hex"),
        WEBRTC_TEST_STREAM_ID: "vstroomtone",
        WEBRTC_TEST_PASSWORD: "owned-room-" + randomBytes(12).toString("hex"),
        WEBRTC_TEST_DISABLE_STUN: "0"
    }
});
if (result.error) throw result.error;
process.exit(result.status ?? 1);
