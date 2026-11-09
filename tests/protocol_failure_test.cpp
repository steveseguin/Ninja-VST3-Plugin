#include "../webrtc_vst/src/WebRTCSession.h"
#include "../webrtc_vst/src/TurnServers.h"
#include "../webrtc_vst/src/WhepSettings.h"
#include <openssl/rand.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace webrtc_vst {
struct WebRTCSessionTestAccess {
    static void configure(WebRTCSession& session, bool encrypted,
                          const std::string& password = "owned-test-password", const std::string& salt = "vdo.ninja") {
        session.started_ = true;
        session.shuttingDown_ = false;
        session.config_.disableEncryption = !encrypted;
        session.config_.password = password;
        session.cachedPassword_.reset();
        session.salt_ = salt;
        session.signalingClient_ = std::make_unique<VDONinjaSignalingClient>("ws://127.0.0.1:1", false);
        session.setLogSignalingMessages(true);
    }
    static void send(WebRTCSession& session) {
        WebRTCSession::PeerSession peer;
        peer.uuid = "owned-peer";
        peer.sessionId = "owned-session";
        session.sendPeerDescription(peer, "offer", "v=0\r\n");
        session.sendIceCandidate(peer, {{"candidate", "candidate:1 1 udp 1 127.0.0.1 9 typ host"},
                                      {"sdpMid", "0"}, {"sdpMLineIndex", 0}}, "local");
    }
    static std::string decrypt(WebRTCSession& session, const nlohmann::json& vector) {
        return session.decryptPayload(vector.at("ciphertextHex").get<std::string>(), vector.at("ivHex").get<std::string>());
    }
    static std::string roundTrip(WebRTCSession& session, const std::string& text) {
        const auto [cipher, iv] = session.encryptPayload(text);
        return session.decryptPayload(cipher, iv);
    }
    static std::string refreshTurnCache(WebRTCSession& session) {
        session.config_.handshakeUrl = "wss://wss.vdo.ninja";
        session.config_.webBaseUrl = "https://vdo.ninja/";
        session.nextTurnRefreshAt_ = {};
        session.refreshTurnServers();
        return session.turnServers_.empty() ? "" : session.turnServers_.front().password;
    }
};
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// A separate process: never replaces the host application's RNG. Exercise the
// actual RAND_bytes failure branch, without adding a production fault option.
struct FailingRandom {
    const RAND_METHOD* previous = RAND_get_rand_method();
    RAND_METHOD failure{};
    FailingRandom() {
        failure.bytes = [](unsigned char*, int) { return 0; };
        failure.pseudorand = failure.bytes;
        require(RAND_set_rand_method(&failure) == 1, "Cannot install test RNG");
    }
    ~FailingRandom() { RAND_set_rand_method(previous); }
};

