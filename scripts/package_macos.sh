#!/bin/bash
set -euo pipefail

[[ $(uname -s) == Darwin ]] || { echo "This script requires macOS." >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$root/build/webrtc_vst_mac"}
build_dir=$(cd "$build_dir" && pwd)
bundle="$build_dir/VST3/Release/webrtc_vst.vst3"
binary="$bundle/Contents/MacOS/webrtc_vst"
[[ -f "$binary" ]] || { echo "Missing Release bundle: $bundle" >&2; exit 1; }
version=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$bundle/Contents/Info.plist")
archs=$(lipo -archs "$binary")
case "$archs" in
    arm64|x86_64) arch=$archs ;;
    "x86_64 arm64"|"arm64 x86_64") arch=universal ;;
    *) echo "Unexpected Mach-O architecture: $archs" >&2; exit 1 ;;
esac
# All non-system dependencies must be statically linked into the plugin.
while IFS= read -r dependency; do
    case "$dependency" in
        /System/Library/*|/usr/lib/*) ;;
        *) echo "Non-portable runtime dependency: $dependency" >&2; exit 1 ;;
    esac
done < <(otool -L "$binary" | awk '/compatibility version/ {print $1}' | sort -u)

identity=${MACOS_SIGNING_IDENTITY:--}
if [[ -n ${MACOS_NOTARY_PROFILE:-} && "$identity" == - ]]; then
    echo "Notarization requires MACOS_SIGNING_IDENTITY (Developer ID Application)." >&2
    exit 1
fi
mkdir -p "$root/build/release"
work=$(mktemp -d "$root/build/release/macos-stage.XXXXXX")
trap 'rm -r "$work"' EXIT
stage="$work/payload"
mkdir "$stage"
ditto "$bundle" "$stage/webrtc_vst.vst3"
cp "$root/LICENSE" "$root/INSTALL.md" "$root/README.md" "$stage/"
mkdir "$stage/licenses"
sdk=${VST3_SDK_ROOT:-"$root/vst3sdk"}
cp "$sdk/LICENSE.txt" "$stage/licenses/VST3-SDK.txt"
# FetchContent sources can be shared using WEBRTC_MAC_DEPS_DIR.
deps=${WEBRTC_MAC_DEPS_DIR:-"$build_dir/_deps"}
cp "$sdk/vstgui4/LICENSE" "$stage/licenses/VSTGUI.txt"
cp "$deps/libdatachannel-src/LICENSE" "$stage/licenses/libdatachannel.txt"
cp "$deps/libdatachannel-src/deps/libjuice/LICENSE" "$stage/licenses/libjuice.txt"
cp "$deps/libdatachannel-src/deps/libsrtp/LICENSE" "$stage/licenses/libsrtp.txt"
cp "$deps/libdatachannel-src/deps/usrsctp/LICENSE.md" "$stage/licenses/usrsctp.txt"
cp "$deps/libdatachannel-src/deps/plog/LICENSE" "$stage/licenses/plog.txt"
cp "$deps/opus-src/COPYING" "$stage/licenses/Opus.txt"
cp "$deps/ixwebsocket-src/LICENSE.txt" "$stage/licenses/IXWebSocket.txt"
cp "$deps/nlohmann_json-src/LICENSE.MIT" "$stage/licenses/nlohmann-json.txt"
cp "${OPENSSL_LICENSE_FILE:-$root/build/macos-deps/openssl-3.5.8/LICENSE.txt}" "$stage/licenses/OpenSSL.txt"
# This header carries the QR generator's complete MIT copyright/license notice.
cp "$root/webrtc_vst/src/qrcodegen.hpp" "$stage/licenses/qrcodegen.hpp"

sign_args=(--force --sign "$identity")
suffix=development
if [[ "$identity" != - ]]; then
    sign_args+=(--timestamp --options runtime)
    suffix=signed
fi
codesign "${sign_args[@]}" "$stage/webrtc_vst.vst3"
codesign --verify --strict --verbose=2 "$stage/webrtc_vst.vst3"
if [[ -n ${MACOS_NOTARY_PROFILE:-} ]]; then suffix=notarized; fi
name="webrtc_vst-v$version-macos-$arch-$suffix"
artifact="$root/build/release/$name"
if [[ -e "$artifact.zip" || -e "$artifact.dmg" ]]; then
    echo "Artifact already exists: $artifact (move it aside before packaging again)." >&2
    exit 1
fi
if [[ -n ${MACOS_NOTARY_PROFILE:-} ]]; then
    # Staple the disk image; standalone VST3 bundles are not supported by stapler.
    candidate="$work/$name.dmg"
    hdiutil create -volname "VDO.Ninja VST3" -srcfolder "$stage" -format UDZO "$candidate"
    codesign --force --sign "$identity" --timestamp "$candidate"
    xcrun notarytool submit "$candidate" --keychain-profile "$MACOS_NOTARY_PROFILE" --wait
    xcrun stapler staple "$candidate"
    xcrun stapler validate "$candidate"
    mv "$candidate" "$artifact.dmg"
    echo "$artifact.dmg"
else
    ditto -c -k --sequesterRsrc "$stage" "$work/$name.zip"
    mv "$work/$name.zip" "$artifact.zip"
    echo "$artifact.zip"
fi
