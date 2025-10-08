# Critical Code Review - All Remaining Issues

## Executive Summary

Found **10 critical/high-severity issues** and **8 medium/low-severity issues** beyond the 5 issues already fixed in REVIEW_FIXES.md.

---

## CRITICAL ISSUES

### Issue #6: Missing Shutdown Guard in onDataChannel::onOpen Callback (CRITICAL)

**File**: `WebRTCSession.cpp:884-897`

**Problem**: The `onOpen` callback captures `this` but doesn't check `shuttingDown_` flag, creating a use-after-free vulnerability.

```cpp
dc->onOpen([this, keyCopy, dc]() {
    log("Datachannel opened, sending viewer preferences");
    // NO shuttingDown_ check here!
    try {
        nlohmann::json prefs = {
            {"audio", true},
            {"video", false}
        };
        std::string msg = prefs.dump();
        dc->send(msg);  // CRASH if session destroyed!
    } catch (const std::exception& ex) {
        log(std::string("Failed to send viewer preferences: ") + ex.what());
    }
});
```

**Impact**: Crash if datachannel opens during/after plugin destruction.

**Fix**:
```cpp
dc->onOpen([this, keyCopy, dc]() {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    log("Datachannel opened, sending viewer preferences");
    // ... rest of code
});
```

---

### Issue #7: Missing Shutdown Guard in onMessage Callback (CRITICAL)

**File**: `WebRTCSession.cpp:899-920`

**Problem**: The datachannel `onMessage` callback captures `this` and calls `handleSignalingMessage()` without checking `shuttingDown_`.

```cpp
dc->onMessage([this](auto data) {
    // NO shuttingDown_ check!
    std::visit([this](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            log("Datachannel message: " + arg);  // Uses 'this'
            try {
                auto msg = nlohmann::json::parse(arg);
                if (msg.contains("description") && msg["description"].contains("type") &&
                    msg["description"]["type"] == "offer") {
                    log("Received new SDP offer via datachannel, processing as signaling message");
                    handleSignalingMessage(msg);  // Calls member function!
                }
            } catch (const std::exception&) {}
        }
    }, data);
});
```

**Impact**: Use-after-free if datachannel message arrives during/after destruction.

**Fix**:
```cpp
dc->onMessage([this](auto data) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    // ... rest of code
});
```

---

### Issue #8: Opus Encoder/Decoder Creation Failure Not Checked (HIGH)

**File**: `WebRTCSession.cpp:1060-1070`

**Problem**: If Opus encoder/decoder creation fails, the pointers are set to `nullptr` but the session continues. Later code assumes they're valid.

```cpp
opusEncoder_ = opus_encoder_create(static_cast<opus_int32>(48000), channelCount_, OPUS_APPLICATION_AUDIO, &opusError);
if (opusError != OPUS_OK) {
    log("Failed to create Opus encoder");
    opusEncoder_ = nullptr;  // Session continues anyway!
}

opusDecoder_ = opus_decoder_create(static_cast<opus_int32>(48000), channelCount_, &opusError);
if (opusError != OPUS_OK) {
    log("Failed to create Opus decoder");
    opusDecoder_ = nullptr;  // Session continues anyway!
}
```

Later in `pushOutgoingAudio()`:
```cpp
void WebRTCSession::pushOutgoingAudio(const float* const* inputs, size_t frames, int channels) {
    if (config_.mode != ConnectionMode::Seed || !opusEncoder_) {
        return;  // Good check here
    }
    std::lock_guard<SpinLock> lock(mutex_);
    // ... uses opusEncoder_ - safe
}
```

But `onFrame()` checks are not foolproof:
```cpp
::OpusDecoder* decoder = nullptr;
{
    std::lock_guard<SpinLock> lock(mutex_);
    if (!started_ || !opusDecoder_) {
        return;  // Good check
    }
    decoder = opusDecoder_;
}
// Uses decoder - safe
```

**Current State**: Actually mostly safe due to existing checks, but error handling is weak.

