#include "WebRTCSession.h"
#include "StreamIdGenerator.h"

#include <rtc/rtc.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>
#include <rtc/rtpdepacketizer.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace webrtc_vst {

namespace {
constexpr size_t kFrameSizeSamples = 960; // 20ms at 48kHz
constexpr int kOpusPayloadType = 111;
constexpr uint32_t kAudioSsrc = 0x11ECACA; // Arbitrary but stable

std::string generateSessionId() {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
    std::string session;
    session.reserve(8);
    for (int i = 0; i < 8; ++i) {
        session.push_back(alphabet[dist(rng)]);
    }
    return session;
}

rtc::binary toBinary(const unsigned char* data, size_t size) {
    rtc::binary buffer;
    buffer.resize(size);
    std::memcpy(buffer.data(), data, size);
    return buffer;
}

std::string toHex(const uint8_t* data, size_t size) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::string toHex(const std::vector<uint8_t>& data) {
    return toHex(data.data(), data.size());
}

std::vector<uint8_t> fromHex(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Invalid hex string length");
    }
    std::vector<uint8_t> bytes(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const auto part = hex.substr(i, 2);
        bytes[i / 2] = static_cast<uint8_t>(std::stoi(part, nullptr, 16));
    }
    return bytes;
}

std::string trimCopy(const std::string& value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }

    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }

    return std::string(begin, end);
}

bool isIpAddress(const std::string& host) {
    static const std::regex ipv4(R"(^((25[0-5]|2[0-4]\d|[0-1]?\d?\d)\.){3}(25[0-5]|2[0-4]\d|[0-1]?\d?\d)$)");
    static const std::regex ipv6(R"(^([0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}$)");
    return std::regex_match(host, ipv4) || std::regex_match(host, ipv6);
}

std::string extractHost(const std::string& url) {
    const auto schemePos = url.find("://");
    size_t hostStart = 0;
    if (schemePos != std::string::npos) {
        hostStart = schemePos + 3;
    }
    const auto pathPos = url.find('/', hostStart);
    std::string host = url.substr(hostStart, pathPos == std::string::npos ? std::string::npos : pathPos - hostStart);
    const auto colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        host = host.substr(0, colonPos);
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

struct SanitizedStreamId {
    std::string value;
    bool changed{false};
    bool generated{false};
};

SanitizedStreamId sanitizeStreamId(const std::string& raw) {
    const std::string trimmed = trimCopy(raw);
    SanitizedStreamId result;
    if (trimmed.empty()) {
        result.value = generateRandomStreamId();
        result.changed = true;
        result.generated = true;
        return result;
    }

    result.value.reserve(trimmed.size());
    for (char ch : trimmed) {
        if (ch == '-') {
            result.value.push_back('_');
            result.changed = true;
        } else {
            result.value.push_back(ch);
        }
    }

    if (result.value.empty()) {
        result.value = generateRandomStreamId();
        result.changed = true;
        result.generated = true;
    }

    return result;
}

} // namespace

void WebRTCSession::LinearResampler::configure(double inputRate, double outputRate, int channels) {
    inputRate_ = inputRate;
    outputRate_ = outputRate;
    channels_ = channels;
    step_ = inputRate_ / outputRate_;
    phase_ = 0.0;
    configured_ = true;
    passthrough_ = std::abs(inputRate_ - outputRate_) < 1e-6;
    havePrev_ = false;
    prevSamples_.assign(static_cast<size_t>(std::max(0, channels_)), 0.0f);
}

void WebRTCSession::LinearResampler::reset() {
    phase_ = 0.0;
    havePrev_ = false;
    prevSamples_.assign(static_cast<size_t>(std::max(0, channels_)), 0.0f);
}

size_t WebRTCSession::LinearResampler::processPlanar(const float* const* inputs,
                                                     size_t frames,
                                                     int channels,
                                                     std::vector<float>& outputInterleaved) {
    if (!configured_ || channels != channels_) {
        configure(inputRate_, outputRate_, channels);
    }

    if (passthrough_) {
        outputInterleaved.resize(frames * static_cast<size_t>(channels));
        for (size_t frame = 0; frame < frames; ++frame) {
            for (int ch = 0; ch < channels; ++ch) {
                outputInterleaved[frame * channels + ch] = inputs[ch][frame];
            }
        }
        if (frames > 0) {
            for (int ch = 0; ch < channels; ++ch) {
                prevSamples_[ch] = inputs[ch][frames - 1];
            }
            havePrev_ = true;
        }
        phase_ = 0.0;
        return frames;
    }

    if (frames == 0) {
        return 0;
    }

    size_t start = 0;
    if (!havePrev_) {
        for (int ch = 0; ch < channels; ++ch) {
            prevSamples_[ch] = inputs[ch][0];
        }
        havePrev_ = true;
        start = 1;
        if (frames == 1) {
            return 0;
        }
    }

    const size_t effectiveFrames = frames - start;
    std::vector<float> buffer((effectiveFrames + 1) * static_cast<size_t>(channels));
    std::memcpy(buffer.data(), prevSamples_.data(), sizeof(float) * static_cast<size_t>(channels));
    for (size_t frame = 0; frame < effectiveFrames; ++frame) {
        for (int ch = 0; ch < channels; ++ch) {
            buffer[(frame + 1) * channels + ch] = inputs[ch][frame + start];
        }
    }

    return processBuffer(buffer.data(), effectiveFrames + 1, channels, outputInterleaved);
}

size_t WebRTCSession::LinearResampler::processInterleaved(const float* data,
                                                          size_t frames,
                                                          int channels,
                                                          std::vector<float>& outputInterleaved) {
    if (!configured_ || channels != channels_) {
        configure(inputRate_, outputRate_, channels);
    }

    if (passthrough_) {
        outputInterleaved.assign(data, data + frames * static_cast<size_t>(channels));
        if (frames > 0) {
            std::memcpy(prevSamples_.data(), data + (frames - 1) * channels, sizeof(float) * static_cast<size_t>(channels));
            havePrev_ = true;
        }
        phase_ = 0.0;
        return frames;
    }

    if (frames == 0) {
        return 0;
    }

    std::vector<float> buffer((frames + 1) * static_cast<size_t>(channels));
    if (!havePrev_) {
        std::memcpy(prevSamples_.data(), data, sizeof(float) * static_cast<size_t>(channels));
        havePrev_ = true;
    }

    std::memcpy(buffer.data(), prevSamples_.data(), sizeof(float) * static_cast<size_t>(channels));
    std::memcpy(buffer.data() + channels, data, sizeof(float) * frames * static_cast<size_t>(channels));

    return processBuffer(buffer.data(), frames + 1, channels, outputInterleaved);
}

