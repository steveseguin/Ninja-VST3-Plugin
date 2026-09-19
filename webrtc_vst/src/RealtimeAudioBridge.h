#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace webrtc_vst {

// Two independent single-producer/single-consumer queues. The host owns TX
// writes/RX reads; the serialized worker owns TX reads/RX writes and reset().
// No try-lock dropout, spin, allocation or waiting on the host audio thread.
class RealtimeAudioBridge {
public:
    static constexpr size_t capacity = 8192;
    static constexpr size_t chunk = 512;
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    void transfer(const float* const* input, float* const* output, size_t frames,
                  int inputChannels, int outputChannels, bool publish) noexcept {
        if (publish && input) {
            const auto written = tx_.written.load(std::memory_order_relaxed);
            const auto read = tx_.read.load(std::memory_order_acquire);
            const auto count = std::min<uint64_t>(frames, capacity - (written - read));
            for (size_t i = 0; i < count; ++i) for (int ch = 0; ch < 2; ++ch) {
                const int source = std::min(ch, inputChannels - 1);
                const float value = source >= 0 && input[source] ? input[source][i] : 0;
                tx_.samples[ch][(written + i) % capacity] = finite(value);
            }
            tx_.written.store(written + count, std::memory_order_release);
        } else if (!publish && output) {
            if (discardRx_.exchange(false, std::memory_order_acquire))
                rx_.read.store(rx_.written.load(std::memory_order_acquire), std::memory_order_release);
            const auto read = rx_.read.load(std::memory_order_relaxed);
            const auto written = rx_.written.load(std::memory_order_acquire);
            const auto count = std::min<uint64_t>(frames, written - read);
            for (size_t i = 0; i < count; ++i) for (int ch = 0; ch < std::min(2, outputChannels); ++ch)
                if (output[ch]) output[ch][i] = outputChannels == 1
                    ? (rx_.samples[0][(read+i)%capacity] + rx_.samples[1][(read+i)%capacity])*0.5f
                    : rx_.samples[ch][(read+i)%capacity];
            rx_.read.store(read + count, std::memory_order_release);
            // Four host blocks of headroom (minimum 1024 frames) absorb worker
            // scheduling variation. The target follows consumption, never time.
            const auto reserve = std::min(capacity, std::max(size_t{1024}, std::min(frames, capacity/4)*4));
            wanted_.store(read + count + reserve, std::memory_order_release);
        }
    }
    size_t takeInput(float* const* output) noexcept {
        const auto read = tx_.read.load(std::memory_order_relaxed);
        const auto written = tx_.written.load(std::memory_order_acquire);
        const auto count = std::min<uint64_t>(chunk, written - read);
        for (size_t i = 0; i < count; ++i) for (int ch = 0; ch < 2; ++ch)
            output[ch][i] = tx_.samples[ch][(read+i)%capacity];
        tx_.read.store(read + count, std::memory_order_release);
        return count;
    }
    size_t takeDemand() noexcept {
        const auto wanted = wanted_.load(std::memory_order_acquire);
        const auto written = rx_.written.load(std::memory_order_relaxed);
        const auto read = rx_.read.load(std::memory_order_acquire);
        const auto missing = wanted > written ? wanted - written : 0;
        return std::min<uint64_t>({chunk, missing, capacity - (written - read)});
    }
    void supplyOutput(const float* const* input, size_t frames) noexcept {
        const auto written = rx_.written.load(std::memory_order_relaxed);
        const auto read = rx_.read.load(std::memory_order_acquire);
        const auto count = std::min<uint64_t>(frames, capacity - (written - read));
        for (size_t i = 0; i < count; ++i) for (int ch = 0; ch < 2; ++ch)
            rx_.samples[ch][(written+i)%capacity] = finite(input[ch][i]);
        rx_.written.store(written + count, std::memory_order_release);
    }
    void reset() noexcept {
        // Only consumers move read cursors. Never rewrite live producer cursors
        // or sample storage from the configuration thread.
        tx_.read.store(tx_.written.load(std::memory_order_acquire), std::memory_order_release);
        wanted_.store(rx_.written.load(std::memory_order_relaxed), std::memory_order_release);
        discardRx_.store(true, std::memory_order_release);
    }
private:
    static float finite(float value) noexcept { return std::isfinite(value) ? value : 0; }
    struct Queue {
        std::array<std::array<float, capacity>, 2> samples{};
        alignas(64) std::atomic<uint64_t> written{0};
        alignas(64) std::atomic<uint64_t> read{0};
    } tx_, rx_;
    std::atomic<uint64_t> wanted_{0};
    std::atomic<bool> discardRx_{false};
};
} // namespace webrtc_vst
