const WebSocket = require('ws');
const port = parseInt(process.env.WS_TEST_PORT || '8765', 10);
const wss = new WebSocket.Server({ port });

wss.on('connection', (socket, req) => {
    console.log('[server] client connected', req.headers['user-agent'] || '');

    socket.on('message', (data) => {
        console.log('[server] message', data.toString());
    });

    socket.on('close', () => {
        console.log('[server] client disconnected');
    });

    socket.on('error', (err) => {
        console.error('[server] error', err);
    });
});

console.log('[server] listening on port', port);

setInterval(() => {}, 1000);
