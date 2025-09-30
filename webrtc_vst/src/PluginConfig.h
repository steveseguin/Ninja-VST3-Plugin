#pragma once

#include <string>

namespace webrtc_vst {

enum class ConnectionMode {
    Seed,  // Publish host audio to VDO.Ninja
    Play   // Receive remote audio into the host
};

struct PluginConfig {
    ConnectionMode mode{ConnectionMode::Play};
    std::string streamId;
    std::string roomId;
    std::string signalingUrl{"wss://wss0.vdo.ninja"};
    bool enableAutoReconnect{true};
    bool enableAec{false};
    bool disableEncryption{false};
    std::string password; // optional password used for hashing/encryption
};

} // namespace webrtc_vst
