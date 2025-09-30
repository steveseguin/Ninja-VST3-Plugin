#include "VDONinjaSignalingClient.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>

#include <iostream>
#include <exception>

namespace webrtc_vst {

namespace {
std::once_flag g_ixInitFlag;
}

VDONinjaSignalingClient::VDONinjaSignalingClient(std::string url)
    : url_(std::move(url)), socket_(std::make_unique<ix::WebSocket>()) {
    std::call_once(g_ixInitFlag, [] { ix::initNetSystem(); });
    configureCallbacks();
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
                    auto jsonMsg = nlohmann::json::parse(msg->str);
                    std::lock_guard<SpinLock> lock(mutex_);
                    if (callbacks_.onMessage) {
                        callbacks_.onMessage(jsonMsg);
                    }
                } catch (const std::exception& ex) {
                    std::lock_guard<SpinLock> lock(mutex_);
                    if (callbacks_.onError) {
                        callbacks_.onError(std::string("Failed to parse signaling message: ") + ex.what());
                    }
                }
                break;
            }
            case ix::WebSocketMessageType::Open: {
                connected_.store(true, std::memory_order_relaxed);
                std::lock_guard<SpinLock> lock(mutex_);
                if (callbacks_.onConnected) {
                    callbacks_.onConnected();
                }
                break;
            }
            case ix::WebSocketMessageType::Close: {
                connected_.store(false, std::memory_order_relaxed);
                std::lock_guard<SpinLock> lock(mutex_);
                if (callbacks_.onDisconnected) {
                    callbacks_.onDisconnected();
                }
                break;
            }
            case ix::WebSocketMessageType::Error: {
                connected_.store(false, std::memory_order_relaxed);
                std::lock_guard<SpinLock> lock(mutex_);
                if (callbacks_.onError) {
                    callbacks_.onError(msg->errorInfo.reason);
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
            socket_ = std::make_unique<ix::WebSocket>();
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

    if (socketToStop) {
        try {
            socketToStop->setOnMessageCallback(nullptr);
            socketToStop->stop(1000, "client shutdown");
        } catch (const std::exception& ex) {
            std::lock_guard<SpinLock> lock(mutex_);
            if (callbacks_.onError) {
                callbacks_.onError(std::string("Failed to close signaling socket: ") + ex.what());
            }
        }
    }

    connected_.store(false, std::memory_order_relaxed);
    stopCalled_.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (!socket_) {
            socket_ = std::make_unique<ix::WebSocket>();
        }
        configureCallbacks();
    }
}


void VDONinjaSignalingClient::send(const nlohmann::json& message) {
    const auto payload = message.dump();
    std::lock_guard<SpinLock> lock(mutex_);
    if (!socket_) {
        connected_.store(false, std::memory_order_relaxed);
        if (callbacks_.onError) {
            callbacks_.onError("Failed to send signaling message: socket unavailable");
        }
        return;
    }

    try {
        socket_->send(payload);
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

