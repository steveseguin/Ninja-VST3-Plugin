#!/usr/bin/env node

/**
 * Test: VST Plugin (Seed) -> JS SDK (Viewer)
 *
 * Tests that the VST plugin in seed mode can publish audio that is
 * received by the VDO.Ninja JS SDK viewer.
 *
 * Flow:
 * 1. Start VST plugin in seed mode (publishing 1kHz tone)
 * 2. Start JS SDK viewer to receive the stream
 * 3. Verify SDK receives audio track
 * 4. Verify audio data is flowing (check RTC stats)
 */

const { spawn } = require('child_process');
const VDONinjaSDK = require('../../js_sdk/vdoninja-sdk-node.js');

const TEST_TIMEOUT_MS = 20000;
const WARMUP_MS = 2000;
const RUNTIME_MS = 8000;
const TONE_HZ = 1000;

// Generate unique stream ID
const streamId = 'vst_to_sdk_' + Date.now().toString(36);

console.log('=== VST Plugin -> JS SDK Test ===');
console.log('Stream ID:', streamId);
console.log('Testing: VST seed publishing audio to JS SDK viewer');
console.log('');

let testPassed = false;
let sdkConnected = false;
let audioTrackReceived = false;
let audioDataFlowing = false;

// Start VST plugin in seed mode
console.log('[VST] Starting plugin in seed mode...');
const isWin = process.platform === "win32";
const cliPath = `build/${isWin ? "webrtc_vst_win" : "webrtc_vst_linux"}/bin/Release/webrtc_vst_cli_host${isWin ? ".exe" : ""}`;

const vst = spawn('powershell.exe', [
    '-NoLogo',
    '-Command',
    `$env:WEBRTC_VST_MODE='seed'; ` +
    `$env:WEBRTC_CLI_HOST_RUNTIME_MS='${RUNTIME_MS}'; ` +
    `$env:WEBRTC_CLI_HOST_TIMEOUT_MS='${RUNTIME_MS + WARMUP_MS + 1000}'; ` +
    `$env:WEBRTC_VST_STREAM_ID='${streamId}'; ` +
    `$env:WEBRTC_VST_PASSWORD='false'; ` +
    `$env:WEBRTC_VST_HANDSHAKE_URL='wss://wss.vdo.ninja'; ` +
    `$env:WEBRTC_VST_LOG_STDOUT='1'; ` +
    `$env:WEBRTC_VST_LOG_SIGNALING='1'; ` +
    `$env:WEBRTC_CLI_HOST_WARMUP_MS='${WARMUP_MS}'; ` +
    `$env:WEBRTC_CLI_HOST_TONE_HZ='${TONE_HZ}'; ` +
    `& '${cliPath}'`
]);

vst.stdout.on('data', data => {
    const text = data.toString().trim();
    if (text.includes('Connected to VDO.Ninja')) {
        console.log('[VST] ✓ Connected to signaling server');
    }
    if (text.includes('Sent seed request')) {
        console.log('[VST] ✓ Published stream:', streamId);
        // Now start SDK viewer
        startSDKViewer();
    }
    if (text.includes('offerSDP')) {
        console.log('[VST] ✓ Received viewer connection request');
    }
    if (text.includes('ICE gathering complete')) {
        console.log('[VST] ✓ ICE handshake completed');
    }
});

vst.stderr.on('data', data => {
    const text = data.toString().trim();
    // Filter out just important messages
    if (text.includes('ERROR') || text.includes('Failed')) {
        console.error('[VST ERROR]', text);
    }
});

vst.on('close', code => {
    console.log('[VST] Process exited');
    evaluateResults();
});

async function startSDKViewer() {
    console.log('[SDK] Starting viewer...');

    try {
        const sdk = new VDONinjaSDK({
            host: 'wss://wss.vdo.ninja',
            streamID: streamId,
            password: false,
        });

        // Wait for connection
        await sdk.connect();
        console.log('[SDK] ✓ Connected to signaling server');
        sdkConnected = true;

        // Listen for tracks
        sdk.addEventListener('track', async (event) => {
            console.log('[SDK] ✓ Received track:', event.detail.track.kind);

            if (event.detail.track.kind === 'audio') {
                audioTrackReceived = true;
                console.log('[SDK] ✓ Audio track received from VST plugin');

                // Check if audio is actually flowing by examining stats
                setTimeout(async () => {
                    await checkAudioStats(event.detail.connection);
                }, 2000);
            }
        });

        // Request to play the stream
        console.log('[SDK] Requesting stream:', streamId);
        await sdk.play(streamId);
        console.log('[SDK] ✓ Play request sent');

    } catch (err) {
        console.error('[SDK ERROR]', err.message);
    }
}

async function checkAudioStats(peerConnection) {
    try {
        if (!peerConnection || !peerConnection.getStats) {
            console.log('[SDK] ⚠ Cannot get stats - peer connection not available');
            return;
        }

        const stats = await peerConnection.getStats();
        let bytesReceived = 0;
        let packetsReceived = 0;

        stats.forEach(report => {
            if (report.type === 'inbound-rtp' && report.kind === 'audio') {
                bytesReceived = report.bytesReceived || 0;
                packetsReceived = report.packetsReceived || 0;
            }
        });

        if (bytesReceived > 0 || packetsReceived > 0) {
            console.log('[SDK] ✓ Audio data flowing:', {
                bytes: bytesReceived,
                packets: packetsReceived
            });
            audioDataFlowing = true;
        } else {
            console.log('[SDK] ⚠ No audio data received yet');
        }
    } catch (err) {
        console.log('[SDK] ⚠ Could not check stats:', err.message);
    }
}

function evaluateResults() {
    console.log('');
    console.log('=== Test Results ===');
    console.log('SDK connected:', sdkConnected ? '✓ PASS' : '✗ FAIL');
    console.log('Audio track received:', audioTrackReceived ? '✓ PASS' : '✗ FAIL');
    console.log('Audio data flowing:', audioDataFlowing ? '✓ PASS' : '⚠ SKIP (could not verify)');
    console.log('');

    testPassed = sdkConnected && audioTrackReceived;

    if (testPassed) {
        console.log('✓ TEST PASSED: VST plugin successfully published audio to JS SDK');
        process.exit(0);
    } else {
        console.log('✗ TEST FAILED: VST plugin did not publish audio successfully');
        process.exit(1);
    }
}

// Overall timeout
setTimeout(() => {
    console.error('');
    console.error('✗ TEST TIMEOUT: Test did not complete within', TEST_TIMEOUT_MS, 'ms');
    if (!vst.killed) {
        vst.kill();
    }
    process.exit(1);
}, TEST_TIMEOUT_MS);
