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

const streamSuffix = Math.random().toString(36).slice(-6);
const baseStreamId = process.env.WEBRTC_TEST_STREAM_ID || "cli-loopback";
const streamId = baseStreamId + "-" + streamSuffix;
const handshakeUrl = process.env.WEBRTC_TEST_WSS || "wss://wss.vdo.ninja";
const seedRuntimeMs = parseInt(process.env.WEBRTC_TEST_SEED_RUNTIME_MS || "35000", 10);
const playRuntimeMs = parseInt(process.env.WEBRTC_TEST_PLAY_RUNTIME_MS || "45000", 10);
const startDelayMs = parseInt(process.env.WEBRTC_TEST_START_DELAY_MS || "5000", 10);
const overallTimeoutMs = parseInt(process.env.WEBRTC_TEST_TIMEOUT_MS || "90000", 10);
const cliTimeoutMs = parseInt(process.env.WEBRTC_TEST_CLI_TIMEOUT_MS || (Math.max(seedRuntimeMs, playRuntimeMs) + 5000).toString(), 10);
const minRms = parseFloat(process.env.WEBRTC_TEST_MIN_RMS || "0.01");
const toneHz = process.env.WEBRTC_TEST_TONE_HZ || "1000";
const blockSleepMs = process.env.WEBRTC_TEST_BLOCK_SLEEP_MS || "5";
const disableStun = process.env.WEBRTC_TEST_DISABLE_STUN ?? "1";
const password = process.env.WEBRTC_TEST_PASSWORD ?? ""; // default disables encryption for loopback.

const activeChildren = new Set();
function register(child) {
    activeChildren.add(child);
    child.on("exit", () => activeChildren.delete(child));
}

function cleanupChildren() {
    for (const child of Array.from(activeChildren)) {
        try {
            child.kill();
        } catch (error) {
            // ignore
        }
    }
}

process.on("exit", cleanupChildren);
process.on("SIGINT", () => {
    cleanupChildren();
    process.exit(130);
});
process.on("SIGTERM", () => {
    cleanupChildren();
    process.exit(143);
});

function withTimeout(promise, ms, label) {
    let timeoutHandle = null;
    return Promise.race([
        promise.finally(() => {
            if (timeoutHandle) {
                clearTimeout(timeoutHandle);
            }
        }),
        new Promise((_, reject) => {
            timeoutHandle = setTimeout(() => {
                reject(new Error(label + " (timeout after " + ms + " ms)"));
            }, ms);
        })
    ]);
}

function spawnCli(label, extraEnv) {
    const env = {
        ...process.env,
        WEBRTC_VST_STREAM_ID: streamId,
        WEBRTC_VST_HANDSHAKE_URL: handshakeUrl,
        WEBRTC_VST_PASSWORD: password,
        WEBRTC_VST_DISABLE_STUN: disableStun,
        WEBRTC_VST_LOG_STDOUT: "1",
        WEBRTC_VST_LOG_SIGNALING: process.env.WEBRTC_VST_LOG_SIGNALING || "1",
        WEBRTC_CLI_HOST_TIMEOUT_MS: cliTimeoutMs.toString(),
        WEBRTC_CLI_HOST_BLOCK_SLEEP_MS: blockSleepMs,
        ...extraEnv
    };

    const child = spawn(cliExecutable, [], {
        env,
        cwd: rootDir,
        stdio: ["ignore", "pipe", "pipe"]
    });
    register(child);

    const logPrefix = "[" + label + "] ";

    const forward = (stream, writer) => {
        stream.setEncoding("utf8");
        let buffer = "";
        stream.on("data", (chunk) => {
            writer(logPrefix + chunk.replace(/\n/g, "\n" + logPrefix));
        });
    };

    forward(child.stdout, (msg) => process.stdout.write(msg));
    forward(child.stderr, (msg) => process.stderr.write(msg));

    return child;
}

function watchMonitor(child, label) {
    return new Promise((resolve, reject) => {
        let buffer = "";
        let resolved = false;

        const handleChunk = (chunk) => {
            buffer += chunk;
            let idx = buffer.indexOf("\n");
            while (idx !== -1) {
                let line = buffer.slice(0, idx);
                buffer = buffer.slice(idx + 1);
                line = line.replace(/\r$/, "");
                if (!resolved && line.includes("[monitor] output_rms=")) {
                    const match = line.match(/output_rms=([0-9]+(?:\.[0-9]+)?)\s+samples=([0-9]+)/);
                    if (match) {
                        resolved = true;
                        const monitor = {
                            rms: parseFloat(match[1]),
                            samples: parseInt(match[2], 10),
                            raw: line
                        };
                        child.stdout.off("data", handleChunk);
                        resolve(monitor);
                        return;
                    }
                }
                idx = buffer.indexOf("\n");
            }
        };

        child.stdout.setEncoding("utf8");
        child.stdout.on("data", handleChunk);

        child.on("error", (error) => {
            if (!resolved) {
                child.stdout.off("data", handleChunk);
                reject(error);
            }
        });
        child.on("exit", (code) => {
            child.stdout.off("data", handleChunk);
            if (!resolved) {
                reject(new Error(label + " exited without reporting monitor output (code=" + code + ")"));
            }
        });
    });
}

function waitForExit(child, label) {
    return new Promise((resolve, reject) => {
        child.on("error", reject);
        child.on("exit", (code) => {
            if (code === 0) {
                resolve();
            } else {
                reject(new Error(label + " exited with code " + code));
            }
        });
    });
}

function delay(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

(async () => {
    console.log("[INFO] CLI loopback test using stream '" + streamId + "'");
    console.log("[INFO] Handshake URL: " + handshakeUrl);
    console.log("[INFO] Encryption disabled: " + (password === "0" || password === "false" || password === "off"));
    console.log("[INFO] STUN disabled: " + (disableStun !== "0" && disableStun !== "false" && disableStun !== "off"));

    const playEnv = {
        WEBRTC_VST_MODE: "play",
        WEBRTC_CLI_HOST_RUNTIME_MS: playRuntimeMs.toString(),
        WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: playRuntimeMs.toString(),
        WEBRTC_CLI_HOST_MONITOR_OUTPUT: "1"
    };
    const play = spawnCli("play", playEnv);
    const playMonitorPromise = withTimeout(watchMonitor(play, "play"), overallTimeoutMs, "Play side monitor did not complete");

    await delay(startDelayMs);

    const seedEnv = {
        WEBRTC_VST_MODE: "seed",
        WEBRTC_CLI_HOST_RUNTIME_MS: seedRuntimeMs.toString(),
        WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: seedRuntimeMs.toString(),
        WEBRTC_CLI_HOST_TONE_HZ: toneHz
    };
    const seed = spawnCli("seed", seedEnv);
    const seedExitPromise = withTimeout(waitForExit(seed, "seed"), overallTimeoutMs, "Seed side did not exit");

    const monitor = await playMonitorPromise;
    await seedExitPromise;

    if (monitor.rms < minRms) {
        throw new Error("RMS " + monitor.rms.toFixed(6) + " below threshold " + minRms);
    }

    console.log("[PASS] Loopback received audio. RMS=" + monitor.rms.toFixed(4) + ", samples=" + monitor.samples);
    cleanupChildren();
    process.exit(0);
})().catch((error) => {
    console.error("[FAIL] " + error.message);
    cleanupChildren();
    process.exit(1);
});



