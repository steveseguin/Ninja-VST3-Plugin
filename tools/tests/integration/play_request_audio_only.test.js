#!/usr/bin/env node
"use strict";

const path = require("path");
const fs = require("fs");
const { spawn } = require("child_process");

const rootDir = path.resolve(__dirname, "../../..");
process.chdir(rootDir);

const cliExecutable = path.join(rootDir, "build", "webrtc_vst_win", "bin", "Release", "webrtc_vst_cli_host.exe");
if (!fs.existsSync(cliExecutable)) {
    console.error("[ERROR] CLI host not found at " + cliExecutable + ". Build the project first.");
    process.exit(1);
}

const streamId = "audioonly_playreq_" + Date.now().toString(36);
const handshakeUrl = process.env.WEBRTC_TEST_WSS || "wss://wss.vdo.ninja";
const runtimeMs = parseInt(process.env.WEBRTC_TEST_RUNTIME_MS || "16000", 10);
const timeoutMs = parseInt(process.env.WEBRTC_TEST_TIMEOUT_MS || "20000", 10);

let connected = false;
let playRequestSeen = false;
let playRequestAudio = null;
let playRequestVideo = null;

const cli = spawn(cliExecutable, [], {
    cwd: rootDir,
    env: {
        ...process.env,
        WEBRTC_VST_MODE: "play",
        WEBRTC_VST_STREAM_ID: streamId,
        WEBRTC_VST_PASSWORD: "",
        WEBRTC_VST_HANDSHAKE_URL: handshakeUrl,
        WEBRTC_VST_LOG_STDOUT: "1",
        WEBRTC_VST_LOG_SIGNALING: "1",
        WEBRTC_CLI_HOST_RUNTIME_MS: String(runtimeMs),
        WEBRTC_CLI_HOST_BLOCK_SLEEP_MS: "5",
        WEBRTC_CLI_HOST_TIMEOUT_MS: String(timeoutMs),
        WEBRTC_CLI_HOST_WARMUP_MS: "1200"
    },
    stdio: ["ignore", "pipe", "pipe"]
});

function parseSignalingLine(line) {
    const marker = "=> signaling:";
    const markerIndex = line.indexOf(marker);
    if (markerIndex < 0) {
        return null;
    }
    const jsonStart = line.indexOf("{", markerIndex);
    const jsonEnd = line.lastIndexOf("}");
    if (jsonStart < 0 || jsonEnd < jsonStart) {
        return null;
    }
    const rawJson = line.slice(jsonStart, jsonEnd + 1);
    try {
        return JSON.parse(rawJson);
    } catch (_) {
        return null;
    }
}

function onOutput(prefix, chunk) {
    const text = chunk.toString("utf8");
    process.stdout.write(prefix + text.replace(/\n/g, "\n" + prefix));

    const lines = text.split(/\r?\n/);
    for (const line of lines) {
        if (!line) {
            continue;
        }
        if (line.includes("Connected to VDO.Ninja signaling server")) {
            connected = true;
        }

        const payload = parseSignalingLine(line);
        if (payload && payload.request === "play") {
            playRequestSeen = true;
            if (Object.prototype.hasOwnProperty.call(payload, "audio")) {
                playRequestAudio = payload.audio;
            }
            if (Object.prototype.hasOwnProperty.call(payload, "video")) {
                playRequestVideo = payload.video;
            }
        }
    }
}

cli.stdout.on("data", (chunk) => onOutput("[cli] ", chunk));
cli.stderr.on("data", (chunk) => onOutput("[cli:err] ", chunk));

const timeout = setTimeout(() => {
    console.error("\n[FAIL] Timed out waiting for CLI test completion.");
    try {
        cli.kill();
    } catch (_) {
        // ignored
    }
    process.exit(1);
}, timeoutMs + 2000);

cli.on("exit", (code) => {
    clearTimeout(timeout);
    console.log("\n=== play_request_audio_only.test.js ===");
    console.log("connected:", connected ? "PASS" : "FAIL");
    console.log("play request seen:", playRequestSeen ? "PASS" : "FAIL");
    console.log("play.request.audio:", playRequestAudio);
    console.log("play.request.video:", playRequestVideo);

    const passed = code === 0 &&
        connected &&
        playRequestSeen &&
        playRequestAudio === true &&
        playRequestVideo === false;

    if (!passed) {
        console.error("[FAIL] Expected play request to include {audio:true, video:false}.");
        process.exit(1);
    }
    console.log("[PASS] Play signaling includes explicit audio-only flags.");
    process.exit(0);
});
