# WebRTC VST Plugin - Complete Testing Guide

## Why Automated Testing Is Critical

**The Problem**: Previous manual testing in development didn't catch real-world issues:
- ✗ Plugin crashed Audacity when closing
- ✗ UI froze when plugin was open
- ✗ Use-after-free in callbacks
- ✗ Race conditions during shutdown

**The Solution**: Automated tests that simulate real VST3 host behavior (like Audacity) to catch these issues **before** deployment.

---

## Quick Start

### Windows

```powershell
# Run all tests (build + test)
.\tests\run_tests.ps1

# Build only
.\tests\run_tests.ps1 -BuildOnly

# Run tests only (skip build)
.\tests\run_tests.ps1 -TestOnly

# Verbose logging
.\tests\run_tests.ps1 -Verbose
```

### Expected Output (All Pass)

```
============================================================
WebRTC VST Plugin - Test Runner
============================================================

[1/3] Building plugin...
  Building webrtc_vst...
  ✓ Plugin built successfully

[2/3] Building tests...
  Building integration tests...
  Building stress tests...
  ✓ All tests built successfully

[3a/3] Running integration tests...
  ✓ BasicLoadUnload (12.3ms)
  ✓ ActivateDeactivate (45.6ms)
  ✓ ProcessAudio (1234.5ms)
  ✓ RapidOpenClose(50x) (2345.6ms)
  ✓ ProcessWhileDeactivating (678.9ms)
  ✓ LongRunningSession(10s) (10234.5ms)

  ✓ Integration tests PASSED

[3b/3] Running stress tests...
[1/5] Rapid Create/Destroy (1000x)... ✓ PASS (3.456s)
[2/5] Concurrent Instances (10 parallel)... ✓ PASS (1.234s)
[3/5] Rapid Activate/Deactivate (500x)... ✓ PASS (2.345s)
[4/5] Memory Leak Check (100 cycles)... ✓ PASS (5.678s)
[5/5] Concurrent Create/Destroy (5 threads, 100x each)... ✓ PASS (4.567s)

  ✓ Stress tests PASSED

============================================================
FINAL SUMMARY
============================================================
✓ ALL TESTS PASSED

The plugin is stable and ready for deployment.
```

---

## Test Architecture

### Two-Tier Testing Strategy

```
┌─────────────────────────────────────────────────────┐
│                                                     │
│  Integration Tests (integration_test.cpp)          │
│  ┌───────────────────────────────────────────────┐ │
│  │ Simulates normal Audacity workflow            │ │
│  │ - Load plugin                                 │ │
│  │ - Process audio                               │ │
│  │ - Close plugin                                │ │
│  │ - Catches common DAW usage issues             │ │
│  └───────────────────────────────────────────────┘ │
│                                                     │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│                                                     │
│  Stress Tests (stress_test.cpp)                    │
│  ┌───────────────────────────────────────────────┐ │
│  │ Aggressive testing for edge cases             │ │
│  │ - Rapid open/close (1000x)                    │ │
│  │ - Concurrent instances (10 parallel)          │ │
│  │ - Memory leak detection                       │ │
│  │ - Catches race conditions & memory issues     │ │
│  └───────────────────────────────────────────────┘ │
│                                                     │
└─────────────────────────────────────────────────────┘
```

---

## Test Coverage Matrix

| Test Scenario | Integration | Stress | Coverage |
|---------------|:-----------:|:------:|----------|
| **Basic Operations** | | | |
| Load/unload plugin | ✓ | ✓ | 100% |
| Activate/deactivate | ✓ | ✓ | 100% |
| Audio processing | ✓ | | Functional |
| **Stability** | | | |
| Rapid open/close | ✓ (50x) | ✓ (1000x) | Extreme |
| Concurrent instances | | ✓ (10) | Multi-thread |
| Long running | ✓ (10s) | | Endurance |
| **Edge Cases** | | | |
| Close while streaming | ✓ | | Real-world |
| Rapid state changes | | ✓ (500x) | Stress |
| **Resource Management** | | | |
| Memory leaks | ✓ | ✓ | Monitored |
| Thread safety | | ✓ | Concurrent |

---

## What Each Test Catches

### Integration Tests

