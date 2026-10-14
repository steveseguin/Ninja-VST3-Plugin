"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");
const { createTonePump } = require("../helpers/paced_tone.js");

test("late Windows-style timer callbacks still supply one second of audio per second", () => {
    let now = 0;
    const frames = [];
    const tone = createTonePump({ onData: frame => frames.push(frame) }, () => now);
    for (now = 15.625; now <= 1000; now += 15.625) tone.pump();
    // A callback-counted sender supplies only 64 blocks / 0.64 seconds here.
    assert.equal(frames.length, 100);
    assert.equal(frames.reduce((sum, frame) => sum + frame.samples.length, 0), 48000);
    assert.equal(tone.getStats().sourceAudioSeconds, 1);
    for (const frame of frames) {
        assert.equal(frame.numberOfFrames, 480);
        assert.equal(frame.sampleRate, 48000);
        assert.equal(frame.channelCount, 1);
        assert.equal(frame.bitsPerSample, 16);
    }
});

test("early or repeated callbacks cannot emit audio ahead of the clock", () => {
    let now = 7000;
    let samples = 0;
    const tone = createTonePump({ onData: frame => { samples += frame.samples.length; } }, () => now);
    for (let elapsed = 0; elapsed <= 1000; elapsed += 2) {
        now = 7000 + elapsed;
        tone.pump();
        tone.pump();
        assert.equal(samples, Math.floor(elapsed / 10) * 480);
    }
    assert.equal(tone.getStats().sourcePacingRatio, 1);
});

test("catch-up after a delayed callback preserves the continuous 440 Hz signal", () => {
    let now = 0;
    const actual = [];
    const tone = createTonePump({ onData: frame => actual.push(...frame.samples) }, () => now);
    for (const time of [10, 20, 117, 118, 200]) {
        now = time;
        tone.pump();
    }
    assert.equal(actual.length, 9600);
    const crossings = [];
    for (let sample = 1; sample < actual.length; ++sample) {
        if (actual[sample - 1] < 0 && actual[sample] >= 0) crossings.push(sample);
        // Resetting phase at a block/catch-up boundary creates a sharp step.
        assert.ok(Math.abs(actual[sample] - actual[sample - 1]) < 462);
    }
    assert.ok(crossings.length >= 87);
    const frequency = (crossings.length - 1) * 48000 / (crossings.at(-1) - crossings[0]);
    assert.ok(Math.abs(frequency - 440) < 1);
    assert.equal(tone.getStats().sourcePacingRatio, 1);
});
