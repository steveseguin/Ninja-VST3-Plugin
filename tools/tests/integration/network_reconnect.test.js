'use strict';
// Owned endpoints only. Every ICE path passes through a UDP proxy which can
// drop STUN, DTLS, SCTP and SRTP together. No system firewall/network changes.
const assert = require('node:assert/strict');
const dgram = require('node:dgram');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { once } = require('node:events');
const { spawn } = require('node:child_process');
const { WebSocketServer, WebSocket } = require('ws');
const rtc = require('@roamhq/wrtc');
const { cliExecutable, pluginBundle, rootDir } = require('./build_paths');
const { assertSignaling, assertControl } = require('../helpers/assert-signaling.cjs');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const noPong = process.argv.includes('--no-pong');
const delayedOffer = process.argv.includes('--connect-timeout');
const encryptedRoom = process.argv.includes('--encrypted-room');
const password = 'owned-reconnect-password';
const key = crypto.createHash('sha256').update(password + 'vdo.ninja').digest();
function encode(message) {
    if (!encryptedRoom || (!message.description && !message.candidates)) return message;
    const iv = crypto.randomBytes(16), result = { ...message, vector: iv.toString('hex') };
    for (const field of ['description', 'candidates']) if (message[field]) {
        const cipher = crypto.createCipheriv('aes-256-cbc', key, iv);
        result[field] = Buffer.concat([cipher.update(JSON.stringify(message[field])), cipher.final()]).toString('hex');
    }
    return result;
}
function decode(message) {
    if (!message.vector) return message;
    const result = { ...message };
    for (const field of ['description', 'candidates']) if (typeof message[field] === 'string') {
        const cipher = crypto.createDecipheriv('aes-256-cbc', key, Buffer.from(message.vector, 'hex'));
        result[field] = JSON.parse(Buffer.concat([cipher.update(Buffer.from(message[field], 'hex')), cipher.final()]).toString());
    }
    delete result.vector;
    return result;
}