size_t WebRTCSession::LinearResampler::processInterleaved(const std::vector<float>& data,
                                                          int channels,
                                                          std::vector<float>& outputInterleaved) {
    const size_t frames = data.size() / static_cast<size_t>(channels);
    return processInterleaved(data.data(), frames, channels, outputInterleaved);
}

size_t WebRTCSession::LinearResampler::processBuffer(const float* source,
                                                     size_t frames,
                                                     int channels,
                                                     std::vector<float>& outputInterleaved) {
    outputInterleaved.clear();
    if (frames < 2) {
        return 0;
    }

    double pos = phase_;
    const double frameCount = static_cast<double>(frames);
    while (pos + 1.0 <= frameCount - 1.0) {
        const size_t idx = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(idx);
        const float* frame0 = source + idx * channels;
        const float* frame1 = source + (idx + 1) * channels;
        for (int ch = 0; ch < channels; ++ch) {
            const float sample0 = frame0[ch];
            const float sample1 = frame1[ch];
            const float sample = static_cast<float>((1.0 - frac) * sample0 + frac * sample1);
            outputInterleaved.push_back(sample);
        }
        pos += step_;
    }

    phase_ = pos - static_cast<double>(frames - 1);
    if (phase_ < 0.0) {
        phase_ = 0.0;
    }

    std::memcpy(prevSamples_.data(), source + (frames - 1) * channels, sizeof(float) * static_cast<size_t>(channels));
    return outputInterleaved.empty() ? 0 : outputInterleaved.size() / static_cast<size_t>(channels);
}

WebRTCSession::WebRTCSession(AudioRingBuffer& receiveBuffer,
                                     LogSink logSink,
                                     ConfigSink configSink,
                                     StatusSink statusSink)
    : receiveBuffer_(receiveBuffer),
      logSink_(std::move(logSink)),
      configUpdateSink_(std::move(configSink)),
      statusSink_(std::move(statusSink)) {}

WebRTCSession::~WebRTCSession() {
    log("WebRTCSession::~WebRTCSession() destructor - enter");
    stop();
    log("WebRTCSession::~WebRTCSession() destructor - complete");
}

void WebRTCSession::setConfigUpdateSink(ConfigSink sink) {
    configUpdateSink_ = std::move(sink);
}

void WebRTCSession::setStatusSink(StatusSink sink) {
    std::lock_guard<SpinLock> lock(statusSinkMutex_);
    statusSink_ = std::move(sink);
}

void WebRTCSession::setLogSignalingMessages(bool enable) {
    const char* message = enable ? "Signaling message logging enabled"
                                 : "Signaling message logging disabled";
    {
        std::lock_guard<std::mutex> lock(signalingLogMutex_);
        logSignalingMessages_ = enable;
        lastSentSignalingJson_.reset();
        lastReceivedSignalingJson_.reset();
        suppressingSentDuplicate_ = false;
        suppressingReceivedDuplicate_ = false;
    }
    log(message);
}

void WebRTCSession::emitStatus(const std::string& status) const {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    StatusSink sink;
    {
        std::lock_guard<SpinLock> lock(statusSinkMutex_);
        sink = statusSink_;
    }
    if (sink && !shuttingDown_.load(std::memory_order_acquire)) {
        sink(status);
    }
}


void WebRTCSession::log(const std::string& line) const {
    if (logSink_) {
        logSink_(line);
    }
}

void WebRTCSession::sendSignalingMessage(const nlohmann::json& payload) {
    if (!signalingClient_) {
        return;
    }

    std::optional<std::string> logLine;
    {
        std::lock_guard<std::mutex> lock(signalingLogMutex_);
        if (logSignalingMessages_) {
            try {
                const std::string dump = payload.dump();
                if (lastSentSignalingJson_.equals(dump)) {
                    if (!suppressingSentDuplicate_) {
                        logLine = std::string("=> signaling: ") + dump +
                                  " (duplicate; suppressing further identical messages)";
                        suppressingSentDuplicate_ = true;
                    }
                } else {
                    logLine = std::string("=> signaling: ") + dump;
                    lastSentSignalingJson_.assign(dump);
                    suppressingSentDuplicate_ = false;
                }
            } catch (...) {
                logLine = "=> signaling: <unserializable payload>";
                lastSentSignalingJson_.reset();
                suppressingSentDuplicate_ = false;
            }
        }
    }

    if (logLine) {
        log(*logLine);
    }

    signalingClient_->send(payload);
}

std::optional<std::string> WebRTCSession::effectivePassword() const {
    if (config_.disableEncryption) {
        cachedPassword_.reset();
        return std::nullopt;
    }

    if (cachedPassword_.has_value()) {
        if (cachedPassword_->empty()) {
            return std::nullopt;
        }
        return cachedPassword_;
    }

    std::string password = trimCopy(config_.password);
    if (password.empty()) {
        password = "someEncryptionKey123";
    }

    cachedPassword_ = password;
    return cachedPassword_;
}

std::string WebRTCSession::deriveSalt(const std::string& url) const {
    const auto host = extractHost(url);
    if (host.empty()) {
        return "vdo.ninja";
    }

    if (host == "vdo.ninja" || host == "steveseguin.github.io") {
        return "vdo.ninja";
    }

    const auto lastDot = host.rfind('.');
    const auto secondLastDot = lastDot == std::string::npos ? std::string::npos : host.rfind('.', lastDot - 1);
    std::string tld = host;
    if (secondLastDot != std::string::npos) {
        tld = host.substr(secondLastDot + 1);
    }

    if (tld == "vdo.ninja" || tld == "rtc.ninja" || tld == "versus.cam" || tld == "socialstream.ninja") {
        return tld;
    }

    if (host == "localhost" || isIpAddress(host)) {
        return "vdo.ninja";
    }

    return host;
}

