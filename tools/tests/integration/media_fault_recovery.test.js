#!/usr/bin/env node
"use strict";
// Fault only owned synthetic endpoints. Rewrite their host ICE candidates to
// local UDP relays; SRTP remains encrypted and system networking is unchanged.
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const dgram = require("node:dgram");
const fs = require("node:fs");
const path = require("node:path");
const {spawn} = require("node:child_process");
const {once} = require("node:events");
const {WebSocket, WebSocketServer} = require("ws");
const {rootDir, cliExecutable, pluginBundle} = require("./build_paths");

const directory = path.join(rootDir, "build/test-tools/media-faults-" + Date.now());
fs.mkdirSync(directory, {recursive:true});
const stream = "ownedfault" + crypto.randomBytes(8).toString("hex");
const relays = new Map(), children = new Set(), timers = new Set(), upstreams = new Set();
const windows = [], counts = {};
let server, mediaStart, failure, stopped = false;
const phase = () => {
    const seconds = mediaStart ? (Date.now() - mediaStart) / 1000 : 0;
    return seconds < 8 ? "clean" : seconds < 14 ? "loss5pct" : seconds < 20 ? "jitter15ms" : seconds < 23 ? "outage3s" : "recovery";
};
function later(fn, ms) {
    const timer = setTimeout(() => {timers.delete(timer); if (!stopped) fn();}, ms);
    timers.add(timer);
}
function fail(error) { failure ||= error; for (const child of children) child.kill(); }

