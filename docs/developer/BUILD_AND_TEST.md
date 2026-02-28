# Build And Test

This project targets a VST3 plugin build and integration validation flow.

## Prerequisites

- Windows + Visual Studio 2022 C++ toolchain
- CMake 3.24+
- A local VST3 SDK checkout at 3.8+ (MIT), recommended path: `./vst3sdk` (not committed)
- OpenSSL development libs (Windows: vcpkg static is supported out of the box)
- Node.js (for integration tests in `tools/tests`)

Recommended SDK checkout:

```powershell
git clone --branch v3.8.0_build_66 --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
```

Optional OpenSSL setup via vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\\deps\\vcpkg"
"$env:USERPROFILE\\deps\\vcpkg\\vcpkg.exe" install openssl:x64-windows-static
$env:VCPKG_ROOT = "$env:USERPROFILE\\deps\\vcpkg"
```

## Configure + build (Windows)

```powershell
cmake -S webrtc_vst -B build/webrtc_vst_win `
  -G "Visual Studio 17 2022" `
  -DCMAKE_BUILD_TYPE=Release `
  -DVST3_SDK_ROOT="$PWD\vst3sdk"

cmake --build build/webrtc_vst_win --config Release --target webrtc_vst webrtc_vst_cli_host
```

## Test commands

```powershell
npm install
npm run test:integration
npm run test:integration:live
```

Notes:

- `test:integration` runs the local gate (`audio-only` + loopback/publish tests).
- `test:integration:live` is a live room smoke test and depends on network + active remote audio source.

## Audacity smoke check

1. Copy `build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3` into the system VST3 folder.
2. Launch Audacity and load the plugin.
3. Set Play mode and a known live stream ID.
4. Verify audio playback and host stability during repeated open/close cycles.
