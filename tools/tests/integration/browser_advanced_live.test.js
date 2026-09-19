#!/usr/bin/env node
"use strict";

// Opt-in live interoperability check against the real VDO.Ninja web client.
// Uses an isolated, headless Chrome profile and synthetic audio only.
// Install playwright-core separately; no browser download or project dependency:
// npm install --prefix build/test-tools/browser --no-package-lock --no-save playwright-core
const assert = require("node:assert/strict");
const path = require("node:path");
const fs = require("node:fs");
const crypto = require("node:crypto");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { rootDir, cliExecutable, pluginBundle } = require("./build_paths");
const { chromium } = require(process.env.WEBRTC_TEST_PLAYWRIGHT ||
    path.join(rootDir, "build/test-tools/browser/node_modules/playwright-core"));

const webBase = process.env.WEBRTC_TEST_WEB_BASE_URL || "https://backup.vdo.ninja/";
const wss = process.env.WEBRTC_TEST_WSS || "wss://apibackup.vdo.ninja/";
const password = process.env.WEBRTC_TEST_BROWSER_PASSWORD || "browser-test-password"; // Synthetic test credential only.
const uiLink = process.env.WEBRTC_TEST_UI_LINK ? new URL(JSON.parse(fs.readFileSync(process.env.WEBRTC_TEST_UI_LINK, "utf8")).link) : null;
const outDir = path.join(rootDir, "build/browser-advanced-validation");
fs.mkdirSync(outDir, { recursive: true });
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const results = [];
const children = new Set();
let browser;
let browserVersion;
let failure;

function launchPlugin(mode, stream, salt, label, runtimeMs = 35000) {
    // Do not inherit CLI state-injection overrides or room/STUN test settings.
    const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
        !key.startsWith("WEBRTC_VST_") && !key.startsWith("WEBRTC_CLI_HOST_")));
    Object.assign(env, {
        WEBRTC_VST_MODE: mode, WEBRTC_VST_STREAM_ID: stream,
        WEBRTC_VST_PASSWORD: password, WEBRTC_VST_HANDSHAKE_URL: wss,
        WEBRTC_VST_WEB_BASE_URL: webBase, WEBRTC_VST_SALT: salt,
        WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: String(runtimeMs),
        WEBRTC_CLI_HOST_TIMEOUT_MS: String(runtimeMs + 10000),
        WEBRTC_CLI_HOST_BLOCK_SLEEP_MS: "5", WEBRTC_CLI_HOST_TONE_HZ: mode === "seed" ? "1000" : "0",
        WEBRTC_CLI_HOST_MONITOR_OUTPUT: "1", WEBRTC_VST_LOG_STDOUT: "1"
    });
    const child = spawn(cliExecutable, [pluginBundle], { env, stdio: ["ignore", "pipe", "pipe"] });
    children.add(child);
    let output = "";
    const log = fs.createWriteStream(path.join(outDir, label + "-plugin.log"));
    for (const pipe of [child.stdout, child.stderr]) pipe.on("data", data => { output += data; log.write(data); });
    const done = once(child, "close").then(([code]) => {
        children.delete(child); log.end(); return { code, output };
    });
    // Attach a rejection handler immediately, even while the browser is starting.
    done.catch(() => {});
    return { child, done };
}

function makeLink(mode, stream, salt, effectiveSalt, includeHash = true) {
    const url = new URL(webBase);
    url.searchParams.set(mode, stream);
    if (mode === "view") url.searchParams.set("style", "2");
    if (salt) url.searchParams.set("salt", salt);
    // wss2 overrides the endpoint without switching the browser to its separate
    // generic-relay protocol (which is what the wss parameter does).
    url.searchParams.set("wss2", wss);
    if (includeHash) url.searchParams.set("hash", crypto.createHash("sha256")
        .update(encodeURIComponent(password) + effectiveSalt).digest("hex").slice(0, 4));
    // Supply the test password instead of automating the site's password dialog.
    url.searchParams.set("password", password);
    url.searchParams.set("novideo", "");
    if (mode === "push") {
        url.searchParams.set("webcam", "");
        url.searchParams.set("autostart", "");
        url.searchParams.set("vd", "0");
        url.searchParams.set("ad", "1");
        url.searchParams.set("stereo", "1");
    }
    return url.href;
}