async function candidate(text) {
    const fields = text.trim().split(/\s+/);
    if (fields.length < 8 || fields[2].toLowerCase() !== "udp" || fields[4].includes(":")) return null;
    const address = fields[4], port = Number(fields[5]), key = address + ":" + port;
    // Both endpoints are local. libjuice may answer a host-address packet from
    // loopback after its same-machine address translation; keep this symmetric.
    let entry = relays.get(key);
    if (!entry) {
        const socket = dgram.createSocket("udp4");
        entry = {socket, remote:null, packets:0}; relays.set(key, entry);
        socket.on("error", fail);
        socket.on("message", (data, source) => {
            const fromTarget = (source.address === address || source.address === "127.0.0.1") && source.port === port;
            if (!fromTarget) entry.remote = source;
            const target = fromTarget ? entry.remote : {address:"127.0.0.1", port};
            if (!target || stopped) return;
            // Leave STUN, DTLS and RTCP alone. Fault encrypted RTP media only.
            const media = data.length >= 12 && (data[0] & 0xc0) === 0x80 && (data[1] < 192 || data[1] > 223);
            let delay = 0;
            if (media) {
                mediaStart ||= Date.now();
                const name = phase(), stats = counts[name] ||= {received:0, dropped:0, delayed:0};
                stats.received++; entry.packets++;
                if (name === "outage3s" || (name === "loss5pct" && stats.received % 20 === 0)) {stats.dropped++; return;}
                if (name === "jitter15ms") {delay = entry.packets * 7 % 16; stats.delayed++;}
            }
            const send = () => socket.send(data, target.port, target.address, error => {if (error && !stopped) fail(error);});
            if (delay) later(send, delay); else send();
        });
        socket.bind(0, "127.0.0.1"); await once(socket, "listening");
    }
    fields[4] = "127.0.0.1"; fields[5] = String(entry.socket.address().port);
    // Do not expose original related-address candidates that could bypass us.
    const related = fields.indexOf("raddr");
    if (related >= 0) fields.splice(related, 4);
    return fields.join(" ");
}
async function rewrite(message) {
    if (message.description?.sdp) {
        const lines = [];
        for (const line of message.description.sdp.split("\r\n")) {
            if (line.startsWith("a=candidate:")) {
                const replaced = await candidate(line.slice(2));
                if (replaced) lines.push("a=" + replaced);
            } else lines.push(line);
        }
        message.description.sdp = lines.join("\r\n");
    }
    if (Array.isArray(message.candidates)) {
        const entries = [];
        for (const item of message.candidates) {
            const replaced = await candidate(item.candidate);
            if (replaced) entries.push({...item, candidate:replaced});
        }
        message.candidates = entries;
    }
    if (typeof message.candidate === "string") message.candidate = await candidate(message.candidate) || "";
    return message;
}
function launch(mode, seconds) {
    const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/^WEBRTC_(VST|CLI_HOST)_/.test(key)));
    Object.assign(env, {
        WEBRTC_VST_MODE:mode, WEBRTC_VST_STREAM_ID:stream, WEBRTC_VST_PASSWORD:"off",
        WEBRTC_VST_HANDSHAKE_URL:`ws://127.0.0.1:${server.address().port}/`, WEBRTC_VST_DISABLE_STUN:"1",
        WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS:String(seconds * 1000), WEBRTC_CLI_HOST_TIMEOUT_MS:String((seconds + 5) * 1000),
        WEBRTC_CLI_HOST_REALTIME_PACING:"1", WEBRTC_CLI_HOST_CONTINUOUS_WARMUP:"1", WEBRTC_CLI_HOST_QUALITY:"1",
        WEBRTC_CLI_HOST_TONE_HZ:"1000", WEBRTC_CLI_HOST_RIGHT_TONE_HZ:"1500"
    });
    const child = spawn(cliExecutable, [pluginBundle], {env, stdio:["ignore","pipe","pipe"]});
    children.add(child);
    const log = fs.createWriteStream(path.join(directory, mode + ".log"));
    let pending = "";
    child.stdout.on("data", data => {
        log.write(data); pending += data;
        const lines = pending.split("\n"); pending = lines.pop();
        if (mode === "play") for (const line of lines) if (line.startsWith("[quality]")) {
            windows.push({...Object.fromEntries([...line.matchAll(/(\w+)=([\d.eE+-]+)/g)].map(([,k,v]) => [k, Number(v)])),
                mediaSeconds:mediaStart ? (Date.now() - mediaStart) / 1000 : 0, phase:phase()});
        }
    });
    child.stderr.on("data", data => log.write(data));
    return once(child,"close").then(async ([code]) => {
        children.delete(child); await new Promise(resolve => log.end(resolve)); assert.equal(code,0,mode + " failed");
    });
}
for (const signal of ["SIGINT","SIGTERM"]) process.on(signal, () => fail(new Error(signal)));
(async () => {
    console.log("Media fault test:", directory);
    server = new WebSocketServer({host:"127.0.0.1",port:0}); await once(server,"listening");
    server.on("connection", socket => {
        const upstream = new WebSocket("wss://apibackup.vdo.ninja/"); upstreams.add(upstream);
        const opened = once(upstream,"open"); opened.catch(fail);
        let chain = Promise.resolve();
        socket.on("message", raw => {
            chain = chain.then(async () => {
                const message = await rewrite(JSON.parse(raw.toString())); await opened;
                if (upstream.readyState === WebSocket.OPEN) upstream.send(JSON.stringify(message));
            }).catch(fail);
        });
        upstream.on("message", data => {if (socket.readyState === WebSocket.OPEN) socket.send(data.toString());});
        upstream.on("error", fail); socket.on("error", fail);
        upstream.on("close", () => {upstreams.delete(upstream); socket.close();});
        socket.on("close", () => upstream.close());
    });
    const seed = launch("seed", 42); seed.catch(fail);
    await new Promise(resolve => setTimeout(resolve, 3000));
    await Promise.all([seed, launch("play", 36)]);
    if (failure) throw failure;
    for (const name of ["clean","loss5pct","jitter15ms","outage3s","recovery"])
        assert.ok(counts[name]?.received > 50, "Media did not traverse relay phase " + name);
    assert.ok(counts.loss5pct.dropped > 5 && counts.jitter15ms.delayed > 50 && counts.outage3s.dropped > 50);
    for (const row of windows) {assert.equal(row.nonfinite,0); assert.ok(row.rms_l < .4 && row.rms_r < .4);}
    const clean = windows.filter(row => row.mediaSeconds > 4 && row.mediaSeconds < 8);
    assert.ok(clean.length >= 2 && clean.every(row => row.rms_l > .08 && row.rms_r > .08), "Missing clean baseline");
    const baseline = ["rms_l", "rms_r"].map(key => clean.reduce((sum,row) => sum + row[key],0) / clean.length);
    // Opus PLC retains comfort noise during a long outage; require a measured
    // level reduction rather than incorrectly demanding digital silence.
    assert.ok(windows.some(row => row.mediaSeconds > 21 && row.mediaSeconds < 23 &&
        row.rms_l < baseline[0] * .5 && row.rms_r < baseline[1] * .5),
        "Outage did not affect captured audio: possible relay bypass");
    const recovered = windows.filter(row => row.mediaSeconds > 28);
    assert.ok(recovered.length >= 5, "Insufficient post-outage recovery audio");
    for (const row of recovered) {
        assert.equal(row.silent_blocks,0,"Post-outage silence");
        assert.ok(row.rms_l > .08 && row.rms_r > .08,"Post-outage level failure");
        assert.ok(Math.abs(row.hz_l - 1000) < 40 && Math.abs(row.hz_r - 1500) < 40,"Post-outage pitch failure");
    }
    console.log("PASS: actual relayed SRTP loss/jitter/outage recovery", JSON.stringify(counts));
})().catch(error => {failure = error; console.error(error); process.exitCode = 1;}).finally(async () => {
    stopped = true;
    for (const timer of timers) clearTimeout(timer);
    for (const child of children) child.kill();
    for (const upstream of upstreams) upstream.terminate();
    for (const {socket} of relays.values()) socket.close();
    if (server) {for (const socket of server.clients) socket.terminate(); await new Promise(resolve => server.close(resolve));}
    fs.writeFileSync(path.join(directory,"results.json"), JSON.stringify({passed:!failure, error:failure?.message, counts, windows},null,2));
});
