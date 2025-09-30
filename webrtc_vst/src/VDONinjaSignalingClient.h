#pragma once

#include <atomic>
#include "SpinLock.h"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace ix {
class WebSocket;
}

namespace webrtc_vst {

class VDONinjaSignalingClient {
public:
    struct Callbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(const nlohmann::json&)> onMessage;
        std::function<void(const std::string&)> onError;
    };

    explicit VDONinjaSignalingClient(std::string url);
    ~VDONinjaSignalingClient();

    void setCallbacks(Callbacks callbacks);

    void connectAsync();
    void disconnect();

    bool isConnected() const;
    void send(const nlohmann::json& message);

private:
    void configureCallbacks();

    std::string url_;
    std::unique_ptr<ix::WebSocket> socket_;
    mutable SpinLock mutex_;
    Callbacks callbacks_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopCalled_{false};
};

} // namespace webrtc_vst

