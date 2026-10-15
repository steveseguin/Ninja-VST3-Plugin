# Linux build and installation

Linux release packages target x86-64 and are built on Ubuntu 22.04. Use a
compatible glibc distribution and an x86-64 VST3 host. Other CPU architectures,
musl-based distributions, and native Wayland host embedding need separate
validation. The editor uses the VST3 X11 embedding interface.

## Install a release

Download the Linux `.tar.gz` and its `SHA256SUMS.txt` from the same release.
Verify the archive in the download directory:

```bash
sha256sum -c webrtc_vst-v0.2.8-linux-SHA256SUMS.txt
mkdir -p ~/.vst3
tar -xzf webrtc_vst-v0.2.8-linux-x86_64-vst3.tar.gz -C ~/.vst3
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
  libgtkmm-3.0-dev libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev libsqlite3-dev xvfb
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

## X11 editor regressions

`LinuxEditorSmoke` and `LinuxFactoryRunLoopSmoke` each open the actual plugin
64 times and verify painted pixels after closing/replacing other open editors.
Run them under Xvfb through CTest.
It requires the host's Linux run loop and XEmbed support; a headless validator
cannot cover these editor operations.

The pinned VSTGUI can retain Cairo device references after disconnecting XCB.
This was reproduced in REAPER and the native host: reopening eventually aborts
inside `cairo_xcb_surface_create`. `LinuxX11Support.cpp` retains each device
until its connection closes, finishes it before disconnecting, and leaves
other open editors' connections intact. This is related to
[VSTGUI #377](https://github.com/steinbergmedia/vstgui/issues/377); the release
decision relies on our own before/after reproduction.

The compatibility layer also replaces VSTGUI's synchronous XCB flushes with
nonblocking flushes. Synchronous replies could consume expose events into an
internal queue, leaving the host's file descriptor polling asleep and an
uncovered editor blank until another event arrived. The pixel test checks
redrawing without that extra event. Linker wrapping is confined to the Linux
plugin's own static calls; it does not intercept the host application's calls.

Audacity 3.7.9 exposes its run loop on `IPlugFrame`, while REAPER also exposes
one through the factory context. VSTGUI initializes X11 before installing the
frame's loop, causing a null dereference on the first Audacity editor open.
`scripts/vstgui_linux_run_loop.cmake` reverses those two operations in a
generated build copy, preserving the SDK checkout. Configuration stops for
review if a different SDK changes the affected code. Both host-interface
arrangements are covered by the native editor tests.
