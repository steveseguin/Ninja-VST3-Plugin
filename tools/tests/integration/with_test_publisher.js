#!/usr/bin/env node
"use strict";

// Give the live receive gates an owned synthetic stream instead of depending on
// somebody else's public stream being online. Never captures microphone audio.
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const wrtc = require("@roamhq/wrtc");
const VDONinjaSDK = require("../../../js_sdk/vdoninja-sdk-node.js");
const streamId = "vstgate" + Date.now().toString(36);
const room = streamId + "room";
const password = process.env.WEBRTC_TEST_PASSWORD || "";
const salt = process.env.WEBRTC_TEST_SALT || "vdo.ninja";
const sdk = new VDONinjaSDK({ host: process.env.WEBRTC_TEST_WSS || "wss://wss.vdo.ninja",
    password, salt, autoReconnect: false, debug: !!process.env.WEBRTC_TEST_DEBUG });
const source = new wrtc.nonstandard.RTCAudioSource();
const track = source.createTrack();
let phase = 0;
const timer = setInterval(() => {
    const samples = new Int16Array(480);
    for (let i = 0; i < samples.length; ++i) samples[i] = Math.round(8000 * Math.sin(phase++ * 2 * Math.PI * 440 / 48000));
    source.onData({ samples, sampleRate: 48000, bitsPerSample: 16, channelCount: 1, numberOfFrames: 480 });
}, 10);
let child;
let cleaned = false;
const watchdog = setTimeout(() => { console.error("Test publisher timed out"); cleanup(1); }, 300000);
async function cleanup(code) {
    if (cleaned) return;
    cleaned = true;
    clearTimeout(watchdog);
    clearInterval(timer);
    if (child && child.exitCode === null) child.kill();
    track.stop();
    try { await sdk.disconnect(); } catch (_) {}
    // wrtc's native finalizers are not reliable at Node shutdown on all Macs.
    process.exit(code);
}
process.on("SIGINT", () => cleanup(130));
process.on("SIGTERM", () => cleanup(143));
(async () => {
    await sdk.connect();
    await sdk.publish(new wrtc.MediaStream([track]), { streamID: streamId, audio: true, video: false });
    if (process.env.WEBRTC_PUBLISH_TEST_ROOM === "1") await sdk.joinRoom({ room });
    console.log("Test publisher ready:", streamId);
    const command = process.argv.slice(2);
    if (!command.length) throw new Error("Usage: node with_test_publisher.js <command> [args...]");
    child = spawn(command[0], command.slice(1), { stdio: "inherit", shell: process.platform === "win32",
        env: { ...process.env, WEBRTC_TEST_STREAM_ID: streamId, WEBRTC_TEST_ROOM_NAME: room } });
    const [code] = await once(child, "exit");
    await cleanup(code ?? 1);
})().catch(error => { console.error(error); cleanup(1); });
