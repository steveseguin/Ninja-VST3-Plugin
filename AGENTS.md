# WebRTC VST - AGENTS.md

## 1) Project Summary

This repository builds a VST3 plugin that bridges DAW audio to/from VDO.Ninja over WebRTC.

- Seed mode: publish DAW audio to a stream/room.
- Play mode: receive remote audio into the DAW return path.
- Core stack: VST3 SDK + libdatachannel + Opus + IXWebSocket + nlohmann_json + OpenSSL.
- License model in repo: AGPLv3 for this project code.

Primary practical use-cases:

- Remote talkback / IFB
- Voiceover direction and mix review
- Lightweight collaboration without self-hosted signaling

Current version macro: `0.1.1` (`webrtc_vst/src/Version.h`).

## 2) Repository Map

- `webrtc_vst/` - plugin source, VST3 target, controller UI, WebRTC session.
- `tools/cli_host/` - headless host that loads and runs the plugin without a DAW.
- `tools/tests/` - JS integration tests (CLI + signaling/live checks).
- `tests/` - native C++ integration + stress tests simulating host behavior.
- `docs/` - GitHub Pages landing/download site.
- `.github/workflows/release.yml` - Windows release packaging workflow.
- `deploy_plugin.ps1` - local Windows deployment to system VST3 folder.

## 3) Runtime Behavior (Current State)

- Mode selection: Play or Seed.
- Stream/session config exposed as VST parameters and persisted in state JSON.
- Password/encryption behavior:
  - Empty password = default VDO.Ninja behavior.
  - Password values `0`, `off`, or `false` force disable-encryption behavior.
- Auto reconnect is implemented for signaling disconnects:
  - Exponential backoff with capped attempts.
  - Re-seeds/re-plays using same stream/room semantics.
- Outgoing FIFO is capped to prevent unbounded growth.
- Incoming playback uses per-peer jitter pre-fill before mixing starts.
- Incoming RED payload primary extraction is supported on receive path.

## 4) Host Guidance

- Preferred hosts for validation: Reaper, Studio One.
- Audacity: use as a smoke check only; open/close behavior can be brittle.
- Process path today is effectively 32-bit float:
  - `kSample64` is not supported.
  - The processor explicitly reports `kSample32`-only support via `canProcessSampleSize()`.

## 5) Build Prerequisites

- Windows:
  - Visual Studio 2022 C++ toolchain
  - CMake 3.24+
  - VST3 SDK 3.8+ (MIT license variant)
  - OpenSSL dev libraries (vcpkg static path supported)
- Linux:
  - GCC 11+ (or compatible C++20 compiler), CMake 3.24+
  - VST3 SDK 3.8+ (MIT)
  - OpenSSL dev libs
  - Additional GUI/system libs listed in `docs/developer/BUILD_AND_TEST.md`
- Node.js is required for JS integration tests in `tools/tests`.

Recommended SDK checkout:

```bash
git clone --branch v3.8.0_build_66 --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
```

## 6) Configure + Build

Windows:

```powershell
cmake -S webrtc_vst -B build/webrtc_vst_win `
  -G "Visual Studio 17 2022" `
  -DCMAKE_BUILD_TYPE=Release `
  -DVST3_SDK_ROOT="$PWD\vst3sdk"

cmake --build build/webrtc_vst_win --config Release --target webrtc_vst webrtc_vst_cli_host
```

Linux:

```bash
cmake -S webrtc_vst -B build/webrtc_vst_linux \
  -DCMAKE_BUILD_TYPE=Release \
  -DVST3_SDK_ROOT="$PWD/vst3sdk"