**Impact**: Silent failure - user sees "connected" but no audio works.

**Fix**: Should fail the session start if codec creation fails:
```cpp
opusEncoder_ = opus_encoder_create(...);
if (opusError != OPUS_OK) {
    log("FATAL: Failed to create Opus encoder");
    emitStatus("Error: Failed to create audio encoder");
    started_ = false;
    return;  // Abort session start
}
```

---

### Issue #9: Race Condition in emitStatus() (MEDIUM-HIGH)

**File**: `WebRTCSession.cpp:339-348`

**Problem**: `statusSink_` callback is copied under lock, then invoked outside the lock. If `stop()` runs concurrently and sets `statusSink_ = nullptr`, we have a TOCTOU.

```cpp
void WebRTCSession::emitStatus(const std::string& status) const {
    StatusSink sink;
    {
        std::lock_guard<SpinLock> lock(statusSinkMutex_);
        sink = statusSink_;  // Copy callback under lock
    }
    // Lock released here
    if (sink) {
        sink(status);  // TOCTOU: sink could reference deleted processor
    }
}
```

**Scenario**:
1. Thread A: Copies `statusSink_` to `sink` (points to `WebRTCProcessor::queueStatus`)
2. Thread A: Releases lock
3. Thread B: `stop()` sets `statusSink_ = nullptr`
4. Thread B: `~WebRTCProcessor()` destructor runs, deletes processor
5. Thread A: Calls `sink(status)` - **use-after-free** because `sink` still captures `this` pointer to deleted processor

**Impact**: Potential crash if status update occurs during shutdown.

**Fix**: Check `shuttingDown_` before calling callback:
```cpp
void WebRTCSession::emitStatus(const std::string& status) const {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    StatusSink sink;
    {
        std::lock_guard<SpinLock> lock(statusSinkMutex_);
        sink = statusSink_;
    }
    if (sink && !shuttingDown_.load(std::memory_order_acquire)) {
        sink(status);
    }
}
```

---

### Issue #10: Missing Shutdown Guard in VDONinja Signaling Callbacks (MEDIUM-HIGH)

**File**: `WebRTCSession.cpp:1083-1109`

**Problem**: Signaling client callbacks capture `this` but don't check `shuttingDown_` before accessing members.

```cpp
signalingClient_->setCallbacks({
    [this]() {
        // NO shuttingDown_ check!
        log("Connected to VDO.Ninja signaling server");
        emitStatus("Signaling connected");
        postInitialRequests();  // Calls member functions
    },
    [this]() {
        log("Signaling connection closed");
        bool notifyDisconnect = false;
        {
            std::lock_guard<SpinLock> innerLock(mutex_);
            notifyDisconnect = started_;
            resetAllPeerConnections();  // Modifies members
        }
        // ...
    },
    [this](const nlohmann::json& message) {
        handleSignalingMessage(message);  // NO shutdown check!
    },
    [this](const std::string& error) {
        log("Signaling error: " + error);
        emitStatus(std::string("Error: ") + error);  // NO shutdown check!
    }
});
```

**Impact**: Callbacks could fire after `stop()` clears callbacks but before socket fully disconnects.

**Fix**: Add shutdown guards to all callbacks:
```cpp
signalingClient_->setCallbacks({
    [this]() {
        if (shuttingDown_.load(std::memory_order_acquire)) return;
        log("Connected to VDO.Ninja signaling server");
        // ...
    },
    // ... all callbacks
});
```

---

### Issue #11: Potential Integer Overflow in AudioRingBuffer (LOW-MEDIUM)

**File**: `AudioRingBuffer.cpp:33-46`

**Problem**: If `frames * channels` overflows `size_t`, undefined behavior occurs.

```cpp
const size_t totalSamples = frames * static_cast<size_t>(channels);
if (totalSamples >= capacity) {
    const size_t framesToCopy = capacity / static_cast<size_t>(channels);
    const size_t startFrame = frames - framesToCopy;  // Could underflow!
```

