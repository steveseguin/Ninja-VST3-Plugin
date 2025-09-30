#include "WebRTCSession.h"

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

WebRTCSession::WebRTCSession(AudioRingBuffer& receiveBuffer, LogSink logSink)
    : receiveBuffer_(receiveBuffer), logSink_(std::move(logSink)) {}

WebRTCSession::~WebRTCSession() {
    stop();
}

void WebRTCSession::log(const std::string& line) const {
    if (logSink_) {
        logSink_(line);
    }
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
    for (auto& [key, session] : peerSessions_) {
        if (session.connection) {
            session.connection->close();
        }
    }
    peerSessions_.clear();
    sessionByUuid_.clear();
    pendingGlobalIce_.clear();
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
        std::lock_guard<SpinLock> lock(mutex_);
        auto it = peerSessions_.find(keyCopy);
        if (it == peerSessions_.end()) {
            return;
        }
        if (state == rtc::PeerConnection::State::Connected) {
            it->second.negotiationReady = true;
            log("Peer connection connected: " + keyCopy);
        } else if (state == rtc::PeerConnection::State::Closed || state == rtc::PeerConnection::State::Failed) {
            log("Peer connection closed: " + keyCopy);
            it->second.negotiationReady = false;
            closePeerSession(keyCopy);
        }
    });

    session.connection->onGatheringStateChange([this, keyCopy](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            log("ICE gathering complete for " + keyCopy);
        }
    });

    session.connection->onLocalDescription([this, keyCopy](rtc::Description description) {
        std::lock_guard<SpinLock> lock(mutex_);
        auto it = peerSessions_.find(keyCopy);
        if (it == peerSessions_.end()) {
            return;
        }
        it->second.localDescriptionSet = true;
        sendPeerDescription(it->second, description.typeString(), std::string(description));
    });

    session.connection->onLocalCandidate([this, keyCopy](rtc::Candidate candidate) {
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
            if (!opusDecoder_) {
                return;
            }

            std::vector<float> decodeBuffer(kFrameSizeSamples * static_cast<size_t>(channelCount_));
            int frameSamples = opus_decode_float(opusDecoder_,
                                                 reinterpret_cast<const unsigned char*>(data.data()),
                                                 static_cast<opus_int32>(data.size()),
                                                 decodeBuffer.data(),
                                                 static_cast<int>(kFrameSizeSamples),
                                                 0);
            if (frameSamples <= 0) {
                return;
            }

            std::vector<float> resampled;
            const size_t outFrames = incomingResampler_.processInterleaved(decodeBuffer.data(),
                                                                           static_cast<size_t>(frameSamples),
                                                                           channelCount_,
                                                                           resampled);
            if (outFrames == 0 || resampled.empty()) {
                return;
            }

            std::vector<std::vector<float>> planar(channelCount_, std::vector<float>(outFrames));
            for (size_t frame = 0; frame < outFrames; ++frame) {
                for (int ch = 0; ch < channelCount_; ++ch) {
                    planar[ch][frame] = resampled[frame * static_cast<size_t>(channelCount_) + ch];
                }
            }

            std::vector<const float*> channelPtrs(channelCount_);
            for (int ch = 0; ch < channelCount_; ++ch) {
                channelPtrs[ch] = planar[ch].data();
            }

            receiveBuffer_.push(channelPtrs.data(), static_cast<size_t>(outFrames), channelCount_);
        });
        it->second.remoteAudioTrack = track;
    });

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
    std::lock_guard<SpinLock> lock(mutex_);
    if (started_) {
        return;
    }

    config_ = config;
    sampleRate_ = sampleRate;
   channelCount_ = channels;

    salt_ = deriveSalt(config_.signalingUrl);
    cachedPassword_.reset();
    hashedStreamId_ = buildHashedStreamId();
    hashedRoomId_.clear();

    outgoingResampler_.configure(sampleRate_, 48000.0, channelCount_);
    incomingResampler_.configure(48000.0, sampleRate_, channelCount_);

    if (!config_.roomId.empty()) {
        if (const auto password = effectivePassword()) {
            hashedRoomId_ = hashRoom(config_.roomId, *password);
        } else {
            hashedRoomId_ = config_.roomId;
        }
    }

    if (sampleRate_ != 48000.0) {
        log("Only 48 kHz sample rate is currently supported; audio will be resampled externally if needed.");
    }

    int opusError = 0;
    opusEncoder_ = opus_encoder_create(static_cast<opus_int32>(48000), channelCount_, OPUS_APPLICATION_AUDIO, &opusError);
    if (opusError != OPUS_OK) {
        log("Failed to create Opus encoder");
        opusEncoder_ = nullptr;
    }

    opusDecoder_ = opus_decoder_create(static_cast<opus_int32>(48000), channelCount_, &opusError);
    if (opusError != OPUS_OK) {
        log("Failed to create Opus decoder");
        opusDecoder_ = nullptr;
    }

    outgoingFifo_.clear();
    peerSessions_.clear();
    sessionByUuid_.clear();
    pendingGlobalIce_.clear();
    roomJoined_ = false;
    roleAnnounced_ = false;

    signalingClient_ = std::make_unique<VDONinjaSignalingClient>(config_.signalingUrl);
    signalingClient_->setCallbacks({
        [this]() {
            log("Connected to VDO.Ninja signaling server");
            postInitialRequests();
        },
        [this]() {
            log("Signaling connection closed");
            std::lock_guard<SpinLock> innerLock(mutex_);
            resetAllPeerConnections();
        },
        [this](const nlohmann::json& message) {
            handleSignalingMessage(message);
        },
        [this](const std::string& error) {
            log("Signaling error: " + error);
        }
    });

    signalingClient_->connectAsync();
    started_ = true;
}

