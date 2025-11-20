# WebRTC VST Plugin - Comprehensive Testing Guide

## Testing Levels

### Level 1: Stability Testing (Automated) ✅ COMPLETE
**Tool:** CLI host (`webrtc_vst_cli_host.exe`)
**Script:** `tests/run_simple_tests.ps1`

**What it tests:**
- ✅ Plugin loads/unloads without crashes
- ✅ Audio processing works correctly
- ✅ Rapid open/close cycles (catches shutdown bugs)
- ✅ Memory leak detection (long-running sessions)

**Status:** All tests passing

```powershell
.\tests\run_simple_tests.ps1 -RapidCycles 50
```

---

### Level 2: WebRTC Connection Testing (Automated)
**Tool:** CLI host with WebRTC environment variables
**Script:** `tests/run_webrtc_tests.ps1`
**Signaling Server:** wss://wss.vdo.ninja

**What it tests:**
- WebRTC signaling connection
- Peer connection establishment
- Audio streaming in both modes
- Rapid connect/disconnect stability

**Usage:**

```powershell
# IMPORTANT: Disable encryption for VDO.ninja compatibility
$env:WEBRTC_VST_PASSWORD = "false"

# Test as broadcaster (SEED mode)
.\tests\run_webrtc_tests.ps1 -Mode seed -StreamId "test123" -Duration 10

# Test as receiver (PLAY mode)
.\tests\run_webrtc_tests.ps1 -Mode play -StreamId "test123" -Duration 10

# Verbose logging
.\tests\run_webrtc_tests.ps1 -Mode seed -Verbose
```

**Known Issue:** VDO.ninja uses encrypted signaling by default. The plugin's encryption doesn't match VDO.ninja's scheme, so you must disable encryption by setting `WEBRTC_VST_PASSWORD="false"` to work with VDO.ninja.

**Manual peer testing:**
1. Run plugin in seed mode: `.\tests\run_webrtc_tests.ps1 -Mode seed -StreamId "mytest"`
2. Connect viewer at: `https://vdo.ninja/?view=mytest`
3. Verify audio is transmitted

---

### Level 3: UI Testing (Manual - Audacity)
**Tool:** Audacity
**Plugin Location:** `%LOCALAPPDATA%\Programs\Common\VST3\webrtc_vst.vst3`

**Installation:**
```powershell
Copy-Item -Recurse -Force build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3 `
  $env:LOCALAPPDATA\Programs\Common\VST3\
```

**Test Cases:**

#### Test 1: Plugin UI Loads
1. Open Audacity
2. Load audio track
3. Add WebRTC Bridge effect
4. **Verify:** UI opens without freezing Audacity
5. **Verify:** Can interact with Audacity while plugin is open

#### Test 2: Seed Mode (Broadcasting)
1. Configure plugin to SEED mode
2. Set stream ID (e.g., "audacity-test-123")
3. Apply effect to audio track
4. **Verify:** Audio plays through and broadcasts
5. **External Verification:** Open `https://vdo.ninja/?view=audacity-test-123`
6. **Expected:** Hear the audio from Audacity

#### Test 3: Play Mode (Receiving)
1. Start broadcasting from another source to stream ID "test456"
   - Use: `https://vdo.ninja/?push=test456`
2. Configure plugin to PLAY mode with stream ID "test456"
3. Apply effect to audio track
4. **Verify:** Received audio is played through Audacity
5. **Verify:** Audio quality is acceptable

#### Test 4: Close Plugin
1. While plugin is active, close it
2. **Verify:** Audacity does NOT crash
3. **Verify:** No error dialogs appear
4. **Verify:** Can reopen plugin immediately

#### Test 5: Rapid Open/Close
1. Open plugin → Close → Open → Close (repeat 10 times rapidly)
2. **Verify:** No crashes
3. **Verify:** No UI freezing

---

### Level 4: Real Audio Application Testing (Advanced)

