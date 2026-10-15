"use strict";

// Offline checks for the playback probe's measurement code, never its live runner.
// Run: node --test tools/tests/offline/playback_quality_analysis.test.js
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { test } = require("node:test");

const sourcePath = process.env.WEBRTC_PLAYBACK_ANALYZER_SOURCE ||
    path.join(__dirname, "../integration/playback_quality.test.js");
const source = fs.readFileSync(sourcePath, "utf8");
const begin = source.indexOf("function read(mode) {");
const end = source.indexOf("for(const signal of [", begin);
assert.ok(begin >= 0 && end > begin, "Measurement extraction markers changed");
const analysisSource = source.slice(begin, end);
assert.ok(!/\b(?:spawn|require|WebSocket|setTimeout|launch|process\.on)\s*\(/.test(analysisSource),
    "Only the offline measurement functions may be extracted");

let RATE = 48000;
let FRAMES = 256;
let PERIOD = 96000;
let HIGH = 24000;
const DURATION = 36;
let STRIDE = 8 + FRAMES * 4 * 2;
let result;
const captures = new Map();
const context = {
    sampleRate: RATE, blockSize: FRAMES, performance: {},
    assert, path, out: "synthetic", process: { env: {} }, console: { log() {} },
    fs: {
        readFileSync(file) { return captures.get(path.basename(file)); },
        writeFileSync(_file, data) { result = JSON.parse(data); }
    }
};
vm.createContext(context, { codeGeneration: { strings: false, wasm: false } });
new vm.Script(analysisSource + "\nglobalThis.analysis = { read, analyze };")
    .runInContext(context, { timeout: 5000 });

// Same 0.25/0.05-peak, 500ms/1500ms producer envelope as cli_host/main.cpp.
// Buffers exist only in memory and are decoded by the original capture reader.
function capture({ delayBlocks = 0, gains = [1, 1], phases = [0, 0], clip,
    alternating = true, clippedGain = 1, clipChannels = [0, 1], varyingDelay = false,
    highGainOnly = false, click = false, harmonic = 0 } = {}) {
    const count = Math.floor(DURATION * RATE / FRAMES);
    const bytes = Buffer.alloc(count * STRIDE);
    for (let block = 0; block < count; block++) {
        const cycle = Math.floor(block * FRAMES / PERIOD);
        const delay = varyingDelay ? [7, 9, 10][cycle % 3] : delayBlocks;
        bytes.writeDoubleLE(100 + block * FRAMES / RATE, block * STRIDE);
        for (let ch = 0; ch < 2; ch++) {
            for (let i = 0; i < FRAMES; i++) {
                const n = (block - delay) * FRAMES + i;
                const amplitude = n < 0 ? 0 : n % PERIOD < HIGH ? 0.25 : 0.05;
                let value = amplitude * Math.sin(2 * Math.PI * (ch ? 1500 : 1000) * n / RATE + phases[ch]);
                value += amplitude * harmonic * Math.sin(2 * Math.PI * (ch ? 3000 : 2000) * n / RATE);
                if (clip !== undefined && clipChannels.includes(ch) &&
                    (!alternating || Math.floor(n / PERIOD) % 2 === 0)) {
                    value = Math.max(-clip, Math.min(clip, value)) * clippedGain;
                }
                const gain = highGainOnly && n % PERIOD >= HIGH ? 1 : gains[ch];
                if (click && ch === 1 && n === Math.round(RATE * 10.2)) value += 0.35;
                bytes.writeFloatLE(value * gain, block * STRIDE + 8 + ch * FRAMES * 4 + i * 4);
            }
        }
    }
    return bytes;
}

captures.set("seed.pcmblocks", capture());
let seed = context.analysis.read("seed");
captures.clear();
function analyze(options) {
    captures.set("play.pcmblocks", capture(options));
    const play = context.analysis.read("play");
    captures.clear();
    result = undefined;
    context.analysis.analyze(seed, play);
    return result;
}
function clean(options) {
    const measured = analyze(options);
    assert.ok(measured.latencyMs.markers >= 5);
    assert.equal(measured.silentBlocks, 0);
    assert.equal(measured.nonfinite, 0);
    assert.equal(measured.clippedBlocks, 0);
    for (const channel of measured.thdnPercent) {
        assert.ok(channel.windows > 300, "Clean plateau coverage must be retained");
        assert.ok(channel.p95 < 0.001, "Phase/gain changes must not count as tone distortion");
    }
    return measured;
}

test("exact producer envelope has complete clean plateau coverage at 48ms", () => {
    const measured = clean({ delayBlocks: 9 });
    assert.ok(Math.abs(measured.latencyMs.p95 - 48) < 0.00001);
    assert.deepEqual(measured.thdnPercent.map(row => row.windows), [406, 406]);
});

test("clean lower gain and independent phase remain valid at 16ms", () => {
    clean({ delayBlocks: 3, gains: [0.8, 0.85], phases: [0.3, 1.1] });
});

test("clean higher gain and independent phase remain valid at 80ms", () => {
    clean({ delayBlocks: 15, gains: [1.1, 1.15], phases: [1.7, 2.2] });
});

test("previously rejected high-gain clean plateaus still fail level qualification", () => {
    assert.throws(() => analyze({ delayBlocks: 9, gains: [1.2, 1.25] }), /expected-level|windows>50/);
});

test("previously rejected quiet right channel still fails level qualification", () => {
    assert.throws(() => analyze({ delayBlocks: 9, gains: [1, 0.5] }), /expected-level|windows>50/);
});

test("per-cycle latency variation within existing limits preserves transition exclusion", () => {
    const measured = clean({ varyingDelay: true, phases: [0.4, 1.2] });
    assert.ok(measured.latencyMs.spread > 0 && measured.latencyMs.spread < 25);
});

test("alternating conventionally hard-clipped plateaus cannot be omitted", () => {
    assert.throws(() => analyze({ delayBlocks: 9, clip: 0.15 }), /Tone residual distortion exceeds/);
    assert.deepEqual(result.thdnPercent.map(row => row.windows), [406, 406]);
    assert.ok(result.thdnPercent.every(row => row.p95 > 18));
});

test("the same clipped plateaus scaled into the old RMS band remain rejected", () => {
    assert.throws(() => analyze({ delayBlocks: 9, clip: 0.15, clippedGain: 1.2 }), /Tone residual distortion exceeds/);
    assert.ok(result.thdnPercent.every(row => row.p95 > 18));
});

test("right-channel clipping cannot hide behind clean left-channel timing markers", () => {
    assert.throws(() => analyze({ delayBlocks: 9, clip: 0.15, clipChannels: [1] }), /Tone residual distortion exceeds/);
    assert.ok(result.thdnPercent[0].p95 < 0.001);
    assert.ok(result.thdnPercent[1].p95 > 18);
});

test("all clipped plateaus are measured rather than reported as missing windows", () => {
    assert.throws(() => analyze({ delayBlocks: 9, clip: 0.15, alternating: false }), /expected-level|windows>50/);
    assert.ok(result.thdnPercent.every(row => row.windows > 300 && row.median > 18));
});

test("existing full-scale clipping guard is unchanged", () => {
    assert.throws(() => analyze({ delayBlocks: 9, gains: [5, 5], highGainOnly: true }), /Expected values to be strictly equal/);
    assert.ok(result.clippedBlocks > 0);
});

test("existing latency limit still rejects an otherwise clean delayed signal", () => {
    assert.throws(() => analyze({ delayBlocks: 21 }), /Local one-way latency exceeds/);
});

test("one isolated right-channel click fails even when percentile distortion is low", () => {
    assert.throws(() => analyze({ delayBlocks: 9, click: true }), /Abrupt waveform discontinuity/);
    assert.ok(result.thdnPercent[1].p95 < 0.001);
    assert.ok(result.maxAdjacentStep[1] > 0.3);
});

function setFormat(rate, frames) {
    RATE = rate; FRAMES = frames; PERIOD = rate * 2; HIGH = rate / 2; STRIDE = 8 + frames * 8;
    context.sampleRate = rate; context.blockSize = frames;
    captures.set("seed.pcmblocks", capture());
    seed = context.analysis.read("seed");
    captures.clear();
}

test("resampled format allows codec reference residual but still bounds distortion", () => {
    try {
        setFormat(44100, 128);
        const measured = analyze({ delayBlocks: 9, harmonic: 0.014 });
        assert.ok(measured.thdnPercent.every(row => row.median > 1.39 && row.median < 1.41));
        assert.throws(() => analyze({ delayBlocks: 9, harmonic: 0.025 }), /Tone residual distortion exceeds/);
        assert.throws(() => analyze({ delayBlocks: 9, click: true }), /Abrupt waveform discontinuity/);
    } finally { setFormat(48000, 256); }
});

test("native 48 kHz allows measured codec variation but retains a stricter gate than resampled audio", () => {
    const measured = analyze({ delayBlocks: 9, harmonic: 0.014 });
    assert.ok(measured.thdnPercent.every(channel => channel.median > 1.3 && channel.median < 1.5));
    assert.throws(() => analyze({ delayBlocks: 9, harmonic: 0.016 }), /Tone residual distortion exceeds/);
});