cmake --build build/webrtc_vst_linux --config Release --target webrtc_vst webrtc_vst_cli_host
```

Key CMake toggle:

- `-DWEBRTC_VST_BUILD_TESTS=ON|OFF` (default ON)

## 7) Artifacts

- Windows plugin bundle:
  - `build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3`
  - Binary inside bundle:
    `build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3/Contents/x86_64-win/webrtc_vst.vst3`
- Linux bundle:
  - `build/webrtc_vst_linux/VST3/Release/webrtc_vst.vst3`
- CLI host:
  - `build/webrtc_vst_win/bin/Release/webrtc_vst_cli_host.exe` (Windows)
  - `build/webrtc_vst_linux/bin/Release/webrtc_vst_cli_host` (Linux)
- Native tests:
  - `build/webrtc_vst_win/tests/Release/webrtc_vst_integration_test.exe`
  - `build/webrtc_vst_win/tests/Release/webrtc_vst_stress_test.exe`

## 8) How To Run (No DAW)

Quick scripted runs:

- Seed: `run_cli_seed.ps1`
- Play: `run_cli_play.ps1`

Direct run:

```powershell
.\build\webrtc_vst_win\bin\Release\webrtc_vst_cli_host.exe
```

Useful CLI host env vars:

- `WEBRTC_CLI_HOST_ITERATIONS`
- `WEBRTC_CLI_HOST_RUNTIME_MS`
- `WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS`
- `WEBRTC_CLI_HOST_TIMEOUT_MS`
- `WEBRTC_CLI_HOST_BLOCK_SLEEP_MS`
- `WEBRTC_CLI_HOST_WARMUP_MS`
- `WEBRTC_CLI_HOST_TONE_HZ`
- `WEBRTC_CLI_HOST_MONITOR_OUTPUT`
- `WEBRTC_CLI_HOST_MODE` / `WEBRTC_CLI_HOST_STREAM_ID` / `WEBRTC_CLI_HOST_ROOM` / `WEBRTC_CLI_HOST_PASSWORD` (state injection helpers)

## 9) Plugin Config Reference

Primary plugin env vars:

- `WEBRTC_VST_MODE` (`play` or `seed`)
- `WEBRTC_VST_STREAM_ID`
- `WEBRTC_VST_ROOM_NAME` (legacy alias: `WEBRTC_VST_ROOM_ID`)
- `WEBRTC_VST_HANDSHAKE_URL` (legacy alias: `WEBRTC_VST_SIGNALING_URL`)
- `WEBRTC_VST_PASSWORD`
- `WEBRTC_VST_DISABLE_ENCRYPTION`
- `WEBRTC_VST_LOG_STDOUT`
- `WEBRTC_VST_LOG_SIGNALING`
- `WEBRTC_VST_LOG_SIGNALING_RAW`
- `WEBRTC_VST_DISABLE_STUN` (used by loopback test scenarios)

Controller parameters exposed in plugin UI/state:

- Connection Mode
- Stream ID
- Room Name
- Handshake URL
- Password
- Disable Encryption (derived/read-only behavior)
- Status (read-only)

## 10) Test Strategy

### 10.1 Native C++ host-simulation tests

Build:

```powershell
cmake --build build/webrtc_vst_win --config Release --target webrtc_vst webrtc_vst_integration_test webrtc_vst_stress_test
```

Run:

```powershell
.\build\webrtc_vst_win\tests\Release\webrtc_vst_integration_test.exe
.\build\webrtc_vst_win\tests\Release\webrtc_vst_stress_test.exe
```

Or:

```powershell
cd build\webrtc_vst_win
ctest -C Release --output-on-failure
```

Integration suite intent:

- load/unload
- activate/deactivate
- process blocks
- rapid open/close
- process during deactivate
- long-running session

Stress suite intent:

- rapid create/destroy
- concurrent instances
- rapid activate/deactivate loops
- repeated memory/leak-pressure cycles
- concurrent create/destroy

Helper script:

```powershell
.\tests\run_tests.ps1
```

Flags:

- `-BuildOnly`
- `-TestOnly`
- `-Verbose`
- `-StressOnly`

### 10.2 JS integration tests

Install:

```bash
npm install
```

Run main gates:

```bash
npm run test:integration
npm run test:integration:live
```

Script groups in `package.json`:

- `test:integration:audio-only`
- `test:integration:local`
- `test:integration:live`
- `test:publish-audio`
- `test:cli-loopback`
- `test:play-request-audio-only`
- `test:play-ignore-video`

Test env highlights:

- `WEBRTC_TEST_WSS`
- `WEBRTC_TEST_STREAM_ID`
- `WEBRTC_TEST_ROOM_NAME`
- `WEBRTC_TEST_PASSWORD`
- `WEBRTC_TEST_RUNTIME_MS`
- `WEBRTC_TEST_TIMEOUT_MS`
- `WEBRTC_TEST_MIN_RMS`
- `WEBRTC_TEST_DISABLE_STUN`

### 10.3 Manual DAW smoke tests

Recommended release-gate manual checks:

1. Reaper or Studio One:
   - Load plugin in Play mode with known live stream.
   - Verify sustained audio and no UI freeze.
2. Reaper or Studio One:
   - Load plugin in Seed mode and verify browser/viewer receives tone/audio.
3. Audacity smoke:
   - Repeated open/close cycles in Play mode with live stream.
   - Watch for hang/crash during activate/deactivate.

## 11) Deploy

### 11.1 Local Windows deploy

Use administrator PowerShell:

```powershell
.\deploy_plugin.ps1
```

Script copies:

- Source: `build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3`
- Destination: `C:\Program Files\Common Files\VST3\`

### 11.2 GitHub release deploy

Automated workflow: `.github/workflows/release.yml`

Trigger:

- Push tag matching `v*` (or manual workflow dispatch)

Current workflow behavior:

- Windows-only build (`windows-2022`)
- Builds `webrtc_vst`
- Enforces Authenticode signing before packaging
- Verifies signatures (`Get-AuthenticodeSignature`) before publish
- Zips the signed `.vst3` bundle
- Auto-detects installer artifacts (`*setup*.exe`, `*installer*.exe`, `*.msi`) and signs/attaches them
- Submits release assets to VirusTotal before publish
- Publishes assets to GitHub Releases via `softprops/action-gh-release`

## 12) Signing / Notarization Status

Current state:

- Windows release workflow now requires signing + VirusTotal submission.
- Required repository secrets:
  - `WINDOWS_SIGN_PFX_BASE64`
  - `WINDOWS_SIGN_PFX_PASSWORD`
  - `VIRUSTOTAL_API_KEY` (preferred) or `VT_API_KEY` (fallback)
- Optional repository variable:
  - `WINDOWS_SIGN_TIMESTAMP_URL` (defaults to `http://timestamp.digicert.com`)
