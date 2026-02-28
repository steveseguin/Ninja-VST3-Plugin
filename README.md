# WebRTC VST for VDO.Ninja

Audio-first VST3 bridge between DAW hosts and VDO.Ninja signaling/WebRTC transport.

## What this repo provides

- `webrtc_vst/`: VST3 plugin source.
- `tools/tests/integration/`: Integration tests, including audio-only play behavior checks.
- `docs/`: Marketing/download page intended for GitHub Pages.

## Build and test quick start

- Build/test doc: [docs/developer/BUILD_AND_TEST.md](./docs/developer/BUILD_AND_TEST.md)
- Primary automation tests: `npm run test:integration`
- Live room smoke test: `npm run test:integration:live`

## Downloads

- Releases: https://github.com/steveseguin/vst/releases
- Docs landing page (when GitHub Pages is enabled): `docs/index.html`

## License and contributions

- License notice: [LICENCE.md](./LICENCE.md)
- Full AGPLv3 text: [AGPLv3.md](./AGPLv3.md)
- CLA / contribution policy: [CONTRIBUTING.md](./CONTRIBUTING.md)
