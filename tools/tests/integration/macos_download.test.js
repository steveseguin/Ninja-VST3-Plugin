#!/usr/bin/env node
"use strict";
// Exercise the public website's actual Mac download buttons in an isolated
// browser. No GitHub credentials, developer artifacts, or user's browser profile.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { rootDir } = require("./build_paths");
const { chromium } = require(path.join(rootDir, "build/test-tools/browser/node_modules/playwright-core"));
const version = process.env.WEBRTC_TEST_RELEASE_VERSION || "0.2.5";
assert.match(version, /^\d+\.\d+\.\d+$/);
const site = "https://steveseguin.github.io/Ninja-VST3-Plugin/";
const release = `https://github.com/steveseguin/Ninja-VST3-Plugin/releases/download/mac-v${version}/`;
const out = fs.mkdtempSync(path.join(rootDir, "build/test-tools/download-smoke-"));
let browser;
async function cleanup() { if (browser) await browser.close(); }
for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, () => cleanup().finally(() => process.exit(1)));
(async () => {
    browser = await chromium.launch({ channel: "chrome", headless: true,
        args: ["--renderer-process-limit=2", "--disable-gpu", "--mute-audio"] });
    const context = await browser.newContext({ acceptDownloads: true });
    const page = await context.newPage();
    const files = [];
    for (const route of ["", "getting-started.html"]) {
        const response = await page.goto(site + route, { waitUntil: "domcontentloaded" });
        assert.equal(response.status(), 200);
        for (const [arch, name] of [["arm64", "Mac — Apple Silicon"], ["x86_64", "Mac — Intel"]]) {
            const file = `webrtc_vst-v${version}-macos-${arch}-notarized.dmg`;
            const button = page.getByRole("link", { name, exact: true });
            assert.equal(await button.getAttribute("href"), release + file);
            assert.ok(await button.isVisible(), "Hidden download link");
            if (route) continue;
            const event = page.waitForEvent("download", { timeout: 60000 });
            await button.click();
            const download = await event;
            assert.equal(download.suggestedFilename(), file);
            await download.saveAs(path.join(out, file));
            assert.equal(await download.failure(), null);
            files.push(file);
        }
    }
    const manifestName = `webrtc_vst-v${version}-macos-SHA256SUMS.txt`;
    const response = await context.request.get(release + manifestName);
    assert.equal(response.status(), 200);
    const manifest = await response.text();
    fs.writeFileSync(path.join(out, manifestName), manifest);
    const results = files.map(file => {
        const hash = crypto.createHash("sha256").update(fs.readFileSync(path.join(out, file))).digest("hex");
        const entry = manifest.split(/\r?\n/).find(line => line.trim().endsWith(file));
        assert.ok(entry && entry.startsWith(hash + " "), "Checksum mismatch: " + file);
        return { file, sha256: hash, bytes: fs.statSync(path.join(out, file)).size };
    });
    fs.writeFileSync(path.join(out, "results.json"), JSON.stringify({ date: new Date().toISOString(),
        site, release, browser: browser.version(), results }, null, 2) + "\n");
    console.log("PASS: both public website pages expose the correct Mac links; both browser downloads match the public checksum manifest.");
    console.log(out);
})().catch(error => { console.error(error); process.exitCode = 1; }).finally(cleanup);
