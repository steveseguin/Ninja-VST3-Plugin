const assert = require('node:assert/strict');
const wrtc = require('@roamhq/wrtc');
// This test checks JavaScript option forwarding. Native media is covered by
// publish/loopback tests; wrtc media finalizers crash at Node 22 exit on macOS.
class OptionsTestMediaStream {
    getTracks() { return []; }
    getAudioTracks() { return []; }
    getVideoTracks() { return []; }
}
const NativeMediaStream = wrtc.MediaStream;
wrtc.MediaStream = OptionsTestMediaStream;
const SDK = require('../../../js_sdk/vdoninja-sdk-node.js');
const { MediaStream } = require('@roamhq/wrtc');

(async () => {
    const sdk = new SDK({ password: false });
    const stream = new MediaStream();
    const messages = [];
    // Exercise the real publish implementation without contacting signaling.
    sdk.state.connected = true;
    sdk._sendMessageWS = message => messages.push(message);
    try {
        await sdk.publish(stream, {streamID: 'sdkoptionscheck', label: 'Option test', audioBitrate: '96k'});
        assert.equal(sdk.localStream, stream);
        assert.equal(sdk.state.streamID, 'sdkoptionscheck');
        assert.equal(sdk._pendingLabel, 'Option test');
        assert.equal(sdk._publishMediaConfig.audio.maxBitrate, 96000);
        assert(messages.some(message => message.request === 'seed' && message.streamID === 'sdkoptionscheck'));
        console.log('PASS: Node SDK forwards publisher identity and media options');
    } finally {
        await sdk.disconnect();
        wrtc.MediaStream = NativeMediaStream;
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
