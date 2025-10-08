# Threading Model Documentation

## Overview

The WebRTC VST plugin uses multiple threads that must coordinate safely during operation and shutdown. This document explains the threading model and synchronization strategy.

## Thread Types

### 1. **Audio Thread** (Real-Time, High Priority)
- **Source**: VST3 host (Audacity, Reaper, etc.)
- **Entry Point**: `WebRTCProcessor::process()`
- **Constraints**:
  - Must NOT block
  - Must NOT allocate memory
  - Must complete in < 1ms typically
- **Operations**:
  - Push outgoing audio to `outgoingBuffer_`
  - Pull incoming audio from `receiveBuffer_`
  - Flush status updates to controller

### 2. **WebRTC Callback Threads** (Background, Multiple)
- **Source**: libdatachannel internal thread pool
- **Entry Points**: All peer connection callbacks
  - `onStateChange()`
  - `onLocalDescription()`
  - `onLocalCandidate()`
  - `onTrack()`
  - `Track::onFrame()` (audio data arrives here)
  - `onDataChannel()`
- **Operations**:
  - Decode OPUS audio
  - Process signaling messages
  - Update connection state
  - Push decoded audio to `receiveBuffer_`

### 3. **WebSocket Thread** (Background, Single)
- **Source**: ixwebsocket internal thread
- **Entry Points**: WebSocket callbacks
  - `onConnected()`
  - `onDisconnected()`
  - `onMessage()`
  - `onError()`
- **Operations**:
  - Parse JSON signaling messages
  - Forward to `handleSignalingMessage()`

### 4. **UI Thread** (Main Thread)
- **Source**: VST3 host UI thread
- **Entry Points**:
  - `initialize()`
  - `terminate()`
  - `setActive()`
  - Controller parameter changes
- **Operations**:
  - Start/stop sessions
  - Update configuration
  - Create/destroy plugin UI

## Synchronization Primitives

### `SpinLock mutex_` (WebRTCSession)
**Protects**:
- `peerSessions_` map
- `opusEncoder_`/`opusDecoder_`
- `config_`
- `started_` flag

**Used by**: All threads when accessing shared state

**Critical**: Keep locked sections SHORT (< 1μs) to avoid audio thread blocking

### `std::atomic<bool> shuttingDown_`
**Purpose**: Fast early-exit for callbacks during shutdown

**Pattern**:
```cpp
if (shuttingDown_.load(std::memory_order_acquire)) {
    return;  // Exit immediately
}
```

**Why atomic**: Prevents cache coherency issues across CPU cores

### `SpinLock statusSinkMutex_`
**Protects**: `statusSink_` callback pointer

**Why separate**: Allows status updates without holding main `mutex_`

## Shutdown Sequence (CRITICAL)

### Problem: Callbacks Can Fire During Destruction

When the plugin is closed:
1. Host calls `terminate()` on UI thread
2. `~WebRTCProcessor()` destructor runs
3. `session_` member begins destruction
4. **BUT**: WebRTC callbacks may still be executing on background threads!

### Solution: Multi-Phase Shutdown

#### Phase 1: Stop New Operations
```cpp
shuttingDown_.store(true, std::memory_order_release);
```
- All callbacks check this flag FIRST
- Prevents new operations from starting

#### Phase 2: Clear Callback Pointers
```cpp
statusSink_ = nullptr;
configUpdateSink_ = nullptr;
```
- Prevents callbacks from invoking processor methods

#### Phase 3: Disconnect Signaling (Non-Blocking)
```cpp
signalingClient_->disconnect();  // Uses close(), not stop()
```
- Closes WebSocket without waiting for thread join
- Prevents deadlock

#### Phase 4: Clear Peer Connection Callbacks
```cpp
session.connection->onStateChange(nullptr);
session.connection->onTrack(nullptr);
// ... etc
```
- Prevents libdatachannel from invoking callbacks

#### Phase 5: Close Connections
```cpp
session.connection->close();
```
- Gracefully shuts down peer connections

#### Phase 6: Destroy Codecs
```cpp
opus_encoder_destroy(opusEncoder_);
opus_decoder_destroy(opusDecoder_);
```
- Safe now - no more callbacks can access them

#### Phase 7: Reset Flag
```cpp
shuttingDown_.store(false, std::memory_order_release);
```
- Allows session to be restarted if needed

## Race Condition Prevention

### TOCTOU (Time-of-Check-Time-of-Use) in `onFrame()`

