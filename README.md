# WebRTC VST for VDO.Ninja

Audio-first VST3 bridge between DAW hosts and VDO.Ninja signaling/WebRTC transport.

## Features

- **Seed & Play modes** — Publish DAW audio to VDO.Ninja or monitor remote streams
- **Multi-peer mixing** — Receives multiple peers in a room, each with independent Opus decoding, mixed to stereo output
- **Opus codec** — 48 kHz stereo encoding/decoding with automatic sample rate conversion
- **End-to-end encryption** — Password-based AES encryption compatible with VDO.Ninja E2EE
- **Room support** — Join rooms with automatic peer discovery and hashed room/stream IDs
- **Cross-platform** — Windows (x64) and Linux (x64), VST3 format

## What this repo provides

- `webrtc_vst/`: VST3 plugin source.
- `tools/cli_host/`: Headless CLI host for testing without a DAW.
- `tests/`: Integration and stress tests.
- `docs/`: Marketing/download page intended for GitHub Pages.

## Build and test quick start

- Build/test doc: [docs/developer/BUILD_AND_TEST.md](./docs/developer/BUILD_AND_TEST.md)
- SDK policy: Steinberg VST3 SDK 3.8+ (MIT) only
- Primary automation tests: `npm run test:integration`
- Live room smoke test: `npm run test:integration:live`

## Downloads

- Releases: https://github.com/steveseguin/vst/releases
- Docs landing page (when GitHub Pages is enabled): `docs/index.html`

## License and contributions

- License notice: [LICENCE.md](./LICENCE.md)
- Full AGPLv3 text: [AGPLv3.md](./AGPLv3.md)
- CLA / contribution policy: [CONTRIBUTING.md](./CONTRIBUTING.md)
