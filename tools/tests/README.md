# VST WebRTC Plugin Tests

Audio-focused integration tests for the VST WebRTC plugin using VDO.Ninja signaling.

## Prerequisites

```bash
npm install
```

## Audio-Only Tests

### 1) Play request includes audio-only flags
File: `tools/tests/integration/play_request_audio_only.test.js`

Validates that Play mode sends:

```json
{
  "request": "play",
  "audio": true,
  "video": false
}
```

Run:

```bash
npm run test:play-request-audio-only
```

### 2) Live stream: verify video is not attached in Play mode
File: `tools/tests/integration/play_ignores_video_track.test.js`

Flow:
1. VST plugin runs in Play mode against a real stream (default `steve1234`).
2. Test verifies Play request carries `audio:true, video:false`.
3. Test verifies datachannel viewer preferences are sent.
4. Test fails if plugin reports receiving any non-audio track.
5. Test reports output RMS as an informational metric.
6. Test verifies publisher datachannel is mapped to the media peer to prevent answer-state regressions.

Run:

```bash
npm run test:play-ignore-video
```

You can override the stream ID:

```bash
WEBRTC_TEST_STREAM_ID=your_stream npm run test:play-ignore-video
```

For room-based streams, set the plugin room env as well:

```bash
WEBRTC_TEST_STREAM_ID=your_stream WEBRTC_VST_ROOM_NAME=your_room npm run test:play-ignore-video
```

### 3) Live room audio-only decode (strict)
File: `tools/tests/integration/play_room_audio_live.test.js`

Flow:
1. VST plugin runs in Play mode against a room + stream (defaults: room `steve123456`, stream `3TtDVfG`).
2. Test verifies play request and viewer preferences remain audio-only.
3. Test verifies publisher datachannel is mapped to the media peer.
4. Test fails if remote-answer signaling-state errors appear.
5. Test requires non-zero output RMS (`WEBRTC_TEST_MIN_RMS`, default `0.01`).

Run:

```bash
npm run test:play-room-live
```

Override room/stream/password:

```bash
WEBRTC_TEST_ROOM_NAME=your_room WEBRTC_TEST_STREAM_ID=your_stream WEBRTC_TEST_PASSWORD=your_password npm run test:play-room-live
```

### Run both

```bash
npm run test:audio-only
```

## Integration Test Groups

- `npm run test:integration:audio-only`: audio-only signaling and playback behavior.
- `npm run test:integration:local`: local loopback + publish path checks.
- `npm run test:integration:live`: live room playback verification.
- `npm run test:integration`: audio-only + local integration gate.

## Existing Integration Tests

- `npm run test:publish-audio`: CLI seed publishes to SDK viewer with audio-only view options.
- `npm run test:cli-loopback`: dual-CLI loopback smoke test.

## Environment variables

- `WEBRTC_TEST_WSS`: signaling URL (default `wss://wss.vdo.ninja`)
- `WEBRTC_TEST_RUNTIME_MS`: runtime for CLI host in relevant tests
- `WEBRTC_TEST_TIMEOUT_MS`: overall test timeout
- `WEBRTC_TEST_MIN_RMS`: minimum RMS threshold for playback checks
- `WEBRTC_VST_ROOM_NAME`: optional room to join before announcing play/seed
- `WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS`: optional real-time runtime override for CLI host runs
- `WEBRTC_TEST_DISABLE_STUN`: loopback helper (defaults to `1`) to force host-only ICE in local dual-CLI tests

## Notes

- Tests use default encryption behavior (empty password), matching VDO.Ninja default key flow.
- These are networked integration tests and require internet access.
