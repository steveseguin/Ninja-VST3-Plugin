# macOS validation — 2026-09-19

Implemented and checked locally on an Apple Silicon Mac running macOS 26.4.1,
with Apple Clang 21, CMake 4.0.1, Ninja 1.12.1 and Node.js 22.14.0. The starting
repository revision was `80023c6` (project version 0.2.4).

## Results

| Check | Apple Silicon | Intel |
| --- | --- | --- |
| Release build, deployment target 11.0 | Passed | Passed |
| Native integration (9 cases) | Passed | Passed under Rosetta |
| Native stress (5 scenarios) | Passed | Passed under Rosetta |
| Native fuzz (6 cases) | Passed | Passed under Rosetta |
| Cocoa editor attach/render/close (5 cycles) | Passed | Passed under Rosetta |
| Plugin-to-plugin WebRTC tone loopback | Passed | Passed under Rosetta |
| Plugin publish to Node SDK audio receiver | Passed | Passed under Rosetta |
| Complete `npm run test:integration` | Passed | Local audio subset passed |
| `npm run test:integration:live` | Passed with an owned SDK tone publisher | Not run |
| Extracted Developer ID signed ZIP load test | Passed, 9 integration cases | Passed, 9 integration cases |
| Bundle signature, timestamp and hardened runtime | Verified | Verified |

The full integration run used a temporary SDK publisher with the default
VDO.Ninja password and explicit `salt: 'vdo.ninja'`. Direct-stream validation ran
before joining a temporary room for the live-room gate. The room receiver
produced RMS 0.173429 (minimum required: 0.01). Apple Silicon Opus configuration
was subsequently adjusted to use mandatory NEON without runtime probing;
native tests and both audio directions were rerun for the final build.

`otool -L` shows only Apple frameworks and `/usr/lib` dependencies. OpenSSL 3.5.8
is built statically from its SHA-256-verified source archive, targeting macOS 11.
`vtool` confirms the bundle's minimum OS of 11.0. No Homebrew dylib or external
OpenSSL provider is needed at runtime. The signed Apple Silicon build was
installed under the current user's `Library/Audio/Plug-Ins/VST3` directory.

The Node SDK option-forwarding test originally crashed during native wrtc media
finalization on this Mac. It now uses a JavaScript media-stream fixture for its
option/state assertions; real media remains covered by the network audio tests.

## Remaining release checks

- Real Intel hardware and macOS 11 were not available; Rosetta/current macOS
  testing does not replace those compatibility checks.
- No REAPER, Studio One or Audacity was installed, so manual DAW routing,
  state recall, browser-link/clipboard buttons and host open/close need validation.
- The CI workflow has been added but was not executed on GitHub in this session.
- Local ZIPs have Developer ID signatures but are **not notarized**. The optional
  notarytool/DMG path is implemented, but no notarization profile was supplied
  or used during this validation.
- No VirusTotal submission or public release was performed.
- Windows and Linux were not rebuilt in this session; Linux release work remains
  outside this change. Audio Units is not included.

About 1.3 GB of regenerable Xcode module cache and cached Electron ZIP downloads
was removed to make room. No project sources or personal documents were removed.
The SDK, Node packages and both build trees occupy about 1.6 GB; roughly 1.8 GB
of disk space remained after validation.
