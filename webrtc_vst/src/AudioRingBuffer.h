#pragma once

#include <cstddef>
#include "SpinLock.h"
#include <mutex>
#include <vector>

namespace webrtc_vst {

// Lock-protected ring buffer for interleaved floating point audio.
class AudioRingBuffer {
public:
    AudioRingBuffer() = default;
    AudioRingBuffer(size_t frameCapacity, int channelCount);

    void reset(size_t frameCapacity, int channelCount);

    // Push planar input data into the buffer. `inputs` is an array of pointers to channel data.
    void push(const float* const* inputs, size_t frames, int channels);

    // Copy interleaved frames out into planar output buffers. Returns frames actually written.
    size_t pop(float* const* outputs, size_t frames, int channels);

    size_t availableFrames(int channels) const;

private:
    void resetLocked(size_t frameCapacity, int channelCount);

    mutable SpinLock mutex_;
    std::vector<float> buffer_;
    size_t frameCapacity_{0};
    size_t writePos_{0};
    size_t readPos_{0};
    size_t size_{0}; // number of samples currently stored
    int channelCount_{0};
};

} // namespace webrtc_vst

