#!/usr/bin/env node
"use strict";
// An owned publisher offers real audio and video in both m-line orders, and
// after a data-only handshake. No dependency on somebody else's live stream.
const { spawnSync } = require("node:child_process");
const path = require("node:path");
const result = spawnSync(process.execPath, [path.join(__dirname, "receive_negotiation.test.js"), "--video-tracks"], {
    env: process.env, stdio: "inherit"
});
if (result.error) throw result.error;
process.exit(result.status ?? 1);