**Scenario**:
- `frames = SIZE_MAX / 2`
- `channels = 3`
- `totalSamples = (SIZE_MAX/2) * 3` → **overflow**

**Impact**: Very unlikely in practice (audio buffer sizes are tiny), but technically UB.

**Fix**: Add overflow check:
```cpp
if (frames > SIZE_MAX / static_cast<size_t>(channels)) {
    // Overflow would occur, reject
    return;
}
const size_t totalSamples = frames * static_cast<size_t>(channels);
```

---

### Issue #12: No Validation of Audio Channel Count (MEDIUM)

**File**: Multiple locations

**Problem**: Plugin assumes 2 channels everywhere but never validates input/output channel counts match.

```cpp
// PluginProcessor.cpp:118-119
addAudioInput(STR16("Input"), SpeakerArr::kStereo);
addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
```

But in `process()`:
```cpp
// No validation that data.inputs[0].numChannels == 2
// No validation that data.outputs[0].numChannels == 2
```

**Impact**: If DAW provides mono input or 5.1 surround, plugin will:
- Read out of bounds (mono → stereo read)
- Write to wrong channels (stereo → 5.1 write)

**Fix**: Add validation in `process()`:
```cpp
if (data.inputs[0].numChannels != 2 || data.outputs[0].numChannels != 2) {
    // Silence output
    return kResultOk;
}
```

---

### Issue #13: Potential Deadlock in WebRTCSession (MEDIUM)

**File**: `WebRTCSession.cpp:1175-1178`

**Problem**: `stop()` acquires `mutex_` while calling `resetAllPeerConnections()`, which may trigger libdatachannel cleanup that could callback into the plugin.

```cpp
{
    std::lock_guard<SpinLock> lock(mutex_);

    resetAllPeerConnections();  // May trigger callbacks
```

Inside `resetAllPeerConnections()`:
```cpp
session.connection->close();  // Could trigger onStateChange callback!
```

If `onStateChange` tries to acquire `mutex_`, **deadlock**.

**Current Protection**: We set `shuttingDown_` first and clear callbacks, so this is mostly safe. But there's a race:

1. Thread A: `stop()` acquires `mutex_`
2. Thread A: Calls `resetAllPeerConnections()`
3. Thread A: Calls `session.connection->onStateChange(nullptr)`
4. Thread B: libdatachannel callback thread already executing `onStateChange` (captured before nullptr)
5. Thread B: Tries to acquire `mutex_` in `onStateChange` → **deadlock**

**Current Mitigation**: `shuttingDown_` causes callback to return early before acquiring lock.

**Risk**: Low but non-zero. If callback doesn't check `shuttingDown_` before lock (Issue #10), deadlock possible.

**Fix**: Already mitigated by `shuttingDown_` flag, but requires fixing Issue #10.

---

### Issue #14: configUpdateSink_ Not Protected by Lock (MEDIUM)

**File**: `WebRTCSession.cpp:1153`

**Problem**: `configUpdateSink_` is set to `nullptr` without a lock in `stop()`, but accessed with a lock in `start()`.

```cpp
// stop() - NO LOCK
configUpdateSink_ = nullptr;

// start() - WITH LOCK
{
    std::lock_guard<SpinLock> lock(mutex_);
    // ...
    if (sanitizedStream.changed && configUpdateSink_) {
        sanitizedForCallback = config_;
    }
    callback = configUpdateSink_;  // Copy callback
}
```

**Impact**: Race condition - `configUpdateSink_` could be called after being set to nullptr.

**Scenario**:
1. Thread A: `start()` checks `configUpdateSink_` (non-null)
2. Thread B: `stop()` sets `configUpdateSink_ = nullptr`
3. Thread A: Copies `configUpdateSink_` to `callback` (now nullptr)
4. Thread A: Outside lock, calls `callback(config)` - no-op (safe)

Actually safe because callback is copied under lock and checked before use. But inconsistent locking pattern.

