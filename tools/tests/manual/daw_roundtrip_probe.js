#!/usr/bin/env node
"use strict";

// Manual DAW smoke: synthetic sender -> Play -> Publish -> synthetic receiver.
// Configure both plugin instances on one audio track, with hardware input off.
const fs = require("node:fs");
const path = require("node:path");
const { parseArgs } = require("node:util");
const wrtc = require("@roamhq/wrtc");
const VDONinjaSDK = require("../../../js_sdk/vdoninja-sdk-node.js");
const { values } = parseArgs({ options: {
    input: { type: "string" }, output: { type: "string" },
    "host-rate": { type: "string" }, "buffer-size": { type: "string" },
    out: { type: "string", default: "build/daw-roundtrip" }
} });
const suffix = Date.now().toString(36);
const streams = { input: values.input || "dawin" + suffix,
    output: values.output || "dawout" + suffix };
if (streams.input === streams.output) throw new Error("Use different input and output stream IDs.");
const outputDir = path.resolve(values.out);
fs.mkdirSync(outputDir, { recursive: true });
fs.writeFileSync(path.join(outputDir, "streams.json"), JSON.stringify(streams, null, 2) + "\n");
const options = { host: "wss://wss.vdo.ninja", password: "", salt: "vdo.ninja", autoReconnect: false };
const publisher = new VDONinjaSDK(options), viewer = new VDONinjaSDK(options);
const source = new wrtc.nonstandard.RTCAudioSource(), track = source.createTrack();
const started = performance.now();
let phase = 0, sourceFrames = 0, sink = null, finished = false;

// Count elapsed audio frames, rather than assuming each timer callback took
// exactly 10 ms. Windows timer scheduling can otherwise underfeed the source.
const timer = setInterval(() => {
    const due = Math.floor((performance.now() - started) / 10);
    while (sourceFrames < due) {
        const samples = new Int16Array(480);
        for (let i = 0; i < samples.length; ++i) {
            samples[i] = Math.round(8000 * Math.sin(phase++ * 2 * Math.PI * 440 / 48000));
        }
        source.onData({ samples, sampleRate: 48000, bitsPerSample: 16, channelCount: 1, numberOfFrames: 480 });
        ++sourceFrames;
    }
}, 2);
const watchdog = setTimeout(() => finish({ passed: false, error: "180-second timeout" }, 1), 180000);

async function finish(result, code) {
    if (finished) return;
    finished = true;
    clearTimeout(watchdog);
    clearInterval(timer);
    if (sink) sink.stop();
    track.stop();
    const sourceAudioSeconds = phase / 48000;
    const sourceWallSeconds = (performance.now() - started) / 1000;
    const sourcePacingRatio = sourceAudioSeconds / sourceWallSeconds;
    if (result.passed && (sourcePacingRatio < 0.99 || sourcePacingRatio > 1.01)) {
        result.passed = false;
        result.error = "Synthetic sender did not maintain real-time pacing";
        code = 1;
    }
    const report = { ...streams, ...result, sourceAudioSeconds, sourceWallSeconds, sourcePacingRatio,
        hostSampleRate: values["host-rate"] ? Number(values["host-rate"]) : null,
        hostBufferSamples: values["buffer-size"] ? Number(values["buffer-size"]) : null,
        hostSettingsSource: "operator supplied", measuredChannel: 0,
        path: "SDK synthetic 440 Hz -> DAW Play -> DAW Publish -> SDK audio sink" };
    fs.writeFileSync(path.join(outputDir, "result.json"), JSON.stringify(report, null, 2) + "\n");
    console.log(JSON.stringify(report, null, 2));
    await Promise.allSettled([publisher.disconnect(), viewer.disconnect()]);
    process.exit(code); // Avoid wrtc native-finalizer shutdown hangs.
}

viewer.addEventListener("track", event => {
    const detail = event.detail || event;
    if (detail.track.kind !== "audio" || sink) return;
    console.log("Return audio connected; warming up for three seconds.");
    sink = new wrtc.nonstandard.RTCAudioSink(detail.track);
    let warmup = 0, frames = 0, samples = 0, energy = 0, peak = 0;
    let positiveCrossings = 0, previous = 0, silentFrames = 0;
    sink.ondata = audio => {
        if (warmup++ < 300) return;
        ++frames;
        let framePeak = 0;
        for (let i = 0; i < audio.samples.length; i += audio.channelCount) {
            const value = audio.samples[i] / 32768;
            energy += value * value;
            ++samples;
            if (previous < 0 && value >= 0) ++positiveCrossings;
            previous = value;
            framePeak = Math.max(framePeak, Math.abs(value));
        }
        peak = Math.max(peak, framePeak);
        if (framePeak < 1e-5) ++silentFrames;
        if (frames >= 1000) {
            const rms = Math.sqrt(energy / samples);
            const frequencyHz = positiveCrossings / (samples / audio.sampleRate);
            const passed = rms > 0.02 && rms < 0.3 && frequencyHz > 435 && frequencyHz < 445 &&
                silentFrames === 0 && peak < 0.99;
            finish({ passed, frames, samples, sampleRate: audio.sampleRate, channels: audio.channelCount,
                rms, peak, frequencyHz, frequencyMethod: "positive zero crossings", silentFrames }, passed ? 0 : 1);
        }
    };
});

process.on("SIGINT", () => finish({ passed: false, error: "Interrupted" }, 130));
process.on("SIGTERM", () => finish({ passed: false, error: "Terminated" }, 143));
process.on("unhandledRejection", error => finish({ passed: false, error: String(error) }, 1));
(async () => {
    await publisher.connect();
    await publisher.publish(new wrtc.MediaStream([track]), { streamID: streams.input, audio: true, video: false });
    console.log(`Set the first plugin to Play: ${streams.input}`);
    console.log(`Set the next plugin to Publish: ${streams.output}`);
    console.log("Keep passwords empty and use the default VDO.Ninja server/domain. Start DAW processing.");
    await viewer.connect();
    await viewer.view(streams.output, { audio: true, video: false });
})().catch(error => finish({ passed: false, error: String(error) }, 1));