void WebRTCSession::stop() {
    std::lock_guard<SpinLock> lock(mutex_);
    if (!started_) {
        return;
    }

    if (signalingClient_) {
        signalingClient_->setCallbacks({});
        signalingClient_->disconnect();
        signalingClient_.reset();
    }

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
    started_ = false;

    outgoingResampler_.reset();
    incomingResampler_.reset();
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

    if (message.contains("request")) {
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
    if (!signalingClient_) {
        return;
    }

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

    signalingClient_->send(payload);
}

void WebRTCSession::sendIceCandidate(PeerSession& session,
                                     const nlohmann::json& candidateJson,
                                     const std::string& type) {
    if (!signalingClient_) {
        return;
    }

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

    signalingClient_->send(payload);
}

void WebRTCSession::postInitialRequests() {
    if (!signalingClient_) {
        return;
    }

    if (!config_.roomId.empty()) {
        nlohmann::json joinMessage = {
            {"request", "joinroom"},
            {"roomid", hashedRoomId_.empty() ? config_.roomId : hashedRoomId_}
        };
        signalingClient_->send(joinMessage);
    } else {
        std::lock_guard<SpinLock> lock(mutex_);
        announceRoleIfReady();
    }
}

void WebRTCSession::announceRoleIfReady() {
    if (roleAnnounced_) {
        return;
    }

    if (!config_.roomId.empty() && !roomJoined_) {
        return;
    }

    if (!signalingClient_) {
        return;
    }

    if (config_.streamId.empty()) {
        log("Stream ID not configured; skipping announcement");
        return;
    }

    if (config_.mode == ConnectionMode::Seed) {
        nlohmann::json seedMessage = {
            {"request", "seed"},
            {"streamID", hashedStreamId_.empty() ? config_.streamId : hashedStreamId_}
        };
        signalingClient_->send(seedMessage);
        log("Sent seed request for stream " + config_.streamId);
    } else {
        nlohmann::json playMessage = {
            {"request", "play"},
            {"streamID", hashedStreamId_.empty() ? config_.streamId : hashedStreamId_}
        };
        signalingClient_->send(playMessage);
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



