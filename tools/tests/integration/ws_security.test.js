#!/usr/bin/env node
"use strict";

const assert = require("assert");
const { once } = require("events");
const WebSocket = require("ws");

// All traffic stays on loopback. The fragment check sends fewer than 150 KB;
// it verifies the limit without attempting to exhaust memory.
const watchdog = setTimeout(() => {
    console.error("[FAIL] WebSocket security checks timed out");
    process.exit(1);
}, 15000);

async function withConnection(check) {
    const server = new WebSocket.Server({ host: "127.0.0.1", port: 0, perMessageDeflate: true });
    let client;
    try {
        await once(server, "listening");
        const connected = once(server, "connection");
        client = new WebSocket("ws://127.0.0.1:" + server.address().port);
        const opened = once(client, "open");
        const [peer] = await connected;
        await opened;
        await check(client, peer);
    } finally {
        if (client) client.terminate();
        for (const peer of server.clients) peer.terminate();
        await new Promise((resolve) => server.close(resolve));
    }
}

async function main() {
    await withConnection(async (client, peer) => {
        const payload = JSON.stringify({
            request: "play", audio: true, video: false,
            description: "a=candidate:loopback\r\n".repeat(2048)
        });
        const received = once(peer, "message");
        const midpoint = Math.floor(payload.length / 2);
        client.send(payload.slice(0, midpoint), { fin: false, compress: true });
        client.send(payload.slice(midpoint), { fin: true, compress: true });
        const [data, isBinary] = await received;
        assert.strictEqual(isBinary, false);
        assert.strictEqual(data.toString(), payload);

        const reply = once(client, "message");
        peer.send(data.toString());
        assert.strictEqual((await reply)[0].toString(), payload);
        const closed = once(peer, "close");
        client.close(1000, "Finished");
        const [code, reason] = await closed;
        assert.strictEqual(code, 1000);
        assert.strictEqual(reason.toString(), "Finished");
    });
    console.log("[PASS] Compressed, fragmented signaling and normal close remain compatible");

    await withConnection(async (client, peer) => {
        const closed = once(peer, "close");
        client.close(1000, Buffer.from("Finished"));
        const [code, reason] = await closed;
        assert.strictEqual(code, 1000);
        assert.strictEqual(reason.toString(), "Finished");
    });
    console.log("[PASS] Buffer close reasons remain compatible");

    await withConnection(async (client) => {
        assert.throws(() => client.close(1000, new Float32Array(20)), TypeError);
    });
    console.log("[PASS] Unsafe TypedArray close reason is rejected");

    // Check both directions with default limits, including empty fragments.
    for (const rejectAtServer of [true, false]) {
        await withConnection(async (client, peer) => {
            const sender = rejectAtServer ? client : peer;
            const receiver = rejectAtServer ? peer : client;
            const rejected = once(receiver, "error");
            const closed = once(sender, "close");
            for (let i = 0; i <= 16384; ++i) {
                sender.send(i % 2 ? "x" : "", { fin: false, compress: false });
            }
            const [error] = await rejected;
            assert.strictEqual(error.code, "WS_ERR_TOO_MANY_BUFFERED_PARTS");
            assert.strictEqual((await closed)[0], 1008);
        });
    }
    console.log("[PASS] Default client/server fragment limits reject excessive fragments");

    // Use a small explicit chunk limit to check the other retained-parts guard
    // without relying on operating-system TCP packet coalescing.
    const receiver = new WebSocket.Receiver({ maxBufferedChunks: 4 });
    try {
        const rejected = once(receiver, "error");
        receiver.write(Buffer.from([0x82, 32]));
        for (let i = 0; i < 5; ++i) receiver.write(Buffer.from([0]));
        const [error] = await rejected;
        assert.strictEqual(error.code, "WS_ERR_TOO_MANY_BUFFERED_PARTS");
    } finally {
        receiver.destroy();
    }
    console.log("[PASS] Buffered chunk limit rejects excessive retained chunks");
}

main().catch((error) => {
    console.error("[FAIL]", error);
    process.exitCode = 1;
}).finally(() => clearTimeout(watchdog));
