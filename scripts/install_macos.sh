#!/bin/bash
set -euo pipefail

[[ $(uname -s) == Darwin ]] || { echo "This script requires macOS." >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
source_bundle=${1:-"$root/build/webrtc_vst_mac/VST3/Release/webrtc_vst.vst3"}
[[ -f "$source_bundle/Contents/MacOS/webrtc_vst" ]] || { echo "Not a macOS VST3 bundle: $source_bundle" >&2; exit 1; }
codesign --verify --strict "$source_bundle"
destination="$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$destination"
stage=$(mktemp -d "$destination/.webrtc-install.XXXXXX")
trap 'rm -r "$stage"' EXIT
ditto "$source_bundle" "$stage/webrtc_vst.vst3"
codesign --verify --strict "$stage/webrtc_vst.vst3"
# Preserve the previous install so a developer build can be rolled back.
if [[ -e "$destination/webrtc_vst.vst3" || -L "$destination/webrtc_vst.vst3" ]]; then
    backup_root="$HOME/Library/Application Support/VDO.Ninja/VST3 Backups"
    mkdir -p "$backup_root"
    backup=$(mktemp -d "$backup_root/webrtc_vst.XXXXXX")
    mv "$destination/webrtc_vst.vst3" "$backup/"
    echo "Previous plugin saved in $backup"
fi
mv "$stage/webrtc_vst.vst3" "$destination/webrtc_vst.vst3"
echo "Installed: $destination/webrtc_vst.vst3"
echo "Restart your VST3 host or rescan its plugins."
