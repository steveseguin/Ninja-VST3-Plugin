#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace webrtc_vst {
// ECMAScript String.trim whitespace, encoded as UTF-8. U+0085 and
// U+180E are deliberately absent, just as in the browser.
inline std::string_view whepTrim(std::string_view value) {
    constexpr std::string_view whitespace[] = {
        " ", "\t", "\r", "\n", "\f", "\v", "\xc2\xa0", "\xe1\x9a\x80",
        "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83",
        "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87",
        "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
        "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80", "\xef\xbb\xbf"
    };
    bool changed;
    do {
        changed = false;
        for (const auto item : whitespace) {
            if (value.starts_with(item)) { value.remove_prefix(item.size()); changed = true; }
            if (value.ends_with(item)) { value.remove_suffix(item.size()); changed = true; }
        }
    } while (changed);
    return value;
}

// String(number) formatting used by webrtc.js:whepSettingsMarker. In particular,
// numeric timestamps and their string forms identify the same publication.
inline std::string whepNumberMarker(double number) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    if (result.ec != std::errc{}) return {};
    std::string digits(buffer, result.ptr);
    const auto exponentAt = digits.find('e');
    const int exponent = exponentAt == std::string::npos ? 0 : std::stoi(digits.substr(exponentAt + 1));
    if (exponentAt != std::string::npos) digits.resize(exponentAt);
    const auto dot = digits.find('.');
    int decimalAt = static_cast<int>(dot == std::string::npos ? digits.size() : dot) + exponent;
    if (dot != std::string::npos) digits.erase(dot, 1);
    while (digits.size() > 1 && digits.front() == '0') { digits.erase(0, 1); --decimalAt; }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    if (number >= 1e-6 && number < 1e21) {
        if (decimalAt <= 0) return "0." + std::string(-decimalAt, '0') + digits;
        if (decimalAt >= static_cast<int>(digits.size())) return digits + std::string(decimalAt - digits.size(), '0');
        digits.insert(decimalAt, ".");
        return digits;
    }
    if (digits.size() > 1) digits.insert(1, ".");
    return digits + "e" + (decimalAt > 0 ? "+" : "") + std::to_string(decimalAt - 1);
}

inline std::string whepSettingsMarker(const nlohmann::json& settings,
                                      const std::string& url, const std::string& token) {
    if (settings.contains("started")) {
        const auto& started = settings["started"];
        if (started.is_boolean() && started.get<bool>()) return "true";
        if (started.is_number()) {
            const double value = started.get<double>();
            if (value > 0 && std::isfinite(value)) return whepNumberMarker(value);
        }
        if (started.is_string()) {
            const auto value = started.get<std::string>();
            auto trimmed = std::string(whepTrim(value));
            if (!trimmed.empty()) {
                std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), [](unsigned char c) { return std::tolower(c); });
                if (trimmed != "0" && trimmed != "false") return value;
            }
        }
    }
    return url + "|" + token;
}
} // namespace webrtc_vst
