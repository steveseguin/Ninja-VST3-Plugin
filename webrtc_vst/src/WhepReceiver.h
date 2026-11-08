#pragma once

#include <rtc/rtc.hpp>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXUrlParser.h>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <thread>

namespace webrtc_vst {

// Mirrors lib.js:whepIn's SDP POST/Bearer/Location flow. Collect ICE before
// POST instead of PATCH trickle. Owned by a peer; destruction runs on cleanup.
class WhepReceiver {
public:
    static std::string origin(const std::string& url) {
        if (url.size() > 4096 || url.find_first_of("\r\n\t \\#") != std::string::npos) return {};
        std::string scheme, host, path, query;
        int port = 0;
        if (!ix::UrlParser::parse(url, scheme, host, path, query, port) || host.empty() || port <= 0 || port > 65535) return {};
        const auto authorityEnd = url.find_first_of("/?", url.find("://") + 3);
        if (url.substr(0, authorityEnd).find('@') != std::string::npos) return {};
        if (scheme != "https" && !(scheme == "http" && (host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]"))) return {};
        std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
        return scheme + "://" + host + ":" + std::to_string(port);
    }

    static std::string resourceUrl(const std::string& endpoint, const std::string& location) {
        if (location.empty()) return {};
        const auto schemeEnd = endpoint.find("://");
        const auto authorityEnd = endpoint.find_first_of("/?", schemeEnd + 3);
        const auto authority = endpoint.substr(0, authorityEnd);
        std::string result;
        if (location.starts_with("//")) result = endpoint.substr(0, schemeEnd + 1) + location;
        else if (location.find("://") != std::string::npos) result = location;
        else if (location.front() == '/') result = authority + location;
        else {
            auto base = endpoint.substr(0, endpoint.find('?'));
            if (base.find('/', schemeEnd + 3) == std::string::npos) base += '/';
            result = location.front() == '?' ? base + location : base.substr(0, base.rfind('/') + 1) + location;
        }
        // Never forward a bearer token to another origin from a Location header.
        return !origin(result).empty() && origin(result) == origin(endpoint) ? result : std::string{};
    }

    WhepReceiver(std::string url, std::string token) : url_(std::move(url)), token_(std::move(token)) {}
    WhepReceiver(const WhepReceiver&) = delete;
    WhepReceiver& operator=(const WhepReceiver&) = delete;
    ~WhepReceiver() {
        cancel();
        if (worker_.joinable()) worker_.join();
        if (!resource_.empty()) {
            try { std::string ignored; request(resource_, "DELETE", "", ignored); } catch (...) {}
        }
    }
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    bool matches(const std::string& url, const std::string& token) const { return url_ == url && token_ == token; }

    void start(std::shared_ptr<rtc::PeerConnection> pc, std::function<void(const std::string&)> error) {
        worker_ = std::thread([this, pc = std::move(pc), error = std::move(error)] {
            try {
                if (cancelled_) return;
                pc->setLocalDescription(rtc::Description::Type::Offer);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (!cancelled_ && pc->gatheringState() != rtc::PeerConnection::GatheringState::Complete) {
                    if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("WHEP ICE gathering timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (cancelled_) return;
                const auto offer = pc->localDescription();
                if (!offer) throw std::runtime_error("WHEP offer unavailable");
                std::string body;
                auto response = request(url_, "POST", std::string(*offer), body);
                if (response && (response->statusCode == 200 || response->statusCode == 201)) {
                    auto location = response->headers.find("Location");
                    if (location != response->headers.end()) {
                        resource_ = resourceUrl(url_, location->second);
                        if (resource_.empty()) throw std::runtime_error("WHEP returned an unsafe session location");
                    }
                }
                if (cancelled_) return; // destructor still deletes any allocated resource
                if (!response || response->errorCode != ix::HttpErrorCode::Ok) throw std::runtime_error("WHEP HTTP request failed");
                if (response->statusCode != 200 && response->statusCode != 201)
                    throw std::runtime_error("WHEP HTTP " + std::to_string(response->statusCode));
                const auto contentType = response->headers.find("Content-Type");
                if (contentType == response->headers.end() || !contentType->second.starts_with("application/sdp") || !body.starts_with("v=0"))
                    throw std::runtime_error("WHEP returned an invalid SDP response");
                // Do not report SDP/parser exception text; it can contain remote input.
                try { pc->setRemoteDescription(rtc::Description(body, rtc::Description::Type::Answer)); }
                catch (...) { throw std::runtime_error("WHEP answer negotiation failed"); }
            } catch (const std::exception& e) {
                if (!cancelled_) error(e.what());
            }
        });
    }

private:
    ix::HttpResponsePtr request(const std::string& url, const std::string& method,
                                const std::string& body, std::string& responseBody) {
        ix::HttpClient client;
        ix::SocketTLSOptions tls;
        tls.caFile = "SYSTEM";
        tls.disable_hostname_validation = false;
        client.setTLSOptions(tls);
        auto args = client.createRequest();
        args->connectTimeout = 2;
        args->transferTimeout = 3;
        args->followRedirects = false;
        args->compress = false;
        args->extraHeaders["Content-Type"] = "application/sdp";
        if (!token_.empty()) args->extraHeaders["Authorization"] = "Bearer " + token_;
        args->onChunkCallback = [&](const std::string& chunk) {
            if (responseBody.size() + chunk.size() > 256 * 1024) args->cancel = true;
            else responseBody += chunk;
        };
        return client.request(url, method, body, args);
    }

    std::string url_, token_, resource_; // resource_ written by worker, read after join
    std::atomic<bool> cancelled_{false};
    std::thread worker_;
};
} // namespace webrtc_vst
