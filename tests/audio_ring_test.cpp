#include "../webrtc_vst/src/AudioRingBuffer.h"
#include "../webrtc_vst/src/LinearResampler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        // Independent FIFO model checks exact samples, overwrite-oldest policy,
        // partial reads, zero fill, arbitrary wrap boundaries and layout changes.
        std::mt19937 random(0x56444f);
        for (size_t capacity : {size_t{0}, size_t{1}, size_t{7}, size_t{513}, size_t{2048}}) {
            webrtc_vst::AudioRingBuffer ring(capacity, 2);
            std::deque<std::vector<float>> expected;
            int layout = 2;
            for (int operation = 0; operation < 10000; ++operation) {
                const int channels = 1 + random() % 4;
                const size_t frames = random() % 1100;
                std::vector<std::vector<float>> data(channels, std::vector<float>(frames, -999));
                std::vector<float*> output;
                std::vector<const float*> input;
                for (auto& channel : data) { output.push_back(channel.data()); input.push_back(channel.data()); }
                if (random() % 2) {
                    if (frames && layout != channels) { layout = channels; expected.clear(); }
                    for (size_t i = 0; i < frames; ++i) {
                        std::vector<float> frame;
                        for (int ch = 0; ch < channels; ++ch) {
                            data[ch][i] = static_cast<float>(operation * 4096 + i * 4 + ch);
                            frame.push_back(data[ch][i]);
                        }
                        expected.push_back(frame);
                        if (expected.size() > capacity) expected.pop_front();
                    }
                    ring.push(input.data(), frames, channels);
                } else {
                    const auto count = layout == channels ? std::min(frames, expected.size()) : 0;
                    require(ring.pop(output.data(), frames, channels) == count, "Read count differs from FIFO model");
                    for (size_t i = 0; i < frames; ++i) {
                        for (int ch = 0; ch < channels; ++ch)
                            require(data[ch][i] == (i < count ? expected.front()[ch] : 0), "Sample order, channel or zero fill changed");
                        if (i < count) expected.pop_front();
                    }
                }
                require(ring.availableFrames(layout) == expected.size(), "Buffered frame count differs from model");
                if (operation % 97 == 0) { ring.reset(capacity, layout); expected.clear(); }
            }
        }
        std::cout << "PASS: 50000 randomized ring operations against independent FIFO model\n";
        // A linear ramp has an exact analytic resampling result. Packet/chunk
        // boundaries must neither lose samples nor alter the fractional phase.
        for (double hostRate : {8000., 32000., 44100., 48000., 88200., 96000., 192000., 384000.}) {
            for (bool incoming : {false, true}) {
                const double from = incoming ? 48000. : hostRate;
                const double to = incoming ? hostRate : 48000.;
                for (size_t block : {size_t{1}, size_t{127}, size_t{960}}) {
                    webrtc_vst::LinearResampler resampler;
                    resampler.configure(from, to, 2);
                    std::vector<float> actual, part, left(block), right(block), interleaved(block * 2);
                    const float* input[]{left.data(), right.data()};
                    constexpr size_t total = 4800;
                    for (size_t offset = 0; offset < total; offset += block) {
                        const auto count = std::min(block, total - offset);
                        for (size_t i = 0; i < count; ++i) {
                            left[i] = static_cast<float>(offset + i);
                            right[i] = -left[i];
                            interleaved[2*i] = left[i]; interleaved[2*i+1] = right[i];
                        }
                        const auto produced = incoming
                            ? resampler.processInterleaved(interleaved.data(), count, 2, part)
                            : resampler.processPlanar(input, count, 2, part);
                        if (produced) {
                            require(part.size() == produced * 2, "Resampler output count mismatch");
                            actual.insert(actual.end(), part.begin(), part.end());
                        }
                    }
                    const double step = from / to;
                    // Preserve receive priming: the interleaved API repeats
                    // its initial sample, while planar input waits for a pair.
                    const size_t expected = from == to ? total : static_cast<size_t>(std::ceil((total - (incoming ? 0 : 1)) / step));
                    // Floating-point phase can place the final endpoint on
                    // either side of a boundary, but must not accumulate loss.
                    if (std::abs(static_cast<double>(actual.size()/2) - expected) > 1)
                        std::cerr << "from=" << from << " to=" << to << " block=" << block
                                  << " expected=" << expected << " actual=" << actual.size()/2 << '\n';
                    require(std::abs(static_cast<double>(actual.size()/2) - expected) <= 1, "Resampler lost frames across chunk boundaries");
                    for (size_t i = 0; i < actual.size()/2; ++i) {
                        require(std::abs(actual[2*i] - std::max(0.0, i * step - (incoming && from != to ? 1 : 0))) < 0.002, "Resampler phase discontinuity");
                        require(actual[2*i+1] == -actual[2*i], "Resampler crossed channels");
                    }
                    resampler.reset();
                    const auto empty = incoming ? resampler.processInterleaved(interleaved.data(), 0, 2, part)
                        : resampler.processPlanar(input, 0, 2, part);
                    require(empty == 0, "Empty resampler input produced frames");
                }
            }
        }
        std::cout << "PASS: exact ramp/phase/frame-count checks at eight rates in both directions, three block sizes\n";
        if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
            for (bool packed : {false, true}) for (size_t frames : {size_t{64}, size_t{256}, size_t{512}, size_t{960}}) {
                webrtc_vst::AudioRingBuffer ring(2048, 2);
                std::vector<float> left(frames, 0.25f), right(frames, -0.5f);
                std::vector<float> interleaved(frames * 2);
                for (size_t i = 0; i < frames; ++i) { interleaved[2*i] = left[i]; interleaved[2*i+1] = right[i]; }
                const float* input[]{left.data(), right.data()};
                float* output[]{left.data(), right.data()};
                constexpr int iterations = 100000;
                const auto start = std::chrono::steady_clock::now();
                for (int n = 0; n < iterations; ++n) {
                    if (packed) ring.pushInterleaved(interleaved.data(), frames, 2);
                    else ring.push(input, frames, 2);
                    ring.pop(output, frames, 2);
                }
                const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
                require(left.back() == 0.25f && right.back() == -0.5f, "Benchmark corrupted samples");
                std::cout << "input=" << (packed ? "interleaved" : "planar") << " frames=" << frames
                          << " ns_per_stereo_frame=" << ns / (iterations * frames) << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
