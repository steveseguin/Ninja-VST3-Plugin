# WebRTC VST Plugin - Test Suite

Comprehensive automated testing that simulates real VST3 host behavior (like Audacity) to catch real-world issues that unit tests might miss.

## Why These Tests Are Needed

Previous testing efforts exposed critical issues:
- **Crashes in Audacity** that weren't caught by basic tests
- **UI freezing** when plugin was open
- **Use-after-free** vulnerabilities in callbacks
- **Race conditions** during shutdown

These integration and stress tests **simulate actual DAW behavior** to expose these issues before deployment.

---

## Test Suites

### 1. Integration Tests (`integration_test.cpp`)

Simulates normal VST3 host workflow like Audacity would execute.

**Tests:**
- `BasicLoadUnload` - Load plugin and unload cleanly
- `ActivateDeactivate` - Full activation cycle without processing
- `ProcessAudio` - Process 100 blocks (~1 second of audio)
- `RapidOpenClose` - 50 rapid open/close cycles (catches shutdown races)
- `ProcessWhileDeactivating` - Close while audio is playing (real-world scenario)
- `LongRunningSession` - Process 1000 blocks (~10 seconds) to catch memory leaks

**Purpose:** Verify normal operation and catch issues that occur in real DAW usage.

---

### 2. Stress Tests (`stress_test.cpp`)

Aggressive testing designed to expose threading issues and race conditions.

**Tests:**
- `RapidCreateDestroy` - 1000x rapid plugin instantiation (single-threaded)
- `ConcurrentInstances` - 10 plugin instances running simultaneously
- `RapidActivateDeactivate` - 500x activate/deactivate without processing
- `MemoryLeakCheck` - 100 cycles with allocation monitoring
- `ConcurrentCreateDestroy` - 5 threads each doing 100 create/destroy cycles

**Purpose:** Expose race conditions, memory leaks, and threading bugs that only occur under extreme conditions.

---

## Building the Tests

### Windows (PowerShell)

```powershell
cd C:\Users\steve\Code\gpt\vst

# Build plugin and tests
cmake --build build/webrtc_vst_win --config Release --target webrtc_vst
cmake --build build/webrtc_vst_win --config Release --target webrtc_vst_integration_test
cmake --build build/webrtc_vst_win --config Release --target webrtc_vst_stress_test
```

### Linux / macOS

```bash
cd /path/to/vst

# Build plugin and tests
cmake --build build --config Release --target webrtc_vst
cmake --build build --config Release --target webrtc_vst_integration_test
cmake --build build --config Release --target webrtc_vst_stress_test
```

---

## Running the Tests

### Integration Tests

```powershell
# Windows
.\build\webrtc_vst_win\tests\Release\webrtc_vst_integration_test.exe

# Or specify plugin path
.\build\webrtc_vst_win\tests\Release\webrtc_vst_integration_test.exe `
    .\build\webrtc_vst_win\VST3\Release\webrtc_vst.vst3
```

```bash
# Linux / macOS
./build/tests/webrtc_vst_integration_test ./build/VST3/Release/webrtc_vst.vst3
```

**Expected Output:**
```
============================================================
WebRTC VST3 Plugin - Integration Tests
Simulates real VST3 host behavior (Audacity-like)
============================================================
Plugin path: build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3

Running tests...
------------------------------------------------------------
  ✓ BasicLoadUnload (12.3ms)
  ✓ ActivateDeactivate (45.6ms)
  ✓ ProcessAudio (1234.5ms)
  ✓ RapidOpenClose(50x) (2345.6ms)
  ✓ ProcessWhileDeactivating (678.9ms)
  ✓ LongRunningSession(10s) (10234.5ms)

============================================================
TEST SUMMARY
============================================================
Total:  6
Passed: 6
Failed: 0
============================================================
```

---

### Stress Tests

```powershell
# Windows
.\build\webrtc_vst_win\tests\Release\webrtc_vst_stress_test.exe
```

```bash
# Linux / macOS
./build/tests/webrtc_vst_stress_test
```

**Expected Output:**
```
============================================================
WebRTC VST3 Plugin - STRESS TESTS
WARNING: Aggressive testing for race conditions
============================================================
Plugin path: build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3

[1/5] Rapid Create/Destroy (1000x)... ✓ PASS (3.456s)
[2/5] Concurrent Instances (10 parallel)... ✓ PASS (1.234s)
[3/5] Rapid Activate/Deactivate (500x)... ✓ PASS (2.345s)
[4/5] Memory Leak Check (100 cycles)... ✓ PASS (5.678s)
[5/5] Concurrent Create/Destroy (5 threads, 100x each)... ✓ PASS (4.567s)

============================================================
STRESS TEST SUMMARY
============================================================
✓ ALL STRESS TESTS PASSED
Plugin is stable under extreme conditions
============================================================
```

---

## Using CTest

CMake's CTest framework is integrated for automated testing:

```powershell
# Windows
cd build\webrtc_vst_win
ctest -C Release --output-on-failure

# Run specific test
ctest -C Release -R IntegrationTest --verbose
ctest -C Release -R StressTest --verbose
```

```bash
# Linux / macOS
cd build
ctest --output-on-failure
```

---

## Continuous Integration

### GitHub Actions Example

Create `.github/workflows/test.yml`:

```yaml
name: VST Plugin Tests

on: [push, pull_request]

