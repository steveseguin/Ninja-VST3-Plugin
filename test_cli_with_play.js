const WebSocket = require('ws');
const { spawn } = require('child_process');

const stream = 'cli_test_auto_' + Date.now().toString(36);

console.log('Using stream ID:', stream);
console.log('Viewer URL: https://vdo.ninja/?view=' + stream + '&password=false');

// Start play websocket first
const play = new WebSocket('wss://wss.vdo.ninja');

play.on('open', () => {
    console.log('[PLAY] connected');
    play.send(JSON.stringify({request: 'play', streamID: stream}));
    console.log('[PLAY] sent play request');

    // Now start CLI seed
    console.log('[CLI] starting seed...');
    const cli = spawn('powershell.exe', [
        '-NoLogo',
        '-Command',
        `$env:WEBRTC_VST_MODE='seed'; $env:WEBRTC_CLI_HOST_RUNTIME_MS='8000'; $env:WEBRTC_VST_STREAM_ID='${stream}'; $env:WEBRTC_VST_PASSWORD='false'; $env:WEBRTC_VST_HANDSHAKE_URL='wss://wss.vdo.ninja'; $env:WEBRTC_VST_LOG_STDOUT='1'; $env:WEBRTC_VST_LOG_SIGNALING='1'; $env:WEBRTC_VST_LOG_SIGNALING_RAW='1'; $env:WEBRTC_CLI_HOST_WARMUP_MS='2000'; & 'build/webrtc_vst_win/bin/Release/webrtc_vst_cli_host.exe'`
    ]);

    cli.stdout.on('data', data => {
        console.log('[CLI]', data.toString().trim());
    });

    cli.stderr.on('data', data => {
        console.error('[CLI ERR]', data.toString().trim());
    });

    cli.on('close', code => {
        console.log('[CLI] exited with code', code);
        play.close();
    });
});

play.on('message', data => {
    console.log('[PLAY] <<', data.toString());
});

play.on('close', () => {
    console.log('[PLAY] closed');
    process.exit(0);
});

play.on('error', err => {
    console.error('[PLAY] error:', err.message);
});
