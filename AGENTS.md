# AGENTS README

This repository produces the **WebRTC Bridge** VST3 plug-in, a bridge that embeds VDO.Ninja WebRTC streams inside audio hosts such as Audacity. The plug-in relies on libdatachannel for media transport and ixwebsocket for signaling.

## Purpose
- Allow desktop audio hosts to receive or publish VDO.Ninja streams.
- Provide both Play (receive) and Seed (send) modes with optional encryption.
- Bundle a CLI harness for headless smoke testing outside of a DAW.

## Build Prerequisites
- Visual Studio 2022 Build Tools with the MSVC 14.43+ toolchain.
- CMake = 3.24.
- vcpkg clone at `C:/Users/steve/Code/gpt/deps/vcpkg`.
- Steinberg VST3 SDK clone at `C:/Users/steve/Code/gpt/vst/vst3sdk`.

Install static OpenSSL to avoid the MSVC runtime crash seen with the DLL CRT:

```powershell
cd C:/Users/steve/Code/gpt/deps/vcpkg
./vcpkg.exe install openssl:x64-windows-static
```

## Configure + Build (Release)

```powershell
cd C:/Users/steve/Code/gpt/vst
cmake -S webrtc_vst -B build/webrtc_vst_win `
  -G "Visual Studio 17 2022" -A x64 `
  -DVST3_SDK_ROOT=C:/Users/steve/Code/gpt/vst/vst3sdk `
  -DOPENSSL_ROOT_DIR=C:/Users/steve/Code/gpt/deps/vcpkg/installed/x64-windows-static `
  -DOPENSSL_USE_STATIC_LIBS=TRUE `
  -DUSE_OPEN_SSL=ON -DUSE_MBED_TLS=OFF -DUSE_TLS=ON -DUSE_ZLIB=OFF

# Force /MT on every generated project (libdatachannel, ixwebsocket, opus, plug-in, CLI host)
./tools/scripts/force_static_runtime.ps1 build/webrtc_vst_win

cmake --build build/webrtc_vst_win --config Release --target webrtc_vst webrtc_vst_cli_host
```

> **Why /MT matters:** The original crash occurred inside `MSVCP140!mtx_do_lock` when ixwebsocket constructed its internal mutex while Audacity hosted the plug-in. Rebuilding **all** dependencies with the static CRT and stripping the runtime DLLs resolves the crash. Re-run `force_static_runtime.ps1` whenever CMake regenerates the solutions.

## Deploying for Audacity
1. Build output: `build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3` (no extra DLLs inside `Contents/x86_64-win`).
2. Copy the entire `webrtc_vst.vst3` bundle to the user VST3 folder:
   `C:\Users\steve\AppData\Local\Programs\Common\VST3\`
3. Launch Audacity ? `Effect ? Add/Remove Plug-ins…` ? enable “WebRTC Bridge”.
4. Audacity will load successfully with the static build; no system runtime edits required.

## Testing
- **Headless smoke test:** `build\webrtc_vst_win\bin\Release\webrtc_vst_cli_host.exe` (prints class info, exits `0`).
- **Audacity:** enable the effect, insert it on a track, toggle between Play/Seed and ensure no crash. For Play mode validate audio flows once a VDO.Ninja stream is configured.
- **Validator (optional):** `build\webrtc_vst_win\bin\Release\validator.exe build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3`

## Release Checklist
1. Run static build steps above; verify `dumpbin /dependents` on `webrtc_vst.vst3` to ensure CRT DLLs are gone.
2. Update version strings in `webrtc_vst/src/Version.h` when needed.
3. Test in Audacity after copying the bundle.
4. Package the `webrtc_vst.vst3` directory for distribution.

## Troubleshooting
- **CRT DLLs reappear:** Re-run `tools/scripts/force_static_runtime.ps1 build/webrtc_vst_win` and rebuild.
- **Audacity crashes upon enable:** confirm you copied the static build (no `msvcp*.dll` inside the bundle) and removed stale versions from `C:\Program Files\Common Files\VST3`.
- **WebRTC signaling issues:** check the VDO.Ninja URL/password parameters; rebuild with the CLI host to inspect console logs.

Maintaining the static toolchain and deployment steps outlined here prevents the mutex crash witnessed with mixed runtimes and keeps host integration predictable.
