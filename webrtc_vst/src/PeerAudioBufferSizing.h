#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace webrtc_vst {

struct PeerAudioBufferSizing {
    size_t capacityFrames;
    size_t preFillFrames;
};

// The receive ring stores resampled host frames, not 48 kHz Opus frames.
// Scale capacity and pre-fill durations with the host rate. The processor
// validates the host rate before this calculation.
inline PeerAudioBufferSizing peerAudioBufferSizing(double sampleRate, size_t maxHostBlock = 512) {
    const double scale = std::max(1.0, sampleRate / 48000.0);
    // Keep a partly consumed 20 ms PLC frame plus three arriving Opus packets.
    // The former 2048-frame minimum could overwrite that recovery burst and
    // introduce a phase jump. Capacity adds scheduling headroom; pre-fill stays
    // unchanged, so clean-stream startup latency does not increase.
    // Large host callbacks also need the bridge's four-block scheduling reserve.
    const size_t burstFrames = 4 * std::min(maxHostBlock, size_t{8192});
    return {std::max(static_cast<size_t>(std::ceil(4096.0 * scale)), burstFrames),
            static_cast<size_t>(std::ceil(960.0 * scale))};
}

} // namespace webrtc_vst
