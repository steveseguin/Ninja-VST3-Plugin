#!/bin/bash
set -euo pipefail
# Build/test, Developer ID sign, notarize and staple both Mac architectures.
# Configure credentials once using scripts/setup_macos_notary.sh; no secrets in git.
root=$(cd "$(dirname "$0")" && pwd)
cd "$root"
[[ $(uname -s) == Darwin ]] || { echo "This script requires macOS." >&2; exit 1; }
package_only=false
case ${1:-} in
    "") ;;
    --package-only) package_only=true ;;
    *) echo "Usage: bash mac.sh [--package-only]" >&2; exit 1 ;;
esac
if [[ $package_only == false ]]; then
    if ! /usr/bin/arch -arm64 /usr/bin/true 2>/dev/null || ! /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
        echo "Building/testing both architectures requires Apple Silicon with Rosetta 2." >&2
        echo "For a single-architecture build, use scripts/build_macos.sh instead." >&2
        exit 1
    fi
fi
export MACOS_SIGNING_IDENTITY=${MACOS_SIGNING_IDENTITY:-'Developer ID Application: Steve Seguin (H3CKR5XB3J)'}
export MACOS_NOTARY_PROFILE=${MACOS_NOTARY_PROFILE:-vdoninja-notary}
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-1}
export WEBRTC_MAC_DEPS_DIR=${WEBRTC_MAC_DEPS_DIR:-"$root/build/webrtc_vst_mac/_deps"}
for arch in arm64 x86_64; do
    build_dir="$root/build/webrtc_vst_mac"
    if [[ $arch == x86_64 ]]; then build_dir="$root/build/webrtc_vst_mac_x86_64"; fi
    if [[ $package_only == false ]]; then
        # The first build downloads dependencies; don't override them until present.
        if [[ -d "$WEBRTC_MAC_DEPS_DIR/libdatachannel-src" ]]; then
            WEBRTC_MAC_BUILD_DIR="$build_dir" WEBRTC_GUARD_SECONDS=3600 \
                node tools/tests/guarded_run.js bash scripts/build_macos.sh "$arch"
        else
            env -u WEBRTC_MAC_DEPS_DIR WEBRTC_MAC_BUILD_DIR="$build_dir" WEBRTC_GUARD_SECONDS=3600 \
                node tools/tests/guarded_run.js bash scripts/build_macos.sh "$arch"
        fi
    fi
    WEBRTC_GUARD_SECONDS=1800 node tools/tests/guarded_run.js bash scripts/package_macos.sh "$build_dir"
done
echo "Notarized DMGs are in build/release/. Verify and submit them to VirusTotal before publishing."
