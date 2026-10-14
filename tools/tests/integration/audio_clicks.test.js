#!/usr/bin/env node
"use strict";
const assert = require("node:assert/strict"), fs = require("node:fs"), path = require("node:path");
const {spawn} = require("node:child_process"), {once} = require("node:events");
const {analyzeTone} = require("../audio_click_analysis");
const {rootDir} = require("./build_paths");
let child;
for (const signal of ["SIGTERM","SIGINT"]) process.on(signal,()=>child?.kill(signal));
(async () => {
    let log = "";
    child = spawn(process.execPath,[path.join(__dirname,"audio_quality_soak.test.js")], {
        env:{...process.env, WEBRTC_SOAK_SECONDS:"32", WEBRTC_SOAK_CAPTURE:"1", WEBRTC_SOAK_STRICT:"1", WEBRTC_SOAK_RECONNECT:"1"},
        stdio:["ignore","pipe","pipe"]
    });
    child.stdout.on("data",data=>{log+=data;process.stdout.write(data);});
    child.stderr.on("data",data=>{log+=data;process.stderr.write(data);});
    const [code] = await once(child,"close");
    assert.equal(code,0,"Short capture failed");
    const directory = log.match(/Running 32s.*; (.+)/)?.[1];
    assert.ok(directory);
    const buffer = fs.readFileSync(path.join(directory,"play.pcmblocks"));
    const block = 256, rate = 48000, stride = 8 + block*2*4;
    assert.equal(buffer.length % stride,0);
    const skip = Math.ceil(10*rate/block), end = Math.floor(buffer.length/stride)-Math.ceil(rate/block);
    const channels = [new Float32Array((end-skip)*block),new Float32Array((end-skip)*block)];
    assert.ok(channels[0].length > 20*rate);
    for(let b=skip;b<end;++b) for(let ch=0;ch<2;++ch) for(let n=0;n<block;++n)
        channels[ch][(b-skip)*block+n]=buffer.readFloatLE(b*stride+8+(ch*block+n)*4);
    const results = channels.map((samples,ch)=>analyzeTone(samples,rate,ch?1500:1000,block));
    fs.writeFileSync(path.join(directory,"clicks.json"),JSON.stringify({rate,block,results},null,2));
    console.log(JSON.stringify(results,null,2));
    for(const r of results) {
        assert.equal(r.nonfinite,0); assert.ok(r.peak<1);
        assert.equal(r.suspiciousSamples,0,"Isolated discontinuity in continuous-tone capture");
        assert.ok(r.maxStep<.15,"Abrupt sample jump");
    }
    // A short stereo WAV lets a human audition exactly the measured return.
    const frames = 5*rate, wav = Buffer.alloc(44+frames*4);
    wav.write("RIFF");wav.writeUInt32LE(wav.length-8,4);wav.write("WAVEfmt ",8);wav.writeUInt32LE(16,16);
    wav.writeUInt16LE(1,20);wav.writeUInt16LE(2,22);wav.writeUInt32LE(rate,24);wav.writeUInt32LE(rate*4,28);
    wav.writeUInt16LE(4,32);wav.writeUInt16LE(16,34);wav.write("data",36);wav.writeUInt32LE(frames*4,40);
    for(let i=0;i<frames;++i)for(let ch=0;ch<2;++ch)
        wav.writeInt16LE(Math.round(Math.max(-1,Math.min(1,channels[ch][i]))*32767),44+i*4+ch*2);
    fs.writeFileSync(path.join(directory,"received-stereo.wav"),wav);
    console.log("PASS: all steady-state samples and block boundaries checked for tone discontinuities; "+path.relative(rootDir,directory));
})().catch(error=>{console.error(error);process.exitCode=1;});
