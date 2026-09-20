# Public macOS v0.2.5 download / installation smoke test

Tested September 19, 2026 on Apple M1, macOS 26.4.1, REAPER 7.80 and
Chrome 153.0.8010.50. These tests loaded the public release downloads, not the
developer build. Intel execution on this machine uses Rosetta.

## Download and installation scope

An isolated Chrome profile clicked both Mac download buttons on the live
landing page. Both downloads matched the public SHA-256 manifest. The landing
page and getting-started page exposed the expected versioned download links.
Both DMGs passed stapled-ticket validation and Gatekeeper assessment as
`Notarized Developer ID`, signed by Steve Seguin / team `H3CKR5XB3J`.

Headless browser download saving did not retain the normal quarantine xattr.
A current-timestamp quarantine flag was explicitly applied before mounting
the DMGs read-only. Entire bundles were copied with `ditto` into isolated VST3
installation directories and the DMGs ejected before host tests. Quarantine
remained present; it was not removed to make loading succeed. Both bundle
signatures verify strictly, identify version 0.2.5 and the correct architecture,
and require only macOS system runtime libraries.

**The user's normal 0.2.4 installation and open REAPER session were left
unchanged.** This validates a copied installation with a configured host scan
path, not replacement of the normal user-folder installation or a clean-Mac
Finder installation. Permission to close/save the user's session was not
assumed. A standard-folder upgrade should wait until that session is closed.

Public artifact hashes are recorded in [MACOS_RELEASE_0.2.5.md](MACOS_RELEASE_0.2.5.md).
The tested arm64 executable hash is
`646eeeb432cae2f57c0b2e0732165dca161d090cb1e32db2a24bb29fc39cb834`.
Local evidence is under `build/test-tools/download-smoke-MXXXal/`.

## P0/P1 journeys

| Journey | Result / evidence |
|---|---|
| Public website download links, arm64 and Intel | Pass: `results.json`, public checksums, notarization and signature checks |
| Native host load / lifecycle / formats / state | Both architectures: 14/14 cases, including 128 format/block/channel combinations; `native-arm64.log`, `native-intel.log` |
| pluginval strictness 10 | Both architectures passed, including parameter fuzzing; `pluginval-arm64.log`, `pluginval-intel.log` |
| Real Cocoa Advanced settings / Apply / Copy | Pass: actual Unicode text/password entry, custom domain/salt/WSS; `ui.log`, `copied-link.json` |
| QR and host automation side effects | Pass: independent Apple Vision decode equals copied URL; 200 host Copy/QR parameter cycles caused no browser/clipboard side effects |
| Default browser interoperability | Pass in both directions on `vdo.ninja` / primary signaling, blank password and automatic salt; `browser-default.log`, `browser-default-results.json` |
| Actual copied link / password prompt / backup service | Pass: typed password accepted by the real backup website, sustained browser-to-plugin audio with custom Unicode salt; `browser-ui.log`, `browser-ui-results.json` |
| REAPER two-instance Seed → WebRTC → Play | Pass: exact downloaded bundle path checked, live bypass/re-enable, repeated editor open/close, saved-project reload and 21 sustained audio windows; `reaper-results/result.txt` |
| REAPER saved-project reopening after process exit | Pass: persisted settings and 13 sustained audio windows after launching a new isolated host with blank startup-project configuration; `reaper-results/result-restart.txt` |
| Measured playback quality | Pass: 60-second stereo run, 48 kHz / 256 frames; `playback-quality-results.json` |

The playback probe measured 60.21 ms median and 61.21 ms p95 one-way software
latency, 57.90–61.64 ms range (3.74 ms spread, 25 markers). All 9,562 analyzed
steady-state blocks were non-silent, finite and unclipped. Left/right tone
residual median was 0.458% / 0.656%, p95 0.626% / 0.898%, within the test gates.
This is a local encrypted WebRTC/Opus path using real backup signaling, not a
WAN/device round-trip measurement, perceptual listening test, or a promise of
zero jitter/distortion on arbitrary hardware/networks.

## Findings and remaining concerns

- No P0/P1 product blocker was observed in these tested journeys.
- **Duplicate-installation trap:** REAPER initially selected the existing 0.2.4
  installation despite the separate test directory. Those runs were stopped
  and are not counted as release passes. REAPER's standard VST3 folders were
  placed before the isolated directory in this test profile's `vstpath_arm64`;
  exact `fx_ident` and mapped executable path then confirmed the downloaded
  bundle. Users upgrading should replace the intended copy and restart/rescan,
  not keep ambiguous duplicate installations in scanned folders.
- The browser harness previously could not request a genuinely blank test
  password because `||` substituted a synthetic password. It now permits an
  explicit empty value and omits password/hash URL parameters in that case.
- Added `WEBRTC_TEST_PLUGIN_BUNDLE` with two regression tests, exact REAPER
  loaded-path assertions, separate result directories, a cold-reopen mode and
  the repeatable public website download test. No plugin runtime code changed.
- Still unverified here: normal-folder replacement while preserving a closed
  user's projects, a separate clean Mac's first-run UI, native Intel hardware,
  macOS 11, other DAWs, Safari/mobile, physical audio devices, remote WAN/NAT,
  packet loss/reordering and long-running independent device clock drift.
- The release is VST3, not Audio Units; Logic/GarageBand remain unsupported.
  Match the download architecture to the DAW, including Rosetta. Separate thin
  builds are not a universal bundle for mixed native/Rosetta hosts.

All audio was generated test tone; no microphone/camera or user project was
recorded. Workloads ran sequentially behind the resource guard. No disk cleanup,
sudo, Gatekeeper bypass, new publication or modification of release assets was
needed. The earlier prolonged fuzz results remain separately documented in
[HARDENING_VALIDATION.md](HARDENING_VALIDATION.md); this pass does not claim a
new prolonged soak of the downloaded artifact.
