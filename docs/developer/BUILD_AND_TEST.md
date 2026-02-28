# Build And Test

This project targets a cross-platform VST3 plugin build and integration validation flow.

## Prerequisites

- Windows + Visual Studio 2022 C++ toolchain OR Linux + GCC 11+
- CMake 3.24+
- A local VST3 SDK checkout at 3.8+ (MIT), recommended path: `./vst3sdk` (not committed)
- OpenSSL development libs (Windows: vcpkg static is supported out of the box. Linux: `libssl-dev`)
- Linux specific GUI dependencies: `sudo apt install libxcb-util-dev libxcb-cursor-dev libxcb-keysyms1-dev libxcb-xkb-dev libxkbcommon-dev libxkbcommon-x11-dev libpango1.0-dev libglib2.0-dev libcairo2-dev libgtkmm-3.0-dev`
- Node.js (for integration tests in `tools/tests`)

Recommended SDK checkout:

```bash
git clone --branch v3.8.0_build_66 --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
```

Optional OpenSSL setup via vcpkg (Windows):

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

## Configure + build (Linux)

```bash
cmake -S webrtc_vst -B build/webrtc_vst_linux \
  -DCMAKE_BUILD_TYPE=Release \
  -DVST3_SDK_ROOT="$PWD/vst3sdk"

cmake --build build/webrtc_vst_linux --config Release --target webrtc_vst webrtc_vst_cli_host
```

## Test commands

```bash
npm install
npm run test:integration
npm run test:integration:live
```

Notes:

- `test:integration` runs the local gate (`audio-only` + loopback/publish tests). It is cross-platform aware.
- `test:integration:live` is a live room smoke test and depends on network + active remote audio source.

## Audacity smoke check

1. Copy `build/webrtc_vst_<os>/VST3/Release/webrtc_vst.vst3` into your system VST3 folder.
2. Launch Audacity and load the plugin.
3. Set Play mode and a known live stream ID.
4. Verify audio playback and host stability during repeated open/close cycles.
