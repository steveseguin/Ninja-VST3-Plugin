# Install WebRTC VST

## macOS (Apple Silicon or Intel, macOS 11+)

1. Choose the `macos-arm64` build for a native Apple Silicon DAW, or `macos-x86_64`
   for an Intel DAW (including a DAW running under Rosetta).
2. Open the disk image or extract the ZIP, and copy the entire `webrtc_vst.vst3`
   bundle to `~/Library/Audio/Plug-Ins/VST3/`. In Finder, use Go → Go to Folder and
   enter that path; create the folder if needed.
3. Restart your DAW and rescan VST3 plugins. Select **VDO.Ninja WebRTC Bridge**.

No Homebrew or additional runtime libraries are needed. Use REAPER, Studio One
or another VST3 host. Logic Pro/GarageBand require Audio Units and cannot load
this VST3 plugin. Only 32-bit float audio processing is currently supported.

Public Mac downloads should be Developer ID signed and notarized. CI artifacts
named `*-development.zip` are ad-hoc signed test builds; `*-signed.zip` has a
Developer ID signature but has not been notarized. Building locally avoids
download quarantine; do not disable Gatekeeper globally for a test build.

For a local build, run `bash scripts/install_macos.sh`. See the
[macOS developer guide](docs/developer/MACOS.md) for building and packaging.

## Windows

### Option 1: Installer (recommended)

1. Download `webrtc_vst-windows-setup.exe` from Releases.
2. Run the installer.
3. Restart your DAW.

Default install path:

- `%LOCALAPPDATA%\\Programs\\Common\\VST3\\webrtc_vst.vst3`

### Option 2: Zip (manual install)

1. Download `webrtc_vst-vX.Y.Z-windows-vst3.zip`.
2. Extract `webrtc_vst.vst3`.
3. Copy `webrtc_vst.vst3` to one of:
   - `%LOCALAPPDATA%\\Programs\\Common\\VST3\\` (user scope)
   - `C:\\Program Files\\Common Files\\VST3\\` (system scope, admin)
4. Restart your DAW and rescan plugins if needed.
