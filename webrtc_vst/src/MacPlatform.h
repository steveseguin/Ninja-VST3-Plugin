#pragma once

#include <string>

namespace webrtc_vst {
std::string normalizeMacEditedText(const std::string& text);
bool openMacExternalUrl(const std::string& url);
}
