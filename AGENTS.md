# Agent Notes - WebRTC VST

## What this plugin is for
- VST3 bridge between a DAW and VDO.Ninja: Seed mode publishes host audio; Play mode brings remote audio back into the DAW.
- Primary value: remote collaboration (talkback, IFB, voiceover, mix review) with no local server setup.

## Current host guidance
- Audacity loads as an effect but is not an ideal real-time host; open/close behavior can be brittle.
- Use Reaper or Studio One for main validation; use Audacity only as a smoke check.
- Audio format support today is 32-bit float in process blocks.

## Known risk areas
- Session restart logic can still be triggered from audio-thread paths.
- Channel count or buffer resize operations can stall if handled under audio-thread locks.
- Reconnection/backoff logic is limited.
- UI telemetry is minimal, so users have little feedback when audio stops flowing.

## Recommended next steps
1. Move session start/stop and heavy WebRTC object lifecycle work fully off the audio thread.
2. Expand host format handling (including clearer 64-bit behavior messaging).
3. Add basic status UI (connected, receiving, publishing, error).
4. Add reconnect/backoff controls.
5. Keep integration tests and host smoke tests in the release gate.

## Audacity smoke test
- Build and deploy the VST3.
- Set `WEBRTC_VST_LOG_STDOUT=1` and `WEBRTC_VST_LOG_SIGNALING=1`.
- In Audacity: load plugin, set Mode to Play, provide a live stream ID, and verify audio arrives during repeated open/close cycles.