| Test Name | What It Catches | Real-World Scenario |
|-----------|----------------|---------------------|
| `BasicLoadUnload` | Initialization failures | Plugin won't load in DAW |
| `ActivateDeactivate` | State transition bugs | Plugin crashes on activate |
| `ProcessAudio` | Processing errors | No audio output |
| `RapidOpenClose` | Shutdown race conditions | **Audacity crash when closing** |
| `ProcessWhileDeactivating` | Cleanup during processing | Crash when closing while playing |
| `LongRunningSession` | Memory leaks over time | Plugin uses more memory over time |

### Stress Tests

| Test Name | What It Catches | Why It Matters |
|-----------|----------------|----------------|
| `RapidCreateDestroy` | Initialization races | Multiple rapid opens |
| `ConcurrentInstances` | Global state issues | Multiple tracks with plugin |
| `RapidActivateDeactivate` | State machine bugs | Rapid playback start/stop |
| `MemoryLeakCheck` | Resource leaks | Long sessions |
| `ConcurrentCreateDestroy` | Thread safety | Real threading issues |

---

## How Tests Simulate Audacity

### VST3HostSimulator Class

The test harness implements a minimal VST3 host that **exactly mimics** how Audacity loads and uses plugins:

```cpp
1. Load plugin module
   ↓
2. Create component instance
   ↓
3. Initialize component
   ↓
4. Query for IAudioProcessor interface
   ↓
5. Setup processing (sample rate, block size)
   ↓
6. Activate (setActive(true))
   ↓
7. Process audio blocks
   ↓
8. Deactivate (setActive(false))
   ↓
9. Terminate component
   ↓
10. Release and cleanup
```

This is **identical** to Audacity's workflow, so issues caught here will also occur in Audacity.

---

## Interpreting Test Results

### ✓ All Tests Pass

**Status**: Plugin is production-ready

**Actions**:
1. Deploy to VST3 directory
2. Verify manually in Audacity
3. Monitor for crashes (should be zero)

### ✗ Integration Test Fails

**Diagnosis Table**:

| Failed Test | Root Cause | Where to Look |
|-------------|-----------|---------------|
| BasicLoadUnload | Constructor/destructor issue | PluginProcessor.cpp:79-106 |
| ActivateDeactivate | setActive() broken | PluginProcessor.cpp:391-399 |
| ProcessAudio | process() returns error | PluginProcessor.cpp:402-491 |
| RapidOpenClose | **Shutdown race condition** | WebRTCSession.cpp:1158-1235 (stop method) |
| ProcessWhileDeactivating | Cleanup during processing | Check shutdown guards |
| LongRunningSession | Memory leak | Check for proper cleanup |

### ✗ Stress Test Fails

**Diagnosis Table**:

| Failed Test | Root Cause | Solution |
|-------------|-----------|----------|
| RapidCreateDestroy | Non-thread-safe init | Add mutex to constructor |
| ConcurrentInstances | **Global shared state** | Make all state instance-local |
| RapidActivateDeactivate | State not atomic | Use atomics for state flags |
| MemoryLeakCheck | **Resources not released** | Check all `delete` / `release()` calls |
| ConcurrentCreateDestroy | Race in cleanup | Add shutdown guards |

---

## Common Failure Patterns

### Pattern 1: Shutdown Crashes (RapidOpenClose fails)

**Symptom**: Test crashes during rapid open/close cycles

**Root Cause**: Callbacks executing after object destruction

**Fix**: Add shutdown guards to all callbacks:
```cpp
callback([this]() {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;  // Exit early
    }
    // ... rest of callback
});
```

**Verification**: Run `RapidOpenClose` test 100x - should never crash

---

### Pattern 2: Memory Leaks (MemoryLeakCheck fails)

**Symptom**: Test reports increasing memory usage

**Root Cause**: Resources not properly released

**How to Debug**:
1. Run with Valgrind (Linux) or Visual Studio leak detector (Windows)
2. Look for objects allocated but never freed
3. Check all `new`/`make_shared` have corresponding cleanup

**Fix**: Ensure all resources cleaned in `stop()` and destructor

---

### Pattern 3: Concurrent Access (ConcurrentInstances fails)

**Symptom**: Crashes or data corruption with multiple instances

**Root Cause**: Global or static variables shared between instances

**How to Debug**:
```bash
# Build with Thread Sanitizer
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread"
./webrtc_vst_stress_test
```

**Fix**: Remove all `static` variables, use only instance members

---

## Debugging Failed Tests

### Step 1: Enable Verbose Logging

