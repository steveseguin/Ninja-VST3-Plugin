#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || {
    echo "The release package currently targets Linux x86_64." >&2; exit 1;
}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(sed -n 's/^#define WEBRTC_VST_VERSION_STRING "\([^"]*\)".*/\1/p' webrtc_vst/src/Version.h)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
build_dir=${WEBRTC_LINUX_BUILD_DIR:-"$root/build/webrtc_vst_linux"}
bundle="$build_dir/VST3/Release/webrtc_vst.vst3"
binary="$bundle/Contents/x86_64-linux/webrtc_vst.so"
[[ -f "$binary" && -f "$bundle/Contents/Resources/webrtc_vst.uidesc" ]]
mkdir -p build/release
ldd "$binary" > build/release/linux-dependencies.txt
if grep -q 'not found' build/release/linux-dependencies.txt; then
    cat build/release/linux-dependencies.txt >&2; exit 1
fi
if grep -Eq 'lib(ssl|crypto|datachannel|opus)\.so' build/release/linux-dependencies.txt; then
    echo "Expected static media and crypto libraries." >&2; exit 1
fi
asset="webrtc_vst-v$version-linux-x86_64-vst3.tar.gz"
tar -czf "build/release/$asset" -C "$(dirname "$bundle")" webrtc_vst.vst3
(cd build/release && sha256sum "$asset" > "webrtc_vst-v$version-linux-SHA256SUMS.txt")
echo "Packaged build/release/$asset"