**Fix**: Consistent locking - protect with same mutex as `statusSink_`, or document that it's safe.

---

### Issue #15: Memory Allocation in Audio Callback Path (LOW-MEDIUM)

**File**: `WebRTCSession.cpp:840-847`

**Problem**: Resampler may allocate memory in audio callback thread.

```cpp
std::vector<float> resampled;
const size_t outFrames = incomingResampler_.processInterleaved(decodeBuffer.data(),
                                                               static_cast<size_t>(frameSamples),
                                                               channels,
                                                               resampled);  // Vector may allocate!
```

Inside `processInterleaved()`:
```cpp
outputInterleaved.clear();
// ...
outputInterleaved.push_back(sample);  // May allocate if capacity exceeded
```

**Impact**: Heap allocation in real-time audio thread → priority inversion, glitches.

**Best Practice**: Pre-allocate buffers and use fixed-size arrays in audio callbacks.

**Fix**:
```cpp
// Pre-allocate with reserve
resampled.reserve(kMaxExpectedFrames * channels);
```

Or use ring-buffer approach with pre-allocated storage.

---

## MEDIUM ISSUES

### Issue #16: Missing Input Validation in handleSignalingMessage (MEDIUM)

**File**: `WebRTCSession.cpp:1216-1283`

**Problem**: No validation that JSON fields are correct types before accessing.

```cpp
if (message.contains("id") && message["id"].is_string()) {
    // Good - checks type
}

if (message.contains("request")) {
    const auto request = message["request"].get<std::string>();  // NO type check!
```

**Impact**: If "request" is integer/boolean/array, `.get<std::string>()` throws exception (caught by generic catch block).

**Fix**: Add type checking:
```cpp
if (message.contains("request") && message["request"].is_string()) {
    const auto request = message["request"].get<std::string>();
```

---

### Issue #17: No Bounds Checking on Opus Decode Buffer (LOW)

**File**: `WebRTCSession.cpp:826-832`

**Problem**: Opus decode buffer is sized for exactly `kFrameSizeSamples * channels`, but `opus_decode_float` could theoretically decode more frames with FEC.

```cpp
std::vector<float> decodeBuffer(kFrameSizeSamples * static_cast<size_t>(channels));
int frameSamples = opus_decode_float(decoder,
                                     reinterpret_cast<const unsigned char*>(data.data()),
                                     static_cast<opus_int32>(data.size()),
                                     decodeBuffer.data(),
                                     static_cast<int>(kFrameSizeSamples),  // Max frames
                                     0);
```

**Impact**: Very low - Opus decoder respects the max frames parameter.

**Fix**: Add assert/check:
```cpp
if (frameSamples > static_cast<int>(kFrameSizeSamples)) {
    log("WARNING: Opus decoder returned more frames than expected");
    frameSamples = static_cast<int>(kFrameSizeSamples);
}
```

---

### Issue #18: Potential String Temporary Lifetime Issue (LOW)

**File**: `WebRTCSession.cpp:552-558`

**Problem**: `effectivePassword()` returns `std::optional<std::string>`, which is evaluated inline.

```cpp
std::string WebRTCSession::buildHashedStreamId() const {
    const auto password = effectivePassword();
    if (!password) {
        return config_.streamId;
    }
    const auto suffix = hashStreamIdSuffix(*password);  // Dereferences optional
    return config_.streamId + suffix;
}
```

Actually safe - the optional is copied to local variable. False alarm.

---

## LOW PRIORITY ISSUES

### Issue #19: Inconsistent Error Handling in setState (LOW)

**File**: `PluginProcessor.cpp:522-524`

**Problem**: Silently ignores malformed state without logging.

```cpp
try {
    const auto json = nlohmann::json::parse(serialized);
    // ...
} catch (...) {
    // ignore malformed state chunks
}
```

**Impact**: User loses saved settings silently.

**Fix**: Log the error:
```cpp
} catch (const std::exception& ex) {
    SMTG_DBPRT1("[WebRTC] Failed to restore state: %s\n", ex.what());
}
```