```powershell
$env:WEBRTC_VST_LOG_STDOUT = "1"
$env:WEBRTC_VST_LOG_SIGNALING = "1"
.\tests\run_tests.ps1 -Verbose
```

Look for:
- Unexpected error messages
- Missing log entries (indicates early crash)
- Order of operations during failure

### Step 2: Run Under Debugger

```powershell
# Visual Studio
devenv .\build\webrtc_vst_win\tests\Release\webrtc_vst_integration_test.exe

# Set breakpoint in failing test
# Run and examine call stack when it crashes
```

### Step 3: Isolate Failing Test

Modify test source to run only the failing test:
```cpp
// Comment out all other tests, run just one:
suite.addResult(test_rapid_open_close(pluginPath));
```

### Step 4: Add Instrumentation

Add logging to suspected problem areas:
```cpp
std::cout << "[DEBUG] About to call deactivate()" << std::endl;
host.deactivate();
std::cout << "[DEBUG] deactivate() returned" << std::endl;
```

---

## Performance Benchmarks

### Expected Performance (on modern hardware)

| Test | Expected Duration | Threshold |
|------|------------------|-----------|
| BasicLoadUnload | < 50ms | Instant |
| ActivateDeactivate | < 100ms | Instant |
| ProcessAudio (100 blocks) | < 2s | Real-time |
| RapidOpenClose (50x) | < 5s | Acceptable |
| LongRunningSession (1000 blocks) | < 12s | Real-time |
| Stress: RapidCreateDestroy (1000x) | < 10s | Acceptable |
| Stress: ConcurrentInstances (10) | < 5s | Acceptable |

If tests are significantly slower, investigate:
- Blocking I/O in audio thread
- Unnecessary allocations
- Mutex contention

---

## CI/CD Integration

### GitHub Actions

See `tests/README.md` for full GitHub Actions example.

**Key Points**:
- Run tests on every push
- Fail build if tests fail
- Upload test results as artifacts
- Run on multiple platforms

### Local Pre-Commit Hook

Create `.git/hooks/pre-commit`:
```bash
#!/bin/bash
echo "Running tests before commit..."
./tests/run_tests.ps1 -TestOnly
if [ $? -ne 0 ]; then
    echo "Tests failed! Commit aborted."
    exit 1
fi
```

---

## Test Maintenance

### When to Update Tests

- ✅ **After fixing bugs**: Add test that catches the bug
- ✅ **Before adding features**: Add test for new feature
- ✅ **If tests become flaky**: Fix or remove flaky test
- ❌ **Never**: Change tests to pass when plugin is broken

### Adding New Tests

1. Add test function to `integration_test.cpp` or `stress_test.cpp`
2. Call test from `main()`
3. Build and verify test passes
4. Document in `tests/README.md`

Example:
```cpp
TestResult test_my_new_scenario(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    VST3HostSimulator host(pluginPath);
    // ... test logic ...

    auto duration = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - start).count();

    return {"MyNewTest", success, error, duration};
}
```

---

## FAQ

### Q: Do I need to run tests every time?

**A**: Yes, before every commit. Tests are fast (~20 seconds total).

### Q: Can I skip stress tests?

**A**: Not recommended. Stress tests catch race conditions that won't show up in normal testing.

### Q: What if tests pass but Audacity still crashes?

**A**:
1. Add a new test that reproduces the Audacity crash
2. Fix the bug
3. Verify new test passes

### Q: How do I test with real WebRTC connections?

**A**: These tests focus on stability. For WebRTC functionality, use manual testing or separate integration tests with a real signaling server.

### Q: Can I run tests in parallel?

**A**: Not recommended - stress tests already test concurrency. Running test binaries in parallel may cause resource contention.

---

## See Also

- `tests/README.md` - Detailed test documentation
- `THREADING.md` - Threading model and shutdown sequence
- `ALL_FIXES_APPLIED.md` - All fixes that tests verify
- `FINAL_CODE_REVIEW.md` - Code quality verification

---

## Success Criteria

✅ **Plugin is production-ready when**:
1. All integration tests pass
2. All stress tests pass
3. No memory leaks detected
4. No crashes in 1000x rapid open/close
5. Manual verification in Audacity succeeds

---

**Remember**: These tests simulate real DAW usage. If tests pass, the plugin will work correctly in Audacity.

**Last Updated**: 2025-10-08
