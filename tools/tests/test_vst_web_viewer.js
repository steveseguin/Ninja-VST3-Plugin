#!/usr/bin/env node

/**
 * Test: VST Plugin -> Web Browser Viewer (Manual)
 *
 * This test helps verify that the VST plugin in seed mode works with
 * the actual VDO.Ninja web interface.
 *
 * Instructions:
 * 1. Run this script
 * 2. Open the provided URL in a web browser
 * 3. You should hear a 1kHz test tone
 * 4. Press Ctrl+C to stop
 */

const { spawn } = require('child_process');

const TONE_HZ = 1000;
const streamId = 'vst_web_test_' + Date.now().toString(36);

console.log('=== VST Plugin -> Web Viewer Test ===');
console.log('');
console.log('1. Starting VST plugin in seed mode...');
console.log('   Stream ID:', streamId);
console.log('   Publishing', TONE_HZ, 'Hz test tone');
console.log('');

const isWin = process.platform === "win32";
const cliPath = `build/${isWin ? "webrtc_vst_win" : "webrtc_vst_linux"}/bin/Release/webrtc_vst_cli_host${isWin ? ".exe" : ""}`;

const vst = spawn(cliPath, [], {
    env: {
        ...process.env,
        WEBRTC_VST_MODE: 'seed',
        WEBRTC_CLI_HOST_ITERATIONS: '100000',
        WEBRTC_VST_STREAM_ID: streamId,
        WEBRTC_VST_PASSWORD: 'false',
        WEBRTC_VST_HANDSHAKE_URL: 'wss://wss.vdo.ninja',
        WEBRTC_VST_LOG_STDOUT: '1',
        WEBRTC_VST_LOG_SIGNALING: '1',
        WEBRTC_CLI_HOST_WARMUP_MS: '2000',
        WEBRTC_CLI_HOST_TONE_HZ: TONE_HZ.toString()
    }
});

let connected = false;
let published = false;
let viewerConnected = false;

vst.stdout.on('data', data => {
    const text = data.toString().trim();
    console.log('[STDOUT]', text); // Debug output

    if (text.includes('Connected to VDO.Ninja') && !connected) {
        connected = true;
        console.log('✓ Connected to signaling server');
    }

    if (text.includes('Sent seed request') && !published) {
        published = true;
        console.log('✓ Stream published');
        console.log('');
        console.log('2. Open this URL in your web browser:');
        console.log('');
        console.log(`   https://vdo.ninja/?view=${streamId}&password=false`);
        console.log('');
        console.log(`   You should hear a ${TONE_HZ} Hz tone.`);
        console.log('');
        console.log('3. Press Ctrl+C to stop when done testing.');
        console.log('');
    }

    if (text.includes('offerSDP') && !viewerConnected) {
        viewerConnected = true;
        console.log('✓ Viewer connected!');
        console.log('✓ Sending audio stream...');
    }

    if (text.includes('ICE gathering complete')) {
        console.log('✓ Connection established');
    }
});

vst.stderr.on('data', data => {
    const text = data.toString().trim();
    console.log('[STDERR]', text); // Debug output
    if (text.includes('ERROR') && !text.includes('timed out')) {
        console.error('[ERROR]', text);
    }
});

vst.on('close', code => {
    console.log('');
    console.log('VST plugin stopped.');
    process.exit(code);
});

process.on('SIGINT', () => {
    console.log('');
    console.log('Stopping...');
    vst.kill();
});
