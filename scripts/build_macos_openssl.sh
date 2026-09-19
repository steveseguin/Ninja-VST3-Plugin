#!/bin/bash
set -euo pipefail

# Build a private static OpenSSL with the same architecture/minimum OS as the plugin.
# Homebrew bottles can require a newer OS than our deployment target.
root=$(cd "$(dirname "$0")/.." && pwd)
arch=${1:-$(uname -m)}
case "$arch" in
    arm64) target=darwin64-arm64-cc ;;
    x86_64) target=darwin64-x86_64-cc ;;
    *) echo "Usage: $0 [arm64|x86_64]" >&2; exit 1 ;;
esac
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
version=3.5.8
sha256=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
deps="$root/build/macos-deps"
prefix="$deps/openssl-$version-$arch-macos$MACOSX_DEPLOYMENT_TARGET"
if [[ -f "$prefix/lib/libssl.a" && -f "$prefix/.complete" ]]; then
    echo "$prefix"
    exit 0
fi
mkdir -p "$deps"
archive="$deps/openssl-$version.tar.gz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 "https://www.openssl.org/source/openssl-$version.tar.gz" -o "$archive"
fi
actual=$(shasum -a 256 "$archive" | awk '{print $1}')
if [[ "$actual" != "$sha256" ]]; then
    echo "OpenSSL checksum mismatch: $archive" >&2
    exit 1
fi
source_dir="$deps/openssl-$version"
if [[ ! -d "$source_dir" ]]; then
    tar -xzf "$archive" -C "$deps"
fi
work="$deps/openssl-build-$arch-macos$MACOSX_DEPLOYMENT_TARGET"
mkdir -p "$work"
(
    cd "$work"
    "$source_dir/Configure" "$target" no-shared no-tests no-module \
        --prefix="$prefix" --libdir=lib "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
    make -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" build_libs
    make install_dev
) >&2
touch "$prefix/.complete"
echo "$prefix"
