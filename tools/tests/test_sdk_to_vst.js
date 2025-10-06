#!/usr/bin/env node

/**
 * Test: JS SDK (Publisher) -> VST Plugin (Viewer)
 *
 * Tests that the VST plugin in play mode can receive audio published
 * by the VDO.Ninja JS SDK.
 *
 * Flow:
 * 1. Start JS SDK publisher (generating test audio tone)
 * 2. Start VST plugin in play mode
 * 3. Verify VST receives audio track
 * 4. Monitor VST output for audio signal
 */

const { spawn } = require('child_process');
const VDONinjaSDK = require('../../js_sdk/vdoninja-sdk-node.js');
const { nonstandard, MediaStream } = require('@roamhq/wrtc');

const TEST_TIMEOUT_MS = 20000;
const WARMUP_MS = 2000;
const RUNTIME_MS = 8000;
const TONE_HZ = 440;

// Generate unique stream ID
const streamId = 'sdk_to_vst_' + Date.now().toString(36);

console.log('=== JS SDK -> VST Plugin Test ===');
console.log('Stream ID:', streamId);
console.log('Testing: JS SDK publishing audio to VST viewer');
console.log('');

let testPassed = false;
let sdkPublished = false;
let vstConnected = false;
let vstReceivedAudio = false;

// Create audio track using wrtc
function createAudioTrack(frequency = 440, sampleRate = 48000) {
    const { RTCAudioSource } = nonstandard;
    const source = new RTCAudioSource();
    const track = source.createTrack();

    // Generate sine wave
    const samplesPerFrame = sampleRate / 100; // 10ms frames
    let phase = 0;
    const phaseIncrement = (2 * Math.PI * frequency) / sampleRate;

    const interval = setInterval(() => {
        const samples = new Int16Array(samplesPerFrame);
        for (let i = 0; i < samplesPerFrame; i++) {
            samples[i] = Math.sin(phase) * 32767 * 0.5; // 50% volume
            phase += phaseIncrement;
            if (phase >= 2 * Math.PI) phase -= 2 * Math.PI;
        }
        source.onData({ samples, sampleRate });
    }, 10);

    // Return track and cleanup function
    return {
        track,
        stop: () => {
            clearInterval(interval);
            track.stop();
        }
    };
}

async function startSDKPublisher() {
    console.log('[SDK] Starting publisher...');

    try {
        const sdk = new VDONinjaSDK({
            host: 'wss://wss.vdo.ninja',
            streamID: streamId,
            password: false,
        });

        await sdk.connect();
        console.log('[SDK] ✓ Connected to signaling server');

        // Create audio track
        console.log('[SDK] Generating', TONE_HZ, 'Hz test tone...');
        const { track, stop } = createAudioTrack(TONE_HZ);

        // Create MediaStream with audio track
        const stream = new MediaStream([track]);

        // Publish stream
        console.log('[SDK] Publishing stream:', streamId);
        await sdk.publish(stream, { streamID: streamId });
        console.log('[SDK] ✓ Stream published');
        sdkPublished = true;

        // Now start VST viewer
        startVSTViewer();

        // Keep publishing for test duration
        setTimeout(() => {
            console.log('[SDK] Stopping publisher...');
            stop();
            sdk.close();
        }, RUNTIME_MS + WARMUP_MS);

    } catch (err) {
        console.error('[SDK ERROR]', err.message);
        process.exit(1);
    }
}

function startVSTViewer() {
    console.log('[VST] Starting plugin in play mode...');
    const cliPath = 'build/webrtc_vst_win/bin/Release/webrtc_vst_cli_host.exe';

    const vst = spawn('powershell.exe', [
        '-NoLogo',
        '-Command',
        `$env:WEBRTC_VST_MODE='play'; ` +
        `$env:WEBRTC_CLI_HOST_RUNTIME_MS='${RUNTIME_MS}'; ` +
        `$env:WEBRTC_CLI_HOST_TIMEOUT_MS='${RUNTIME_MS + WARMUP_MS + 1000}'; ` +
        `$env:WEBRTC_VST_STREAM_ID='${streamId}'; ` +
        `$env:WEBRTC_VST_PASSWORD='false'; ` +
        `$env:WEBRTC_VST_HANDSHAKE_URL='wss://wss.vdo.ninja'; ` +
        `$env:WEBRTC_VST_LOG_STDOUT='1'; ` +
        `$env:WEBRTC_VST_LOG_SIGNALING='1'; ` +
        `$env:WEBRTC_CLI_HOST_WARMUP_MS='${WARMUP_MS}'; ` +
        `$env:WEBRTC_CLI_HOST_MONITOR_OUTPUT='1'; ` +
        `& '${cliPath}'`
    ]);

    vst.stdout.on('data', data => {
        const text = data.toString().trim();
        console.log('[VST]', text);

        if (text.includes('Connected to VDO.Ninja')) {
            vstConnected = true;
        }
        if (text.includes('Sent play request')) {
            console.log('[VST] ✓ Requested stream:', streamId);
        }
        if (text.includes('ICE gathering complete')) {
            console.log('[VST] ✓ Connection established');
        }
        if (text.includes('output_rms=')) {
            // Extract RMS value
            const match = text.match(/output_rms=([\d.]+)/);
            if (match) {
                const rms = parseFloat(match[1]);
                if (rms > 0.01) { // Threshold for detecting audio
                    console.log('[VST] ✓ Audio detected! RMS:', rms.toFixed(4));
                    vstReceivedAudio = true;
                } else {
                    console.log('[VST] ⚠ No audio detected (RMS:', rms.toFixed(4), ')');
                }
            }
        }
    });

    vst.stderr.on('data', data => {
        const text = data.toString().trim();
        if (text.includes('ERROR') && !text.includes('timed out')) {
            console.error('[VST ERROR]', text);
        }
    });

    vst.on('close', code => {
        console.log('[VST] Process exited');
        evaluateResults();
    });
}

function evaluateResults() {
    console.log('');
    console.log('=== Test Results ===');
    console.log('SDK published stream:', sdkPublished ? '✓ PASS' : '✗ FAIL');
    console.log('VST connected:', vstConnected ? '✓ PASS' : '✗ FAIL');
    console.log('VST received audio:', vstReceivedAudio ? '✓ PASS' : '✗ FAIL');
    console.log('');

    testPassed = sdkPublished && vstConnected && vstReceivedAudio;

    if (testPassed) {
        console.log('✓ TEST PASSED: VST plugin successfully received audio from JS SDK');
        process.exit(0);
    } else {
        console.log('✗ TEST FAILED: VST plugin did not receive audio successfully');
        process.exit(1);
    }
}

// Overall timeout
setTimeout(() => {
    console.error('');
    console.error('✗ TEST TIMEOUT: Test did not complete within', TEST_TIMEOUT_MS, 'ms');
    process.exit(1);
}, TEST_TIMEOUT_MS);

// Start the test
startSDKPublisher();
