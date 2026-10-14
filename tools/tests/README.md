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

## Audio formats and performance measurements

The CLI host accepts `WEBRTC_CLI_HOST_SAMPLE_RATE` (8000–384000) and
`WEBRTC_CLI_HOST_BLOCK_SIZE` (1–8192). Defaults remain 48000 Hz and 256 frames.
Invalid formats and failed plugin processing calls return a nonzero exit code.
`WEBRTC_CLI_HOST_CONTINUOUS_WARMUP=1` keeps callbacks running during connection
setup. The live quality probe enables it so receive queues do not fill during
an artificial two-second callback stall. Ordinary CLI defaults are unchanged.
Use `WEBRTC_PROBE_STALL_WARMUP=1` to exercise the original saturated-startup
condition deliberately; results record which warmup mode was used.

The live playback probe creates two synthetic stereo endpoints and measures
level-marker latency, tone residual, individual waveform jumps, silence,
clipping and nonfinite output. On macOS it also records CPU time and peak RSS
for each complete CLI process, including the host's pacing and capture overhead.
These are same-machine transport measurements, excluding audio-interface delay
and remote-network latency; they are not a listening evaluation.

```bash
# Six sequential formats: 44.1/48/96/192 kHz and 32–1024-frame callbacks.
WEBRTC_GUARD_SECONDS=360 node tools/tests/guarded_run.js npm run test:playback-matrix

# One format, with the same quality gates.
WEBRTC_PROBE_SAMPLE_RATE=96000 WEBRTC_PROBE_BLOCK_SIZE=64 \
  node tools/tests/guarded_run.js npm run test:playback-quality

# Verify the analyzer rejects an isolated click and clipped plateaus offline.
node --test tools/tests/offline/playback_quality_analysis.test.js

# Exact FIFO/model and resampler checks, followed by a buffer-only benchmark.
build/webrtc_vst_mac/bin/Release/webrtc_vst_audio_ring_test --benchmark
```

The default latency limits remain 100 ms p95 and 25 ms spread. Larger callbacks
receive additional allowance for the existing four-block scheduling reserve and
block-level timestamp resolution. Waveform-jump checks exclude intentional
amplitude transitions and apply to the probe's known tones. Raw captures and
JSON measurements remain under `build/test-tools/playback-probe-*`.

Median tone residual is limited to 1% at native 48 kHz and 2% when resampling;
the p95 limit is 3% at every rate. A standalone reference using the unchanged
resampler and default Opus settings, without networking or queues, measured up
to 1.67% median residual depending on the starting phase at 44.1 kHz. This avoids
misclassifying existing codec/resampling behavior as a transport glitch. The
waveform-jump, silence and clipping gates remain separate and unchanged.

Reproduce the reference on a built Mac (optional diagnostic, not a timing test):

```bash
c++ -std=c++20 -O3 tools/tests/diagnostics/codec_quality_reference.cpp \
  -Iwebrtc_vst/src -Ibuild/webrtc_vst_mac/_deps/opus-src/include \
  build/webrtc_vst_mac/lib/Release/libopus.a -o build/test-tools/codec-quality-reference
node tools/tests/guarded_run.js build/test-tools/codec-quality-reference 44100
```

The extended lifecycle runner accepts `WEBRTC_FUZZ_SEED` as its first seed and
increments it each cycle. It owns a loopback WebSocket server so rapid
activation/deactivation actually overlaps successful signaling opens. The
reconnect regression sweeps short connection delays across 80 cycles. For the
shutdown-race seed found during macOS sanitizer validation:

```bash
WEBRTC_FUZZ_SEED=5653587 WEBRTC_EXTENDED_SECONDS=600 WEBRTC_GUARD_SECONDS=720 \
  node tools/tests/guarded_run.js npm run test:extended
```

## Short transport and multi-instance checks

`npm run test:media-faults` rewrites only its owned endpoints' IPv4 UDP ICE
candidates to loopback relays. The relay passes STUN/DTLS/RTCP and impairs actual
encrypted RTP: 5% packet loss, up to 15 ms jitter, then a three-second media
outage. Counters require traffic in every phase, and measured stereo audio must
recover after the outage. Signaling password encryption is off for candidate
rewriting; DTLS/SRTP remains active. It does not modify system routing or firewalls.
Run it under `node tools/tests/guarded_run.js npm run test:media-faults`.

`WEBRTC_REAPER_TEST_PAIRS=4` makes `reaper_smoke.lua` exercise eight plugin
instances in four independent live Seed/Play pairs. Every pair must have audio
in each measured window, including after bypass and project reload. The default
remains one pair; use the same count when checking a fresh-process reopen.

## Seeded fuzz and individual click checks

`npm run test:signaling-fuzz` sends 2,004 seeded mutations/boundary inputs to each
mode through an owned loopback signaling server. It checks subsequent valid
negotiation, reconnect, controller error status, finite audio and normal teardown.
Peer identities are constrained so the separate limits test remains responsible
for peer-cap exhaustion. Set `WEBRTC_FUZZ_SEED` to replay another corpus. This
test is also part of `test:hardening`. No malformed traffic reaches public servers.
`WEBRTC_CLI_HOST_MONITOR_STATUS=1` makes the headless host poll status outside
`process()`, using the same message as the editor's GUI timer.

The native fuzz test adds 3,014 actual saved-state cases, asserting atomic
rejection and canonical save/restore for both VST interfaces. Use
`WEBRTC_FUZZ_STATE_ONLY=1` for just that short corpus. The audio impairment suite
also feeds 5,000 mutated/random Opus payloads to the production decoder, checking
finite output, bounded queues and recovery with valid packets. This is after the
SRTP authentication boundary, not a test that corrupted network packets bypass
authentication.

`npm run test:audio-clicks` runs a 32-second captured stereo tone with signaling
interruption. After startup it examines every sample, including host block
boundaries, using the sinusoid's second-order prediction residual to detect
isolated spikes and phase jumps. Its offline tests inject deliberate defects to
verify sensitivity. Captures, `clicks.json` and a five-second `received-stereo.wav`
remain in the reported `build/test-tools/audio-soak-*` directory. This objective
test does not replace listening to speech/music or testing physical audio devices.

```bash
node tools/tests/guarded_run.js npm run test:signaling-fuzz
node tools/tests/guarded_run.js npm run test:audio-clicks
```
