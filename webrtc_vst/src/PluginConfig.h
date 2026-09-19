#pragma once

#include <string>
#include "AdvancedSettings.h"

namespace webrtc_vst {

enum class ConnectionMode {
    Publish,  // Publish host audio to VDO.Ninja
    Play      // Receive remote audio into the host
};

struct PluginConfig {
    ConnectionMode mode{ConnectionMode::Play};
    std::string streamId;
    std::string roomName;
    std::string handshakeUrl{kDefaultHandshakeUrl};
    std::string webBaseUrl{kDefaultWebBaseUrl};
    std::string salt; // empty = derive from the web domain, not the signaling server
    bool enableAutoReconnect{true};
    bool enableAec{false};
    bool disableEncryption{false};
    std::string password; // optional password used for hashing/encryption
};

} // namespace webrtc_vst
