'use strict';
// Browser sequence: webrtc.js createDataChannel('sendChannel'), then viewer
// audio/video preferences, then the publisher's media offer. No SDK oracle.
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { WebSocketServer } = require('ws');
const rtc = require('@roamhq/wrtc');
const { cliExecutable, pluginBundle } = require('./build_paths');
const { assertSignaling, assertControl } = require('../helpers/assert-signaling.cjs');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check, label, timeout = 10000) {
    const end = Date.now() + timeout;
    while (!check()) {
        if (Date.now() > end) throw new Error('Timed out: ' + label);
        await delay(25);
    }
}

async function main() {
    const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
    await new Promise(resolve => server.once('listening', resolve));
    let pc = new rtc.RTCPeerConnection({ iceServers: [] });
    const restart = process.argv.includes('--restart');
    let cli, sdk, sourceTrack, tone, control, sink, ws, session, failure, chain = Promise.resolve();
    let offers = [], samples = 0, maxRms = 0, pong = false, log = '';
    const pending = [];
    const send = (message, channel) => {
        const packet = { ...message, session };
        if (!channel) packet.UUID = 'owned-viewer';
        else packet.UUID = 'owned-publisher'; // Browser sendRequest uses the destination UUID on a DC.
        (channel || ws).send(JSON.stringify(packet));
    };
    const receive = (message, channel) => {
        chain = chain.then(async () => {
            (channel ? assertControl : assertSignaling)(message);
            if (message.description) {
                const firstInSession = session !== message.session;
                if (restart && session && firstInSession) {
                    sink?.stop(); sink = null;
                    pc.onicecandidate = null;
                    pc.getReceivers().forEach(receiver => receiver.track?.stop());
                    pc.close();
                    pc = new rtc.RTCPeerConnection({ iceServers: [] });
                    pending.length = 0; control = null;
                    configurePeer();
                }
                session = message.session;
                const { sdp } = message.description;
                offers.push(sdp);
                if (firstInSession) {
                    assert.match(sdp, /^m=application /m);
                    assert.doesNotMatch(sdp, /^m=(audio|video) /m, 'Initial offer must be data-only');
                } else {
                    assert.match(sdp, /^m=audio /m);
                    assert.doesNotMatch(sdp, /^m=video /m, 'Audio plugin must not add video');
                }
                await pc.setRemoteDescription(message.description);
                for (const candidate of pending.splice(0)) await pc.addIceCandidate(candidate);
                const answer = await pc.createAnswer();
                await pc.setLocalDescription(answer);
                send({ description: { type: answer.type, sdp: answer.sdp } }, channel);
            }
            for (const candidate of message.candidates || []) {
                if (pc.remoteDescription) await pc.addIceCandidate(candidate);
                else pending.push(candidate);
            }
            if (message.pong === 1234) pong = true;
            if (message.ping !== undefined && channel) channel.send(JSON.stringify({ pong: message.ping }));
        }).catch(error => { failure = error; });
    };
    function configurePeer() {
    pc.onicecandidate = ({ candidate }) => {
        if (candidate) send({ candidates: [candidate.toJSON()], type: 'remote' }, control?.readyState === 'open' ? control : null);
    };
    pc.ondatachannel = ({ channel }) => {
        assert.equal(channel.label, 'sendChannel');
        control = channel;
        channel.onmessage = ({ data }) => receive(JSON.parse(data), channel);
    };
    pc.ontrack = ({ track }) => {
        assert.equal(track.kind, 'audio');
        sink = new rtc.nonstandard.RTCAudioSink(track);
        sink.ondata = ({ samples: pcm }) => {
            samples++;
            const rms = Math.sqrt(pcm.reduce((sum, value) => sum + (value / 32768) ** 2, 0) / pcm.length);
            maxRms = Math.max(maxRms, rms);
        };
    };
    }
    configurePeer();
    server.on('connection', socket => {
        ws = socket;
        socket.send(JSON.stringify({ UUID: 'owned-publisher' }));
        let requested = false;
        socket.on('message', raw => {
            const message = JSON.parse(raw);
            if (message.request === 'seed' && !requested) {
                requested = true;
                socket.send(JSON.stringify({ request: 'offerSDP', UUID: 'owned-viewer' }));
            } else if (message.description || message.candidates) receive(message);
        });
    });
    try {
        if (process.env.VDON_TEST_SDK) {
            Object.assign(global, rtc, { WebSocket: require('ws') });
            const SDK = require(process.env.VDON_TEST_SDK);
            sdk = new SDK({ host: `ws://127.0.0.1:${server.address().port}`, password: false,
                turnServers: false, stunServers: [], autoReconnect: false, debug: false });
            sdk._log = (...parts) => { log += parts.map(String).join(' ') + '\n'; };
            await sdk.connect();
            const source = new rtc.nonstandard.RTCAudioSource();
            sourceTrack = source.createTrack();
            let frame = 0;
            tone = setInterval(() => {
                const pcm = new Int16Array(480);
                for (let i = 0; i < pcm.length; i++) pcm[i] = Math.round(8000 * Math.sin(2 * Math.PI * 440 * frame++ / 48000));
                source.onData({ samples: pcm, sampleRate: 48000, bitsPerSample: 16, channelCount: 1, numberOfFrames: 480 });
            }, 10);
            await sdk.publish(new rtc.MediaStream([sourceTrack]), { streamID: 'owned_data_first' });
        } else {
            cli = spawn(cliExecutable, [pluginBundle], { env: { ...process.env,
            WEBRTC_VST_MODE: 'seed', WEBRTC_VST_STREAM_ID: 'owned-data-first', WEBRTC_VST_PASSWORD: '0',
            WEBRTC_VST_HANDSHAKE_URL: `ws://127.0.0.1:${server.address().port}`,
            WEBRTC_VST_DISABLE_STUN: '1', WEBRTC_VST_LOG_STDOUT: '1',
            WEBRTC_CLI_HOST_TONE_HZ: '440', WEBRTC_CLI_HOST_BLOCK_SLEEP_MS: '5',
            WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS: '20000', WEBRTC_CLI_HOST_TIMEOUT_MS: '25000'
        }, stdio: ['ignore', 'pipe', 'pipe'] });
            cli.stdout.on('data', data => { log += data; });
            cli.stderr.on('data', data => { log += data; });
        }
        await until(() => failure || control?.readyState === 'open', 'initial control channel');
        if (failure) throw failure;
        await delay(400);
        assert.equal(offers.length, 1);
        assert.equal(samples, 0);
        control.send(JSON.stringify({ audio: false, video: true }));
        await delay(400);
        assert.equal(offers.length, 1, 'video-only preferences must not enable audio');
        control.send(JSON.stringify({ ping: 1234 }));
        await until(() => failure || pong, 'publisher pong');
        if (failure) throw failure;
        control.send(JSON.stringify({ audio: true, video: true }));
        await until(() => failure || maxRms > 0.01, 'audio after preferences');
        if (failure) throw failure;
        assert.equal(offers.length, 2, 'One media renegotiation expected');
        control.send(JSON.stringify({ audio: false }));
        await delay(800); // Drain received audio and Opus concealment.
        maxRms = 0;
        await delay(800);
        assert.ok(maxRms < 0.001, `Audio continued after opt-out: ${maxRms}`);
        control.send(JSON.stringify({ audio: true }));
        await until(() => failure || maxRms > 0.01, 'audio resume');
        if (failure) throw failure;
        assert.equal(offers.length, 2, 'Mute/resume must reuse the negotiated sender');
        if (restart) for (let i = 0; i < 4; i++) {
            const previous = session, before = offers.length;
            send({ iceRestartRequest: true }, i % 2 === 0 ? control : null);
            await until(() => failure || (session !== previous && control?.readyState === 'open'), 'fresh publisher control connection');
            if (failure) throw failure;
            maxRms = 0;
            control.send(JSON.stringify({ audio: true, video: false }));
            await until(() => failure || maxRms > 0.01, 'audio after publisher restart');
            if (failure) throw failure;
            assert.equal(offers.length, before + 2, 'Restart must negotiate data before media');
            console.log('PASS: native publisher recovery via ' + (i % 2 === 0 ? 'data channel' : 'WebSocket'));
        }
        console.log(`PASS: ${sdk ? 'SDK' : 'native'} data-only offer, opt-in audio, video ignored, pong, mute/resume; RMS=${maxRms.toFixed(4)}`);
    } catch (error) {
        console.error(log.slice(-10000));
        throw error;
    } finally {
        sink?.stop();
        clearInterval(tone);
        sourceTrack?.stop();
        if (sdk) await sdk.disconnect();
        pc.getReceivers().forEach(receiver => receiver.track?.stop());
        pc.close();
        if (cli && cli.exitCode === null) cli.kill();
        for (const socket of server.clients) socket.terminate();
        await new Promise(resolve => server.close(resolve));
    }
}
main().then(() => process.exit(0), error => { console.error(error); process.exit(1); });
