"use strict";

// For a constant sinusoid, x[n] = 2*cos(w)*x[n-1] - x[n-2]. The residual
// exposes isolated spikes and phase jumps even when their RMS is negligible.
// This is a synthetic-tone diagnostic, not a perceptual model for music.
function analyzeTone(samples, rate, hz, blockSize, tolerance = 0.001) {
    const coefficient = 2 * Math.cos(2 * Math.PI * hz / rate);
    let nonfinite = 0, peak = 0, maxStep = 0, maxResidual = 0, maxBoundaryResidual = 0, suspiciousSamples = 0;
    for (let i=0; i<samples.length; ++i) {
        const value = samples[i];
        if (!Number.isFinite(value)) {nonfinite++; continue;}
        peak = Math.max(peak,Math.abs(value));
        if (i) maxStep = Math.max(maxStep,Math.abs(value-samples[i-1]));
        if (i<2) continue;
        const residual = Math.abs(value-coefficient*samples[i-1]+samples[i-2]);
        maxResidual = Math.max(maxResidual,residual);
        if (i % blockSize < 2) maxBoundaryResidual = Math.max(maxBoundaryResidual,residual);
        if (residual > tolerance) ++suspiciousSamples;
    }
    return {frames:samples.length,nonfinite,peak,maxStep,maxResidual,maxBoundaryResidual,suspiciousSamples,tolerance};
}
module.exports = {analyzeTone};
