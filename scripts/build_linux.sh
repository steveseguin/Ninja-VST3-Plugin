#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo "This script requires Linux." >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
sdk=${VST3_SDK_ROOT:-"$root/vst3sdk"}
build_dir=${WEBRTC_LINUX_BUILD_DIR:-"$root/build/webrtc_vst_linux"}
if [[ ! -d "$sdk" ]]; then
    git clone --depth 1 --branch v3.8.0_build_66 --recurse-submodules --shallow-submodules \
        https://github.com/steinbergmedia/vst3sdk.git "$sdk"
fi
cmake -S webrtc_vst -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DVST3_SDK_ROOT="$sdk" -DOPENSSL_USE_STATIC_LIBS=ON \
    -DSMTG_CREATE_PLUGIN_LINK=OFF -DWEBRTC_VST_BUILD_TESTS=ON \
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" --target \
    webrtc_vst webrtc_vst_cli_host validator webrtc_vst_linux_editor_test webrtc_vst_callback_assignment_test \
    webrtc_vst_audio_impairment_test webrtc_vst_peer_audio_buffer_test \
    webrtc_vst_resampler_test webrtc_vst_audio_ring_test webrtc_vst_settings_test \
    webrtc_vst_integration_test webrtc_vst_stress_test webrtc_vst_fuzz_test
ctest --test-dir "$build_dir" -C Release --output-on-failure --parallel 1