std::string WebRTCSession::hashRoom(const std::string& room, const std::string& password) const {
    std::string payload = room + password + salt_;
    std::array<uint8_t, SHA256_DIGEST_LENGTH> hash{};
    SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), hash.data());
    return toHex(hash.data(), 8); // 16 hex characters
}

std::string WebRTCSession::hashStreamIdSuffix(const std::string& password) const {
    std::string payload = password + salt_;
    std::array<uint8_t, SHA256_DIGEST_LENGTH> hash{};
    SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), hash.data());
    return toHex(hash.data(), 3); // 6 hex characters
}

std::pair<std::string, std::string> WebRTCSession::encryptPayload(const std::string& payload) const {
    const auto password = effectivePassword();
    if (!password) {
        throw std::runtime_error("Encryption requested without password");
    }

    std::array<uint8_t, 16> iv{};
    if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {
        throw std::runtime_error("Failed to generate IV");
    }

    std::array<uint8_t, SHA256_DIGEST_LENGTH> key{};
    const std::string phrase = *password + salt_;
    SHA256(reinterpret_cast<const unsigned char*>(phrase.data()), phrase.size(), key.data());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to allocate cipher context");
    }

    std::vector<uint8_t> cipher(payload.size() + EVP_MAX_BLOCK_LENGTH);
    int outLen1 = 0;
    int outLen2 = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EncryptInit failed");
    }
    if (EVP_EncryptUpdate(ctx,
                          cipher.data(),
                          &outLen1,
                          reinterpret_cast<const unsigned char*>(payload.data()),
                          static_cast<int>(payload.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EncryptUpdate failed");
    }
    if (EVP_EncryptFinal_ex(ctx, cipher.data() + outLen1, &outLen2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EncryptFinal failed");
    }
    EVP_CIPHER_CTX_free(ctx);

    cipher.resize(static_cast<size_t>(outLen1 + outLen2));
    return {toHex(cipher), toHex(iv.data(), iv.size())};
}

std::string WebRTCSession::decryptPayload(const std::string& payload, const std::string& vector) const {
    const auto password = effectivePassword();
    if (!password) {
        throw std::runtime_error("Decryption requested without password");
    }

    std::vector<uint8_t> cipherBytes = fromHex(payload);
    std::vector<uint8_t> iv = fromHex(vector);
    if (iv.size() != 16) {
        throw std::runtime_error("Invalid IV length");
    }

    std::array<uint8_t, SHA256_DIGEST_LENGTH> key{};
    const std::string phrase = *password + salt_;
    SHA256(reinterpret_cast<const unsigned char*>(phrase.data()), phrase.size(), key.data());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to allocate cipher context");
    }

    std::vector<uint8_t> plain(cipherBytes.size() + EVP_MAX_BLOCK_LENGTH);
    int outLen1 = 0;
    int outLen2 = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("DecryptInit failed");
    }
    if (EVP_DecryptUpdate(ctx,
                          plain.data(),
                          &outLen1,
                          cipherBytes.data(),
                          static_cast<int>(cipherBytes.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("DecryptUpdate failed");
    }
    if (EVP_DecryptFinal_ex(ctx, plain.data() + outLen1, &outLen2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("DecryptFinal failed");
    }
    EVP_CIPHER_CTX_free(ctx);

    plain.resize(static_cast<size_t>(outLen1 + outLen2));
    return std::string(reinterpret_cast<const char*>(plain.data()), plain.size());
}

std::string WebRTCSession::buildHashedStreamId() const {
    const auto password = effectivePassword();
    if (!password) {
        return config_.streamId;
    }
    const auto suffix = hashStreamIdSuffix(*password);
    return config_.streamId + suffix;
}

void WebRTCSession::maybeDecryptDescription(nlohmann::json& message) const {
    if (!message.contains("description")) {
        return;
    }
    if (!message.contains("vector")) {
        return;
    }
    if (!message["description"].is_string()) {
        return;
    }

    try {
        const auto decrypted = decryptPayload(message["description"].get<std::string>(),
                                              message["vector"].get<std::string>());
        message["description"] = nlohmann::json::parse(decrypted);
    } catch (const std::exception& ex) {
        log(std::string("Failed to decrypt description: ") + ex.what());
    }
}

void WebRTCSession::maybeDecryptCandidates(nlohmann::json& message) const {
    if (!message.contains("vector")) {
        return;
    }

    const auto password = effectivePassword();
    if (!password) {
        return;
    }

    auto decryptField = [&](const char* field) {
        if (!message.contains(field)) {
            return;
        }
        if (!message[field].is_string()) {
            return;
        }
        try {
            const auto decrypted = decryptPayload(message[field].get<std::string>(),
                                                  message["vector"].get<std::string>());
            message[field] = nlohmann::json::parse(decrypted);
        } catch (const std::exception& ex) {
            log(std::string("Failed to decrypt ") + field + ": " + ex.what());
        }
    };

    decryptField("candidate");
    decryptField("candidates");
}

WebRTCSession::PeerKey WebRTCSession::makePeerKey(const std::string& uuid, const std::string& session) const {
    if (session.empty()) {
        return uuid;
    }
    return uuid + ":" + session;
}

void WebRTCSession::resetAllPeerConnections() {
    log("resetAllPeerConnections() - closing " + std::to_string(peerSessions_.size()) + " peer connections");

    for (auto& [key, session] : peerSessions_) {
        if (session.connection) {
            try {
                // Clear all callbacks before closing to prevent use-after-free
                session.connection->onStateChange(nullptr);
                session.connection->onGatheringStateChange(nullptr);
                session.connection->onLocalDescription(nullptr);
                session.connection->onLocalCandidate(nullptr);
                session.connection->onTrack(nullptr);
                session.connection->onDataChannel(nullptr);
                session.connection->close();
            } catch (...) {
                // Ignore exceptions during cleanup
            }
        }
        // Clear track references
        session.localAudioTrack.reset();
        session.remoteAudioTrack.reset();
        session.dataChannel.reset();
        session.rtpConfig.reset();
    }
    peerSessions_.clear();
    sessionByUuid_.clear();
    pendingGlobalIce_.clear();

    log("resetAllPeerConnections() - complete");
}

void WebRTCSession::closePeerSession(const PeerKey& key) {
    auto it = peerSessions_.find(key);
    if (it == peerSessions_.end()) {
        return;
    }

    if (it->second.connection) {
        it->second.connection->close();
    }

    sessionByUuid_.erase(it->second.uuid);
    peerSessions_.erase(it);
}

WebRTCSession::PeerSession& WebRTCSession::ensurePeerSession(const std::string& uuid,
                                                             const std::string& sessionHint,
                                                             bool createLocalTracks) {
    const std::string resolvedSession = sessionHint.empty() ? generateSessionId() : sessionHint;
    const PeerKey key = makePeerKey(uuid, resolvedSession);

    auto mapIt = peerSessions_.find(key);
    if (mapIt != peerSessions_.end()) {
        return mapIt->second;
    }

    auto existing = sessionByUuid_.find(uuid);
    if (existing != sessionByUuid_.end() && existing->second != key) {
        closePeerSession(existing->second);
    }

    PeerSession& session = peerSessions_[key];
    session.uuid = uuid;
    session.sessionId = resolvedSession;
    session.streamId = hashedStreamId_.empty() ? config_.streamId : hashedStreamId_;
    session.nextTimestamp = 0;

    rtc::Configuration configuration;
    configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
    configuration.iceServers.emplace_back("stun:stun1.l.google.com:19302");

    session.connection = std::make_shared<rtc::PeerConnection>(configuration);
    auto keyCopy = key;

    session.connection->onStateChange([this, keyCopy](rtc::PeerConnection::State state) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;  // Prevent callback execution during/after shutdown
        }
        std::optional<std::string> statusMessage;
        bool resetMediaFlags = false;
        {
            std::lock_guard<SpinLock> lock(mutex_);
            auto it = peerSessions_.find(keyCopy);
            if (it == peerSessions_.end()) {
                return;
            }

            switch (state) {
                case rtc::PeerConnection::State::Connecting:
                    statusMessage = "Peer connecting...";
                    break;
                case rtc::PeerConnection::State::Connected:
                    it->second.negotiationReady = true;
                    log("Peer connection connected: " + keyCopy);
                    statusMessage = (config_.mode == ConnectionMode::Seed)
                                        ? std::string("Peer connected (publishing)")
                                        : std::string("Peer connected");
                    break;
                case rtc::PeerConnection::State::Disconnected:
                    log("Peer connection disconnected: " + keyCopy);
                    it->second.negotiationReady = false;
                    resetMediaFlags = true;
                    statusMessage = "Peer disconnected";
                    break;
                case rtc::PeerConnection::State::Failed:
                    log("Peer connection failed: " + keyCopy);
                    it->second.negotiationReady = false;
                    closePeerSession(keyCopy);
                    resetMediaFlags = true;
                    statusMessage = "Peer connection failed";
                    break;
                case rtc::PeerConnection::State::Closed:
                    log("Peer connection closed: " + keyCopy);
                    it->second.negotiationReady = false;
                    closePeerSession(keyCopy);
                    resetMediaFlags = true;
                    statusMessage = "Peer connection closed";
                    break;
                default:
                    break;
            }
        }

        if (resetMediaFlags) {
            publishingAudio_.store(false, std::memory_order_relaxed);
            receivingAudio_.store(false, std::memory_order_relaxed);
        }

        if (statusMessage) {
            emitStatus(*statusMessage);
        }
    });

    session.connection->onGatheringStateChange([this, keyCopy](rtc::PeerConnection::GatheringState state) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            log("ICE gathering complete for " + keyCopy);
        }
    });

    session.connection->onLocalDescription([this, keyCopy](rtc::Description description) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<SpinLock> lock(mutex_);
        auto it = peerSessions_.find(keyCopy);
        if (it == peerSessions_.end()) {
            return;
        }
        it->second.localDescriptionSet = true;
        sendPeerDescription(it->second, description.typeString(), std::string(description));
    });

    session.connection->onLocalCandidate([this, keyCopy](rtc::Candidate candidate) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<SpinLock> lock(mutex_);
        auto it = peerSessions_.find(keyCopy);
        if (it == peerSessions_.end()) {
            return;
        }

        nlohmann::json candidateJson;
        candidateJson["candidate"] = candidate.candidate();
        if (!candidate.mid().empty()) {
            candidateJson["sdpMid"] = candidate.mid();
        }
        candidateJson["sdpMLineIndex"] = 0;

        const bool isPublisher = config_.mode == ConnectionMode::Seed;
        sendIceCandidate(it->second, candidateJson, isPublisher ? "local" : "remote");
    });

    session.connection->onTrack([this, keyCopy](std::shared_ptr<rtc::Track> track) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<SpinLock> lock(mutex_);
        auto it = peerSessions_.find(keyCopy);
        if (it == peerSessions_.end()) {
            return;
        }

        if (track->description().type() != "audio") {
            return;
        }

        auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
        track->setMediaHandler(depacketizer);
        track->onFrame([this](rtc::binary data, rtc::FrameInfo) {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }

            // Double-check with mutex to prevent TOCTOU race
            ::OpusDecoder* decoder = nullptr;
            int channels = 0;
            {
                std::lock_guard<SpinLock> lock(mutex_);
                if (!started_ || !opusDecoder_) {
                    return;
                }
                decoder = opusDecoder_;
                channels = channelCount_;
            }

            std::vector<float> decodeBuffer(kFrameSizeSamples * static_cast<size_t>(channels));
            int frameSamples = opus_decode_float(decoder,
                                                 reinterpret_cast<const unsigned char*>(data.data()),
                                                 static_cast<opus_int32>(data.size()),
                                                 decodeBuffer.data(),
                                                 static_cast<int>(kFrameSizeSamples),
                                                 0);
            if (frameSamples <= 0) {
                return;
            }
            if (!receivingAudio_.exchange(true, std::memory_order_acq_rel)) {
                emitStatus("Receiving audio");
            }

            std::vector<float> resampled;
            const size_t outFrames = incomingResampler_.processInterleaved(decodeBuffer.data(),
                                                                           static_cast<size_t>(frameSamples),
                                                                           channels,
                                                                           resampled);
            if (outFrames == 0 || resampled.empty()) {
                return;
            }

            std::vector<std::vector<float>> planar(channels, std::vector<float>(outFrames));
            for (size_t frame = 0; frame < outFrames; ++frame) {
                for (int ch = 0; ch < channels; ++ch) {
                    planar[ch][frame] = resampled[frame * static_cast<size_t>(channels) + ch];
                }
            }

            std::vector<const float*> channelPtrs(channels);
            for (int ch = 0; ch < channels; ++ch) {
                channelPtrs[ch] = planar[ch].data();
            }

            receiveBuffer_.push(channelPtrs.data(), static_cast<size_t>(outFrames), channels);
        });
        it->second.remoteAudioTrack = track;
    });

    // For play mode, handle incoming datachannel and send viewer preferences
    if (!createLocalTracks && config_.mode == ConnectionMode::Play) {
        session.connection->onDataChannel([this, keyCopy](std::shared_ptr<rtc::DataChannel> dc) {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            log("Datachannel received from publisher");

            // Store datachannel in peer session for sending responses
            {
                std::lock_guard<SpinLock> lock(mutex_);
                auto it = peerSessions_.find(keyCopy);
                if (it != peerSessions_.end()) {
                    it->second.dataChannel = dc;
                    log("Stored datachannel in peer session " + keyCopy);
                }
            }

            dc->onOpen([this, keyCopy, dc]() {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                log("Datachannel opened, sending viewer preferences");
                try {
                    nlohmann::json prefs = {
                        {"audio", true},
                        {"video", false}
                    };
                    std::string msg = prefs.dump();
                    dc->send(msg);
                    log("Sent viewer preferences: " + msg);
                } catch (const std::exception& ex) {
                    log(std::string("Failed to send viewer preferences: ") + ex.what());
                }
            });

            dc->onMessage([this](auto data) {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                // Handle incoming datachannel messages
                std::visit([this](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, std::string>) {
                        log("Datachannel message: " + arg);

                        // Check if this is a new SDP offer from publisher
                        try {
                            auto msg = nlohmann::json::parse(arg);
                            if (msg.contains("description") && msg["description"].contains("type") &&
                                msg["description"]["type"] == "offer") {
                                log("Received new SDP offer via datachannel, processing as signaling message");
                                // Handle this as a regular signaling message (pass JSON, not string)
                                handleSignalingMessage(msg);
                            }
                        } catch (const std::exception&) {
                            // Not JSON or parse error, ignore
                        }
                    }
                }, data);
            });
        });
    }

    if (createLocalTracks && config_.mode == ConnectionMode::Seed) {
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(kOpusPayloadType);
        audio.addSSRC(kAudioSsrc, "audio-stream", "stream1", "audio-stream");
        session.localAudioTrack = session.connection->addTrack(audio);

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(kAudioSsrc,
                                                                       "audio-stream",
                                                                       kOpusPayloadType,
                                                                       rtc::OpusRtpPacketizer::DefaultClockRate);
        session.rtpConfig = rtpConfig;
        session.nextTimestamp = rtpConfig->timestamp;

        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
        auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
        packetizer->addToChain(srReporter);
        auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
        packetizer->addToChain(nackResponder);
        session.localAudioTrack->setMediaHandler(packetizer);
    }

    sessionByUuid_[uuid] = key;

    // Flush any queued global ICE for this peer
    for (auto it = pendingGlobalIce_.begin(); it != pendingGlobalIce_.end();) {
        const auto& payload = it->payload;
        const auto payloadUuid = payload.value("UUID", payload.value("uuid", std::string{}));
        const auto payloadSession = payload.value("session", std::string{});
        if (payloadUuid == uuid && (payloadSession.empty() || payloadSession == session.sessionId)) {
            processCandidateMessage(session, payload);
            it = pendingGlobalIce_.erase(it);
        } else {
            ++it;
        }
    }

    return session;
}

