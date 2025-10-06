#pragma once

#include <cstddef>
#include <random>
#include <string>

namespace webrtc_vst {

inline std::string generateRandomStreamId(std::size_t length = 8) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(alphabet) - 2);

    std::string id;
    id.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        id.push_back(alphabet[dist(rng)]);
    }
    return id;
}

} // namespace webrtc_vst
