#!/usr/bin/env node
"use strict";
const assert = require("node:assert/strict");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { once } = require("node:events");

// Sequential real-media probes: fractional rates, small callbacks, large
// scheduling reserves, and a host-rate packet larger than the old RX buffer.
const formats = [[44100, 128], [48000, 32], [48000, 256], [48000, 1024], [96000, 64], [192000, 256]];
let child;
let interrupted = false;
for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, () => {
    interrupted = true;
    if (child) child.kill(signal);
});
(async () => {
    for (const [rate, block] of formats) {
        assert.ok(!interrupted, "Playback matrix interrupted");
        console.log(`Testing stereo WebRTC audio: ${rate} Hz, ${block} frames`);
        child = spawn(process.execPath, [path.join(__dirname, "playback_quality.test.js")], {
            stdio: "inherit", env: {...process.env,
                WEBRTC_PROBE_SECONDS: process.env.WEBRTC_PROBE_SECONDS || "30",
                WEBRTC_PROBE_SAMPLE_RATE: String(rate), WEBRTC_PROBE_BLOCK_SIZE: String(block)}
        });
        const [code] = await once(child, "close");
        child = null;
        assert.equal(code, 0, `${rate} Hz / ${block} frames failed`);
    }
    assert.ok(!interrupted, "Playback matrix interrupted");
    console.log("PASS: all six live audio formats");
})().catch(error => { console.error(error); process.exitCode = 1; });
