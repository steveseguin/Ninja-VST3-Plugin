#include "../webrtc_vst/src/AudioRingBuffer.h"
#include "../webrtc_vst/src/PeerAudioBufferSizing.h"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkRate(double rate) {
    const auto sizing = webrtc_vst::peerAudioBufferSizing(rate);
    const auto packetFrames = static_cast<size_t>(std::ceil(rate / 50.0));
    require(sizing.capacityFrames >= 2 * packetFrames,
            "Receive ring cannot hold two resampled 20 ms packets");
    require(sizing.preFillFrames <= sizing.capacityFrames, "Pre-fill can never complete");
    require(sizing.preFillFrames >= packetFrames, "High-rate pre-fill is shorter than 20 ms");
    if (rate <= 48000) {
        require(sizing.capacityFrames == 2048 && sizing.preFillFrames == 960,
                "Existing low-rate minimum sizes changed");
    }

    webrtc_vst::AudioRingBuffer buffer(sizing.capacityFrames, 2);
    // A packet callback pushes its entire resampled block at once. Let two
    // callbacks finish before the consumer runs, just as the 48 kHz ring allows.
    std::vector<float> left(packetFrames), right(packetFrames);
    const float* input[] = {left.data(), right.data()};
    for (int packet = 0; packet < 2; ++packet) {
        for (size_t i = 0; i < packetFrames; ++i) {
            left[i] = static_cast<float>(packet * packetFrames + i) / 20000.0f;
            right[i] = -left[i];
        }
        buffer.push(input, packetFrames, 2);
    }
    const size_t expected = 2 * packetFrames;
    require(buffer.availableFrames(2) == expected, "A resampled packet was truncated");
    std::vector<float> outLeft(expected), outRight(expected);
    float* output[] = {outLeft.data(), outRight.data()};
    require(buffer.pop(output, expected, 2) == expected, "Receive ring lost frames");
    for (size_t i = 0; i < expected; ++i) {
        const auto sample = static_cast<float>(i) / 20000.0f;
        require(outLeft[i] == sample && outRight[i] == -sample,
                "Receive ring dropped or reordered stereo samples");
    }
    buffer.reset(sizing.capacityFrames, 2);
    require(buffer.availableFrames(2) == 0, "Reset retained old peer audio");
}
}

int main() {
    try {
        for (double rate : {8000., 16000., 22050., 44100., 48000., 88200., 96000., 192000., 384000.})
            checkRate(rate);
        std::cout << "PASS: receive packet retention, stereo continuity, pre-fill and reset across host rates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
