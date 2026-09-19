#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace webrtc_vst {
inline nlohmann::json parseBoundedJson(const std::string& text, size_t limit) {
    if (text.size() > limit) throw std::invalid_argument("JSON exceeds size limit");
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32) throw std::invalid_argument("JSON exceeds nesting limit");
        } else if (c == '}' || c == ']') {
            if (!depth) throw std::invalid_argument("Unbalanced JSON");
            --depth;
        }
    }
    return nlohmann::json::parse(text);
}
} // namespace webrtc_vst
