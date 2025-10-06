# VST WebRTC Plugin Tests

Automated tests for the VST WebRTC plugin using VDO.Ninja signaling.

## Prerequisites

```bash
npm install ws @roamhq/wrtc
```

## Tests

### 1. Handshake Test
**File:** `test_seed_handshake.js`

Tests WebSocket signaling handshake between VST plugin (seed) and a simple viewer.

**Verifies:**
- VST connects to signaling server
- VST sends seed request
- VST receives offerSDP when viewer connects
- SDP answer is sent back
- ICE candidates are exchanged

**Run:**
```bash
node tools/tests/test_seed_handshake.js
```

---

### 2. VST → JS SDK Test
**File:** `test_vst_to_sdk.js`

Tests VST plugin in **seed mode** publishing audio to JS SDK viewer.

**Verifies:**
- VST publishes a 1kHz test tone
- JS SDK viewer connects and receives audio track
- Audio track is properly received

**Run:**
```bash
node tools/tests/test_vst_to_sdk.js
```

**Expected output:**
```
✓ TEST PASSED: VST plugin successfully published audio to JS SDK
```

---

### 3. VST → Web Browser (Manual)
**File:** `test_vst_web_viewer.js`

Helper script to test VST plugin with actual VDO.Ninja web interface.

**Usage:**
1. Run the script:
   ```bash
   node tools/tests/test_vst_web_viewer.js
   ```

2. Open the provided URL in your browser (e.g., Chrome, Firefox)

3. You should hear a 1kHz test tone

4. Press Ctrl+C to stop

**This confirms the VST plugin works with the real VDO.Ninja web app!**

---

### 4. JS SDK → VST Test (Work in Progress)
**File:** `test_sdk_to_vst.js`

Tests JS SDK publishing audio to VST plugin in **play mode**.

**Status:** Implementation in progress (MediaStream context issue)

---

## Test Results Summary

| Test | Status | Notes |
|------|--------|-------|
| Handshake | ✅ PASS | WebSocket signaling working |
| VST → JS SDK | ✅ PASS | Audio publishing verified |
| VST → Web Browser | ✅ MANUAL | Use test_vst_web_viewer.js |
| JS SDK → VST | 🚧 WIP | Context isolation issue |

## Environment Variables

The tests use these environment variables to configure the VST plugin:

- `WEBRTC_VST_MODE` - `seed` or `play`
- `WEBRTC_VST_STREAM_ID` - Stream identifier
- `WEBRTC_VST_PASSWORD` - Set to `false` to disable encryption
- `WEBRTC_VST_HANDSHAKE_URL` - Signaling server (default: `wss://wss.vdo.ninja`)
- `WEBRTC_VST_LOG_STDOUT` - Enable stdout logging (`1`)
- `WEBRTC_VST_LOG_SIGNALING` - Enable signaling message logging (`1`)
- `WEBRTC_CLI_HOST_RUNTIME_MS` - How long to run (milliseconds)
- `WEBRTC_CLI_HOST_WARMUP_MS` - Warmup delay before processing (milliseconds)
- `WEBRTC_CLI_HOST_TIMEOUT_MS` - Force exit after timeout
- `WEBRTC_CLI_HOST_TONE_HZ` - Test tone frequency (Hz)
- `WEBRTC_CLI_HOST_MONITOR_OUTPUT` - Monitor output audio level

## Known Issues

1. **JS SDK MediaStream context** - The SDK runs in an isolated VM context, making it difficult to pass MediaStream objects created outside. Need to use SDK's internal media generation.

2. **CLI doesn't auto-exit** - The CLI host continues running after `RUNTIME_MS` completes. Tests use `TIMEOUT_MS` to force exit.
