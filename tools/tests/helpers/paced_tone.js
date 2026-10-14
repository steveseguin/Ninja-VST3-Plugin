"use strict";

// RTCAudioSource expects 480 samples per 10 ms at 48 kHz. Timer callbacks can
// arrive late (notably on Windows); count elapsed audio blocks, not callbacks.
function createTonePump(source, now = () => performance.now()) {
    const started = now();
    let blocks = 0;
    let phase = 0;
    return {
        pump() {
            const due = Math.floor((now() - started) / 10);
            while (blocks < due) {
                const samples = new Int16Array(480);
                for (let i = 0; i < samples.length; ++i) {
                    samples[i] = Math.round(8000 * Math.sin(phase++ * 2 * Math.PI * 440 / 48000));
                }
                source.onData({ samples, sampleRate: 48000, bitsPerSample: 16,
                    channelCount: 1, numberOfFrames: 480 });
                ++blocks;
            }
        },
        getStats() {
            const sourceAudioSeconds = phase / 48000;
            const sourceWallSeconds = (now() - started) / 1000;
            return { sourceAudioSeconds, sourceWallSeconds,
                sourcePacingRatio: sourceWallSeconds > 0 ? sourceAudioSeconds / sourceWallSeconds : 0 };
        }
    };
}

function startTestTone(source) {
    const tone = createTonePump(source);
    const timer = setInterval(() => tone.pump(), 2);
    return { getStats: () => tone.getStats(), stop: () => clearInterval(timer) };
}

module.exports = { createTonePump, startTestTone };