void WebRTCSession::flushPendingIceLocked(PeerSession& session) {
    for (const auto& pending : session.pendingRemoteIce) {
        processCandidateMessage(session, pending.payload);
    }
    session.pendingRemoteIce.clear();
}

void WebRTCSession::queueOrApplyCandidate(PeerSession& session, const nlohmann::json& candidateObject) {
    if (!session.remoteDescriptionSet) {
        session.pendingRemoteIce.push_back({"candidate", candidateObject});
        return;
    }

    const auto candidateStr = candidateObject.value("candidate", std::string{});
    if (candidateStr.empty()) {
        return;
    }

    rtc::Candidate candidate(candidateStr);
    if (candidateObject.contains("sdpMid")) {
        candidate.hintMid(candidateObject["sdpMid"].get<std::string>());
    }

    if (session.connection) {
        session.connection->addRemoteCandidate(candidate);
    }
}

void WebRTCSession::processCandidateMessage(PeerSession& session, const nlohmann::json& candidateMessage) {
    if (candidateMessage.contains("candidates") && candidateMessage["candidates"].is_array()) {
        for (const auto& cand : candidateMessage["candidates"]) {
            if (cand.is_object()) {
                queueOrApplyCandidate(session, cand);
            }
        }
        return;
    }

    if (candidateMessage.contains("candidate")) {
        if (candidateMessage["candidate"].is_object()) {
            queueOrApplyCandidate(session, candidateMessage["candidate"]);
        } else if (candidateMessage["candidate"].is_string()) {
            nlohmann::json wrapper;
            wrapper["candidate"] = candidateMessage["candidate"];
            queueOrApplyCandidate(session, wrapper);
        }
    }
}

