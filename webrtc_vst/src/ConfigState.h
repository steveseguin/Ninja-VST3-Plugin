#pragma once

#include "AdvancedSettingsState.h"
#include "BrowserCryptoCompat.h"
#include "BoundedJson.h"
#include <pluginterfaces/base/ibstream.h>
#include <array>

namespace webrtc_vst {

inline constexpr size_t kMaxStateBytes = 16384;
inline constexpr size_t kMaxSettingBytes = 512;

inline void validateSettingText(const std::string& value) {
    if (value.size() > kMaxSettingBytes || value.find_first_of(std::string("\0\r\n", 3)) != std::string::npos)
        throw std::invalid_argument("Invalid or oversized setting");
    size_t utf16Units = 0;
    for (unsigned char c : value) if ((c & 0xc0) != 0x80) utf16Units += c >= 0xf0 ? 2 : 1;
    if (utf16Units > 127) throw std::invalid_argument("Setting exceeds 127 UTF-16 characters");
    (void)browserAesKeyBytes(value); // Strict UTF-8 validation, including environment/UI input.
}

// Validate into a local candidate. Callers commit once, only after success.
inline PluginConfig parseConfigState(const std::string& text, const PluginConfig& previous) {
    if (text.size() > kMaxStateBytes) throw std::invalid_argument("State too large");
    const auto json = parseBoundedJson(text, kMaxStateBytes);
    if (!json.is_object()) throw std::invalid_argument("State must be an object");
    auto candidate = previous;
    readAdvancedSettings(json, candidate);
    for (const auto* key : {"streamId", "roomName", "roomId", "password", "salt", "mode", "webBaseUrl", "handshakeUrl", "signalingUrl"}) {
        if (json.contains(key)) validateSettingText(json.at(key).get<std::string>());
    }
    if (json.contains("disableEncryption")) (void)json.at("disableEncryption").get<bool>();
    if (json.contains("streamId")) candidate.streamId = json.at("streamId").get<std::string>();
    if (json.contains("roomName")) candidate.roomName = json.at("roomName").get<std::string>();
    else if (json.contains("roomId")) candidate.roomName = json.at("roomId").get<std::string>();
    if (json.contains("password")) candidate.password = json.at("password").get<std::string>();
    if (json.contains("mode")) {
        const auto mode = json.at("mode").get<std::string>();
        if (mode != "seed" && mode != "publish" && mode != "play") throw std::invalid_argument("Invalid mode");
        candidate.mode = mode == "play" ? ConnectionMode::Play : ConnectionMode::Publish;
    }
    auto password = trimSetting(candidate.password);
    std::transform(password.begin(), password.end(), password.begin(), [](unsigned char c) { return std::tolower(c); });
    candidate.disableEncryption = password == "0" || password == "off" || password == "false";
    return candidate;
}

inline bool readBoundedState(Steinberg::IBStream* stream, std::string& result) {
    if (!stream) return false;
    result.clear();
    std::array<char, 4096> buffer{};
    for (;;) {
        Steinberg::int32 count = 0;
        const auto status = stream->read(buffer.data(), static_cast<Steinberg::int32>(buffer.size()), &count);
        if (count < 0 || count > static_cast<Steinberg::int32>(buffer.size()) || result.size() + count > kMaxStateBytes)
            return false;
        result.append(buffer.data(), static_cast<size_t>(count));
        if (!count) return status == Steinberg::kResultOk || status == Steinberg::kResultFalse;
        // Some hosts return kResultFalse for a successful short read at EOF.
        if (status == Steinberg::kResultFalse && count < static_cast<Steinberg::int32>(buffer.size())) return true;
        if (status != Steinberg::kResultOk) return false;
    }
}

} // namespace webrtc_vst
