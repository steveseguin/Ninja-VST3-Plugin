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
const pluginExt = isWin ? "webrtc_vst.vst3" : "webrtc_vst.so";
const pluginArch = isWin ? "x86_64-win" : "x86_64-linux";
const pluginBinary = path.join(rootDir, "build", buildDir, "VST3", "Release", "webrtc_vst.vst3", "Contents", pluginArch, pluginExt);

if (!fs.existsSync(cliExecutable)) {
    console.error("[ERROR] CLI host not found at " + cliExecutable + ". Build the project first.");
    process.exit(1);
}

if (!fs.existsSync(pluginBinary)) {
    console.warn("[WARN] Plugin bundle not found at " + pluginBinary + ". Ensure Release build is present.");
}

const streamId = "integrationtone" + Date.now().toString(36);
const handshakeUrl = process.env.WEBRTC_TEST_WSS || "wss://wss.vdo.ninja";
const runtimeMs = process.env.WEBRTC_TEST_RUNTIME_MS || "45000";
const toneHz = process.env.WEBRTC_TEST_TONE_HZ || "1000";

const pluginPassword = process.env.WEBRTC_TEST_PASSWORD ?? "";
const passwordLower = typeof pluginPassword === "string" ? pluginPassword.trim().toLowerCase() : "";
const disableEncryption = passwordLower === "0" || passwordLower === "false" || passwordLower === "off";
const viewerPassword = disableEncryption ? false : "";

const cliEnv = {
    ...process.env,
    WEBRTC_VST_MODE: "seed",
    WEBRTC_VST_STREAM_ID: streamId,
    WEBRTC_VST_PASSWORD: pluginPassword,
    WEBRTC_VST_HANDSHAKE_URL: handshakeUrl,
    WEBRTC_CLI_HOST_RUNTIME_MS: runtimeMs,
    WEBRTC_CLI_HOST_TONE_HZ: toneHz,
    WEBRTC_VST_LOG_STDOUT: "1"
};

console.log("[INFO] Launching CLI host with stream ID '" + streamId + "'");
console.log("[INFO] Encryption disabled: " + disableEncryption);
const cli = spawn(cliExecutable, [], {
    env: cliEnv,
    stdio: ["ignore", "pipe", "pipe"]
});

let cliExited = false;
let cliExitCode = null;
cli.stdout.on("data", (chunk) => {
    process.stdout.write("[cli] " + chunk);
});
cli.stderr.on("data", (chunk) => {
    process.stderr.write("[cli] " + chunk);
});
cli.on("exit", (code) => {
    cliExited = true;
    cliExitCode = code;
    console.log("[INFO] CLI host exited with code " + code);
});

