# v0.2.5 — notarized macOS VST3

Developer ID–signed and **Apple-notarized** VST3 builds for Apple Silicon and
Intel, targeting macOS 11+. Both disk images carry a validated stapled ticket
and pass Gatekeeper assessment. This is a Mac-only release; Windows downloads
remain unchanged. It remains marked as a prerelease while the compatibility
limitations below are outstanding.

- [Download for Apple Silicon](https://github.com/steveseguin/Ninja-VST3-Plugin/releases/download/mac-v0.2.5/webrtc_vst-v0.2.5-macos-arm64-notarized.dmg)
- [Download for Intel / Rosetta](https://github.com/steveseguin/Ninja-VST3-Plugin/releases/download/mac-v0.2.5/webrtc_vst-v0.2.5-macos-x86_64-notarized.dmg)
- [SHA-256 checksums](https://github.com/steveseguin/Ninja-VST3-Plugin/releases/download/mac-v0.2.5/webrtc_vst-v0.2.5-macos-SHA256SUMS.txt)

## Changes

- Advanced per-instance web domain, salt and signaling-server settings, with an
  explicit Apply button. Generated links support the backup service and custom
  deployments; Unicode passwords/salts are compatible with the browser client.
- Improved playback buffering and moved codec/network work off the host audio
  callback. Fixed REAPER prefetch silence and reconnect-related duplicate audio.
- Copy/QR actions now require an actual editor click. Host parameter changes
  cannot unexpectedly overwrite the clipboard or open browser tabs.
- Bounded and validated saved state, signaling input and peer/candidate queues;
  improved lifecycle handling and regression coverage.

## Validation

REAPER send/receive, bypass/editor cycling and saved-project reopening passed.
Real Chrome/VDO.Ninja browser audio passed in both directions, including custom
salts, a password prompt and wrong-salt rejection. Native suites and pluginval
strictness 10 passed for arm64 and x86_64 (Intel execution used Rosetta).

Before the version-only release rebuild, the same runtime changes passed a
30-minute sanitizer-assisted mutation/lifecycle run: 692 suites, zero failures.
A separate five-minute stereo soak had zero silent blocks after warmup despite
two signaling interruptions. Measured local one-way latency was about 68 ms;
that excludes audio-interface and WAN latency and does not promise zero jitter
or distortion. Native and extracted-artifact smoke tests are repeated for the
versioned, notarized release artifacts. Both clean GitHub architecture builds
and CodeQL checks also passed. Apple reported no notarization issues for either
disk image; final DMGs are submitted to VirusTotal before publication.

## Install and limitations

Open the DMG for your DAW's architecture and copy the entire
`webrtc_vst.vst3` bundle into `~/Library/Audio/Plug-Ins/VST3/`. Restart your DAW
and rescan. Use the Intel build for a DAW running under Rosetta.

Requires a VST3 host and 32-bit float processing. Logic Pro/GarageBand need Audio
Units and are not supported. Physical Intel hardware, macOS 11, other Mac DAWs
(including Audacity), remote WAN/restrictive NAT, independent device-clock drift
and full-stack sanitizers remain outside this validation. No TURN relay is
configured. See the [testing report](https://github.com/steveseguin/Ninja-VST3-Plugin/blob/mac-v0.2.5/docs/developer/HARDENING_VALIDATION.md) for
scope and remaining gaps. See the
[installation guide](https://steveseguin.github.io/Ninja-VST3-Plugin/getting-started.html#mac-install).