jobs:
  test-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
        with:
          submodules: recursive

      - name: Configure
        run: |
          cmake -B build/webrtc_vst_win -S webrtc_vst `
            -G "Visual Studio 17 2022" `
            -DVST3_SDK_ROOT="${{github.workspace}}/vst3sdk" `
            -DCMAKE_BUILD_TYPE=Release

      - name: Build Plugin
        run: cmake --build build/webrtc_vst_win --config Release --target webrtc_vst

      - name: Build Tests
        run: |
          cmake --build build/webrtc_vst_win --config Release --target webrtc_vst_integration_test
          cmake --build build/webrtc_vst_win --config Release --target webrtc_vst_stress_test

      - name: Run Integration Tests
        run: |
          cd build/webrtc_vst_win
          ctest -C Release -R IntegrationTest --output-on-failure

      - name: Run Stress Tests
        run: |
          cd build/webrtc_vst_win
          ctest -C Release -R StressTest --output-on-failure
```

---

## Environment Variables

### Plugin Path

```powershell
# Set plugin path explicitly
$env:WEBRTC_VST_PLUGIN_PATH = "path\to\webrtc_vst.vst3"
.\webrtc_vst_integration_test.exe
```

### Plugin Configuration (for testing different modes)

```powershell
# Test in Play mode
$env:WEBRTC_VST_MODE = "play"
$env:WEBRTC_VST_STREAM_ID = "test123"

# Test in Seed mode
$env:WEBRTC_VST_MODE = "seed"
$env:WEBRTC_VST_STREAM_ID = "publisher456"
```

---

## Interpreting Results

### ✓ All Tests Pass
Plugin is stable and ready for production deployment.

### ✗ Integration Test Fails
- **BasicLoadUnload fails**: Plugin initialization is broken
- **ActivateDeactivate fails**: Activation sequence has issues
- **ProcessAudio fails**: Audio processing is broken
- **RapidOpenClose fails**: Shutdown has race conditions (check shutdown guards)
- **ProcessWhileDeactivating fails**: Cleanup during active processing is broken
- **LongRunningSession fails**: Memory leak or resource exhaustion

### ✗ Stress Test Fails
- **RapidCreateDestroy fails**: Initialization or cleanup is not thread-safe
- **ConcurrentInstances fails**: Plugin has shared global state (bad!)
- **RapidActivateDeactivate fails**: State transitions are not atomic
- **MemoryLeakCheck fails**: Resources are not being released properly
- **ConcurrentCreateDestroy fails**: Race conditions in constructor/destructor

---

## Debugging Test Failures

### Enable Verbose Logging

```powershell
$env:WEBRTC_VST_LOG_STDOUT = "1"
$env:WEBRTC_VST_LOG_SIGNALING = "1"
.\webrtc_vst_integration_test.exe
```

### Run Under Debugger (Windows)

```powershell
# Visual Studio
devenv build\webrtc_vst_win\tests\Release\webrtc_vst_integration_test.exe

# Or use WinDbg
windbg .\build\webrtc_vst_win\tests\Release\webrtc_vst_integration_test.exe
```

### Run with Address Sanitizer (Linux/macOS)

```bash
# Build with ASAN
cmake -B build -S webrtc_vst -DCMAKE_CXX_FLAGS="-fsanitize=address -g"
cmake --build build

# Run tests
./build/tests/webrtc_vst_integration_test
```

### Run with Thread Sanitizer (Linux/macOS)

```bash
# Build with TSAN
cmake -B build -S webrtc_vst -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build

# Run stress tests
./build/tests/webrtc_vst_stress_test
```

---

## Memory Leak Detection

### Windows (Visual Studio)

1. Build in Debug mode
2. Run tests with Visual Studio debugger
3. Check Output window for memory leaks

### Linux (Valgrind)

```bash
valgrind --leak-check=full --show-leak-kinds=all \
  ./build/tests/webrtc_vst_integration_test
```

### macOS (Instruments)

```bash
instruments -t Leaks ./build/tests/webrtc_vst_integration_test
```

---

## What Makes These Tests Different

### Compared to Unit Tests
- **Unit tests**: Test individual functions in isolation
- **Integration tests**: Test the plugin as a complete system, exactly how a DAW uses it

### Compared to Manual Testing
- **Manual testing**: Slow, inconsistent, hard to reproduce
- **Automated tests**: Fast, repeatable, catch regressions immediately

### Compared to Basic Host Tests
- **Basic host**: Loads plugin, maybe processes a few blocks
- **These tests**: Simulate real DAW scenarios (rapid close, concurrent processing, edge cases)

---

## Test Coverage

| Scenario | Integration | Stress | Manual (Audacity) |
|----------|:-----------:|:------:|:-----------------:|
| Basic load/unload | ✓ | ✓ | ✓ |
| Activate/deactivate | ✓ | ✓ | ✓ |
| Audio processing | ✓ | | ✓ |
| Rapid open/close | ✓ | ✓ | Hard to test |
| Close while streaming | ✓ | | Sometimes happens |
| Concurrent instances | | ✓ | Rare |
| Memory leaks | ✓ | ✓ | Hard to detect |
| Race conditions | | ✓ | Intermittent |

---

## Next Steps After Tests Pass

1. ✅ All tests pass → **Deploy to production**
2. ⚠️ Some tests fail → **Fix issues, repeat**
3. ✅ Tests pass in CI → **Automated quality gate**
4. 📊 Monitor in production → **No crashes expected**

---

## Questions?

See also:
- `THREADING.md` - Threading model documentation
- `ALL_FIXES_APPLIED.md` - Detailed fix documentation
- `BUILD_INSTRUCTIONS.md` - How to build the plugin

---

**Last Updated**: 2025-10-08
**Test Coverage**: 6 integration tests + 5 stress tests = 11 test scenarios