int main() {
    try {
        {
            std::ifstream input(VDON_BROWSER_WHEP_VECTORS);
            require(input.good(), "Cannot read browser WHEP vectors");
            const auto vectors = nlohmann::json::parse(input);
            for (const auto& entry : vectors.at("vectors")) {
                const auto& settings = entry.at("settings");
                require(webrtc_vst::whepSettingsMarker(settings, settings.at("url").get<std::string>(), settings.at("token").get<std::string>()) == entry.at("marker").get<std::string>(),
                        "WHEP started marker differs from browser");
            }
        }
        for (const auto& url : {"https://turnservers.vdo.ninja/", "https://turnservers.rtc.ninja/",
                               "https://turnservers.socialstream.ninja/"}) {
            const auto east = webrtc_vst::turnDiscoveryFallback(url, 300);
            const auto europe = webrtc_vst::turnDiscoveryFallback(url, -60);
            require(east.size() == 2 && east[0].hostname == "turn-use5.vdo.ninja" &&
                    east[1].hostname == "turn-use3.vdo.ninja", "US browser backup relay selection differs");
            require(europe.size() == 2 && europe[0].hostname == "turn-de3.vdo.ninja" &&
                    europe[1].hostname == "turn-pl2.vdo.ninja", "European browser backup relay selection differs");
            for (const auto& server : east) {
                require(server.relayType == rtc::IceServer::RelayType::TurnUdp &&
                        !server.username.empty() && !server.password.empty(), "Unusable backup relay");
            }
        }
        require(webrtc_vst::turnDiscoveryFallback("https://private.invalid/").empty(), "Custom discovery enabled public backup");
        require(webrtc_vst::turnDiscoveryFallback("http://turnservers.vdo.ninja/").empty(), "Insecure discovery enabled backup");
        std::cout << "PASS: official discovery uses browser backup relays; custom deployments remain private\n";
        std::vector<nlohmann::json> sent;
        webrtc_vst::AudioRingBuffer buffer(2048, 2);
        webrtc_vst::WebRTCSession session(buffer, [&](const std::string& line) {
            const std::string prefix = "=> signaling: ";
            if (line.starts_with(prefix)) sent.push_back(nlohmann::json::parse(line.substr(prefix.size())));
        });
        using Access = webrtc_vst::WebRTCSessionTestAccess;
        Access::configure(session, true);
        {
            const std::string url = "https://turnservers.vdo.ninja/";
            const auto now = std::chrono::steady_clock::now();
            int requests = 0;
            bool fail = false;
            const auto fetch = [&](const std::string&) {
                ++requests;
                if (fail) return std::vector<rtc::IceServer>{};
                auto servers = webrtc_vst::parseTurnServers(R"({"servers":[{"urls":"turn:127.0.0.1:3478","username":"owned","credential":"owned"}]})");
                servers.front().password = "rotation-" + std::to_string(requests);
                return servers;
            };
            webrtc_vst::loadTurnServers(url, now, fetch);
            webrtc_vst::loadTurnServers(url, now + std::chrono::minutes(59), fetch);
            require(requests == 1, "Unexpired TURN credentials fetched twice");
            require(Access::refreshTurnCache(session) == "rotation-1", "Instance did not adopt cached discovery");
            webrtc_vst::loadTurnServers(url, now + std::chrono::hours(1), fetch);
            require(requests == 2 && Access::refreshTurnCache(session) == "rotation-2",
                    "Healthy signaling prevented instance credential renewal");
            fail = true;
            auto backup = webrtc_vst::loadTurnServers(url, now + std::chrono::hours(2), fetch);
            require(requests == 3 && backup.size() == 2, "Discovery outage did not enable backup");
            fail = false;
            webrtc_vst::loadTurnServers(url, now + std::chrono::hours(2) + std::chrono::seconds(29), fetch);
            require(requests == 3, "Failed discovery retried too frequently");
            webrtc_vst::loadTurnServers(url, now + std::chrono::hours(2) + std::chrono::seconds(30), fetch);
            require(requests == 4 && Access::refreshTurnCache(session) == "rotation-4",
                    "Discovery did not recover after its failure cache expired");
            std::cout << "PASS: TURN expiry, outage retry and instance credential renewal with healthy signaling\n";
        }
        Access::send(session);
        require(sent.size() == 2 && sent[0]["description"].is_string() && sent[1]["candidates"].is_string(),
                "Encrypted control did not reach the signaling send path");
        sent.clear();
        {
            FailingRandom failure;
            Access::send(session);
        }
        require(sent.empty(), "Encryption failure sent plaintext SDP or ICE");
        Access::configure(session, false);
        Access::send(session);
        require(sent.size() == 2 && sent[0]["description"].is_object() && sent[1]["candidates"].is_array(),
                "Explicit encryption-disabled mode stopped working");
        std::ifstream input(VDON_BROWSER_CRYPTO_VECTORS);
        require(input.good(), "Cannot read browser AES reference vectors");
        const auto vectors = nlohmann::json::parse(input);
        for (const auto& vector : vectors.at("encryption")) {
            Access::configure(session, true, vector.at("rawPassword").get<std::string>(), vector.at("salt").get<std::string>());
            const auto plaintext = vector.at("plaintext").get<std::string>();
            require(Access::decrypt(session, vector) == plaintext, "Browser AES vector did not decrypt");
            require(Access::roundTrip(session, plaintext) == plaintext, "AES text round trip failed");
        }
        std::cout << "PASS: SDP/ICE encryption failures send nothing; encrypted and explicit plaintext modes work\n";
        std::cout << "PASS: browser ASCII/Unicode-salt/Latin-1 AES vectors and native encryption round trips\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