- No macOS notarization pipeline exists yet.

For local/manual releases, apply the same policy: sign first, verify signature, then submit artifact(s) to VirusTotal before distribution.

Example path to sign on Windows:

- `build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3\Contents\x86_64-win\webrtc_vst.vst3`

## 13) Known Problems (Open)

High-impact correctness/compatibility:

- 64-bit sample processing path is not supported; non-32-bit sample sizes are explicitly rejected.
- Peer session cap is now enforced (current hard limit: 16); make this configurable if needed.

Audio-thread/performance risks:

- Scratch buffers are now preallocated/reused in `process`, `pushOutgoingAudio`, and `pullIncomingAudio`;
  capacity growth can still occur when channel or block-size requirements increase.
- Spin-lock usage in/around audio-sensitive code can cause contention under load.

Resilience gaps:

- Opus PLC concealment is now used on pull underrun; monitor quality/CPU tradeoffs under load.
- Peer-level recovery in Play mode now refreshes `play` requests on peer disconnect/fail/close; no advanced per-peer backoff policy yet.
- Reconnect/backoff is present for signaling disconnects but still basic.

UX/observability:

- Minimal status telemetry in UI (limited user-visible diagnostics).
- Buffer/fill level diagnostics are not surfaced in UI.

Platform/release gaps:

- CI release automation is Windows only.
- No first-class macOS build/sign/notarize pipeline.

## 14) Nice-To-Haves / Backlog

1. Add full 64-bit processing support:
   - Current behavior intentionally rejects non-32-bit sample sizes.
2. Add configurable peer cap and optional eviction strategy.
3. Extend allocation audit to cover all real-time paths (including rare resize cases).
4. Tune PLC/recovery behavior with telemetry and guardrails (quality vs CPU).
5. Improve peer reconnect/ICE restart handling beyond basic play-refresh recovery.
6. Add richer UI status:
   - connected / disconnected / publishing / receiving / error code.
7. Make jitter pre-fill and buffering tunable.
8. Expose bitrate and transport tuning controls.
9. Implement or remove currently-unused `enableAec` config field.
10. Add macOS release pipeline with signing/notarization.

## 15) Threading Notes

Reference: `THREADING.md`

Important rules:

- Audio thread must avoid blocking and avoid heavy allocations.
- All callback paths should early-exit on shutdown flags.
- Keep lock hold times extremely short.
- Start/stop lifecycle sequencing is critical to avoid callback-after-destroy races.

## 16) Release Gate Checklist

Before publishing:

1. Clean configure/build succeeds for target OS.
2. Native integration + stress tests pass.
3. JS integration gate passes (`npm run test:integration`).
4. Live-room verification passes (`npm run test:integration:live`) or is explicitly waived.
5. Manual host smoke in Reaper/Studio One completed.
6. Audacity smoke open/close cycle completed.
7. Artifact packaging verified.
8. Authenticode signing completed and signature validity verified.
9. Release artifacts submitted to VirusTotal and analysis URLs captured in release notes.
10. Release notes include known limitations.

## 17) Troubleshooting Quick Notes

- Build fails on SDK detection:
  - Verify `VST3_SDK_ROOT` points to SDK root with `cmake/modules/SMTG_VST3_SDK.cmake`.
  - Ensure SDK license file includes MIT license text (3.8+).
- Build fails on OpenSSL:
  - Set `VCPKG_ROOT` (Windows) or install `libssl-dev` (Linux).
- Release fails with missing `VT_API_KEY`/`VIRUSTOTAL_API_KEY` in environments using `../code-signing`:
  - Decrypted bundle is likely stale; rerun:
    ```bash
    export CODESIGN_BUNDLE_PASSPHRASE='<your passphrase>'
    bash scripts/unlock-bundle.sh
    ```
- Plugin appears silent in Play:
  - Confirm stream/room/password correctness.
  - Enable `WEBRTC_VST_LOG_STDOUT=1` and `WEBRTC_VST_LOG_SIGNALING=1`.
  - Run CLI host with `WEBRTC_CLI_HOST_MONITOR_OUTPUT=1` to inspect RMS.
- Loopback/local tests flaky:
  - Use `WEBRTC_TEST_DISABLE_STUN=1` for host-only ICE in local dual-CLI tests.
  - Increase timeout/runtime env values.

## 18) Practical Defaults

When in doubt for developer validation:

1. Build Release for Windows.
2. Run native integration + stress tests.
3. Run `npm run test:integration`.
4. Run one manual Reaper seed/play smoke.
5. Deploy with `deploy_plugin.ps1` for local host validation.