---

### Issue #20: No Protection Against Infinite Pending ICE Queue (LOW)

**File**: `WebRTCSession.cpp:1397`

**Problem**: `pendingGlobalIce_` queue has no size limit.

```cpp
PeerSession* session = locateSession();
if (!session) {
    pendingGlobalIce_.push_back({"candidate", message});  // Unbounded!
    return;
}
```

**Impact**: If peer never connects, queue grows indefinitely (memory leak).

**Fix**: Add limit:
```cpp
constexpr size_t kMaxPendingIce = 100;
if (pendingGlobalIce_.size() >= kMaxPendingIce) {
    pendingGlobalIce_.erase(pendingGlobalIce_.begin());  // Drop oldest
}
pendingGlobalIce_.push_back({"candidate", message});
```

---

## SUMMARY TABLE

| Issue | Severity | File | Lines | Description |
|-------|----------|------|-------|-------------|
| #6 | **CRITICAL** | WebRTCSession.cpp | 884-897 | Missing shutdown guard in onDataChannel::onOpen |
| #7 | **CRITICAL** | WebRTCSession.cpp | 899-920 | Missing shutdown guard in onMessage callback |
| #8 | **HIGH** | WebRTCSession.cpp | 1060-1070 | Opus codec creation failure not fatal |
| #9 | **HIGH** | WebRTCSession.cpp | 339-348 | TOCTOU race in emitStatus() |
| #10 | **HIGH** | WebRTCSession.cpp | 1083-1109 | Missing shutdown guards in signaling callbacks |
| #11 | MEDIUM | AudioRingBuffer.cpp | 33-46 | Potential integer overflow (unlikely) |
| #12 | MEDIUM | PluginProcessor.cpp | process() | No validation of channel count |
| #13 | MEDIUM | WebRTCSession.cpp | 1175-1178 | Potential deadlock (mitigated) |
| #14 | MEDIUM | WebRTCSession.cpp | 1153 | Inconsistent locking for configUpdateSink_ |
| #15 | MEDIUM | WebRTCSession.cpp | 840-847 | Memory allocation in audio callback |
| #16 | MEDIUM | WebRTCSession.cpp | 1260-1273 | Missing JSON type validation |
| #17 | LOW | WebRTCSession.cpp | 826-832 | No bounds check on Opus decode |
| #18 | LOW | WebRTCSession.cpp | 552-558 | False alarm - actually safe |
| #19 | LOW | PluginProcessor.cpp | 522-524 | Silent error in setState |
| #20 | LOW | WebRTCSession.cpp | 1397 | Unbounded pendingGlobalIce queue |

---

## RECOMMENDED FIX PRIORITY

1. **Immediate (CRITICAL)**: Issues #6, #7 - Add shutdown guards to datachannel callbacks
2. **High Priority**: Issues #8, #9, #10 - Codec error handling, emitStatus race, signaling callback guards
3. **Medium Priority**: Issues #12, #15 - Channel validation, memory allocation in callback
4. **Low Priority**: Issues #11, #16, #19, #20 - Edge cases and defensive programming

---

## TESTING RECOMMENDATIONS

After fixes:
1. **Thread Sanitizer**: Compile with `-fsanitize=thread` to catch remaining races
2. **Stress Test**: Rapid open/close 1000x to trigger shutdown races
3. **Invalid Input Test**: Send malformed JSON, wrong channel counts
4. **Memory Test**: Run under Valgrind/AddressSanitizer to catch leaks

---

## CONCLUSION

The codebase has **10 critical/high-severity issues** beyond the 5 already fixed. Most involve missing `shuttingDown_` guards in callbacks that capture `this`. The shutdown sequence is complex with multiple threads, and every callback must defend against use-after-free.

**Root Cause**: Insufficient shutdown protection in callbacks added after the initial TOCTOU fix.

**Systemic Fix**: Audit ALL lambdas that capture `[this]` and ensure they check `shuttingDown_` before accessing members.
