const WebSocket = require('ws');

const stream = 'cli_loopback_9dte8t';

const seed = new WebSocket('wss://wss.vdo.ninja');
const play = new WebSocket('wss://wss.vdo.ninja');

seed.on('open', () => {
    console.log('[SEED] connected');
    seed.send(JSON.stringify({request: 'seed', streamID: stream}));
    console.log('[SEED] sent seed request');
});

seed.on('message', data => {
    console.log('[SEED] <<', data.toString());
});

seed.on('close', () => {
    console.log('[SEED] closed');
});

seed.on('error', err => {
    console.error('[SEED] error:', err.message);
});

play.on('open', () => {
    console.log('[PLAY] connected');
    // Wait a bit for seed to register
    setTimeout(() => {
        play.send(JSON.stringify({request: 'play', streamID: stream}));
        console.log('[PLAY] sent play request');
    }, 1000);
});

play.on('message', data => {
    console.log('[PLAY] <<', data.toString());
});

play.on('close', () => {
    console.log('[PLAY] closed');
});

play.on('error', err => {
    console.error('[PLAY] error:', err.message);
});

setTimeout(() => {
    console.log('[TIMEOUT] closing connections');
    seed.close();
    play.close();
    process.exit(0);
}, 10000);
