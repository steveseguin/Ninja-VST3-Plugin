# WebRTC VST Bridge

A prototype VST3 audio effect that routes audio over WebRTC using the [libdatachannel](https://github.com/paullouisageneau/libdatachannel) stack and the VDO.Ninja signaling API. The plugin can operate in two modes:

- **Seed** – publish host audio to a remote peer (WebRTC send-only from the point of view of the DAW).
- **Play** – receive remote audio and inject it into the DAW return path.

> ⚠️ This is an early integration layer. It does not yet expose a GUI, assumes a 48 kHz sample-rate workflow, and focuses on establishing the infrastructure for signaling and audio transport. Additional polish (UI, reconnection policies, latency controls, etc.) will be required before production use.

## Features

- VST3 audio effect built with Steinberg's VST3 SDK (configured via CMake).
- WebRTC transport via `libdatachannel` with Opus encode/decode for audio frames.
- Signaling client for VDO.Ninja handshake semantics using the provided JS SDK as reference.
- WebSocket signaling handled with `IXWebSocket`.
- Cross-platform dependencies resolved via `FetchContent` (libdatachannel, ixwebsocket, opus, nlohmann_json).

## Building

1. Obtain the Steinberg VST3 SDK (https://github.com/steinbergmedia/vst3sdk) and set `VST3_SDK_ROOT` to the directory that contains the `cmake/modules` folder. Make sure to initialize the SDK's submodules so the CMake scripts are present. For example, on Windows/WSL:

   ```bash
   git clone --depth 1 https://github.com/steinbergmedia/vst3sdk.git ~/deps/vst3sdk
   (cd ~/deps/vst3sdk && git submodule update --init --recursive)
   export VST3_SDK_ROOT=~/deps/vst3sdk
   ```

2. Configure and build:

```bash
cmake -S webrtc_vst -B build/webrtc_vst -DVST3_SDK_ROOT=/path/to/VST3_SDK
cmake --build build/webrtc_vst --config Release
```

Artifacts will appear under `build/webrtc_vst/bin` (platform specific bundle layout).

### Build Dependencies

- CMake ≥ 3.24
- C++20 compiler toolchain
- VST3 SDK
- libdatachannel (fetched automatically)
- IXWebSocket (fetched automatically)
- Opus (fetched automatically)
- nlohmann_json (fetched automatically)

## Configuration

All runtime settings are exposed as VST3 parameters via the edit controller. Most hosts surface these in their generic parameter view, allowing automation and preset storage without shell variables:

| Parameter | Description | Default |
| --- | --- | --- |
| `Mode` | `Play` (viewer) or `Seed` (publisher). | `Play` |
| `Stream ID` | Stream identifier advertised to VDO.Ninja peers. | `vst-stream` |
| `Room ID` | Optional room joined before seeding or playing. | *(empty)* |
| `Signaling URL` | WebSocket endpoint for the VDO.Ninja handshake. | `wss://wss0.vdo.ninja` |
| `Password` | Optional room password / encryption key. Leave blank to rely on the SDK default. | *(empty)* |
| `Disable Encryption` | When enabled, SDP/ICE payloads are sent in plain text. | `false` |

For backwards compatibility the plugin still honours the legacy environment variables below during initialisation. If present they prime the same parameters; any subsequent edits inside the host override the values.

| Variable | Description |
| --- | --- |
| `WEBRTC_VST_MODE` | Preferred mode (`seed` / `play`). |
| `WEBRTC_VST_STREAM_ID` | Initial stream identifier. |
| `WEBRTC_VST_ROOM_ID` | Initial room identifier. |
| `WEBRTC_VST_SIGNALING_URL` | Signaling WebSocket URL. |
| `WEBRTC_VST_PASSWORD` | Initial password value. |
| `WEBRTC_VST_DISABLE_ENCRYPTION` | `1/true` to disable SDP/ICE encryption. |

State chunks written by the host serialise the same fields as JSON. You can author a preset by serialising:

```json
{
  "mode": "seed",
  "streamId": "my-stream",
  "roomId": "mixbus",
  "signalingUrl": "wss://wss0.vdo.ninja",
  "password": "super-secret",
  "disableEncryption": false
}
```

## DAW Integration Tips

- Place the plugin on an audio track or bus.
- In **Seed** mode, the plugin passes input audio through to the output while also streaming it to the remote peer.
- In **Play** mode, the plugin ignores the input bus and fills the output with remote audio. Mute/solo routing is handled by the DAW.
- The processor performs internal sample-rate conversion when the host is not at 48 kHz, so mismatched project rates now work without pitch shifts (48 kHz still yields the lowest latency).
- Multiple simultaneous viewers are supported when seeding; each remote peer negotiates its own session and ICE state.
- The controller parameters can be automated to swap streams or toggle modes; changing connection primitives forces a reconnect.

## Extension Points / Next Steps

- Improve resiliency (reconnects, ICE restarts, multi-peer management).
- Expose latency/buffer controls and provide monitoring of connection statistics.
- Expand to AU/CLAP wrappers by reusing the core WebRTC session module.
- Harden security (password hashing/encryption as implemented in the JS SDK).

## License

The new code in this directory is provided under the AGPLv3 to align with the upstream VDO.Ninja SDK. Adjust as appropriate for your project needs.
