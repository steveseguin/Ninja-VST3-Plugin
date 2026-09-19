#!/bin/bash
set -euo pipefail

[[ $(uname -s) == Darwin ]] || { echo "This script requires macOS." >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
arch=${1:-$(uname -m)}
case "$arch" in arm64|x86_64) ;; *) echo "Usage: $0 [arm64|x86_64]" >&2; exit 1 ;; esac
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
sdk=${VST3_SDK_ROOT:-"$root/vst3sdk"}
build_dir=${WEBRTC_MAC_BUILD_DIR:-"$root/build/webrtc_vst_mac"}

if [[ ! -d "$sdk" ]]; then
    git clone --depth 1 --branch v3.8.0_build_66 --recurse-submodules --shallow-submodules \
        https://github.com/steinbergmedia/vst3sdk.git "$sdk"
fi
openssl_root=${OPENSSL_ROOT_DIR:-$(bash "$root/scripts/build_macos_openssl.sh" "$arch")}

# Share downloaded sources (not object files) when building a second architecture.
dependency_args=(-DFETCHCONTENT_UPDATES_DISCONNECTED=ON)
if [[ -n ${WEBRTC_MAC_DEPS_DIR:-} ]]; then
    for dependency in nlohmann_json libdatachannel ixwebsocket opus; do
        upper=$(echo "$dependency" | tr '[:lower:]' '[:upper:]')
        dependency_args+=("-DFETCHCONTENT_SOURCE_DIR_$upper=$WEBRTC_MAC_DEPS_DIR/$dependency-src")
    done
fi

cmake -S webrtc_vst -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DVST3_SDK_ROOT="$sdk" \
    -DOPENSSL_ROOT_DIR="$openssl_root" \
    -DOPENSSL_CRYPTO_LIBRARY="$openssl_root/lib/libcrypto.a" \
    -DOPENSSL_SSL_LIBRARY="$openssl_root/lib/libssl.a" \
    -DOPENSSL_INCLUDE_DIR="$openssl_root/include" \
    -DSMTG_CREATE_PLUGIN_LINK=OFF \
    -DWEBRTC_VST_BUILD_TESTS=ON "${dependency_args[@]}"
cmake --build "$build_dir" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
    --target webrtc_vst webrtc_vst_cli_host webrtc_vst_integration_test \
        webrtc_vst_stress_test webrtc_vst_fuzz_test
cmake --build "$build_dir" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
    --target webrtc_vst_mac_editor_test
ctest --test-dir "$build_dir" -C Release --output-on-failure --parallel 1
