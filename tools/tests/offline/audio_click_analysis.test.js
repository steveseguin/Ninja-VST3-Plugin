"use strict";
const {test} = require("node:test"), assert = require("node:assert/strict");
const {analyzeTone} = require("../audio_click_analysis");
const tone = () => Float32Array.from({length:48000}, (_,i) => .25*Math.sin(2*Math.PI*1000*i/48000));
test("click detector accepts a continuous tone across host block boundaries", () => {
    const result = analyzeTone(tone(),48000,1000,256);
    assert.equal(result.suspiciousSamples,0);
    assert.ok(result.maxResidual < 1e-6);
});
test("click detector detects a single low-energy spike at a host boundary", () => {
    const data = tone(); data[256*20] += .002;
    const result = analyzeTone(data,48000,1000,256);
    assert.equal(result.suspiciousSamples,3);
    assert.ok(result.maxBoundaryResidual > .003);
});
test("click detector detects a discontinuous phase splice", () => {
    const data = tone();
    for (let i=10000;i<data.length;++i) data[i]=.25*Math.sin(2*Math.PI*1000*i/48000+1);
    assert.ok(analyzeTone(data,48000,1000,256).suspiciousSamples > 0);
});
