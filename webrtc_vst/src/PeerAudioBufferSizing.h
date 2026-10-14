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
// Keep the existing minimum sizes for low rates and preserve the 48 kHz
// buffering durations at higher rates. The processor validates the host rate.
inline PeerAudioBufferSizing peerAudioBufferSizing(double sampleRate, size_t maxHostBlock = 512) {
    const double scale = std::max(1.0, sampleRate / 48000.0);
    // Large host callbacks produce/consume several packets in a burst. Match
    // the bridge's four-block scheduling reserve without increasing pre-fill
    // or the existing capacity for normal <=512-frame, 48 kHz callbacks.
    const size_t burstFrames = 4 * std::min(maxHostBlock, size_t{8192});
    return {std::max(static_cast<size_t>(std::ceil(2048.0 * scale)), burstFrames),
            static_cast<size_t>(std::ceil(960.0 * scale))};
}

} // namespace webrtc_vst
