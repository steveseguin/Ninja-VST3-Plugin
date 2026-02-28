#!/usr/bin/env node
"use strict";

const path = require("path");
const fs = require("fs");
const { spawn } = require("child_process");

const rootDir = path.resolve(__dirname, "../../..");
process.chdir(rootDir);

const isWin = process.platform === "win32";
const buildDir = isWin ? "webrtc_vst_win" : "webrtc_vst_linux";
const cliExeName = isWin ? "webrtc_vst_cli_host.exe" : "webrtc_vst_cli_host";
const cliExecutable = path.join(rootDir, "build", buildDir, "bin", "Release", cliExeName);
if (!fs.existsSync(cliExecutable)) {
    console.error("[ERROR] CLI host not found at " + cliExecutable + ". Build the project first.");
    process.exit(1);
}

const streamId = process.env.WEBRTC_TEST_STREAM_ID || "steve1234";
const handshakeUrl = process.env.WEBRTC_TEST_WSS || "wss://wss.vdo.ninja";
const runtimeMs = parseInt(process.env.WEBRTC_TEST_RUNTIME_MS || "22000", 10);
const timeoutMs = parseInt(process.env.WEBRTC_TEST_TIMEOUT_MS || "65000", 10);

let connected = false;
let playRequestSeen = false;
let playRequestAudio = null;
let playRequestVideo = null;
let datachannelOpened = false;
let viewerPrefsSent = false;
let datachannelMappedToPeer = false;
let ignoredNonAudioTrack = false;
let remoteAudioTrackAttached = false;
let unexpectedRemoteAnswerStateError = false;
let monitorRms = null;

function parsePlayRequest(line) {
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
    try {
        return JSON.parse(line.slice(jsonStart, jsonEnd + 1));
    } catch (_) {
        return null;
    }
}

function processChunk(prefix, chunk) {
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
        if (line.includes("Datachannel opened, sending viewer preferences")) {
            datachannelOpened = true;
        }
        if (line.includes("Sent viewer preferences:")) {
            viewerPrefsSent = true;
        }
        if (line.includes("Mapped publisher datachannel to peer session")) {
            datachannelMappedToPeer = true;
        }
        if (line.includes("Ignoring non-audio remote track")) {
            ignoredNonAudioTrack = true;
        }
        if (line.includes("Remote audio track attached")) {
            remoteAudioTrackAttached = true;
        }
        if (line.includes("Failed to apply remote answer")) {
            unexpectedRemoteAnswerStateError = true;
        }

        const rmsMatch = line.match(/output_rms=([0-9]+(?:\.[0-9]+)?)/);
        if (rmsMatch) {
            monitorRms = parseFloat(rmsMatch[1]);
        }

        const payload = parsePlayRequest(line);
        if (payload && payload.request === "play") {
            playRequestSeen = true;
            playRequestAudio = payload.audio;
            playRequestVideo = payload.video;
        }
    }
}

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
        WEBRTC_CLI_HOST_MONITOR_OUTPUT: "1",
        WEBRTC_CLI_HOST_WARMUP_MS: "2500",
        WEBRTC_CLI_HOST_RUNTIME_MS: String(runtimeMs),
        WEBRTC_CLI_HOST_BLOCK_SLEEP_MS: "5",
        WEBRTC_CLI_HOST_TIMEOUT_MS: String(timeoutMs)
    },
    stdio: ["ignore", "pipe", "pipe"]
});

cli.stdout.on("data", (chunk) => processChunk("[cli] ", chunk));
cli.stderr.on("data", (chunk) => processChunk("[cli:err] ", chunk));

const timer = setTimeout(() => {
    console.error("\n[FAIL] Timed out waiting for CLI process.");
    try {
        cli.kill();
    } catch (_) {
        // ignored
    }
}, timeoutMs + 5000);

cli.on("exit", (code) => {
    clearTimeout(timer);

    console.log("\n=== play_ignores_video_track.test.js ===");
    console.log("stream:", streamId);
    console.log("connected:", connected ? "PASS" : "FAIL");
    console.log("play request seen:", playRequestSeen ? "PASS" : "FAIL");
    console.log("play.request.audio:", playRequestAudio);
    console.log("play.request.video:", playRequestVideo);
    console.log("datachannel opened:", datachannelOpened ? "PASS" : "FAIL");
    console.log("viewer prefs sent:", viewerPrefsSent ? "PASS" : "FAIL");
    console.log("datachannel mapped to peer:", datachannelMappedToPeer ? "PASS" : "FAIL");
    console.log("remote audio track attached:", remoteAudioTrackAttached ? "PASS" : "FAIL");
    console.log("non-audio track received:", ignoredNonAudioTrack ? "YES" : "NO");
    console.log("unexpected remote-answer signaling-state error:", unexpectedRemoteAnswerStateError ? "YES" : "NO");
    console.log("monitor rms:", monitorRms);

    const passed =
        code === 0 &&
        connected &&
        playRequestSeen &&
        playRequestAudio === true &&
        playRequestVideo === false &&
        datachannelOpened &&
        viewerPrefsSent &&
        datachannelMappedToPeer &&
        remoteAudioTrackAttached &&
        !ignoredNonAudioTrack &&
        !unexpectedRemoteAnswerStateError;

    if (!passed) {
        console.error("[FAIL] Audio-only play mode checks did not pass.");
        process.exit(1);
    }

    if (!(typeof monitorRms === "number" && monitorRms > 0.0)) {
        console.warn("[WARN] No measurable audio RMS in this run (stream may be silent/offline).");
    }

    console.log("[PASS] Audio-only preferences were sent and no video track was attached.");
    process.exit(0);
});
