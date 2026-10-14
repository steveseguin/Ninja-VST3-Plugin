# Linux build and installation

Linux release packages target x86-64 and are built on Ubuntu 22.04. Use a
compatible glibc distribution and an x86-64 VST3 host. Other CPU architectures,
musl-based distributions, and native Wayland host embedding need separate
validation. The editor uses the VST3 X11 embedding interface.

## Install a release

Download the Linux `.tar.gz` and its `SHA256SUMS.txt` from the same release.
Verify the archive in the download directory:

```bash
sha256sum -c webrtc_vst-v0.2.7-linux-SHA256SUMS.txt
mkdir -p ~/.vst3
tar -xzf webrtc_vst-v0.2.7-linux-x86_64-vst3.tar.gz -C ~/.vst3
```

Keep the entire `webrtc_vst.vst3` bundle, including `Contents/Resources`.
Restart the host, rescan its VST3 paths, and load **VDO.Ninja WebRTC Bridge**.
Back up an existing installation before replacing it. Only 32-bit float audio
processing is supported; this is separate from the 64-bit CPU requirement.

To diagnose missing desktop libraries:

```bash
ldd ~/.vst3/webrtc_vst.vst3/Contents/x86_64-linux/webrtc_vst.so
```

Install any missing libraries through the distribution's package manager.
Opus, libdatachannel, and OpenSSL are linked statically; the host still supplies
the standard Linux desktop and C/C++ runtime libraries.

## Build and test

Install GCC 11+, CMake 3.25+, Node.js 22+, and these Ubuntu build dependencies:

```bash
sudo apt-get install ninja-build pkg-config libssl-dev libwayland-dev wayland-protocols \
  libx11-xcb-dev libxcb-util-dev libxcb-cursor-dev libxcb-keysyms1-dev libxcb-xkb-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libpango1.0-dev libglib2.0-dev libcairo2-dev \
  libgtkmm-3.0-dev libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev xvfb
xvfb-run -a bash scripts/build_linux.sh
npm ci
npm run test:offline
npm run test:hardening
node tools/tests/integration/with_test_publisher.js npm run test:integration
WEBRTC_PUBLISH_TEST_ROOM=1 node tools/tests/integration/with_test_publisher.js npm run test:integration:live
bash scripts/package_linux.sh
```

The build script obtains the pinned MIT-licensed VST3 SDK if absent, builds the
plugin and test tools, and runs native tests. Set `VST3_SDK_ROOT` to use an
existing SDK checkout. `WEBRTC_LINUX_BUILD_DIR` overrides the build directory.

The Linux CI workflow adds the extensive Steinberg validator, packet-loss
recovery, and peer-replacement checks. Its artifacts are release candidates;
validate the archived bundle in a host and review completed malware scans
before publishing it.
