// No networking, queues or host scheduling: measure the existing resampler and
// default Opus AUDIO mode alone. Vary both the encoding start and the
// 10 ms analysis-window origin: the latter is independent of Opus frames in
// a live host. Generate the same float samples/envelope as the CLI probe.
#include "LinearResampler.h"
#include <opus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr double tau = 2 * std::numbers::pi;

double residual(const float* samples, size_t count, int channel, double hz, double rate) {
    double energy = 0, sine = 0, cosine = 0, dc = 0;
    for (size_t i = 0; i < count; ++i) {
        const double value = samples[2 * i + channel], angle = tau * hz * i / rate;
        energy += value * value;
        sine += value * std::sin(angle);
        cosine += value * std::cos(angle);
        dc += value;
    }
    const double fundamental = 2 * (sine * sine + cosine * cosine) / count;
    return std::sqrt(std::max(0., energy - fundamental - dc * dc / count) /
                     std::max(fundamental, 1e-15)) * 100;
}
}

int main(int argc, char** argv) {
    try {
        const double rate = argc > 1 ? std::stod(argv[1]) : 44100;
        const std::array<double, 6> rates{32000, 44100, 48000, 88200, 96000, 192000};
        if (std::find(rates.begin(), rates.end(), rate) == rates.end())
            throw std::invalid_argument("Use a rate supported by the playback probe");
        const size_t window = static_cast<size_t>(rate / 100);
        for (int offset : {0, 256, 512, 768, 1024, 1280, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 147456, 150016, 153600}) {
            int error = 0;
            std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
                opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error), opus_encoder_destroy);
            if (!encoder || error != OPUS_OK) throw std::runtime_error("Encoder creation failed");
            std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
                opus_decoder_create(48000, 2, &error), opus_decoder_destroy);
            if (!decoder || error != OPUS_OK) throw std::runtime_error("Decoder creation failed");
            webrtc_vst::LinearResampler send, receive;
            send.configure(rate, 48000, 2);
            receive.configure(48000, rate, 2);
            std::vector<float> left(128), right(128), part, queued, pcm(1920), output, converted;
            const float* inputs[]{left.data(), right.data()};
            double phases[2] = {0, 0};
            for (int base = 0; base < rate * 45 + offset; base += 128) {
                for (int i = 0; i < 128; ++i) {
                    float values[2];
                    for (int ch = 0; ch < 2; ++ch) {
                        values[ch] = static_cast<float>(.25 * std::sin(phases[ch]));
                        phases[ch] += tau * (ch ? 1500 : 1000) / rate;
                        if (phases[ch] >= tau) phases[ch] -= tau;
                        if ((base + i) % static_cast<int>(rate * 2) >= rate / 2)
                            values[ch] *= .2f;
                    }
                    left[i] = values[0]; right[i] = values[1];
                }
                if (base < offset) continue;
                send.processPlanar(inputs, 128, 2, part);
                queued.insert(queued.end(), part.begin(), part.end());
                while (queued.size() >= 1920) {
                    std::array<unsigned char, 4096> packet{};
                    const int bytes = opus_encode_float(encoder.get(), queued.data(), 960,
                                                       packet.data(), packet.size());
                    if (bytes <= 0 || opus_decode_float(decoder.get(), packet.data(), bytes,
                                                       pcm.data(), 960, 0) != 960)
                        throw std::runtime_error("Codec frame failed");
                    receive.processInterleaved(pcm.data(), 960, 2, converted);
                    output.insert(output.end(), converted.begin(), converted.end());
                    queued.erase(queued.begin(), queued.begin() + 1920);
                }
            }
            for (size_t analysisOffset = 0; analysisOffset < window; analysisOffset += 32) {
                std::array<std::vector<double>, 2> measures;
                for (size_t frame = static_cast<size_t>(rate * 8) + analysisOffset; frame + window < output.size() / 2; frame += window) {
                    const double time = std::fmod((offset + frame) / rate, 2.);
                    // Stay well inside a high plateau, clear of Opus lookahead and
                    // the envelope transitions, after eight seconds of settling.
                    if (time < .11 || time > .39) continue;
                    for (int channel = 0; channel < 2; ++channel)
                        measures[channel].push_back(residual(output.data() + 2 * frame, window,
                                                            channel, channel ? 1500 : 1000, rate));
                }
                std::cout << "rate=" << rate << " offset=" << offset << " analysisOffset=" << analysisOffset;
                for (auto& values : measures) {
                    if (values.empty()) throw std::runtime_error("No measurement windows");
                    std::sort(values.begin(), values.end());
                    std::cout << " median=" << values[values.size() / 2]
                              << " p95=" << values[static_cast<size_t>(values.size() * .95)];
                }
                std::cout << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
