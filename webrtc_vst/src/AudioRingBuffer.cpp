#include "AudioRingBuffer.h"

#include <algorithm>
#include <cstring>

namespace webrtc_vst {

AudioRingBuffer::AudioRingBuffer(size_t frameCapacity, int channelCount) {
    resetLocked(frameCapacity, channelCount);
}

void AudioRingBuffer::reset(size_t frameCapacity, int channelCount) {
    std::lock_guard<SpinLock> lock(mutex_);
    resetLocked(frameCapacity, channelCount);
}

void AudioRingBuffer::push(const float* const* inputs, size_t frames, int channels) {
    if (channels <= 0 || frames == 0) {
        return;
    }

    std::lock_guard<SpinLock> lock(mutex_);

    if (channelCount_ != channels) {
        resetLocked(frameCapacity_, channels);
    }

    const size_t capacity = buffer_.size();
    if (capacity == 0) {
        return;
    }

    const size_t totalSamples = frames * static_cast<size_t>(channels);
    if (totalSamples >= capacity) {
        const size_t framesToCopy = capacity / static_cast<size_t>(channels);
        const size_t startFrame = frames - framesToCopy;
        size_t idx = 0;
        for (size_t frame = startFrame; frame < frames; ++frame) {
            for (int ch = 0; ch < channels; ++ch) {
                buffer_[idx++] = inputs[ch][frame];
            }
        }
        readPos_ = 0;
        size_ = capacity;
        writePos_ = size_ % capacity;
        return;
    }

    if (size_ + totalSamples > capacity) {
        const size_t discarded = size_ + totalSamples - capacity;
        readPos_ = (readPos_ + discarded) % capacity;
        size_ -= discarded;
    }

    // At most two contiguous spans. Avoid a runtime integer division for every
    // sample while holding the lock; channel-wise loops can also vectorize.
    for (size_t frame = 0; frame < frames;) {
        const size_t count = std::min(frames - frame, (capacity - writePos_) / channels);
        for (int ch = 0; ch < channels; ++ch) {
            for (size_t i = 0; i < count; ++i)
                buffer_[writePos_ + i * channels + ch] = inputs[ch][frame + i];
        }
        writePos_ += count * channels;
        if (writePos_ == capacity) writePos_ = 0;
        frame += count;
    }

    size_ += totalSamples;
}

void AudioRingBuffer::pushInterleaved(const float* input, size_t frames, int channels) {
    if (channels <= 0 || frames == 0) return;
    std::lock_guard<SpinLock> lock(mutex_);
    if (channelCount_ != channels) resetLocked(frameCapacity_, channels);
    const size_t capacity = buffer_.size();
    if (capacity == 0) return;

    // Keep the newest complete frames, matching the planar push policy.
    if (frames > frameCapacity_) {
        input += (frames - frameCapacity_) * static_cast<size_t>(channels);
        frames = frameCapacity_;
    }
    const size_t samples = frames * static_cast<size_t>(channels);
    const size_t dropped = size_ + samples > capacity ? size_ + samples - capacity : 0;
    readPos_ = (readPos_ + dropped) % capacity;
    size_ -= dropped;
    const size_t first = std::min(samples, capacity - writePos_);
    std::copy_n(input, first, buffer_.data() + writePos_);
    std::copy_n(input + first, samples - first, buffer_.data());
    writePos_ = (writePos_ + samples) % capacity;
    size_ += samples;
}

size_t AudioRingBuffer::pop(float* const* outputs, size_t frames, int channels) {
    if (channels <= 0 || frames == 0) {
        return 0;
    }

    std::lock_guard<SpinLock> lock(mutex_);

    if (channelCount_ != channels) {
        for (int ch = 0; ch < channels; ++ch) {
            std::fill(outputs[ch], outputs[ch] + frames, 0.0f);
        }
        return 0;
    }

    const size_t capacity = buffer_.size();
    if (capacity == 0 || size_ == 0) {
        for (int ch = 0; ch < channels; ++ch) {
            std::fill(outputs[ch], outputs[ch] + frames, 0.0f);
        }
        return 0;
    }

    const size_t requestedSamples = frames * static_cast<size_t>(channels);
    const size_t samplesToRead = std::min(requestedSamples, size_);
    const size_t framesToRead = samplesToRead / static_cast<size_t>(channels);

    for (size_t frame = 0; frame < framesToRead;) {
        const size_t count = std::min(framesToRead - frame, (capacity - readPos_) / channels);
        for (int ch = 0; ch < channels; ++ch) {
            for (size_t i = 0; i < count; ++i)
                outputs[ch][frame + i] = buffer_[readPos_ + i * channels + ch];
        }
        readPos_ += count * channels;
        if (readPos_ == capacity) readPos_ = 0;
        frame += count;
    }

    for (size_t frame = framesToRead; frame < frames; ++frame) {
        for (int ch = 0; ch < channels; ++ch) {
            outputs[ch][frame] = 0.0f;
        }
    }

    size_ -= samplesToRead;
    return framesToRead;
}

size_t AudioRingBuffer::availableFrames(int channels) const {
    std::lock_guard<SpinLock> lock(mutex_);
    if (channels <= 0 || channelCount_ != channels) {
        return 0;
    }
    return size_ / static_cast<size_t>(channels);
}

void AudioRingBuffer::resetLocked(size_t frameCapacity, int channelCount) {
    frameCapacity_ = frameCapacity;
    channelCount_ = channelCount;
    buffer_.assign(frameCapacity * static_cast<size_t>(channelCount), 0.0f);
    writePos_ = 0;
    readPos_ = 0;
    size_ = 0;
}

} // namespace webrtc_vst
