#pragma once

#include "AdvancedSettings.h"
#include "BoundedJson.h"
#include <rtc/configuration.hpp>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace webrtc_vst {

// Same service and response shape as code/vdo.ninja/webrtc.js:getTURNList.
// Custom signaling deployments opt in with their own server-list URL.
inline std::string turnServerListUrl(const std::string& handshake, const std::string& web) {
    if (const char* overrideUrl = std::getenv("WEBRTC_VST_TURN_SERVER_LIST_URL"))
        return overrideUrl;
    const auto signalingHost = endpointHost(handshake);
    if (signalingHost != "vdo.ninja" && !signalingHost.ends_with(".vdo.ninja") &&
        signalingHost != "rtc.ninja" && !signalingHost.ends_with(".rtc.ninja")) return {};
    const auto host = endpointHost(web);
    if (host == "rtc.ninja") return "https://turnservers.rtc.ninja/";
    if (host == "vdo.socialstream.ninja") return "https://turnservers.socialstream.ninja/";
    return "https://turnservers.vdo.ninja/";
}

inline std::vector<rtc::IceServer> parseTurnServers(const std::string& body) {
    const auto json = parseBoundedJson(body, 64 * 1024);
    std::vector<rtc::IceServer> result;
    if (!json.is_object() || !json.contains("servers") || !json["servers"].is_array()) return result;
    for (const auto& entry : json["servers"]) {
        if (!entry.is_object() || !entry.contains("urls") ||
            !entry.contains("username") || !entry["username"].is_string() ||
            !entry.contains("credential") || !entry["credential"].is_string()) continue;
        auto urls = entry["urls"];
        if (urls.is_string()) urls = nlohmann::json::array({urls});
        if (!urls.is_array()) continue;
        for (const auto& url : urls) {
            if (!url.is_string()) continue;
            try {
                rtc::IceServer server(url.get<std::string>());
                // The pinned libjuice backend supports TURN/UDP only. Do not
                // advertise TCP/TLS fallback that this build cannot provide.
                if (server.type != rtc::IceServer::Type::Turn ||
                    server.relayType != rtc::IceServer::RelayType::TurnUdp || server.hostname.empty()) continue;
                server.username = entry["username"].get<std::string>();
                server.password = entry["credential"].get<std::string>();
                if (server.username.empty() || server.password.empty()) continue;
                result.push_back(std::move(server));
                if (result.size() == 2) return result; // Bound native relay allocations (browser fallback also selects two UDP relays).
            } catch (const std::exception&) { /* Skip an unsupported/malformed URL. */ }
        }
    }
    return result;
}

// Call only from the signaling worker, outside the session/audio locks.
// Cache successful discovery for one hour, matching the web client; briefly
// cache failures so unavailable discovery does not hammer the service.
inline std::vector<rtc::IceServer> loadTurnServers(const std::string& url) {
    const auto host = endpointHost(url);
    if (!url.starts_with("https://") && !(url.starts_with("http://") &&
        (host == "127.0.0.1" || host == "[::1]" || host == "localhost"))) return {};
    static std::mutex cacheMutex;
    static std::string cachedUrl;
    static std::vector<rtc::IceServer> cachedServers;
    static std::chrono::steady_clock::time_point expires;
    {
        std::lock_guard lock(cacheMutex);
        if (cachedUrl == url && std::chrono::steady_clock::now() < expires) return cachedServers;
    }

    std::vector<rtc::IceServer> servers;
    try {
        ix::HttpClient client;
        ix::SocketTLSOptions tls;
        tls.caFile = "SYSTEM";
        tls.disable_hostname_validation = false;
        client.setTLSOptions(tls);
        auto args = client.createRequest();
        args->connectTimeout = 2;
        args->transferTimeout = 2;
        args->followRedirects = false;
        args->compress = false;
        std::string body;
        args->onChunkCallback = [&](const std::string& chunk) {
            if (body.size() + chunk.size() > 64 * 1024) args->cancel = true;
            else body += chunk;
        };
        // Timestamp convention used by getTURNList; contains no stream/password.
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - 1653305816700LL;
        const auto response = client.get(url + (url.find('?') == std::string::npos ? "?ts=" : "&ts=") +
                                         std::to_string(timestamp), args);
        if (!args->cancel && response && response->statusCode == 200 && response->errorCode == ix::HttpErrorCode::Ok)
            servers = parseTurnServers(body);
    } catch (const std::exception&) { /* Keep direct/STUN connectivity available. */ }
    {
        std::lock_guard lock(cacheMutex);
        cachedUrl = url;
        cachedServers = servers;
        expires = std::chrono::steady_clock::now() + (servers.empty() ? std::chrono::seconds(30) : std::chrono::seconds(3600));
    }
    return servers;
}

} // namespace webrtc_vst