void WebRTCSession::start(const PluginConfig& config, double sampleRate, int channels) {
    // Clear shutdown flag when starting
    shuttingDown_.store(false, std::memory_order_release);

    ConfigSink callback;
    std::optional<PluginConfig> sanitizedForCallback;
    bool shouldEmitConnecting = false;
    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (started_) {
            return;
        }

        config_ = config;
        const auto sanitizedStream = sanitizeStreamId(config_.streamId);
        if (sanitizedStream.generated) {
            log("Stream ID missing or empty; generated fallback '" + sanitizedStream.value + "'");
        } else if (sanitizedStream.changed) {
            log("Sanitized stream ID to '" + sanitizedStream.value + "'");
        }
        if (sanitizedStream.changed) {
            config_.streamId = sanitizedStream.value;
        }

        sampleRate_ = sampleRate;
        channelCount_ = channels;

        salt_ = deriveSalt(config_.handshakeUrl);
        cachedPassword_.reset();
        hashedStreamId_ = buildHashedStreamId();
        hashedRoomId_.clear();

        outgoingResampler_.configure(sampleRate_, 48000.0, channelCount_);
        incomingResampler_.configure(48000.0, sampleRate_, channelCount_);

        if (!config_.roomName.empty()) {
            if (const auto password = effectivePassword()) {
                hashedRoomId_ = hashRoom(config_.roomName, *password);
            } else {
                hashedRoomId_ = config_.roomName;
            }
        }

        if (sampleRate_ != 48000.0) {
            log("Only 48 kHz sample rate is currently supported; audio will be resampled externally if needed.");
        }

        int opusError = 0;
        opusEncoder_ = opus_encoder_create(static_cast<opus_int32>(48000), channelCount_, OPUS_APPLICATION_AUDIO, &opusError);
        if (opusError != OPUS_OK) {
            log("FATAL: Failed to create Opus encoder");
            emitStatus("Error: Failed to create audio encoder");
            opusEncoder_ = nullptr;
            return;  // Abort session start
        }

        opusDecoder_ = opus_decoder_create(static_cast<opus_int32>(48000), channelCount_, &opusError);
        if (opusError != OPUS_OK) {
            log("FATAL: Failed to create Opus decoder");
            emitStatus("Error: Failed to create audio decoder");
            if (opusEncoder_) {
                opus_encoder_destroy(opusEncoder_);
                opusEncoder_ = nullptr;
            }
            opusDecoder_ = nullptr;
            return;  // Abort session start
        }

        outgoingFifo_.clear();
        peerSessions_.clear();
        sessionByUuid_.clear();
        pendingGlobalIce_.clear();
        roomJoined_ = false;
        roleAnnounced_ = false;

        publishingAudio_.store(false, std::memory_order_relaxed);
        receivingAudio_.store(false, std::memory_order_relaxed);
        signalingClient_ = std::make_unique<VDONinjaSignalingClient>(config_.handshakeUrl);
        signalingClient_->setCallbacks({
            [this]() {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                log("Connected to VDO.Ninja signaling server");
                emitStatus("Signaling connected");
                postInitialRequests();
            },
            [this]() {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                log("Signaling connection closed");
                bool notifyDisconnect = false;
                {
                    std::lock_guard<SpinLock> innerLock(mutex_);
                    notifyDisconnect = started_;
                    resetAllPeerConnections();
                }
                if (notifyDisconnect) {
                    publishingAudio_.store(false, std::memory_order_relaxed);
                    receivingAudio_.store(false, std::memory_order_relaxed);
                    emitStatus("Signaling disconnected");
                }
            },
            [this](const nlohmann::json& message) {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                handleSignalingMessage(message);
            },
            [this](const std::string& error) {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                log("Signaling error: " + error);
                emitStatus(std::string("Error: ") + error);
            }
        });

        signalingClient_->connectAsync();
        started_ = true;
        shouldEmitConnecting = true;
        if (sanitizedStream.changed && configUpdateSink_) {
            sanitizedForCallback = config_;
        }
        callback = configUpdateSink_;
    }

    if (shouldEmitConnecting) {
        emitStatus("Connecting...");
    }

    if (callback && sanitizedForCallback) {
        callback(*sanitizedForCallback);
    }
}

