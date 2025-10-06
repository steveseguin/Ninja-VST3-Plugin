const SDK = require('./js_sdk/vdoninja-sdk-node.js');
(async () => {
  const sdk = new SDK({ host: 'wss://wss.vdo.ninja', debug: true });
  try {
    await sdk.connect();
    console.log('connected promise resolved, state.uuid=', sdk.state?.uuid);
    if (sdk.signaling) {
      const original = sdk.signaling.onmessage;
      sdk.signaling.onmessage = async (event) => {
        console.log('[RAW] message', event?.data);
        if (original) {
          await original.call(sdk.signaling, event);
        }
      };
    }
    const streamID = 'test' + Math.random().toString(36).slice(-6);
    console.log('announcing stream', streamID);
    await sdk.announce({ streamID });
    console.log('announce resolved, state.uuid=', sdk.state?.uuid);
    await new Promise((resolve) => setTimeout(resolve, 30000));
    console.log('after wait state.uuid=', sdk.state?.uuid);
    await sdk.disconnect();
    console.log('disconnected promise resolved');
    process.exit(0);
  } catch (err) {
    console.error('error', err);
    process.exit(1);
  }
})();
