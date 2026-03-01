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

## Release security gate (Windows)

Do not publish unsigned installers/artifacts.

Required GitHub repository secrets for `.github/workflows/release.yml`:

- `WINDOWS_SIGN_PFX_BASE64` (base64-encoded `.pfx`)
- `WINDOWS_SIGN_PFX_PASSWORD`
- `VIRUSTOTAL_API_KEY` (preferred) or `VT_API_KEY` (fallback)

Optional GitHub repository variable:

- `WINDOWS_SIGN_TIMESTAMP_URL` (defaults to `http://timestamp.digicert.com`)

Release workflow policy:

1. Build plugin.
2. Run native integration + stress tests.
3. Sign release binaries (plugin binary and installer artifacts when present).
4. Verify signatures are present and timestamped.
5. Package artifacts.
6. Submit packaged release assets to VirusTotal.
7. Publish release.

## Local deployment (Windows)

Default (no admin required):

```powershell
.\deploy_plugin.ps1 -Scope User
```

System-wide (admin required):

```powershell
.\deploy_plugin.ps1 -Scope System
```

## Manual release flow (local, no CI wait)

1. Build release plugin:

```powershell
cmake --build build/webrtc_vst_win --config Release --target webrtc_vst
```

2. Sign plugin binary (use cert material from `../code-signing/secrets/decrypted`):

```powershell
signtool sign /fd SHA256 /f "..\code-signing\secrets\decrypted\certs\socialstream.pfx" /p "<pfx_password>" /tr "http://timestamp.digicert.com" /td SHA256 "build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3\Contents\x86_64-win\webrtc_vst.vst3"
```

3. Verify signature/timestamp:

```powershell
signtool verify /pa /v "build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3\Contents\x86_64-win\webrtc_vst.vst3"
```

4. Package artifact:

```powershell
tar -a -cf build\release\webrtc_vst-v<version>-windows-vst3.zip -C build\webrtc_vst_win\VST3\Release webrtc_vst.vst3
```

5. Submit to VirusTotal:

```powershell
curl.exe --request POST --url https://www.virustotal.com/api/v3/files --header "x-apikey: <VT_API_KEY>" --form "file=@build\release\webrtc_vst-v<version>-windows-vst3.zip"
```

6. Publish release manually:

```powershell
gh release create v<version> build\release\webrtc_vst-v<version>-windows-vst3.zip --repo steveseguin/vst --title "v<version>" --notes "Manual local release"
```

If you source cert material from `../code-signing`, do not commit decrypted keys or passphrases to this repository.

If an environment still reports missing `VT_API_KEY`, it is usually using an older decrypted signing bundle. Refresh it:

```bash
export CODESIGN_BUNDLE_PASSPHRASE='<your passphrase>'
bash scripts/unlock-bundle.sh
```