void WebRTCSession::stop() {
    log("WebRTCSession::stop() - enter");

    // Set shutdown flag FIRST to stop all callbacks immediately
    shuttingDown_.store(true, std::memory_order_release);

    // Mark as stopped to prevent new operations
    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (!started_) {
            log("WebRTCSession::stop() - already stopped");
            shuttingDown_.store(false, std::memory_order_release);
            return;
        }
        started_ = false;
    }

    log("WebRTCSession::stop() - clearing callbacks to prevent use-after-free");

    // Clear all callbacks first to prevent any callbacks during destruction
    {
        std::lock_guard<SpinLock> lock(statusSinkMutex_);
        statusSink_ = nullptr;
    }
    configUpdateSink_ = nullptr;

    log("WebRTCSession::stop() - disconnecting signaling");

    // Disconnect signaling without holding the main lock to avoid deadlocks
    std::unique_ptr<VDONinjaSignalingClient> clientToDestroy;
    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (signalingClient_) {
            signalingClient_->setCallbacks({});
            clientToDestroy = std::move(signalingClient_);
        }
    }

    if (clientToDestroy) {
        clientToDestroy->disconnect();
        clientToDestroy.reset();
    }

    log("WebRTCSession::stop() - cleaning up resources");

    // Now cleanup everything else with the lock
    {
        std::lock_guard<SpinLock> lock(mutex_);

        resetAllPeerConnections();

        if (opusEncoder_) {
            opus_encoder_destroy(opusEncoder_);
            opusEncoder_ = nullptr;
        }
        if (opusDecoder_) {
            opus_decoder_destroy(opusDecoder_);
            opusDecoder_ = nullptr;
        }

        selfUuid_.clear();
        hashedRoomId_.clear();
        hashedStreamId_.clear();

        outgoingResampler_.reset();
        incomingResampler_.reset();
        publishingAudio_.store(false, std::memory_order_relaxed);
        receivingAudio_.store(false, std::memory_order_relaxed);
    }

    log("WebRTCSession::stop() - complete");

    // Reset shutdown flag BEFORE emitting status so it can be sent
    shuttingDown_.store(false, std::memory_order_release);

    // Now emit "Idle" status (will succeed since shuttingDown_ is false)
    emitStatus("Idle");
}

bool WebRTCSession::isConnected() const {
    std::lock_guard<SpinLock> lock(mutex_);
    for (const auto& [key, session] : peerSessions_) {
        if (session.connection && session.connection->state() == rtc::PeerConnection::State::Connected) {
            return true;
        }
    }
    return false;
}

