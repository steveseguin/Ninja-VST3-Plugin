#pragma once

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#if defined(_WIN32)
#include <base/source/fstring.h>
#endif

namespace webrtc_vst {

// Text settings are UTF-8 internally. Windows getenv uses the active ANSI
// code page, so read its UTF-16 environment before applying UTF-8 validation.
inline std::optional<std::string> environmentSetting(const char* name) {
#if defined(_WIN32)
    const std::wstring wideName(name, name + std::strlen(name));
    const auto* value = _wgetenv(wideName.c_str());
    if (!value) return std::nullopt;
    Steinberg::String converted(reinterpret_cast<const Steinberg::char16*>(value));
    if (!converted.toMultiByte(Steinberg::kCP_Utf8)) return std::string{};
    return std::string(converted.text8());
#else
    if (const auto* value = std::getenv(name)) return std::string(value);
    return std::nullopt;
#endif
}

} // namespace webrtc_vst
