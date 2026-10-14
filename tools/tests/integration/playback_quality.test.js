#!/usr/bin/env node
"use strict";
// Actual VST3 Seed -> encrypted WebRTC/Opus -> VST3 Play, independent processes.
// Same-machine monotonic capture timestamps exclude hardware ADC/DAC latency.
const assert=require("node:assert/strict"), fs=require("node:fs"), path=require("node:path"), crypto=require("node:crypto");
const {spawn}=require("node:child_process"), {once}=require("node:events");
const {rootDir,cliExecutable,pluginBundle}=require("./build_paths");
const out=path.join(rootDir,"build/test-tools/playback-probe-"+Date.now());fs.mkdirSync(out,{recursive:true});
const performance={};
const children=new Set(), stream="ownedprobe"+crypto.randomBytes(8).toString("hex");
const duration=Number(process.env.WEBRTC_PROBE_SECONDS||45);
const sampleRate=Number(process.env.WEBRTC_PROBE_SAMPLE_RATE||48000);
const blockSize=Number(process.env.WEBRTC_PROBE_BLOCK_SIZE||256);
assert.ok([32000,44100,48000,88200,96000,192000].includes(sampleRate));
assert.ok(Number.isInteger(blockSize)&&blockSize>=16&&blockSize<=2048);
assert.ok(duration>=20&&duration<=100);
function launch(mode,seconds) {
 const env=Object.fromEntries(Object.entries(process.env).filter(([k])=>!/^WEBRTC_(VST|CLI_HOST)_/.test(k)));
 Object.assign(env,{WEBRTC_VST_MODE:mode,WEBRTC_VST_STREAM_ID:stream,WEBRTC_VST_PASSWORD:"quality &é+pass",
  WEBRTC_VST_SALT:"quality é🔊",WEBRTC_VST_WEB_BASE_URL:"https://backup.vdo.ninja/",WEBRTC_VST_HANDSHAKE_URL:"wss://apibackup.vdo.ninja/",
  WEBRTC_VST_DISABLE_STUN:"1",WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS:String(seconds*1000),WEBRTC_CLI_HOST_TIMEOUT_MS:String((seconds+10)*1000),
  WEBRTC_CLI_HOST_SAMPLE_RATE:String(sampleRate),WEBRTC_CLI_HOST_BLOCK_SIZE:String(blockSize),
  WEBRTC_CLI_HOST_PROCESS_MODE:mode==="seed" ? process.env.WEBRTC_PROBE_SEED_PROCESS_MODE||"realtime" : "realtime",
  WEBRTC_CLI_HOST_REALTIME_PACING:"1",WEBRTC_CLI_HOST_CONTINUOUS_WARMUP:"1",WEBRTC_CLI_HOST_TONE_HZ:"1000",WEBRTC_CLI_HOST_RIGHT_TONE_HZ:"1500",
  WEBRTC_CLI_HOST_PROBE_ENVELOPE:"1",WEBRTC_CLI_HOST_CAPTURE:path.join(out,mode+".pcmblocks")});
 if(process.env.WEBRTC_PROBE_STALL_WARMUP==="1") delete env.WEBRTC_CLI_HOST_CONTINUOUS_WARMUP;
 const profile=process.platform==="darwin";
 const child=spawn(profile ? "/usr/bin/time" : cliExecutable,profile ? ["-l",cliExecutable,pluginBundle] : [pluginBundle],{env,detached:process.platform!=="win32",stdio:["ignore","pipe","pipe"]});children.add(child);
 const log=fs.createWriteStream(path.join(out,mode+".log")); for(const pipe of [child.stdout,child.stderr])pipe.on("data",d=>log.write(d));
 let timing="";
 child.stderr.on("data",data=>{if(timing.length<65536) timing+=data;});
 return once(child,"close").then(async ([code])=>{
  children.delete(child); await new Promise(resolve=>log.end(resolve)); assert.equal(code,0,mode+" failed");
  if(profile) {
   const cpu=timing.match(/([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys/);
   assert.ok(cpu,"Missing CPU timing");
   performance[mode]={wallSeconds:Number(cpu[1]),cpuSeconds:Number(cpu[2])+Number(cpu[3]),
    peakRssBytes:Number(timing.match(/(\d+)\s+maximum resident set size/)?.[1])};
  }
 });
}
function read(mode) {
 const b=fs.readFileSync(path.join(out,mode+".pcmblocks")), stride=8+blockSize*4*2;
 assert.equal(b.length%stride,0,"Truncated capture"); const blocks=[];
 for(let offset=0;offset<b.length;offset+=stride) {
  const data=[[],[]], rms=[];let nonfinite=0,peak=0;
  for(let ch=0;ch<2;ch++){let energy=0;for(let i=0;i<blockSize;i++){const v=b.readFloatLE(offset+8+ch*blockSize*4+i*4);data[ch].push(v);energy+=v*v;if(!Number.isFinite(v))nonfinite++;peak=Math.max(peak,Math.abs(v));}rms.push(Math.sqrt(energy/blockSize));}
  blocks.push({time:b.readDoubleLE(offset),rms,data,nonfinite,peak});
 }
 return blocks;
}
function quantile(values,q){const sorted=values.slice().sort((a,b)=>a-b);return sorted[Math.floor((sorted.length-1)*q)];}
function edges(blocks) {
 const result=[];let high=false;
 for(const b of blocks){if(!high&&b.rms[0]>0.10){result.push(b.time);high=true;}else if(high&&b.rms[0]<0.06)high=false;}
 return result;
}
function distortion(samples,hz) {
 // Orthogonal 10 ms windows contain integer cycles at 1000/1500 Hz.
 let energy=0,s=0,c=0,dc=0;
 for(let i=0;i<samples.length;i++){const a=2*Math.PI*hz*i/sampleRate,v=samples[i];energy+=v*v;s+=v*Math.sin(a);c+=v*Math.cos(a);dc+=v;}
 const fundamental=2*(s*s+c*c)/samples.length, residual=Math.max(0,energy-fundamental-dc*dc/samples.length);
 return Math.sqrt(residual/Math.max(fundamental,1e-15))*100;
}
function analyze(seed,play) {
 if(process.env.WEBRTC_PROBE_SEED_PROCESS_MODE==="offline") {
  assert.ok(seed.some(b=>b.rms[0]>0.1),"Offline Seed must preserve dry host audio");
  assert.ok(play.length>1000&&play.every(b=>b.peak===0),"Offline bounce broadcast audio to the live receiver");
  console.log("PASS: offline Seed retained dry audio and sent no audio to a real-time Play receiver; "+out);return;
 }
 const begins=play[0].time+8, ends=play.at(-1).time-1;
 const tx=edges(seed).filter(t=>t>begins&&t<ends-1), rx=edges(play);
 const latencies=tx.map(t=>{const edge=rx.find(x=>x>=t&&x<t+0.5);assert.ok(edge,"Missing received level marker");return (edge-t)*1000;});
 assert.ok(latencies.length>=5,"Insufficient latency markers");
 const steady=play.filter(b=>b.time>=begins&&b.time<=ends);
 const nonfinite=steady.reduce((s,b)=>s+b.nonfinite,0), clipped=steady.filter(b=>b.peak>=1).length;
 const silent=steady.filter(b=>b.rms[0]<0.001||b.rms[1]<0.001).length;
 const all=[[],[]];for(const b of steady)for(let ch=0;ch<2;ch++)all[ch].push(...b.data[ch]);
 const distortions=[[],[]], maxAdjacentStep=[0,0], expectedLevelWindows=[0,0];
 const windowFrames=sampleRate/100;
 for(let ch=0;ch<2;ch++) for(let i=0;i+windowFrames<=all[ch].length;i+=windowFrames){
  const samples=all[ch].slice(i,i+windowFrames), level=Math.sqrt(samples.reduce((s,v)=>s+v*v,0)/windowFrames);
  // Known steady plateau, excluding intentional amplitude steps. Otherwise
  // a 10 ms window spanning a probe edge incorrectly counts that step as THD.
  const time=steady[0].time+i/sampleRate;
  if(rx.some(edge=>time>edge+0.1&&time+0.01<edge+0.4)) {
      if(level>0.13&&level<0.21) expectedLevelWindows[ch]++;
      distortions[ch].push(distortion(samples,ch?1500:1000));
      for(let j=0;j<samples.length;j++) if(i+j>0)
          maxAdjacentStep[ch]=Math.max(maxAdjacentStep[ch],Math.abs(samples[j]-all[ch][i+j-1]));
  }
 }
 // A queue/network-free Opus + production-resampler reference varies with
 // starting phase: up to 1.67% median residual at 44.1 kHz. Preserve the
 // original 48 kHz gate; allow the existing resampling path its measured floor.
 const medianResidualLimit=sampleRate===48000?1:2;
 const result={performance,medianResidualLimit,continuousWarmup:process.env.WEBRTC_PROBE_STALL_WARMUP!=="1",scope:`${sampleRate} Hz / ${blockSize} frames, same machine, real backup signaling, direct encrypted media; excludes audio devices/WAN`,
  latencyMs:{median:quantile(latencies,.5),p95:quantile(latencies,.95),min:Math.min(...latencies),max:Math.max(...latencies),spread:Math.max(...latencies)-Math.min(...latencies),markers:latencies.length},
  thdnPercent:distortions.map(v=>({median:quantile(v,.5),p95:quantile(v,.95),windows:v.length})),maxAdjacentStep,silentBlocks:silent,totalBlocks:steady.length,nonfinite,clippedBlocks:clipped};
 fs.writeFileSync(path.join(out,"results.json"),JSON.stringify(result,null,2));console.log(JSON.stringify(result,null,2));
 assert.equal(nonfinite,0);assert.equal(clipped,0);assert.equal(silent,0,"Playback had fully silent blocks after warmup");
 // These 0.25-amplitude tones have an ideal adjacent step below 0.074 at
 // every tested rate. Check isolated jumps as well as percentile distortion.
 assert.ok(maxAdjacentStep.every(step=>step<0.15),"Abrupt waveform discontinuity on a steady tone plateau");
 assert.ok(result.latencyMs.p95<100+Math.max(0,4*blockSize/sampleRate*1000-4*256/48000*1000),"Local one-way latency exceeds 100 ms plus extra host buffering");
 assert.ok(result.latencyMs.spread<25+Math.max(0,blockSize/sampleRate*1000-256/48000*1000),"Latency spread exceeds 25 ms plus extra block quantization");
 for(const [ch,d] of result.thdnPercent.entries()){assert.ok(expectedLevelWindows[ch]>50,"Insufficient expected-level plateau windows");assert.ok(d.windows>50);assert.ok(d.median<medianResidualLimit&&d.p95<3,`Tone residual distortion exceeds ${medianResidualLimit}% median / 3% p95`);}
 console.log("PASS: measured local playback latency, level-marker variation, tone residual, finite/unclipped uninterrupted output; "+out);
}
for(const signal of ["SIGINT","SIGTERM"])process.on(signal,()=>{for(const child of children)try{if(process.platform==="win32")child.kill();else process.kill(-child.pid,"SIGTERM");}catch{}process.exitCode=1;});
(async()=>{console.log("Playback measurement: "+out);const seed=launch("seed",duration+7);await new Promise(r=>setTimeout(r,5000));const play=launch("play",duration);await Promise.all([seed,play]);analyze(read("seed"),read("play"));})()
.catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>{for(const child of children)try{if(process.platform==="win32")child.kill();else process.kill(-child.pid,"SIGTERM");}catch{}});
