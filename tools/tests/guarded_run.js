#!/usr/bin/env node
"use strict";
// One owned validation workload at a time. Abort before starving this workstation.
const fs=require("node:fs"), path=require("node:path");
const {spawn,execFileSync}=require("node:child_process");
const root=path.resolve(__dirname,"../.."), lock=path.join(root,"build/test-tools/validation.lock");
const command=process.argv.slice(2);
if(!command.length) throw Error("Usage: node tools/tests/guarded_run.js <command> [args...]");
fs.mkdirSync(path.dirname(lock),{recursive:true});
try {fs.writeFileSync(lock,String(process.pid),{flag:"wx"});}
catch {throw Error("A validation lock exists. Check its owner before removing it: "+lock);}
let child, timer, watchdog, stopped=false, failure, peakRss=0, minHeadroom=100;
let ownedGroups=new Set();
let swapBaseline=null, requiredHeadroom=20;
const maxMiB=Number(process.env.WEBRTC_GUARD_MAX_MIB||1024);
function inspect() {
    const disk=fs.statfsSync(root), freeGiB=disk.bavail*disk.bsize/1073741824;
    if(freeGiB<8) throw Error(`Only ${freeGiB.toFixed(1)} GiB disk free (minimum 8)`);
    if(process.platform!=="darwin") return;
    const pressure=execFileSync("/usr/bin/memory_pressure",["-Q"],{encoding:"utf8",timeout:4000});
    const free=Number(pressure.match(/free percentage:\s*(\d+)/)?.[1]);
    minHeadroom=Math.min(minHeadroom,free);
    if(!Number.isFinite(free)||free<requiredHeadroom) throw Error(`Memory headroom ${free}% (minimum ${requiredHeadroom}%)`);
    const swap=execFileSync("/usr/sbin/sysctl",["-n","vm.swapusage"],{encoding:"utf8",timeout:4000});
    const usedSwap=Number(swap.match(/used = ([\d.]+)M/)?.[1]||0);
    if(swapBaseline===null) {
        // Closing applications can restore ample memory while cold pages remain
        // swapped. Accept that baseline only with substantially stricter limits.
        if(usedSwap>512 && (free<50||usedSwap>2048)) throw Error("Existing swap pressure requires ≥50% headroom and ≤2 GiB swap before starting");
        swapBaseline=usedSwap;
        if(usedSwap>512) requiredHeadroom=40;
    }
    if(usedSwap>Math.min(2048,Math.max(512,swapBaseline+128))) throw Error("Swap growth exceeds safe workload allowance");
    if(child) {
        const rows=execFileSync("/bin/ps",["-axo","pid=,ppid=,pgid=,rss="],{encoding:"utf8",timeout:4000})
            .trim().split("\n").map(line=>line.trim().split(/\s+/).map(Number));
        const owned=new Set([child.pid]);
        for(let n=0;n<12;n++) for(const [pid,ppid,pgid] of rows) if(owned.has(ppid)||pgid===child.pid) owned.add(pid);
        ownedGroups=new Set(rows.filter(([pid,,pgid])=>owned.has(pid)&&pid===pgid).map(([pid])=>pid));
        const rss=rows.reduce((sum,[pid,,,kb])=>sum+(owned.has(pid)?kb:0),0)/1024;
        peakRss=Math.max(peakRss,rss);
        if(rss>maxMiB) throw Error(`Owned workload RSS ${rss.toFixed(0)} MiB exceeds ${maxMiB}`);
    }
}
function stop(reason) {
    if(stopped) return; stopped=true; failure=reason;
    console.error("VALIDATION STOPPED: "+reason);
    if(child) {try {if(process.platform==="win32")child.kill("SIGTERM");else process.kill(-child.pid,"SIGTERM");}catch{}
        for(const group of ownedGroups)try{process.kill(-group,"SIGTERM");}catch{}
        setTimeout(()=>{try {if(process.platform==="win32")child.kill("SIGKILL");else process.kill(-child.pid,"SIGKILL");}catch{}},2000).unref();}
}
function cleanup() {clearInterval(timer);clearTimeout(watchdog);try{fs.unlinkSync(lock);}catch{}}
process.on("exit",cleanup);
for(const signal of ["SIGINT","SIGTERM"]) process.on(signal,()=>stop(signal));
try {
    inspect();
    console.log(`Guard enabled: single workload; ≥${requiredHeadroom}% memory headroom; ≥8 GiB disk; swap baseline ${swapBaseline??0} MiB with ≤128 MiB growth (512 MiB floor, 2 GiB ceiling); ≤${maxMiB} MiB owned RSS`);
    child=spawn(command[0],command.slice(1),{cwd:root,stdio:"inherit",detached:process.platform!=="win32"});
    child.on("error",error=>{console.error(error);process.exitCode=1;cleanup();});
    child.on("close",code=>{console.log(`Guard summary: peak owned RSS ${peakRss.toFixed(0)} MiB; minimum memory headroom ${minHeadroom}%`);process.exitCode=failure?1:(code??1);cleanup();});
    timer=setInterval(()=>{try{inspect();}catch(error){stop(error.message);}},5000);
    watchdog=setTimeout(()=>stop("Wall-clock timeout"),Number(process.env.WEBRTC_GUARD_SECONDS||240)*1000);
} catch(error) {console.error(error.message);process.exitCode=1;cleanup();}