async function main() {
    const directory = path.join(rootDir, 'build/test-tools/network-reconnect-' + Date.now());
    fs.mkdirSync(directory, { recursive: true });
    const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
    await once(server, 'listening');
    let ws, cli, current, failure, stopped = false, sourceOnline = true, signalingOnline = true;
    let generation = 0, plays = 0, connections = 0, log = '', partial = '', phase = 'initial', wireStream, wireRoom;
    const peers = [], rows = [], results = [];
    const source = new rtc.nonstandard.RTCAudioSource();
    const track = source.createTrack();
    let sample = 0;
    const tone = setInterval(() => {
        const samples = new Int16Array(480);
        for (let i = 0; i < samples.length; i++) samples[i] = Math.round(8000 * Math.sin(2 * Math.PI * 440 * sample++ / 48000));
        source.onData({ samples, sampleRate: 48000, bitsPerSample: 16, channelCount: 1, numberOfFrames: 480 });
    }, 10);
    function fail(error) { failure ||= error; }
    async function until(check, label, timeout = 15000) {
        const start = Date.now();
        while (!check()) {
            if (failure) throw failure;
            if (Date.now() - start > timeout) throw new Error('Timed out: ' + label);
            await wait(40);
        }
        return Date.now() - start;
    }
    function send(peer, message, dc = false) {
        if (stopped || peer !== current) return;
        const packet = { ...message, session: peer.session };
        if (!dc) packet.UUID = 'owned-publisher';
        const transport = dc ? peer.dc : ws;
        if (transport?.readyState === 'open' || transport?.readyState === WebSocket.OPEN)
            transport.send(JSON.stringify(encode(packet)));
    }
    async function candidate(peer, text) {
        const fields = text.trim().split(/\s+/);
        if (fields.length < 8 || fields[2].toLowerCase() !== 'udp' || fields[4].includes(':')) return null;
        const address = fields[4], port = Number(fields[5]), key = address + ':' + port;
        let relay = peer.relays.get(key);
        if (!relay) {
            const socket = dgram.createSocket('udp4');
            relay = { socket, remote: null };
            peer.relays.set(key, relay);
            socket.on('error', fail);
            socket.on('message', (data, remote) => {
                peer.packets++;
                if (peer.blocked) { peer.dropped++; return; }
                const fromTarget = remote.port === port && (remote.address === address || remote.address === '127.0.0.1');
                if (!fromTarget) relay.remote = remote;
                const target = fromTarget ? relay.remote : { address: '127.0.0.1', port };
                if (target && !stopped) socket.send(data, target.port, target.address, error => { if (error && !stopped) fail(error); });
            });
            socket.bind(0, peer.address);
            await once(socket, 'listening');
        }
        fields[4] = peer.address;
        fields[5] = String(relay.socket.address().port);
        const related = fields.indexOf('raddr');
        if (related >= 0) fields.splice(related, 4);
        return fields.join(' ');
    }
    async function rewrite(peer, message) {
        if (message.description) {
            const lines = [];
            for (const line of message.description.sdp.split('\r\n')) {
                if (line.startsWith('a=candidate:')) {
                    const text = await candidate(peer, line.slice(2));
                    if (text) lines.push('a=' + text);
                } else lines.push(line);
            }
            message.description.sdp = lines.join('\r\n');
        }
        if (message.candidates) {
            const list = [];
            for (const item of message.candidates) {
                const text = await candidate(peer, item.candidate);
                if (text) list.push({ ...item, candidate: text });
            }
            message.candidates = list;
        }
        return message;
    }
    function enqueue(peer, work) { peer.chain = peer.chain.then(work).catch(fail); }
    async function receive(peer, message, dc) {
        (dc ? assertControl : assertSignaling)(message);
        message = decode(message);
        if (peer !== current) return;
        if (message.session && message.session !== peer.session) return;
        if (message.ping !== undefined) { if (!noPong) send(peer, { pong: message.ping }, true); return; }
        if (message.audio === true && !peer.media) {
            peer.media = true;
            peer.pc.addTrack(track);
            const offer = await peer.pc.createOffer();
            await peer.pc.setLocalDescription(offer);
            send(peer, await rewrite(peer, { description: { type: offer.type, sdp: offer.sdp } }), true);
        }
        if (message.description) {
            await rewrite(peer, message);
            await peer.pc.setRemoteDescription(message.description);
            for (const entry of peer.pending.splice(0)) await peer.pc.addIceCandidate(entry);
        }
        for (const entry of (await rewrite(peer, { candidates: message.candidates || [] })).candidates) {
            if (peer.pc.remoteDescription) await peer.pc.addIceCandidate(entry);
            else peer.pending.push(entry);
        }
    }
    async function publish() {
        const old = current;
        if (old && !old.blocked && old.pc.connectionState !== 'failed' && old.pc.connectionState !== 'closed') return;
        // Healthy signaling reconnects should never ask for a duplicate peer.
        if (old) old.pc.close();
        const peer = { pc: new rtc.RTCPeerConnection({ iceServers: [] }), session: 'owned-' + ++generation,
            address: generation % 2 ? '127.0.0.1' : '127.0.0.2', relays: new Map(), pending: [],
            blocked: false, dropped: 0, packets: 0, chain: Promise.resolve(), media: false };
        peers.push(peer); current = peer;
        if (delayedOffer && generation === 1) peer.blocked = true;
        peer.dc = peer.pc.createDataChannel('sendChannel');
        peer.dc.onmessage = ({ data }) => enqueue(peer, () => receive(peer, JSON.parse(data), true));
        peer.pc.onicecandidate = ({ candidate: entry }) => {
            if (entry) enqueue(peer, async () => send(peer, await rewrite(peer, { candidates: [entry.toJSON()], type: 'local' })));
        };
        const offer = await peer.pc.createOffer();
        await peer.pc.setLocalDescription(offer);
        peer.firstOffer = await rewrite(peer, { description: { type: offer.type, sdp: offer.sdp } });
        send(peer, peer.firstOffer);
    }
    server.on('connection', socket => {
        connections++;
        if (!signalingOnline) { socket.close(); return; }
        ws = socket;
        socket.send(JSON.stringify({ id: 'owned-viewer-' + connections }));
        socket.on('message', raw => {
            try {
                const message = JSON.parse(raw);
                assertSignaling(message);
                if (message.request === 'joinroom') {
                    assert.ok(encryptedRoom);
                    wireRoom ||= message.roomid;
                    assert.equal(message.roomid, wireRoom, 'Room changed across reconnect');
                    assert.notEqual(wireRoom, 'owned-room');
                    socket.send(JSON.stringify({ request: 'listing', list: [] }));
                } else if (message.request === 'play') {
                    wireStream ||= message.streamID;
                    assert.equal(message.streamID, wireStream, 'Stream/password changed across reconnect');
                    if (encryptedRoom) assert.notEqual(wireStream, 'owned_reconnect');
                    plays++;
                    if (sourceOnline) publish().catch(fail);
                } else if (current) enqueue(current, () => receive(current, message, false));
            } catch (error) { fail(error); }
        });
    });
    async function audioAfter(start, label, count = 2) {
        return until(() => rows.filter(row => row.at > start && row.rms_l > .04 && row.nonfinite === 0).length >= count, label);
    }
    async function changePath(label, combined = false) {
        phase = label;
        const old = current, start = Date.now(), before = generation;
        old.blocked = true;
        if (combined) {
            signalingOnline = false; ws.close();
            await wait(6500);
            signalingOnline = true;
        }
        await until(() => generation > before, label + ' new generation', noPong ? 60000 : 20000);
        await audioAfter(Date.now(), label + ' audio');
        assert.notEqual(current.address, old.address, 'Recovery did not change source IP');
        assert.ok(old.dropped > 20, 'Blackout did not hit real packets');
        results.push({ phase, milliseconds: Date.now() - start, dropped: old.dropped });
        console.log('PASS:', JSON.stringify(results.at(-1)));
    }
    try {
        console.log('Network recovery logs:', directory);
        cli = spawn(cliExecutable, [pluginBundle], { env: { ...process.env,
            WEBRTC_VST_MODE: 'play', WEBRTC_VST_STREAM_ID: 'owned-reconnect', WEBRTC_VST_PASSWORD: encryptedRoom ? password : '0',
            WEBRTC_VST_ROOM_NAME: encryptedRoom ? 'owned-room' : '',
            WEBRTC_VST_HANDSHAKE_URL: `ws://127.0.0.1:${server.address().port}`,
            WEBRTC_VST_DISABLE_STUN: '1', WEBRTC_VST_LOG_STDOUT: '1', WEBRTC_VST_LOG_SIGNALING: '1',
            WEBRTC_CLI_HOST_REALTIME_PACING: '1', WEBRTC_CLI_HOST_CONTINUOUS_WARMUP: '1', WEBRTC_CLI_HOST_QUALITY: '1',
            WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: '240000', WEBRTC_CLI_HOST_TIMEOUT_MS: '245000'
        }, stdio: ['ignore', 'pipe', 'pipe'] });
        cli.on('error', fail);
        cli.stdout.on('data', data => {
            log += data; partial += data;
            const lines = partial.split('\n'); partial = lines.pop();
            for (const line of lines) if (line.startsWith('[quality]')) rows.push({ at: Date.now(), phase,
                ...Object.fromEntries([...line.matchAll(/(\w+)=([\d.eE+-]+)/g)].map(([, k, v]) => [k, Number(v)])) });
        });
        cli.stderr.on('data', data => { log += data; });
        if (delayedOffer) {
            await until(() => generation > 1, 'initial connect deadline retry', 30000);
            await audioAfter(Date.now(), 'audio after initial connection timeout');
            assert.ok(log.includes('(connect timeout)'));
            console.log('PASS: stalled initial connection retired and playback recovered');
            return;
        }
        await audioAfter(Date.now(), 'baseline audio');
        assert.equal(generation, 1);
        if (noPong) {
            await wait(12000);
            assert.equal(generation, 1, 'Older app disconnected for ignoring ping');
            await changePath('older-app-without-pong');
            console.log('PASS: older app without pong stayed connected, then recovered using ICE state');
            return;
        }
        phase = 'transient-two-second-outage';
        current.blocked = true; await wait(2000); current.blocked = false;
        await audioAfter(Date.now(), 'transient recovery');
        assert.equal(generation, 1, 'Short outage unnecessarily replaced healthy peer');
        console.log('PASS: two-second complete UDP outage recovered on the same peer');
        phase = 'signaling-only';
        const beforePlays = plays, beforeConnections = connections, before = Date.now();
        ws.close();
        await until(() => connections > beforeConnections, 'signaling reconnect');
        await audioAfter(before, 'audio during signaling outage', 3);
        assert.equal(plays, beforePlays, 'Healthy media replaced on signaling reconnect');
        assert.equal(generation, 1);
        console.log('PASS: signaling reconnect preserved media');
        for (let i = 1; i <= 4; i++) await changePath('IP-change-' + i);
        await changePath('combined-signaling-media-outage', true);
        phase = 'publisher-offline';
        sourceOnline = false; current.blocked = true;
        const offlinePlays = plays;
        await until(() => plays > offlinePlays, 'first offline retry', 20000);
        await until(() => plays > offlinePlays + 1, 'queued offline retry', 20000);
        sourceOnline = true;
        await audioAfter(Date.now(), 'source returned without announcement');
        console.log('PASS: unavailable publisher retried and recovered');
        phase = 'stale-offer';
        const stale = peers[0];
        ws.send(JSON.stringify(encode({ ...stale.firstOffer, UUID: 'owned-publisher', session: stale.session })));
        await audioAfter(Date.now(), 'audio after stale offer');
        assert.ok(log.includes('Ignoring retired peer session'), 'Stale generation was not rejected');
        const beforeStop = plays;
        cli.kill(); await once(cli, 'close'); await wait(2300);
        assert.equal(plays, beforeStop, 'Retry continued after stop');
        if (failure) throw failure;
        console.log(`PASS: ${generation} peer generations, ${plays} play requests, repeated recovery and stop cancellation`);
    } catch (error) {
        failure ||= error;
        console.error(log.slice(-5000));
        throw error;
    } finally {
        stopped = true; clearInterval(tone); track.stop();
        if (cli?.exitCode === null) cli.kill();
        for (const peer of peers) { peer.pc.close(); for (const { socket } of peer.relays.values()) socket.close(); }
        for (const socket of server.clients) socket.terminate();
        await new Promise(resolve => server.close(resolve));
        fs.writeFileSync(path.join(directory, 'plugin.log'), log);
        fs.writeFileSync(path.join(directory, 'results.json'), JSON.stringify({ passed: !failure, error: failure?.message,
            generation, plays, connections, results, rows }, null, 2));
    }
}
main().then(() => process.exit(0), error => { console.error(error); process.exit(1); });
