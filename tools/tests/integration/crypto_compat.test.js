#!/usr/bin/env node
"use strict";
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const SDK = require("../../../js_sdk/vdoninja-sdk-node.js");
(async () => {
    const sdk = new SDK({password:"test &é+pass"});
    try {
        assert.equal(sdk.password,encodeURIComponent("test &é+pass"));
        for(const salt of ["vdo.ninja","SALT","mix &+/=?#-é🔊"]) {
            sdk.salt=salt;
            const phrase=sdk.password+salt;
            // Independent encoding implementation, matching web client charCodeAt.
            const bytes=Buffer.from(Array.from({length:phrase.length},(_,i)=>phrase.charCodeAt(i)&255));
            const key=crypto.createHash("sha256").update(bytes).digest();
            const message=JSON.stringify({description:{type:"offer",sdp:"v=0\r\n"}});
            const [encrypted,iv]=await sdk._encryptMessage(message);
            const decipher=crypto.createDecipheriv("aes-256-cbc",key,Buffer.from(iv,"hex"));
            assert.equal(Buffer.concat([decipher.update(Buffer.from(encrypted,"hex")),decipher.final()]).toString(),message);
            assert.equal(await sdk._decryptMessage(encrypted,iv),message);
        }
        sdk.state.connected=true;
        const roomMessages=[];
        sdk._sendMessageWS=message=>{roomMessages.push(message); setImmediate(()=>sdk._emit('_roomJoined',{}));};
        await sdk.joinRoom({room:"ownedunicode"});
        assert.equal(sdk.password,encodeURIComponent("test &é+pass"),"Room join re-encoded the existing password");
        const firstRoom=roomMessages[0].roomid;
        sdk.state.roomJoined=false;
        await sdk._restoreConnectionIntent();
        assert.equal(roomMessages[1].roomid,firstRoom,"Room reconnect changed the password hash");
        assert.equal(sdk.password,encodeURIComponent("test &é+pass"));
        console.log("PASS: independent ASCII/Unicode browser AES vectors, password escaping and SDK decryption");
    } finally {await sdk.disconnect();}
})().catch(error=>{console.error(error);process.exitCode=1;});