void WebRTCSession::handleSignalingMessage(const nlohmann::json& originalMessage) {
    std::optional<std::string> logLine;
    {
        std::lock_guard<std::mutex> lock(signalingLogMutex_);
        if (logSignalingMessages_) {
            try {
                const std::string dump = originalMessage.dump();
                if (lastReceivedSignalingJson_.equals(dump)) {
                    if (!suppressingReceivedDuplicate_) {
                        logLine = std::string("<= signaling: ") + dump +
                                  " (duplicate; suppressing further identical messages)";
                        suppressingReceivedDuplicate_ = true;
                    }
                } else {
                    logLine = std::string("<= signaling: ") + dump;
                    lastReceivedSignalingJson_.assign(dump);
                    suppressingReceivedDuplicate_ = false;
                }
            } catch (...) {
                logLine = "<= signaling: <unserializable message>";
                lastReceivedSignalingJson_.reset();
                suppressingReceivedDuplicate_ = false;
            }
        }
    }

    if (logLine) {
        log(*logLine);
    }

    nlohmann::json message = originalMessage;

    if (message.contains("vector")) {
        maybeDecryptDescription(message);
        maybeDecryptCandidates(message);
    }

    if (message.contains("id") && message["id"].is_string()) {
        std::lock_guard<SpinLock> lock(mutex_);
        selfUuid_ = message["id"].get<std::string>();
        log("Assigned signaling UUID: " + selfUuid_);
        return;
    }

    if (message.contains("request") && message["request"].is_string()) {
        const auto request = message["request"].get<std::string>();
        if (request == "offerSDP") {
            handleOfferRequest(message);
            return;
        }
        if (request == "listing") {
            handleListingMessage(message);
            return;
        }
        if (request == "alert") {
            log("Alert from server: " + message.value("message", std::string{}));
            return;
        }
    }

    if (message.contains("description")) {
        handleRemoteDescription(message);
    } else if (message.contains("candidates")) {
        handleRemoteCandidateBundle(message);
    } else if (message.contains("candidate")) {
        handleRemoteCandidate(message);
    }
}

void WebRTCSession::handleOfferRequest(const nlohmann::json& message) {
    if (config_.mode != ConnectionMode::Seed) {
        log("Ignoring offer request because plugin is not in seeding mode");
        return;
    }

    const std::string uuid = message.value("UUID", std::string{});
    if (uuid.empty()) {
        return;
    }

    std::lock_guard<SpinLock> lock(mutex_);
    PeerSession& session = ensurePeerSession(uuid, std::string{}, true);
    session.negotiationReady = false;

    if (session.connection) {
        session.connection->setLocalDescription();
    }
}

void WebRTCSession::handleRemoteDescription(const nlohmann::json& message) {
    if (!message.contains("description")) {
        return;
    }

    const std::string uuid = message.value("UUID", message.value("uuid", std::string{}));
    const std::string sessionId = message.value("session", std::string{});
    const auto descJson = message["description"];
    if (!descJson.contains("type") || !descJson.contains("sdp")) {
        return;
    }

    const std::string type = descJson["type"].get<std::string>();
    const std::string sdp = descJson["sdp"].get<std::string>();

    std::lock_guard<SpinLock> lock(mutex_);

    if (type == "offer") {
        PeerSession& session = ensurePeerSession(uuid, sessionId, false);
        session.streamId = message.value("streamID", session.streamId);
        session.negotiationReady = false;

        if (session.connection) {
            session.connection->setRemoteDescription(rtc::Description(sdp, type));
            session.remoteDescriptionSet = true;
            flushPendingIceLocked(session);
            session.connection->setLocalDescription(rtc::Description::Type::Answer);
        }
    } else if (type == "answer") {
        const PeerKey key = makePeerKey(uuid, sessionId);
        auto it = peerSessions_.find(key);
        if (it == peerSessions_.end()) {
            auto existing = sessionByUuid_.find(uuid);
            if (existing == sessionByUuid_.end()) {
                return;
            }
            it = peerSessions_.find(existing->second);
            if (it == peerSessions_.end()) {
                return;
            }
        }

        it->second.streamId = message.value("streamID", it->second.streamId);
        if (it->second.connection) {
            it->second.connection->setRemoteDescription(rtc::Description(sdp, type));
            it->second.remoteDescriptionSet = true;
            flushPendingIceLocked(it->second);
            it->second.negotiationReady = true;
        }
    }
}

void WebRTCSession::handleRemoteCandidateBundle(const nlohmann::json& message) {
    if (!message.contains("candidates")) {
        return;
    }

    for (const auto& candidate : message["candidates"]) {
        nlohmann::json candidateMessage = message;
        candidateMessage.erase("candidates");
        candidateMessage["candidate"] = candidate;
        handleRemoteCandidate(candidateMessage);
    }
}

void WebRTCSession::handleRemoteCandidate(const nlohmann::json& message) {
    std::string uuid = message.value("UUID", message.value("uuid", std::string{}));
    const std::string sessionId = message.value("session", std::string{});
    if (uuid.empty()) {
        return;
    }

    std::lock_guard<SpinLock> lock(mutex_);

    auto locateSession = [&]() -> PeerSession* {
        const PeerKey key = makePeerKey(uuid, sessionId);
        auto it = peerSessions_.find(key);
        if (it != peerSessions_.end()) {
            return &it->second;
        }
        auto mapped = sessionByUuid_.find(uuid);
        if (mapped != sessionByUuid_.end()) {
            auto mappedIt = peerSessions_.find(mapped->second);
            if (mappedIt != peerSessions_.end()) {
                return &mappedIt->second;
            }
        }
        return nullptr;
    };

    PeerSession* session = locateSession();
    if (!session) {
        // Limit pending ICE queue to prevent unbounded memory growth
        constexpr size_t kMaxPendingIce = 100;
        if (pendingGlobalIce_.size() >= kMaxPendingIce) {
            log("Warning: Pending ICE queue full, dropping oldest candidate");
            pendingGlobalIce_.erase(pendingGlobalIce_.begin());
        }
        pendingGlobalIce_.push_back({"candidate", message});
        return;
    }

    processCandidateMessage(*session, message);
}

void WebRTCSession::handleListingMessage(const nlohmann::json&) {
    std::lock_guard<SpinLock> lock(mutex_);
    roomJoined_ = true;
    announceRoleIfReady();
}

