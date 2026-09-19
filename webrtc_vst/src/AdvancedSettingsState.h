#pragma once

#include "PluginConfig.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace webrtc_vst {

inline void readAdvancedSettings(const nlohmann::json& json, PluginConfig& config) {
    const auto handshake = normalizeEndpoint(json.value("handshakeUrl", json.value("signalingUrl", config.handshakeUrl)), false);
    const auto web = normalizeEndpoint(json.value("webBaseUrl", std::string(kDefaultWebBaseUrl)), true);
    if (!handshake || !web) throw std::invalid_argument("Invalid advanced endpoint");
    // Preserve the WSS-derived salt used by projects saved before these options.
    auto salt = json.value("salt", json.contains("webBaseUrl") ? std::string{} : saltForUrl(*handshake));
    if (salt.find('\0') != std::string::npos) throw std::invalid_argument("Invalid salt");
    config.handshakeUrl = *handshake;
    config.webBaseUrl = *web;
    config.salt = std::move(salt);
}

} // namespace webrtc_vst