async function openPage(url, label, expectedSalt) {
    const context = await browser.newContext();
    await context.grantPermissions(["microphone"], { origin: new URL(webBase).origin });
    await context.addInitScript(() => {
        window.__testPCs = [];
        window.__testSockets = [];
        window.__testSources = [];
        const Peer = window.RTCPeerConnection;
        window.RTCPeerConnection = new Proxy(Peer, { construct(target, args) {
            const peer = new target(...args); window.__testPCs.push(peer); return peer;
        } });
        const Socket = window.WebSocket;
        window.WebSocket = new Proxy(Socket, { construct(target, args) {
            window.__testSockets.push(String(args[0])); return new target(...args);
        } });
        // Only the media source is replaced. The site's signaling, URL parsing,
        // encryption, WebRTC negotiation and audio receive path run unchanged.
        navigator.mediaDevices.getUserMedia = async constraints => {
            if (!constraints.audio || constraints.video) throw new Error("Test permits synthetic audio only");
            const audio = new AudioContext({ sampleRate: 48000 });
            const oscillator = audio.createOscillator();
            const gain = audio.createGain();
            const destination = audio.createMediaStreamDestination();
            oscillator.frequency.value = 440;
            gain.gain.value = 0.25;
            oscillator.connect(gain).connect(destination);
            oscillator.start();
            await audio.resume();
            window.__testSources.push({ audio, oscillator });
            return destination.stream;
        };
    });
    const page = await context.newPage();
    const log = fs.createWriteStream(path.join(outDir, label + "-browser.log"));
    page.on("console", message => log.write(message.type() + " " + message.text() + "\n"));
    page.on("pageerror", error => log.write("PAGE ERROR " + error.stack + "\n"));
    page.on("dialog", dialog => dialog.dismiss());
    try {
        await page.goto(url, { waitUntil: "domcontentloaded", timeout: 45000 });
        if (uiLink) {
            const prompt = page.locator('.promptModal input[type="password"]:visible').first();
            await prompt.waitFor({timeout:30000});
            await prompt.fill(password);
            await page.locator('.promptModal:visible button[id^="submit_"]').first().click();
            console.log("PASS: actual browser password dialog accepted typed test password");
        }
        await page.waitForFunction(({ salt, server }) => window.session &&
            session.salt === salt && session.wss === server && !session.customWSS &&
            window.__testSockets.some(socket => socket === server),
        { salt: expectedSalt, server: wss }, { timeout: 30000 });
        return { page, close: async () => { await context.close(); log.end(); } };
    } catch (error) {
        console.error("Browser setup:", await page.evaluate(() => ({
            url: location.href, salt: window.session?.salt, wss: window.session?.wss,
            sockets: window.__testSockets, text: document.body.innerText.slice(0, 1000)
        })).catch(() => ({})));
        await context.close(); log.end(); throw error;
    }
}

async function incomingAudio(page) {
    return page.evaluate(async () => {
        let energy = 0, duration = 0, packets = 0, samples = 0;
        for (const pc of window.__testPCs) {
            if (pc.connectionState === "closed") continue;
            for (const stat of (await pc.getStats()).values()) {
                if (stat.type !== "inbound-rtp" || stat.kind !== "audio") continue;
                energy += stat.totalAudioEnergy || 0;
                duration += stat.totalSamplesDuration || 0;
                samples += stat.totalSamplesReceived || 0;
                packets += stat.packetsReceived || 0;
            }
        }
        return { energy, duration, packets, samples, rms: duration > 0 ? Math.sqrt(energy / duration) : 0 };
    });
}

async function pluginToBrowser(test) {
    const label = test.name + "-view";
    const stream = "vstbrowser" + crypto.randomBytes(6).toString("hex");
    const host = launchPlugin("seed", stream, test.salt, label, 90000);
    let viewer;
    try {
        if (test.negative) {
            const wrongSalt = test.salt + "-wrong";
            viewer = await openPage(makeLink("view", stream, wrongSalt, wrongSalt, false), label + "-wrong", wrongSalt);
            await pause(8000);
            const wrong = await incomingAudio(viewer.page);
            assert.equal(wrong.packets, 0, "Mismatched salt unexpectedly received audio packets");
            console.log("PASS mismatched-salt control: no audio packets after 8 seconds");
            results.push({ test: "mismatched-salt", ...wrong });
            await viewer.close(); viewer = null;
        }
        viewer = await openPage(makeLink("view", stream, test.salt, test.effective), label, test.effective);
        const deadline = Date.now() + 40000;
        let stats;
        do {
            await pause(500);
            stats = await incomingAudio(viewer.page);
            if (stats.duration >= 2 && stats.rms > 0.01) break;
        } while (Date.now() < deadline);
        if (!(stats.duration >= 2 && stats.rms > 0.01)) {
            console.error("Peer diagnostics:", await viewer.page.evaluate(() => window.__testPCs.map(pc => ({
                connection: pc.connectionState, ice: pc.iceConnectionState,
                signaling: pc.signalingState, local: pc.localDescription?.sdp, remote: pc.remoteDescription?.sdp
            }))));
        }
        assert.ok(stats.duration >= 2 && stats.rms > 0.01, "Browser received no sustained tone: " + JSON.stringify(stats));
        console.log("PASS " + label, JSON.stringify(stats));
        results.push({ test: label, salt: test.salt, ...stats });
    } finally {
        if (viewer) await viewer.close();
        host.child.kill(); await host.done;
    }
}

