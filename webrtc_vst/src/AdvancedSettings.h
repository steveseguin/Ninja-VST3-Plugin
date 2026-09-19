#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <regex>
#include <string>

namespace webrtc_vst {

inline constexpr auto kDefaultHandshakeUrl = "wss://wss.vdo.ninja";
inline constexpr auto kDefaultWebBaseUrl = "https://vdo.ninja/";

inline std::string trimSetting(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} :
        text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Deliberately accept URLs, not browser commands, credentials, or query strings
// that could override the generated stream/password/server parameters.
inline std::optional<std::string> normalizeEndpoint(const std::string& input, bool web) {
    auto value = trimSetting(input);
    if (value.size() > 127) return {};
    if (value.empty()) return web ? kDefaultWebBaseUrl : kDefaultHandshakeUrl;
    if (value.find("://") == std::string::npos) value = (web ? "https://" : "wss://") + value;
    const auto schemeEnd = value.find("://");
    auto scheme = value.substr(0, schemeEnd);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
    if (web ? (scheme != "https" && scheme != "http") : (scheme != "wss" && scheme != "ws")) return {};
    value.replace(0, schemeEnd, scheme);
    for (unsigned char c : value) {
        if (c <= 32 || c >= 127 || c == '\\' || c == '"' || c == '<' || c == '>') return {};
    }
    if (value.find('#') != std::string::npos || (web && value.find('?') != std::string::npos)) return {};
    const auto end = value.find_first_of("/?", schemeEnd + 3);
    auto authority = value.substr(schemeEnd + 3, end == std::string::npos ? end : end - schemeEnd - 3);
    const auto originalAuthoritySize = authority.size();
    static const std::regex authorityPattern(R"(^([A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?|\[[0-9a-fA-F:]+\])(:[0-9]{1,5})?$)");
    if (!std::regex_match(authority, authorityPattern)) return {};
    if (authority.front() == '[') {
        const auto bracket = authority.find(']');
        const auto address = authority.substr(1, bracket - 1);
        if (address.find(':') == std::string::npos) return {};
        const auto compression = address.find("::");
        if (compression != std::string::npos && address.find("::", compression + 2) != std::string::npos) return {};
        if (address.find(":::") != std::string::npos) return {};
        if ((address.front() == ':' && !address.starts_with("::")) ||
            (address.back() == ':' && !address.ends_with("::"))) return {};
        size_t groups = 0, start = 0;
        while (start < address.size()) {
            const auto next = address.find(':', start);
            const auto count = (next == std::string::npos ? address.size() : next) - start;
            if (count > 4) return {};
            if (count) ++groups;
            if (next == std::string::npos) break;
            start = next + 1;
        }
        if (compression == std::string::npos ? groups != 8 : groups >= 8) return {};
        // Canonical browser hostname spelling matters when the hostname is salt.
        std::array<unsigned, 8> words{};
        size_t index = 0;
        for (size_t pos = 0; pos < address.size();) {
            if (compression != std::string::npos && pos == compression) { index += 8 - groups; pos += 2; continue; }
            if (address[pos] == ':') { ++pos; continue; }
            auto next = address.find(':', pos);
            if (next == std::string::npos) next = address.size();
            words[index++] = static_cast<unsigned>(std::stoul(address.substr(pos, next - pos), nullptr, 16));
            pos = next;
        }
        size_t best = 8, length = 0;
        for (size_t i = 0; i < 8;) {
            if (words[i]) { ++i; continue; }
            size_t stop = i;
            while (stop < 8 && !words[stop]) ++stop;
            if (stop - i > length && stop - i >= 2) { best = i; length = stop - i; }
            i = stop;
        }
        std::string canonical;
        for (size_t i = 0; i < 8;) {
            if (i == best) { canonical += "::"; i += length; continue; }
            if (!canonical.empty() && canonical.back() != ':') canonical += ':';
            std::string word;
            unsigned n = words[i++];
            do { word.insert(word.begin(), "0123456789abcdef"[n & 15]); n >>= 4; } while (n);
            canonical += word;
        }
        authority = '[' + canonical + ']' + authority.substr(bracket + 1);
    } else {
        const auto host = authority.substr(0, authority.find(':'));
        const auto last = host.substr(host.rfind('.') == std::string::npos ? 0 : host.rfind('.') + 1);
        if (std::all_of(last.begin(), last.end(), [](unsigned char c) { return std::isdigit(c); }) ||
            (last.size() > 2 && last[0] == '0' && (last[1] == 'x' || last[1] == 'X'))) {
            static const std::regex canonicalIpv4(R"(^((0|[1-9][0-9]{0,2})\.){3}(0|[1-9][0-9]{0,2})$)");
            if (!std::regex_match(host, canonicalIpv4)) return {};
            size_t begin = 0;
            for (int n = 0; n < 4; ++n) {
                const auto dot = host.find('.', begin);
                if (std::stoul(host.substr(begin, dot == std::string::npos ? dot : dot - begin)) > 255) return {};
                begin = dot + 1;
            }
        }
        size_t start = 0;
        while (start < host.size()) {
            const auto next = host.find('.', start);
            const auto count = (next == std::string::npos ? host.size() : next) - start;
            if (!count || count > 63 || host[start] == '-' || host[start + count - 1] == '-') return {};
            if (next == std::string::npos) break;
            start = next + 1;
        }
    }
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.back() != ']') {
        const auto port = std::stoul(authority.substr(colon + 1));
        if (port == 0 || port > 65535) return {};
    }
    if (web && end == std::string::npos) value += '/';
    std::transform(authority.begin(), authority.end(), authority.begin(), [](unsigned char c) { return std::tolower(c); });
    value.replace(schemeEnd + 3, originalAuthoritySize, authority);
    if (value.size() > 127) return {};
    return value;
}

inline std::string endpointHost(const std::string& url) {
    const auto scheme = url.find("://");
    const auto start = scheme == std::string::npos ? 0 : scheme + 3;
    const auto end = url.find_first_of("/?#", start);
    auto host = url.substr(start, end == std::string::npos ? end : end - start);
    if (!host.empty() && host.front() == '[') host = host.substr(0, host.find(']') + 1);
    else host = host.substr(0, host.find(':'));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    return host;
}

// Same hostname rules as obsninja/webrtc.js. WSS and web domains are independent.
inline std::string saltForUrl(const std::string& url) {
    const auto host = endpointHost(url);
    if (host.empty() || host == "localhost" || host == "steveseguin.github.io")
        return "vdo.ninja";
    static const std::regex ipv4(R"(^((25[0-5]|2[0-4]\d|[0-1]?\d?\d)\.){3}(25[0-5]|2[0-4]\d|[0-1]?\d?\d)$)");
    if (std::regex_match(host, ipv4)) return "vdo.ninja";
    for (const std::string domain : {"vdo.ninja", "rtc.ninja", "versus.cam", "socialstream.ninja"}) {
        if (host == domain || (host.size() > domain.size() && host.ends_with("." + domain))) return domain;
    }
    return host;
}

inline std::string effectiveSalt(const std::string& salt, const std::string& webBaseUrl) {
    return salt.empty() ? saltForUrl(webBaseUrl) : salt;
}

} // namespace webrtc_vst
