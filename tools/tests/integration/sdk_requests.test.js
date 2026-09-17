const assert = require('node:assert/strict');
const SDK = require('../../../js_sdk/vdoninja-sdk-node.js');
const settle = () => new Promise(resolve => setImmediate(resolve));

(async () => {
    const sdk = new SDK();
    const responses = [];
    sdk.respond = (...args) => responses.push(args);
    sdk.onRequest('echo', (data, uuid) => ({data, uuid}));
    for (const requestType of ['constructor', 'toString', '__proto__', 'hasOwnProperty', ['echo']]) {
        sdk._handleDataChannelMessage({type: 'request', requestType, requestId: 'unregistered'}, 'peer');
    }
    await settle();
    assert.equal(responses.length, 0, 'Only registered string request types may dispatch');
    sdk._handleDataChannelMessage({type: 'request', requestType: 'echo', requestId: 'normal', data: 42}, 'peer');
    await settle();
    assert.deepEqual(responses.shift(), ['normal', {data: 42, uuid: 'peer'}, 'peer']);
    sdk.onRequest('__proto__', () => 'explicitly registered');
    sdk._handleDataChannelMessage({type: 'request', requestType: '__proto__', requestId: 'reserved'}, 'peer');
    await settle();
    assert.equal(responses.shift()[1], 'explicitly registered');
    sdk.onRequest('throws', () => { throw new Error('expected error'); });
    sdk._handleDataChannelMessage({type: 'request', requestType: 'throws', requestId: 'failure'}, 'peer');
    await settle();
    assert.equal(responses.shift()[1].error, 'expected error');

    let sent;
    sdk.sendData = message => { sent = message; return true; };
    const pending = sdk.request('echo', 7, 'peer', 1000);
    for (const requestId of ['constructor', 'toString', '__proto__']) {
        sdk._handleDataChannelMessage({type: 'response', requestId, data: 'unwanted'}, 'peer');
    }
    sdk._handleDataChannelMessage({type: 'response', requestId: sent.requestId, data: 'ok'}, 'peer');
    assert.equal(await pending, 'ok');
    await sdk.disconnect();
    console.log('PASS: SDK dispatch accepts registered handlers and genuine pending responses only');
})().catch(error => { console.error(error); process.exitCode = 1; });