async function browserToPlugin(test) {
    const label = test.name + "-push";
    const stream = uiLink ? uiLink.searchParams.get("push") : "vstbrowser" + crypto.randomBytes(6).toString("hex");
    let link = makeLink("push", stream, test.salt, test.effective);
    if (uiLink) {
        const copied = new URL(uiLink);
        assert.ok(!copied.searchParams.has("password"), "Copy must not expose the password");
        for (const [key,value] of new URL(link).searchParams) {
            if (["novideo","webcam","autostart","vd","ad","stereo"].includes(key)) copied.searchParams.set(key,value);
        }
        link = copied.href;
    }
    const publisher = await openPage(link, label, test.effective);
    let host;
    try {
        await publisher.page.waitForFunction(() => window.__testSources.length > 0, null, { timeout: 30000 });
        host = launchPlugin("play", stream, test.salt, label);
        const { code, output } = await host.done;
        assert.equal(code, 0, "CLI host failed; see " + label + "-plugin.log");
        const rms = Number(output.match(/output_rms=([\d.eE+-]+)/)?.[1] || 0);
        assert.ok(rms > 0.01, "Plugin received no sustained browser tone, RMS=" + rms);
        console.log("PASS " + label + " RMS=" + rms);
        results.push({ test: label, salt: test.salt, rms });
    } finally {
        if (host?.child.exitCode === null) { host.child.kill(); await host.done; }
        await publisher.close();
    }
}

async function cleanup() {
    for (const child of children) child.kill();
    if (browser) await browser.close();
}
const watchdog = setTimeout(() => { console.error("Browser validation timed out"); cleanup().finally(() => process.exit(1)); }, 600000);
for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, () => cleanup().finally(() => process.exit(1)));

(async () => {
    browser = await chromium.launch({
        ...(process.env.WEBRTC_TEST_CHROME ? { executablePath: process.env.WEBRTC_TEST_CHROME } : { channel: "chrome" }),
        headless: true,
        args: ["--renderer-process-limit=2", "--disable-gpu", "--autoplay-policy=no-user-gesture-required", "--use-fake-device-for-media-stream",
            "--use-fake-ui-for-media-stream", "--mute-audio"]
    });
    browserVersion = browser.version();
    console.log("Browser:", browserVersion, "Web:", webBase, "Signaling:", wss);
    if (uiLink) {
        const salt = uiLink.searchParams.get("salt");
        await browserToPlugin({name:"actual-ui",salt,effective:salt});
        return;
    }
    const cases = [
        { name: "custom", salt: "SALT", effective: "SALT", negative: true },
        { name: "escaped", salt: "mix &+/=?#-ASCII", effective: "mix &+/=?#-ASCII" },
        { name: "automatic", salt: "", effective: "vdo.ninja" },
        { name: "unicode", salt: "mix &+/=?#-é🔊", effective: "mix &+/=?#-é🔊" }
    ].filter(test => process.env.WEBRTC_TEST_CASE ? test.name === process.env.WEBRTC_TEST_CASE : !test.unsupported);
    assert.ok(cases.length, "Unknown WEBRTC_TEST_CASE");
    for (const test of cases) {
        await pluginToBrowser(test);
        await browserToPlugin(test);
    }
})().catch(error => { failure = error.message; console.error(error); process.exitCode = 1; }).finally(async () => {
    clearTimeout(watchdog);
    const suffix = ["custom", "escaped", "automatic", "unicode"].includes(process.env.WEBRTC_TEST_CASE)
        ? "-" + process.env.WEBRTC_TEST_CASE : "";
    fs.writeFileSync(path.join(outDir, "results" + suffix + ".json"), JSON.stringify({
        date: new Date().toISOString(), browser: browserVersion, webBase, wss,
        success: !failure, failure, results,
        note: "AES key phrases use browser-compatible UTF-16 low bytes; hashes remain UTF-8."
    }, null, 2) + "\n");
    await cleanup();
});
