#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace webrtc_vst {

// Streaming linear interpolation shared by the publish and receive paths.
class LinearResampler {
public:
    void configure(double inputRate, double outputRate, int channels);
    void reset();
    bool isPassthrough() const { return passthrough_; }
    size_t processPlanar(const float* const* inputs, size_t frames, int channels, std::vector<float>& outputInterleaved);
    size_t processInterleaved(const float* data, size_t frames, int channels, std::vector<float>& outputInterleaved);
    size_t processInterleaved(const std::vector<float>& data, int channels, std::vector<float>& outputInterleaved);

private:
    size_t processBuffer(const float* source, size_t frames, int channels, std::vector<float>& outputInterleaved);

    double inputRate_{48000.0};
    double outputRate_{48000.0};
    double step_{1.0};
    double phase_{0.0};
    bool configured_{false};
    bool passthrough_{true};
    bool havePrev_{false};
    int channels_{0};
    std::vector<float> prevSamples_;
    std::vector<float> sourceBuffer_;
};

inline void LinearResampler::configure(double inputRate, double outputRate, int channels) {
    inputRate_ = inputRate;
    outputRate_ = outputRate;
    channels_ = channels;
    step_ = inputRate_ / outputRate_;
    phase_ = 0.0;
    configured_ = true;
    passthrough_ = std::abs(inputRate_ - outputRate_) < 1e-6;
    havePrev_ = false;
    prevSamples_.assign(static_cast<size_t>(std::max(0, channels_)), 0.0f);
}

inline void LinearResampler::reset() {
    phase_ = 0.0;
    havePrev_ = false;
    prevSamples_.assign(static_cast<size_t>(std::max(0, channels_)), 0.0f);
}

inline size_t LinearResampler::processPlanar(const float* const* inputs,
                                                     size_t frames,
                                                     int channels,
                                                     std::vector<float>& outputInterleaved) {
    if (!configured_ || channels != channels_) {
        configure(inputRate_, outputRate_, channels);
    }

    if (passthrough_) {
        outputInterleaved.resize(frames * static_cast<size_t>(channels));
        for (size_t frame = 0; frame < frames; ++frame) {
            for (int ch = 0; ch < channels; ++ch) {
                outputInterleaved[frame * channels + ch] = inputs[ch][frame];
            }
        }
        if (frames > 0) {
            for (int ch = 0; ch < channels; ++ch) {
                prevSamples_[ch] = inputs[ch][frames - 1];
            }
            havePrev_ = true;
        }
        phase_ = 0.0;
        return frames;
    }

    if (frames == 0) {
        return 0;
    }

    size_t start = 0;
    if (!havePrev_) {
        for (int ch = 0; ch < channels; ++ch) {
            prevSamples_[ch] = inputs[ch][0];
        }
        havePrev_ = true;
        start = 1;
        if (frames == 1) {
            return 0;
        }
    }

    const size_t effectiveFrames = frames - start;
    sourceBuffer_.resize((effectiveFrames + 1) * static_cast<size_t>(channels));
    std::memcpy(sourceBuffer_.data(), prevSamples_.data(), sizeof(float) * static_cast<size_t>(channels));
    for (size_t frame = 0; frame < effectiveFrames; ++frame) {
        for (int ch = 0; ch < channels; ++ch) {
            sourceBuffer_[(frame + 1) * channels + ch] = inputs[ch][frame + start];
        }
    }

    return processBuffer(sourceBuffer_.data(), effectiveFrames + 1, channels, outputInterleaved);
}

inline size_t LinearResampler::processInterleaved(const float* data,
                                                          size_t frames,
                                                          int channels,
                                                          std::vector<float>& outputInterleaved) {
    if (!configured_ || channels != channels_) {
        configure(inputRate_, outputRate_, channels);
    }

    if (passthrough_) {
        outputInterleaved.assign(data, data + frames * static_cast<size_t>(channels));
        if (frames > 0) {
            std::memcpy(prevSamples_.data(), data + (frames - 1) * channels, sizeof(float) * static_cast<size_t>(channels));
            havePrev_ = true;
        }
        phase_ = 0.0;
        return frames;
    }

    if (frames == 0) {
        return 0;
    }

    sourceBuffer_.resize((frames + 1) * static_cast<size_t>(channels));
    if (!havePrev_) {
        std::memcpy(prevSamples_.data(), data, sizeof(float) * static_cast<size_t>(channels));
        havePrev_ = true;
    }

    std::memcpy(sourceBuffer_.data(), prevSamples_.data(), sizeof(float) * static_cast<size_t>(channels));
    std::memcpy(sourceBuffer_.data() + channels, data, sizeof(float) * frames * static_cast<size_t>(channels));

    return processBuffer(sourceBuffer_.data(), frames + 1, channels, outputInterleaved);
}

inline size_t LinearResampler::processInterleaved(const std::vector<float>& data,
                                                          int channels,
                                                          std::vector<float>& outputInterleaved) {
    const size_t frames = data.size() / static_cast<size_t>(channels);
    return processInterleaved(data.data(), frames, channels, outputInterleaved);
}

inline size_t LinearResampler::processBuffer(const float* source,
                                                     size_t frames,
                                                     int channels,
                                                     std::vector<float>& outputInterleaved) {
    outputInterleaved.clear();
    if (frames < 2) {
        return 0;
    }

    double pos = phase_;
    const double frameCount = static_cast<double>(frames);
    // A fractional position between the last two samples still has both
    // interpolation endpoints. Keep it, and carry only the next position
    // into the following block; otherwise upsampling loses frames per block.
    while (pos < frameCount - 1.0) {
        const size_t idx = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(idx);
        const float* frame0 = source + idx * channels;
        const float* frame1 = source + (idx + 1) * channels;
        for (int ch = 0; ch < channels; ++ch) {
            const float sample0 = frame0[ch];
            const float sample1 = frame1[ch];
            const float sample = static_cast<float>((1.0 - frac) * sample0 + frac * sample1);
            outputInterleaved.push_back(sample);
        }
        pos += step_;
    }

    phase_ = pos - static_cast<double>(frames - 1);
    if (phase_ < 0.0) {
        phase_ = 0.0;
    }

    std::memcpy(prevSamples_.data(), source + (frames - 1) * channels, sizeof(float) * static_cast<size_t>(channels));
    return outputInterleaved.empty() ? 0 : outputInterleaved.size() / static_cast<size_t>(channels);
}

} // namespace webrtc_vst