async function waitForAudio(sdk) {
    const wrtc = require("@roamhq/wrtc");
    if (!wrtc.nonstandard || !wrtc.nonstandard.RTCAudioSink) {
        throw new Error("RTCAudioSink is not available in @roamhq/wrtc build.");
    }

    return new Promise((resolve, reject) => {
        const { RTCAudioSink } = wrtc.nonstandard;
        const timeout = setTimeout(() => {
            cleanup();
            reject(new Error("Timed out waiting for audio track."));
        }, 30000);

        const metrics = {
            frames: 0,
            samples: 0,
            energy: 0,
            channelCount: 0,
            sampleRate: 48000
        };

        let sink = null;

        const cleanup = () => {
            clearTimeout(timeout);
            if (sink) {
                try { sink.stop(); } catch (_) {}
                sink = null;
            }
            sdk.removeEventListener("track", onTrack);
        };

        const onTrack = (event) => {
            const detail = event && event.detail ? event.detail : event;
            if (!detail || !detail.track || detail.track.kind !== "audio") {
                return;
            }

            console.log("[INFO] Audio track received from peer.");
            sink = new RTCAudioSink(detail.track);
            if (typeof detail.track.addEventListener === "function") {
                detail.track.addEventListener("ended", () => {
                    if (sink) {
                        console.log("[WARN] Audio track ended before threshold reached.");
                        cleanup();
                        reject(new Error("Audio track ended prematurely."));
                    }
                });
            }

            sink.ondata = (audioFrame) => {
                const samples = audioFrame.samples;
                const sampleRate = audioFrame.sampleRate;
                const channelCount = audioFrame.channelCount;
                if (!samples || samples.length === 0) {
                    return;
                }

                metrics.sampleRate = sampleRate;
                metrics.channelCount = channelCount;
                metrics.frames += 1;
                metrics.samples += samples.length;

                let sumSquares = 0;
                for (let i = 0; i < samples.length; ++i) {
                    const normalized = samples[i] / 32768;
                    sumSquares += normalized * normalized;
                }
                metrics.energy += sumSquares;

                const samplesPerSecond = sampleRate * Math.max(channelCount, 1);
                const minSamples = samplesPerSecond * 2;

                if (metrics.samples >= minSamples) {
                    const meanSquare = metrics.energy / metrics.samples;
                    const rms = Math.sqrt(meanSquare);
                    if (rms > 1e-3) {
                        cleanup();
                        resolve({
                            rms,
                            frames: metrics.frames,
                            samples: metrics.samples,
                            sampleRate,
                            channelCount
                        });
                    }
                }
            };
        };

        sdk.addEventListener("track", onTrack);
    });
}

async function main() {
    const sdkPath = path.join(rootDir, "js_sdk", "vdoninja-sdk-node.js");
    const VDONinjaSDK = require(sdkPath);

    const viewerOptions = {
        host: handshakeUrl,
        password: viewerPassword,
        debug: true,
        autoReconnect: false
    };
    if (!disableEncryption) {
        viewerOptions.salt = "vdo.ninja";
    }
    const viewer = new VDONinjaSDK(viewerOptions);

    viewer.addEventListener("peerConnected", (event) => {
        const detail = event && event.detail ? event.detail : {};
        const uuid = detail.uuid || (detail.connection && detail.connection.uuid) || "";
        console.log("[VIEWER] peerConnected " + uuid);
    });
    viewer.addEventListener("streamAdded", (event) => {
        const detail = event && event.detail ? event.detail : {};
        const id = detail.streamID || detail.uuid || "";
        console.log("[VIEWER] streamAdded " + id);
    });
    viewer.addEventListener("listing", (event) => {
        const detail = event && event.detail ? event.detail : {};
        if (detail.streamID) {
            console.log("[VIEWER] listing stream " + detail.streamID);
        }
    });

    try {
        const audioPromise = waitForAudio(viewer);
        console.log("[INFO] Connecting viewer to signaling...");
        await viewer.connect();
        console.log("[INFO] Connected to signaling. Requesting stream...");

        await viewer.view(streamId, {
            audio: true,
            video: false,
            label: "integration-audio"
        });

        const audioStats = await audioPromise;
        console.log("[PASS] Received audio. RMS=" + audioStats.rms.toFixed(4) + ", frames=" + audioStats.frames + ", channels=" + audioStats.channelCount);

        if (!cliExited) {
            try {
                cli.kill();
            } catch (_) {}
        }

        await new Promise((resolve) => setTimeout(resolve, 500));
        if (typeof viewer.disconnect === "function") {
            await viewer.disconnect();
        }
        process.exit(0);
    } catch (error) {
        console.error("[FAIL] " + error.message);
        if (!cliExited) {
            try {
                cli.kill();
            } catch (_) {}
        }
        if (typeof viewer.disconnect === "function") {
            try {
                await viewer.disconnect();
            } catch (_) {}
        }

        if (cliExited && cliExitCode !== 0) {
            console.error("[INFO] CLI host exited with " + cliExitCode);
        }
        process.exit(1);
    }
}

process.on("unhandledRejection", (reason) => {
    console.error("[ERROR] Unhandled rejection: " + reason);
    process.exit(1);
});

main();

