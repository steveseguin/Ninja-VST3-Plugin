const assert = require('node:assert/strict');
const SDK = require('../../../js_sdk/vdoninja-sdk-node.js');
const { MediaStream, nonstandard } = require('@roamhq/wrtc');

(async () => {
    const sdk = new SDK({ password: false });
    const source = new nonstandard.RTCAudioSource();
    const track = source.createTrack();
    const stream = new MediaStream([track]);
    const messages = [];
    // Exercise the real publish implementation without contacting signaling.
    sdk.state.connected = true;
    sdk._sendMessageWS = message => messages.push(message);
    try {
        await sdk.publish(stream, {streamID: 'sdkoptionscheck', label: 'Option test', audioBitrate: '96k'});
        assert.equal(sdk.state.streamID, 'sdkoptionscheck');
        assert.equal(sdk._pendingLabel, 'Option test');
        assert.equal(sdk._publishMediaConfig.audio.maxBitrate, 96000);
        assert(messages.some(message => message.request === 'seed' && message.streamID === 'sdkoptionscheck'));
        console.log('PASS: Node SDK forwards publisher identity and media options');
    } finally {
        await sdk.disconnect();
        track.stop();
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
