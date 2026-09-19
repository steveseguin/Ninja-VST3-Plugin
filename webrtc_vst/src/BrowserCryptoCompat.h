#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

namespace webrtc_vst {

inline std::string encodeBrowserPassword(const std::string& text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')')
            encoded += static_cast<char>(c);
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return encoded;
}

// VDO.Ninja's legacy AES phrase encoder uses Uint8Array(str.length) and
// charCodeAt: the LOW BYTE OF EACH UTF-16 CODE UNIT, not UTF-8. Only AES key
// material uses this; stream/room/password hashes must remain UTF-8 SHA-256.
inline std::string browserAesKeyBytes(const std::string& utf8) {
    std::string bytes;
    for (size_t i = 0; i < utf8.size();) {
        const auto first = static_cast<uint8_t>(utf8[i++]);
        uint32_t point = first;
        unsigned remaining = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { point &= 31; remaining = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { point &= 15; remaining = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { point &= 7; remaining = 3; minimum = 0x10000; }
        else if (first >= 0x80) throw std::invalid_argument("Invalid UTF-8");
        for (unsigned n = 0; n < remaining; ++n) {
            if (i == utf8.size()) throw std::invalid_argument("Truncated UTF-8");
            const auto c = static_cast<uint8_t>(utf8[i++]);
            if ((c & 0xc0) != 0x80) throw std::invalid_argument("Invalid UTF-8 continuation");
            point = (point << 6) | (c & 63);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
            throw std::invalid_argument("Invalid UTF-8 code point");
        if (point >= 0x10000) {
            point -= 0x10000;
            bytes += static_cast<char>((0xd800 + (point >> 10)) & 255);
            bytes += static_cast<char>((0xdc00 + (point & 1023)) & 255);
        } else bytes += static_cast<char>(point & 255);
    }
    return bytes;
}

} // namespace webrtc_vst