void WebRTCSession::sendPeerDescription(PeerSession& session,
                                         const std::string& type,
                                         const std::string& sdp) {
    nlohmann::json description = {
        {"type", type},
        {"sdp", sdp}
    };

    nlohmann::json payload;
    payload["UUID"] = session.uuid;
    if (!session.sessionId.empty()) {
        payload["session"] = session.sessionId;
    }
    if (!session.streamId.empty()) {
        payload["streamID"] = session.streamId;
    }

    const bool shouldEncrypt = effectivePassword().has_value();
    if (shouldEncrypt) {
        try {
            const auto [encrypted, vector] = encryptPayload(description.dump());
            payload["description"] = encrypted;
            payload["vector"] = vector;
        } catch (const std::exception& ex) {
            log(std::string("Failed to encrypt description: ") + ex.what());
            payload["description"] = description;
        }
    } else {
        payload["description"] = description;
    }

    // Send via datachannel if available (play mode), otherwise via WebSocket
    if (session.dataChannel && session.dataChannel->isOpen()) {
        try {
            std::string msg = payload.dump();
            session.dataChannel->send(msg);
            log("Sent SDP " + type + " via datachannel: " + msg.substr(0, 100) + "...");
        } catch (const std::exception& ex) {
            log(std::string("Failed to send via datachannel: ") + ex.what());
        }
    } else if (signalingClient_) {
        sendSignalingMessage(payload);
    }
}

void WebRTCSession::sendIceCandidate(PeerSession& session,
                                     const nlohmann::json& candidateJson,
                                     const std::string& type) {
    nlohmann::json payload;
    payload["UUID"] = session.uuid;
    if (!session.sessionId.empty()) {
        payload["session"] = session.sessionId;
    }
    if (!session.streamId.empty()) {
        payload["streamID"] = session.streamId;
    }
    payload["type"] = type;

    nlohmann::json candidates = nlohmann::json::array({candidateJson});
    const bool shouldEncrypt = effectivePassword().has_value();
    if (shouldEncrypt) {
        try {
            const auto [encrypted, vector] = encryptPayload(candidates.dump());
            payload["candidates"] = encrypted;
            payload["vector"] = vector;
        } catch (const std::exception& ex) {
            log(std::string("Failed to encrypt ICE candidates: ") + ex.what());
            payload["candidates"] = candidates;
        }
    } else {
        payload["candidates"] = candidates;
    }

    // Send via datachannel if available (play mode), otherwise via WebSocket
    if (session.dataChannel && session.dataChannel->isOpen()) {
        try {
            std::string msg = payload.dump();
            session.dataChannel->send(msg);
        } catch (const std::exception& ex) {
            log(std::string("Failed to send ICE via datachannel: ") + ex.what());
        }
    } else if (signalingClient_) {
        sendSignalingMessage(payload);
    }
}

void WebRTCSession::postInitialRequests() {
    if (!signalingClient_) {
        return;
    }

    if (!config_.roomName.empty()) {
        nlohmann::json joinMessage = {
            {"request", "joinroom"},
            {"roomid", hashedRoomId_.empty() ? config_.roomName : hashedRoomId_}
        };
        sendSignalingMessage(joinMessage);
    } else {
        std::lock_guard<SpinLock> lock(mutex_);
        log(std::string("postInitialRequests: mode=") + (config_.mode == ConnectionMode::Seed ? "Seed" : "Play") +
            ", stream=" + config_.streamId);
        announceRoleIfReady();
    }
}

void WebRTCSession::announceRoleIfReady() {
    log(std::string("announceRoleIfReady invoked; roleAnnounced=") + (roleAnnounced_ ? "true" : "false"));
    if (roleAnnounced_) {
        log("announceRoleIfReady: role already announced");
        return;
    }

    if (!config_.roomName.empty() && !roomJoined_) {
        log("announceRoleIfReady: waiting for room join");
        return;
    }

    if (!signalingClient_) {
        log("announceRoleIfReady: signaling client unavailable");
        return;
    }

    if (config_.streamId.empty()) {
        log("Stream ID not configured; skipping announcement");
        return;
    }

    if (config_.mode == ConnectionMode::Seed) {
        log("announceRoleIfReady: sending seed request");
        nlohmann::json seedMessage = {
            {"request", "seed"},
            {"streamID", hashedStreamId_.empty() ? config_.streamId : hashedStreamId_}
        };
        sendSignalingMessage(seedMessage);
        log("Sent seed request for stream " + config_.streamId);
    } else {
        log("announceRoleIfReady: sending play request");
        nlohmann::json playMessage = {
            {"request", "play"},
            {"streamID", hashedStreamId_.empty() ? config_.streamId : hashedStreamId_}
        };
        sendSignalingMessage(playMessage);
        log("Sent play request for stream " + config_.streamId);
    }

    roleAnnounced_ = true;
}

void WebRTCSession::pushOutgoingAudio(const float* const* inputs, size_t frames, int channels) {
    if (config_.mode != ConnectionMode::Seed || !opusEncoder_) {
        return;
    }

    std::lock_guard<SpinLock> lock(mutex_);
    if (peerSessions_.empty()) {
        return;
    }

    if (!publishingAudio_.exchange(true, std::memory_order_acq_rel)) {
        emitStatus("Publishing audio");
    }
    std::vector<float> interleaved;
    const size_t producedFrames = outgoingResampler_.processPlanar(inputs, frames, channels, interleaved);
    if (producedFrames == 0 || interleaved.empty()) {
        return;
    }

    for (float sample : interleaved) {
        outgoingFifo_.push_back(sample);
    }

    while (outgoingFifo_.size() >= kFrameSizeSamples * static_cast<size_t>(channels)) {
        std::vector<float> frame(kFrameSizeSamples * static_cast<size_t>(channels));
        for (size_t i = 0; i < frame.size(); ++i) {
            frame[i] = outgoingFifo_.front();
            outgoingFifo_.pop_front();
        }

        std::vector<unsigned char> encoded(4000);
        const int encodedBytes = opus_encode_float(opusEncoder_,
                                                   frame.data(),
                                                   static_cast<int>(kFrameSizeSamples),
                                                   encoded.data(),
                                                   static_cast<opus_int32>(encoded.size()));
        if (encodedBytes <= 0) {
            continue;
        }

        for (auto& [key, session] : peerSessions_) {
            if (!session.localAudioTrack || !session.rtpConfig || !session.negotiationReady) {
                continue;
            }
            session.rtpConfig->timestamp = session.nextTimestamp;
            session.localAudioTrack->send(toBinary(encoded.data(), static_cast<size_t>(encodedBytes)));
            session.nextTimestamp += static_cast<uint32_t>(kFrameSizeSamples);
        }
    }
}

size_t WebRTCSession::pullIncomingAudio(float* const* outputs, size_t frames, int channels) {
    return receiveBuffer_.pop(outputs, frames, channels);
}

} // namespace webrtc_vst