**Problem**:
```cpp
// BAD: Race condition!
if (shuttingDown_) return;
// ← Decoder could be destroyed HERE
use(opusDecoder_);  // CRASH!
```

**Solution**: Double-check with mutex
```cpp
// GOOD: Atomic check + mutex-protected access
if (shuttingDown_.load(std::memory_order_acquire)) {
    return;
}

::OpusDecoder* decoder = nullptr;
{
    std::lock_guard<SpinLock> lock(mutex_);
    if (!started_ || !opusDecoder_) {
        return;
    }
    decoder = opusDecoder_;  // Safe: decoder won't be destroyed while mutex held
}

use(decoder);  // Safe: we hold a valid pointer
```

## Member Destruction Order

C++ destroys members in **REVERSE** declaration order:

```cpp
class WebRTCProcessor {
    WebRTCSession session_;      // Declared first → destroyed LAST
    AudioRingBuffer receiveBuffer_;
    std::atomic<bool> statusDirty_;
    // ...
    bool configDirty_;           // Declared last → destroyed FIRST
};
```

**Why this matters**:
- `session_` destructor calls `stop()`
- `stop()` triggers callbacks that may access `receiveBuffer_`, status members, etc.
- By declaring `session_` first, other members are still valid when `stop()` runs

## Audio Buffer Thread Safety

### `AudioRingBuffer receiveBuffer_`

**Writers**: WebRTC callback threads (via `onFrame()`)
**Readers**: Audio thread (via `process()`)

**Synchronization**: Internal lock-free ring buffer
- `push()` from callback threads
- `pull()` from audio thread
- Lock-free operations prevent priority inversion

### `outgoingBuffer_` (WebRTCSession)

**Writers**: Audio thread (via `pushOutgoingAudio()`)
**Readers**: WebRTC encoding thread

**Synchronization**: `std::deque` protected by `mutex_`
- Audio thread: Brief lock to push samples
- Encoder thread: Lock to pull samples
- FIFO ensures correct ordering

## Best Practices for Future Development

### 1. Always Check `shuttingDown_` First
```cpp
void callback() {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    // Rest of callback
}
```

### 2. Use RAII for Locks
```cpp
{
    std::lock_guard<SpinLock> lock(mutex_);
    // Critical section
}  // Lock automatically released
```

### 3. Minimize Lock Hold Time
```cpp
// BAD: Holds lock during slow operation
{
    std::lock_guard<SpinLock> lock(mutex_);
    data = state_;
    processData(data);  // SLOW!
}

// GOOD: Copy data, release lock, then process
Data dataCopy;
{
    std::lock_guard<SpinLock> lock(mutex_);
    dataCopy = state_;
}
processData(dataCopy);  // Lock released
```

### 4. Never Block Audio Thread
```cpp
tresult WebRTCProcessor::process(ProcessData& data) {
    // GOOD: Lock-free or very short locks only
    receiveBuffer_.pull(outputs, numSamples);

    // BAD: Never do this!
    // std::this_thread::sleep_for(...);
    // signalingClient_->send(...);  // Network I/O!
}
```

### 5. Test with Thread Sanitizer
```bash
# Compile with TSan
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread" ...

# Run tests
./validator.exe plugin.vst3
```

## Known Limitations

1. **SpinLock on Audio Thread**: Could cause priority inversion on RT systems
   - Consider lock-free structures for audio path

2. **Resampler in Callback**: `incomingResampler_` allocates memory
   - Should use pre-allocated buffers

3. **No Graceful Shutdown Timeout**: If callbacks hang, shutdown waits forever
   - Consider adding timeout with forced cleanup

## Debugging Tips

### Enable Debug Logging
```powershell
$env:WEBRTC_VST_LOG_STDOUT="1"
```

### Use DebugView
1. Download from Microsoft Sysinternals
2. Run as Administrator
3. Enable "Capture Win32" and "Capture Global Win32"
4. Watch shutdown sequence logs

### Check for Deadlocks
- If plugin hangs on close, check if any thread is waiting on `mutex_`
- Use Visual Studio "Break All" and check call stacks

### Verify Destruction Order
- Add logging to all destructors
- Ensure `~WebRTCSession()` runs before other members destroyed

## References

- [C++ Memory Order](https://en.cppreference.com/w/cpp/atomic/memory_order)
- [VST3 Threading Model](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Change+History/3.6.5/IProcessContextRequirements.html)
- [Lock-Free Programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)
