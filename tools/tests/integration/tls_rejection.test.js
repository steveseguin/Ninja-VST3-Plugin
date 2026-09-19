#!/usr/bin/env node
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const https = require("node:https");
const { spawn, spawnSync } = require("node:child_process");
const { once } = require("node:events");
const { WebSocketServer } = require("ws");
const { cliExecutable, pluginBundle } = require("./build_paths");
(async () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "vst-owned-tls-"));
    let server, sockets, child, timer;
    try {
        const key = path.join(directory,"key.pem"), cert = path.join(directory,"cert.pem");
        const generated = spawnSync("openssl",["req","-x509","-newkey","rsa:2048","-nodes","-days","1",
            "-subj","/CN=localhost","-keyout",key,"-out",cert],{encoding:"utf8"});
        assert.equal(generated.status,0,generated.stderr);
        server = https.createServer({key:fs.readFileSync(key),cert:fs.readFileSync(cert)});
        sockets = new WebSocketServer({server});
        let accepted=0, attempted=0, logs="";
        sockets.on("connection",()=>++accepted);
        server.on("tlsClientError",()=>++attempted);
        server.listen(0,"127.0.0.1"); await once(server,"listening");
        const env = Object.fromEntries(Object.entries(process.env).filter(([name])=>!/^WEBRTC_(VST|CLI_HOST)_/.test(name)));
        Object.assign(env,{WEBRTC_VST_MODE:"play",WEBRTC_VST_STREAM_ID:"ownedtls",WEBRTC_VST_LOG_STDOUT:"1",
            WEBRTC_VST_HANDSHAKE_URL:`wss://127.0.0.1:${server.address().port}/`,WEBRTC_VST_DISABLE_STUN:"1",
            WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS:"5000",WEBRTC_CLI_HOST_TIMEOUT_MS:"9000",WEBRTC_CLI_HOST_REALTIME_PACING:"1"});
        child=spawn(cliExecutable,[pluginBundle],{env,stdio:["ignore","pipe","pipe"]});
        timer=setTimeout(()=>child.kill("SIGKILL"),12000);
        for(const stream of [child.stdout,child.stderr]) stream.on("data",data=>logs+=data);
        const [code]=await once(child,"close");
        assert.equal(code,0,logs);
        assert.equal(accepted,0,"Plugin accepted an untrusted TLS certificate");
        assert.ok(attempted > 0,"No TLS connection attempt reached owned fixture\n" + logs);
        assert.match(logs,/certificate|SSL|TLS|Signaling error/i,"Expected explicit connection failure\n" + logs);
        assert.ok(!logs.includes("Connected to VDO.Ninja signaling server"),"Untrusted endpoint connected");
        console.log("PASS: self-signed loopback TLS rejected; no trust-store changes or public fallback");
    } finally {
        clearTimeout(timer);
        if(child && child.exitCode === null) child.kill();
        if(sockets) {for(const socket of sockets.clients) socket.terminate(); sockets.close();}
        if(server) await new Promise(resolve=>server.close(resolve));
        fs.rmSync(directory,{recursive:true,force:true}); // Only this test's mkdtemp artifact.
    }
})().catch(error=>{console.error(error);process.exitCode=1;});