#### Option A: GStreamer (Linux/Windows)
GStreamer has VST3 support via [gstreamer-vst3](https://github.com/centricular/gstreamer-vst3)

**Setup:**
```bash
# Install GStreamer (Ubuntu example)
sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

# Build gstreamer-vst3
git clone https://github.com/centricular/gstreamer-vst3
cd gstreamer-vst3
meson build
ninja -C build
```

**Test with WebRTC VST:**
```bash
# Test with file input
gst-launch-1.0 filesrc location=test.wav ! wavparse ! vst3 plugin=webrtc_vst.vst3 ! autoaudiosink

# Test with microphone input (live streaming)
gst-launch-1.0 autoaudiosrc ! vst3 plugin=webrtc_vst.vst3 ! autoaudiosink
```

#### Option B: VST3PluginTestHost (Official Steinberg Tool)
Already included in VST3 SDK

**Usage:**
```powershell
# Windows
.\vst3sdk\build\bin\Release\VST3PluginTestHost.exe

# Then load plugin via UI
# Select: build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3
```

**Test features:**
- Load plugin
- Process audio input (file or live)
- Verify WebRTC connection works
- Check CPU usage and latency

#### Option C: Reaper (DAW - Trial Available)
Reaper supports VST3 and has a free 60-day trial

1. Download: https://www.reaper.fm/download.php
2. Install plugin to VST3 folder
3. Add plugin to audio track
4. Test broadcasting/receiving with VDO.ninja

---

### Level 5: Performance Testing

#### CPU Usage Test
```powershell
# Run plugin with performance monitoring
$process = Start-Process -FilePath "build\webrtc_vst_win\bin\Release\webrtc_vst_cli_host.exe" `
  -ArgumentList "build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3" `
  -PassThru

# Monitor CPU usage
while (!$process.HasExited) {
    $cpu = (Get-Process -Id $process.Id).CPU
    Write-Host "CPU Time: $cpu seconds"
    Start-Sleep 1
}
```

#### Memory Leak Test
```powershell
# Long-running test with memory monitoring
$env:WEBRTC_CLI_HOST_ITERATIONS = "10000"  # ~100 seconds
.\tests\run_simple_tests.ps1 -RapidCycles 100

# Monitor memory in Task Manager during test
# Expected: Memory usage should be stable (no growth)
```

#### Latency Test
Use Audacity's "Analyze → Plot Spectrum" to measure audio delay:
1. Record a click/impulse sound
2. Process through WebRTC Bridge plugin
3. Compare input/output timing
4. **Target:** < 50ms latency for acceptable real-time use

---

## Testing Matrix

| Test Type | Tool | Automated | Tests UI | Tests WebRTC | Status |
|-----------|------|:---------:|:--------:|:------------:|:------:|
| **Stability** | CLI host | ✅ | ❌ | ❌ | ✅ PASS |
| **WebRTC Connect** | CLI host + env vars | ✅ | ❌ | ✅ | 🔄 TBD |
| **UI + Audacity** | Audacity | ❌ | ✅ | ✅ | 🔄 Manual |
| **GStreamer** | gstreamer-vst3 | ⚠️ | ❌ | ✅ | 🔄 Optional |
| **VST3TestHost** | Steinberg SDK | ⚠️ | ✅ | ✅ | 🔄 Optional |
| **Performance** | Custom scripts | ⚠️ | ❌ | ❌ | 🔄 TBD |

**Legend:**
- ✅ = Supported
- ❌ = Not supported
- ⚠️ = Partial automation
- 🔄 = To be done

---

## Quick Test Sequence (Recommended)

### For Development Testing:
```powershell
# 1. Quick stability check (30 seconds)
.\tests\run_simple_tests.ps1 -RapidCycles 10

# 2. WebRTC connectivity test (10 seconds)
.\tests\run_webrtc_tests.ps1 -Mode seed -Duration 10
```

### For Pre-Release Testing:
```powershell
# 1. Full stability test (2 minutes)
.\tests\run_simple_tests.ps1 -RapidCycles 100

# 2. WebRTC connection test with peer
.\tests\run_webrtc_tests.ps1 -Mode seed -StreamId "release-test" -Duration 30

# 3. Manual Audacity testing (see Level 3)
# - Install plugin
# - Test UI
# - Test seed mode with vdo.ninja viewer
# - Test play mode with vdo.ninja broadcaster
# - Test rapid open/close
```

### For Production Validation:
```powershell
# 1. Extended stability (10 minutes)
$env:WEBRTC_CLI_HOST_ITERATIONS = "60000"
.\tests\run_simple_tests.ps1 -RapidCycles 500

# 2. Memory leak detection
# Run long test while monitoring Task Manager

# 3. Full Audacity regression test
# All test cases from Level 3

# 4. Performance benchmarks
# CPU and latency measurements
```

---

## Known Limitations

### Current Testing Gaps:
1. **UI testing is manual** - Audacity UI automation is complex
2. **Audio quality verification** - No automated audio analysis yet
3. **Cross-platform testing** - Currently Windows-focused
4. **Network conditions** - No simulation of packet loss, jitter, etc.

### Future Enhancements:
- [ ] Automated UI testing with Audacity scripting
- [ ] Audio quality metrics (PESQ, POLQA)
- [ ] Linux/macOS test runners
- [ ] Network simulator integration
- [ ] Continuous integration (GitHub Actions)

---

## Troubleshooting

### Tests fail with "CLI host not found"
```powershell
cmd.exe /c "cmake --build build\webrtc_vst_win --config Release --target webrtc_vst_cli_host"
```

### WebRTC tests timeout
- Check internet connection
- Verify wss://wss.vdo.ninja is accessible
- Try with `-Verbose` flag to see signaling logs

### Audacity crashes on close
- This was the original bug - should be fixed now
- If still happening, check THREADING.md for shutdown sequence
- Review shutdown guards in WebRTCSession.cpp

### Plugin not appearing in Audacity
```powershell
# Verify installation
Test-Path "$env:LOCALAPPDATA\Programs\Common\VST3\webrtc_vst.vst3"

# Reinstall
Copy-Item -Recurse -Force build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3 `
  $env:LOCALAPPDATA\Programs\Common\VST3\

# Rescan in Audacity: Effects → Add/Remove Effects → Rescan
```

---

## See Also
- `tests/README.md` - Test suite documentation
- `TESTING_GUIDE.md` - Detailed testing procedures
- `THREADING.md` - Threading model and shutdown sequence
- `ALL_FIXES_APPLIED.md` - Bug fixes and stability improvements
