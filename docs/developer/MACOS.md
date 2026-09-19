# macOS VST3

The macOS target supports Apple Silicon (`arm64`) and Intel (`x86_64`). The scripted
build targets macOS 11 or later. Use a VST3 host such as REAPER or Studio One;
Logic Pro and GarageBand require an Audio Units plugin, which this project does
not currently provide. Audio processing is 32-bit float, as on Windows.

See [HARDENING_VALIDATION.md](HARDENING_VALIDATION.md) for current host journeys,
audio measurements and remaining gaps. [MACOS_VALIDATION.md](MACOS_VALIDATION.md)
is the historical initial-port report.

## Requirements and build

- Xcode or its Command Line Tools, with an installed macOS SDK and C++20 compiler.
- CMake 3.25+ (required by VST3 SDK 3.8), Ninja, Git, Perl, curl and make.
- Node.js 22+ for the JavaScript integration tests.
- Allow about 2 GB for a single architecture, and extra space for a second build.

```bash
brew install cmake ninja node
bash scripts/build_macos.sh
```

The script fetches the MIT-licensed VST3 SDK 3.8 and pinned OpenSSL 3.5.8 sources,
checks the OpenSSL archive SHA-256, and builds static OpenSSL for the selected
architecture and deployment target. CMake fetches libdatachannel, Opus,
IXWebSocket and nlohmann_json. The plugin uses Apple's system trust store for
WebSocket TLS; no Homebrew libraries or CA files are required on users' Macs.
Build dependencies and object files stay under `build/`; the SDK is in `vst3sdk/`.
No sudo or signing account is needed for a local build.

Outputs:

- `build/webrtc_vst_mac/VST3/Release/webrtc_vst.vst3`
- `build/webrtc_vst_mac/bin/Release/webrtc_vst_cli_host`
- Native test executables in the same `bin/Release` directory.

The script runs integration, stress, fuzz and Cocoa editor tests. The editor test
attaches the actual bundled UI to a native window and closes it five times.
Run it in a logged-in macOS GUI session.

To build the other architecture into a separate directory:

```bash
WEBRTC_MAC_BUILD_DIR="$PWD/build/webrtc_vst_mac_x86_64" \
WEBRTC_MAC_DEPS_DIR="$PWD/build/webrtc_vst_mac/_deps" \
  bash scripts/build_macos.sh x86_64
```

On Apple Silicon, running Intel tests requires Rosetta 2. Intel Macs cannot run
Apple Silicon binaries; use the native arm64 CI runner for those tests. The two
architectures have separate archives so hosts can select the matching build.

Overrides: `VST3_SDK_ROOT`, `WEBRTC_MAC_BUILD_DIR`, `WEBRTC_MAC_DEPS_DIR`,
`CMAKE_BUILD_PARALLEL_LEVEL` (default 4), `MACOSX_DEPLOYMENT_TARGET` (default 11.0),
and `OPENSSL_ROOT_DIR`. Supply OpenSSL static libraries for the same architecture
and OS target. When packaging a custom OpenSSL build, set `OPENSSL_LICENSE_FILE`
to its license notice. Homebrew bottles may have a newer minimum OS even if the plugin
itself is compiled for 11.0; use the scripted source build for distribution.

## Audio and host validation

```bash
npm ci
npm run test:security
npm run test:sdk
npm run test:play-request-audio-only
npm run test:integration:local
```

The last two commands use live VDO.Ninja signaling. Local integration creates its
own tone publishers and verifies plugin-to-plugin and plugin-to-SDK audio.
`npm run test:integration` additionally needs an active publisher named by
`WEBRTC_TEST_STREAM_ID`; `npm run test:integration:live` needs a live room/source.
Set `WEBRTC_TEST_BUILD_DIR=build/webrtc_vst_mac_x86_64` to test the Intel build.
The CLI accepts a `.vst3` **bundle directory**, not its `Contents/MacOS` executable.

```bash
bash scripts/install_macos.sh
```

This installs in `~/Library/Audio/Plug-Ins/VST3/`, preserving an existing plugin in
`~/Library/Application Support/VDO.Ninja/VST3 Backups/`. Close the DAW before
installing; restart it and rescan afterward. Build configuration does not install
SDK symlinks. Verify both Seed and Play, state reload, Copy/Open Viewer Link,
QR code, and repeated editor open/close in REAPER or Studio One. Also perform the
Audacity smoke check before a public release. Native Cocoa tests complement these
checks; they do not certify a particular DAW or the oldest supported OS.

## Packaging, signing and notarization

```bash
bash scripts/package_macos.sh
```

The resulting `build/release/*-development.zip` contains an ad-hoc-signed bundle
and dependency license notices. The packager rejects external dylib dependencies
and refuses to overwrite existing archives. It copies the build before signing.
Development artifacts are for testing, not public releases.

To create a Developer ID signed ZIP, set `MACOS_SIGNING_IDENTITY`. For a notarized
and stapled disk image, also set `MACOS_NOTARY_PROFILE` to an existing
`notarytool` keychain profile:

```bash
MACOS_SIGNING_IDENTITY='Developer ID Application: Your Name (TEAMID)' \
MACOS_NOTARY_PROFILE='your-notary-profile' \
  bash scripts/package_macos.sh
```

The script enables the hardened runtime and timestamps the signature, submits
the DMG to Apple, waits, and staples/validates the ticket. A VST3 bundle cannot be
stapled directly, so the disk image carries the ticket. See Apple's
[notarization documentation](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
Keep signing keys and credentials in Keychain or CI secrets, never in this repo.
Signing and notarization are separate: a `*-signed.zip` is not notarized.

`.github/workflows/macos.yml` builds and tests on native Apple Silicon and Intel
runners, then uploads development ZIPs. It does not publish releases or require
signing secrets. The Windows release workflow remains separate. Before public
distribution, complete the repository's manual host gates, notarize, submit the
final artifact to VirusTotal, and record those results internally.
