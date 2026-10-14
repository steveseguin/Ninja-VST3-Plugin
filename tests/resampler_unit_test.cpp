#include "../webrtc_vst/src/LinearResampler.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<float> resample(const std::vector<float>& input, double inputRate,
                            double outputRate, size_t blockSize, bool planar) {
    webrtc_vst::LinearResampler resampler;
    resampler.configure(inputRate, outputRate, 2);
    std::vector<float> result, block, left, right;
    for (size_t offset = 0; offset < input.size() / 2; offset += blockSize) {
        const auto frames = std::min(blockSize, input.size() / 2 - offset);
        size_t produced = 0;
        if (planar) {
            left.resize(frames); right.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                left[i] = input[2 * (offset + i)];
                right[i] = input[2 * (offset + i) + 1];
            }
            const float* channels[] = {left.data(), right.data()};
            produced = resampler.processPlanar(channels, frames, 2, block);
        } else {
            produced = resampler.processInterleaved(input.data() + offset * 2, frames, 2, block);
        }
        if (produced) result.insert(result.end(), block.begin(), block.end());
    }
    return result;
}

void checkBlockBoundaries() {
    for (bool sine : {false, true}) {
        std::vector<float> input(4096 * 2);
        for (size_t i = 0; i < input.size() / 2; ++i) {
            input[2 * i] = sine ? static_cast<float>(0.5 * std::sin(i * 0.13))
                                : static_cast<float>(i) / 8192;
            input[2 * i + 1] = -input[2 * i];
        }
        for (bool planar : {false, true}) {
            for (const auto rates : {std::array<double, 2>{24000, 48000}, {48000, 192000},
                                     {48000, 48000}, {48000, 24000}, {44100, 48000}}) {
                const auto whole = resample(input, rates[0], rates[1], 4096, planar);
                for (size_t block : {1, 2, 16, 127, 512, 960}) {
                    const auto split = resample(input, rates[0], rates[1], block, planar);
                    // Non-integer ratios can differ by one final sample due to
                    // floating point summation; they must not lose one per block.
                    const auto difference = whole.size() > split.size()
                        ? whole.size() - split.size() : split.size() - whole.size();
                    require(difference <= 2, "Resampled length depends on host block size");
                    for (size_t i = 0; i < std::min(whole.size(), split.size()); ++i)
                        require(std::abs(whole[i] - split[i]) < 0.00001f,
                                "Resampled samples skip or drift at block boundaries");
                }
            }
        }
    }
}

void checkReceiveDurations() {
    const std::vector<float> input(48000 * 2, 0.25f);
    for (double rate : {48000., 88200., 96000., 192000., 384000.}) {
        const auto output = resample(input, 48000, rate, 960, false);
        require(std::abs(static_cast<double>(output.size() / 2) - rate) <= 1,
                "One second of Opus packets lost resampled frames");
    }
}

void checkEmptyAndSingleFrames() {
    for (bool planar : {false, true}) {
        webrtc_vst::LinearResampler resampler;
        resampler.configure(24000, 48000, 2);
        float left = 0.25f, right = -0.25f;
        const float* channels[] = {&left, &right};
        float interleaved[] = {left, right};
        std::vector<float> output;
        auto process = [&](size_t frames) {
            return planar ? resampler.processPlanar(channels, frames, 2, output)
                : resampler.processInterleaved(interleaved, frames, 2, output);
        };
        require(process(0) == 0, "Empty input produced frames");
        require(process(1) == (planar ? 0 : 2), "Initial single-frame buffering changed");
        require(process(0) == 0, "Empty input advanced resampler state");
        left = interleaved[0] = 0.75f;
        right = interleaved[1] = -0.75f;
        require(process(1) == 2, "Single-frame continuation lost interpolation endpoints");
        require(output == std::vector<float>({0.25f, -0.25f, 0.5f, -0.5f}),
                "Single-frame interpolation changed endpoint or midpoint samples");
    }
}

void checkReset() {
    webrtc_vst::LinearResampler resampler;
    resampler.configure(24000, 48000, 2);
    std::vector<float> input(20, 0.25f), output, first;
    resampler.processInterleaved(input, 2, first);
    resampler.processInterleaved(input, 2, output);
    resampler.reset();
    resampler.processInterleaved(input, 2, output);
    require(output == first, "Reset retained the previous stream phase");
}
}

int main() {
    try {
        checkBlockBoundaries();
        checkReceiveDurations();
        checkEmptyAndSingleFrames();
        checkReset();
        std::cout << "PASS: resampler ramp/sine continuity, block counts, receive duration, empty/single frames and reset\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
