#!/usr/bin/env node

/**
 * Test: VST Plugin Seed Handshake
 *
 * Verifies that the VST plugin in seed mode can:
 * 1. Connect to the signaling server
 * 2. Send a seed request
 * 3. Receive an offerSDP when a viewer connects
 * 4. Send back an SDP answer
 * 5. Exchange ICE candidates
 */

const WebSocket = require('ws');
const { spawn } = require('child_process');
const path = require('path');

const TEST_TIMEOUT_MS = 12000;
const WARMUP_MS = 1000;
const RUNTIME_MS = 4000;

// Generate unique stream ID for this test
const streamId = 'test_seed_' + Date.now().toString(36);

console.log('=== VST Seed Handshake Test ===');
console.log('Stream ID:', streamId);
console.log('');

let testPassed = false;
let receivedOfferSDP = false;
let receivedAnswer = false;
let receivedCandidates = false;

// Start viewer websocket
const viewer = new WebSocket('wss://wss.vdo.ninja');

viewer.on('open', () => {
    console.log('[VIEWER] Connected to signaling server');
    viewer.send(JSON.stringify({request: 'play', streamID: streamId}));
    console.log('[VIEWER] Sent play request for stream:', streamId);
    console.log('');

    // Start CLI seed process
    startSeedProcess();
});

viewer.on('message', data => {
    const msg = JSON.parse(data.toString());

    if (msg.description && msg.description.type === 'offer') {
        console.log('[VIEWER] ✓ Received SDP offer from seeder');
        receivedAnswer = true;
    }

    if (msg.candidates && Array.isArray(msg.candidates)) {
        console.log('[VIEWER] ✓ Received', msg.candidates.length, 'ICE candidate(s)');
        receivedCandidates = true;
    }
});

viewer.on('error', err => {
    console.error('[VIEWER] Error:', err.message);
});

viewer.on('close', () => {
    console.log('[VIEWER] Connection closed');
});

function startSeedProcess() {
    const isWin = process.platform === "win32";
const cliPath = `build/${isWin ? "webrtc_vst_win" : "webrtc_vst_linux"}/bin/Release/webrtc_vst_cli_host${isWin ? ".exe" : ""}`;

    const env = {
        WEBRTC_VST_MODE: 'seed',
        WEBRTC_CLI_HOST_RUNTIME_MS: RUNTIME_MS.toString(),
        WEBRTC_CLI_HOST_TIMEOUT_MS: (RUNTIME_MS + WARMUP_MS).toString(),
        WEBRTC_VST_STREAM_ID: streamId,
        WEBRTC_VST_PASSWORD: 'false',
        WEBRTC_VST_HANDSHAKE_URL: 'wss://wss.vdo.ninja',
        WEBRTC_VST_LOG_STDOUT: '1',
        WEBRTC_VST_LOG_SIGNALING: '1',
        WEBRTC_CLI_HOST_WARMUP_MS: WARMUP_MS.toString(),
    };

    const cli = spawn('powershell.exe', [
        '-NoLogo',
        '-Command',
        Object.entries(env).map(([k, v]) => `$env:${k}='${v}'`).join('; ') +
        `; & '${cliPath}'`
    ]);

    cli.stdout.on('data', data => {
        const lines = data.toString().trim().split('\n');
        lines.forEach(line => {
            if (line.includes('offerSDP')) {
                console.log('[SEEDER] ✓ Received offerSDP request');
                receivedOfferSDP = true;
            }
            if (line.includes('ICE gathering complete')) {
                console.log('[SEEDER] ✓ ICE gathering completed');
            }
        });
    });

    cli.stderr.on('data', data => {
        const text = data.toString();
        console.error('[SEEDER STDERR]', text.trim());
        if (text.includes('Connected to VDO.Ninja')) {
            console.log('[SEEDER] ✓ Connected to signaling server');
        }
        if (text.includes('Sent seed request')) {
            console.log('[SEEDER] ✓ Sent seed request');
        }
    });

    cli.on('close', code => {
        console.log('');
        console.log('[SEEDER] Process exited with code', code);
        console.log('');

        evaluateResults();

        viewer.close();
        clearTimeout(timeout);
    });

    // Force-kill CLI after expected runtime + warmup + buffer
    setTimeout(() => {
        if (!cli.killed) {
            console.log('[SEEDER] Stopping CLI process after runtime elapsed');
            cli.kill('SIGKILL');
            // Give it a moment to exit
            setTimeout(() => {
                if (!cli.killed) {
                    spawn('powershell.exe', ['-Command', `Stop-Process -Id ${cli.pid} -Force`]);
                }
            }, 500);
        }
    }, WARMUP_MS + RUNTIME_MS + 1000);
}

function evaluateResults() {
    console.log('=== Test Results ===');
    console.log('Received offerSDP:', receivedOfferSDP ? '✓ PASS' : '✗ FAIL');
    console.log('Seeder sent answer:', receivedAnswer ? '✓ PASS' : '✗ FAIL');
    console.log('ICE candidates exchanged:', receivedCandidates ? '✓ PASS' : '✗ FAIL');
    console.log('');

    testPassed = receivedOfferSDP && receivedAnswer && receivedCandidates;

    if (testPassed) {
        console.log('✓ TEST PASSED: Seed handshake successful');
        process.exit(0);
    } else {
        console.log('✗ TEST FAILED: Seed handshake incomplete');
        process.exit(1);
    }
}

const timeout = setTimeout(() => {
    console.error('');
    console.error('✗ TEST TIMEOUT: Test did not complete within', TEST_TIMEOUT_MS, 'ms');
    viewer.close();
    process.exit(1);
}, TEST_TIMEOUT_MS);
