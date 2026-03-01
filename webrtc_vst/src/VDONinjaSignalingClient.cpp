#include "VDONinjaSignalingClient.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketCloseConstants.h>

#include <iostream>
#include <exception>
#include <cstdlib>

namespace webrtc_vst {

namespace {
std::once_flag g_ixInitFlag;

void ensureIxInitialized() {
    std::call_once(g_ixInitFlag, [] { ix::initNetSystem(); });
}

std::unique_ptr<ix::WebSocket> makeWebSocket() {
    ensureIxInitialized();
    (void)ix::WebSocketCloseConstants::kInternalErrorMessage;
    return std::make_unique<ix::WebSocket>();
}

bool rawSignalingLoggingEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("WEBRTC_VST_LOG_SIGNALING_RAW");
        return env && *env != '\0';
    }();
    return enabled;
}
} // namespace

VDONinjaSignalingClient::VDONinjaSignalingClient(std::string url)
    : url_(std::move(url)) {
    ensureIxInitialized();
}

VDONinjaSignalingClient::~VDONinjaSignalingClient() {
    disconnect();
}

void VDONinjaSignalingClient::setCallbacks(Callbacks callbacks) {
    std::lock_guard<SpinLock> lock(mutex_);
    callbacks_ = std::move(callbacks);
}

void VDONinjaSignalingClient::configureCallbacks() {
    if (!socket_) {
        return;
    }

    socket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (!msg) {
            return;
        }

        switch (msg->type) {
            case ix::WebSocketMessageType::Message: {
                try {
                    if (rawSignalingLoggingEnabled()) {
                        std::cout << "[raw signaling] " << msg->str << std::endl;
                    }
                    auto jsonMsg = nlohmann::json::parse(msg->str);

                    std::function<void(const nlohmann::json&)> onMessage;
                    {
                        std::lock_guard<SpinLock> lock(mutex_);
                        onMessage = callbacks_.onMessage;
                    }
                    if (onMessage) {
                        onMessage(jsonMsg);
                    }
                } catch (const std::exception& ex) {
                    std::function<void(const std::string&)> onError;
                    {
                        std::lock_guard<SpinLock> lock(mutex_);
                        onError = callbacks_.onError;
                    }
                    if (onError) {
                        onError(std::string("Failed to parse signaling message: ") + ex.what());
                    }
                }
                break;
            }
            case ix::WebSocketMessageType::Open: {
                connected_.store(true, std::memory_order_relaxed);
                std::function<void()> onConnected;
                {
                    std::lock_guard<SpinLock> lock(mutex_);
                    onConnected = callbacks_.onConnected;
                }
                if (onConnected) {
                    onConnected();
                }
                break;
            }
            case ix::WebSocketMessageType::Close: {
                connected_.store(false, std::memory_order_relaxed);
                std::function<void()> onDisconnected;
                {
                    std::lock_guard<SpinLock> lock(mutex_);
                    onDisconnected = callbacks_.onDisconnected;
                }
                if (onDisconnected) {
                    onDisconnected();
                }
                break;
            }
            case ix::WebSocketMessageType::Error: {
                connected_.store(false, std::memory_order_relaxed);
                std::function<void(const std::string&)> onError;
                {
                    std::lock_guard<SpinLock> lock(mutex_);
                    onError = callbacks_.onError;
                }
                if (onError) {
                    onError(msg->errorInfo.reason);
                }
                break;
            }
            default:
                break;
        }
    });
}

void VDONinjaSignalingClient::connectAsync() {
    stopCalled_.store(false, std::memory_order_relaxed);

    ix::WebSocket* socket = nullptr;
    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (!socket_) {
            socket_ = makeWebSocket();
        }
        socket = socket_.get();
    }

    configureCallbacks();

    if (!socket) {
        connected_.store(false, std::memory_order_relaxed);
        return;
    }

    socket->setUrl(url_);

    ix::SocketTLSOptions tlsOptions;
    tlsOptions.tls = true;
    tlsOptions.caFile = "SYSTEM"; // Use system trust store
    tlsOptions.disable_hostname_validation = false;
    socket->setTLSOptions(tlsOptions);

    socket->setPingInterval(30);
    socket->disableAutomaticReconnection();
    socket->start();
}

void VDONinjaSignalingClient::disconnect() {
    std::unique_ptr<ix::WebSocket> socketToStop;
    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (!socket_) {
            connected_.store(false, std::memory_order_relaxed);
            return;
        }
        if (stopCalled_.exchange(true, std::memory_order_acq_rel)) {
            connected_.store(false, std::memory_order_relaxed);
            return;
        }
        socketToStop = std::move(socket_);
    }

    connected_.store(false, std::memory_order_relaxed);

    if (socketToStop) {
        try {
            // Clear callbacks first to prevent callbacks during shutdown
            socketToStop->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
            // Close the socket - use close() instead of stop() to avoid blocking
            socketToStop->close();
        } catch (...) {
            // Ignore exceptions during shutdown
        }
    }

    stopCalled_.store(false, std::memory_order_relaxed);
}


void VDONinjaSignalingClient::send(const nlohmann::json& message) {
    const auto payload = message.dump();
    if (rawSignalingLoggingEnabled()) {
        std::cout << "[raw signaling >>] " << payload << std::endl;
    }
    std::lock_guard<SpinLock> lock(mutex_);
    if (!socket_) {
        connected_.store(false, std::memory_order_relaxed);
        if (callbacks_.onError) {
            callbacks_.onError("Failed to send signaling message: socket unavailable");
        }
        return;
    }

    try {
        const auto sendInfo = socket_->send(payload);
        if (!sendInfo.success) {
            if (callbacks_.onError) {
                callbacks_.onError("Failed to send signaling message: write refused");
            }
        }
    } catch (const std::exception& ex) {
        if (callbacks_.onError) {
            callbacks_.onError(std::string("Failed to send signaling message: ") + ex.what());
        }
    }
}

bool VDONinjaSignalingClient::isConnected() const {
    return connected_.load(std::memory_order_relaxed);
}


} // namespace webrtc_vst
