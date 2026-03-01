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
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include <utility>

namespace webrtc_vst {

namespace {
constexpr size_t kFrameSizeSamples = 960; // 20ms at 48kHz
constexpr int kOpusPayloadType = 111;
constexpr uint32_t kAudioSsrc = 0x11ECACA; // Arbitrary but stable
constexpr size_t kPeerBufferFrames = 2048; // ~42ms at 48kHz
constexpr int kMaxReconnectAttempts = 5;
constexpr int kReconnectBaseDelayMs = 1000; // 1 second initial delay
constexpr int kReconnectMaxDelayMs = 30000; // 30 second cap
constexpr int kIdlePlayReconnectDelayMs = 15 * 60 * 1000; // 15 minute idle retry cadence
constexpr size_t kOutgoingFifoMaxSamples = 48000 * 2; // 1 second of stereo at 48kHz
constexpr size_t kJitterPreFillFrames = 960; // ~20ms at 48kHz before playback starts
constexpr size_t kMaxPeerSessions = 16;
constexpr auto kPlayRefreshCooldown = std::chrono::seconds(30);

bool truthyEnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return false;
    }
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !(lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no");
}

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

std::string generateUUID() {
    unsigned char bytes[16];
    RAND_bytes(bytes, sizeof(bytes));
    // Set version 4 (random) and variant bits per RFC 4122
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    static constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid.push_back('-');
        }
        uuid.push_back(hex[bytes[i] >> 4]);
        uuid.push_back(hex[bytes[i] & 0x0F]);
    }
    return uuid;
}

rtc::binary toBinary(const unsigned char* data, size_t size) {
    rtc::binary buffer;
    buffer.resize(size);
    std::memcpy(buffer.data(), data, size);
    return buffer;
}

struct RtpPayloadView {
    size_t offset{0};
    size_t size{0};
    uint8_t payloadType{0};
};

std::optional<RtpPayloadView> parseRtpPayloadView(const rtc::binary& packet) {
    const auto totalSize = packet.size();
    if (totalSize < 12) {
        return std::nullopt;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(packet.data());
    const uint8_t version = static_cast<uint8_t>((bytes[0] >> 6) & 0x03);
    if (version != 2) {
        return std::nullopt;
    }

    const bool hasPadding = (bytes[0] & 0x20) != 0;
    const bool hasExtension = (bytes[0] & 0x10) != 0;
    const size_t csrcCount = static_cast<size_t>(bytes[0] & 0x0F);
    const uint8_t payloadType = static_cast<uint8_t>(bytes[1] & 0x7F);

    size_t headerSize = 12 + (csrcCount * 4);
    if (headerSize > totalSize) {
        return std::nullopt;
    }

    if (hasExtension) {
        if (headerSize + 4 > totalSize) {
            return std::nullopt;
        }
        const auto extensionWords = static_cast<size_t>((static_cast<uint16_t>(bytes[headerSize + 2]) << 8) |
                                                         static_cast<uint16_t>(bytes[headerSize + 3]));
        const size_t extensionBytes = 4 + (extensionWords * 4);
        headerSize += extensionBytes;
        if (headerSize > totalSize) {
            return std::nullopt;
        }
    }

    size_t payloadSize = totalSize - headerSize;
    if (hasPadding) {
        const uint8_t paddingSize = bytes[totalSize - 1];
        if (paddingSize == 0 || paddingSize > payloadSize) {
            return std::nullopt;
        }
        payloadSize -= paddingSize;
    }

    if (payloadSize == 0) {
        return std::nullopt;
    }

    return RtpPayloadView{headerSize, payloadSize, payloadType};
}

std::optional<rtc::binary> extractRedPrimaryPayload(const uint8_t* payload, size_t payloadSize) {
    if (!payload || payloadSize < 2) {
        return std::nullopt;
    }

    size_t index = 0;
    while (index < payloadSize) {
        const uint8_t blockHeader = payload[index];
        if ((blockHeader & 0x80) == 0) {
            ++index;
            if (index >= payloadSize) {
                return std::nullopt;
            }
            rtc::binary primary(payloadSize - index);
            std::memcpy(primary.data(), payload + index, primary.size());
            return primary;
        }

        // RFC2198 non-primary block header is 4 bytes.
        if (index + 4 > payloadSize) {
            return std::nullopt;
        }
        index += 4;
    }

    return std::nullopt;
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
    sourceBuffer_.resize((effectiveFrames + 1) * static_cast<size_t>(channels));
    std::memcpy(sourceBuffer_.data(), prevSamples_.data(), sizeof(float) * static_cast<size_t>(channels));
    for (size_t frame = 0; frame < effectiveFrames; ++frame) {
        for (int ch = 0; ch < channels; ++ch) {
            sourceBuffer_[(frame + 1) * channels + ch] = inputs[ch][frame + start];
        }
    }

    return processBuffer(sourceBuffer_.data(), effectiveFrames + 1, channels, outputInterleaved);
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

    sourceBuffer_.resize((frames + 1) * static_cast<size_t>(channels));
    if (!havePrev_) {
        std::memcpy(prevSamples_.data(), data, sizeof(float) * static_cast<size_t>(channels));
        havePrev_ = true;
    }

    std::memcpy(sourceBuffer_.data(), prevSamples_.data(), sizeof(float) * static_cast<size_t>(channels));
    std::memcpy(sourceBuffer_.data() + channels, data, sizeof(float) * frames * static_cast<size_t>(channels));

    return processBuffer(sourceBuffer_.data(), frames + 1, channels, outputInterleaved);
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

    // Inject "from" UUID into every outgoing message — VDO.Ninja's signaling
    // server drops messages without this field.
    nlohmann::json enriched = payload;
    if (!selfUuid_.empty()) {
        enriched["from"] = selfUuid_;
    }

    std::optional<std::string> logLine;
    {
        std::lock_guard<std::mutex> lock(signalingLogMutex_);
        if (logSignalingMessages_) {
            try {
                const std::string dump = enriched.dump();
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

    signalingClient_->send(enriched);
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
        if (session.audioContext) {
            session.audioContext->active.store(false, std::memory_order_release);
        }
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

    if (it->second.audioContext) {
        it->second.audioContext->active.store(false, std::memory_order_release);
    }

    if (it->second.connection) {
        try {
            // Prevent re-entrant callbacks while tearing down under session lock.
            it->second.connection->onStateChange(nullptr);
            it->second.connection->onGatheringStateChange(nullptr);
            it->second.connection->onLocalDescription(nullptr);
            it->second.connection->onLocalCandidate(nullptr);
            it->second.connection->onTrack(nullptr);
            it->second.connection->onDataChannel(nullptr);
            it->second.connection->close();
        } catch (...) {
            // Ignore teardown exceptions.
        }
    }

    it->second.localAudioTrack.reset();
    it->second.remoteAudioTrack.reset();
    it->second.dataChannel.reset();
    it->second.rtpConfig.reset();
    sessionByUuid_.erase(it->second.uuid);
    peerSessions_.erase(it);
}

WebRTCSession::PeerSession* WebRTCSession::ensurePeerSession(const std::string& uuid,
                                                             const std::string& sessionHint,
                                                             bool createLocalTracks) {
    const std::string resolvedSession = sessionHint.empty() ? generateSessionId() : sessionHint;
    const PeerKey key = makePeerKey(uuid, resolvedSession);

    auto mapIt = peerSessions_.find(key);
    if (mapIt != peerSessions_.end()) {
        return &mapIt->second;
    }

    auto existing = sessionByUuid_.find(uuid);
    if (existing != sessionByUuid_.end() && existing->second != key) {
        closePeerSession(existing->second);
    }

    if (peerSessions_.size() >= kMaxPeerSessions) {
        log("Peer session cap reached (" + std::to_string(kMaxPeerSessions) +
            "), rejecting new peer " + key);
        emitStatus("Peer limit reached");
        return nullptr;
    }

    auto insertResult = peerSessions_.try_emplace(key);
    PeerSession& session = insertResult.first->second;
    session.uuid = uuid;
    session.sessionId = resolvedSession;
    session.streamId = hashedStreamId_.empty() ? config_.streamId : hashedStreamId_;
    session.nextTimestamp = 0;

    // Create per-peer audio decode context
    session.audioContext = std::make_shared<PeerAudioContext>();
    int peerOpusError = 0;
    session.audioContext->decoder = opus_decoder_create(48000, channelCount_, &peerOpusError);
    if (peerOpusError != OPUS_OK) {
        log("Failed to create per-peer Opus decoder for " + key);
        session.audioContext->decoder = nullptr;
    }
    session.audioContext->resampler.configure(48000.0, sampleRate_, channelCount_);
    session.audioContext->buffer = std::make_shared<AudioRingBuffer>(kPeerBufferFrames, channelCount_);

    rtc::Configuration configuration;
    if (!truthyEnvEnabled("WEBRTC_VST_DISABLE_STUN")) {
        configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
        configuration.iceServers.emplace_back("stun:stun1.l.google.com:19302");
    } else {
        log("STUN disabled via WEBRTC_VST_DISABLE_STUN");
    }

    session.connection = std::make_shared<rtc::PeerConnection>(configuration);
    auto keyCopy = key;

    session.connection->onStateChange([this, keyCopy](rtc::PeerConnection::State state) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;  // Prevent callback execution during/after shutdown
        }
        std::optional<std::string> statusMessage;
        bool resetMediaFlags = false;
        bool refreshPlayRequest = false;
        std::string refreshReason;
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
                    statusMessage = "Peer disconnected";
                    break;
                case rtc::PeerConnection::State::Failed:
                    log("Peer connection failed: " + keyCopy);
                    it->second.negotiationReady = false;
                    closePeerSession(keyCopy);
                    resetMediaFlags = true;
                    statusMessage = "Peer connection failed";
                    if (config_.mode == ConnectionMode::Play) {
                        refreshPlayRequest = true;
                        refreshReason = "peer failed";
                    }
                    break;
                case rtc::PeerConnection::State::Closed:
                    log("Peer connection closed: " + keyCopy);
                    it->second.negotiationReady = false;
                    closePeerSession(keyCopy);
                    resetMediaFlags = true;
                    statusMessage = "Peer connection closed";
                    if (config_.mode == ConnectionMode::Play) {
                        refreshPlayRequest = true;
                        refreshReason = "peer closed";
                    }
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

        if (refreshPlayRequest) {
            requestPlayRefresh(refreshReason);
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

        const auto trackType = track->description().type();
        if (trackType != "audio") {
            log("Ignoring non-audio remote track: " + trackType);
            return;
        }
        log("Remote audio track attached for peer " + keyCopy);
        track->onOpen([this]() {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            log("Remote audio track opened");
        });
        track->onClosed([this]() {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            log("Remote audio track closed");
        });
        track->onError([this](const std::string& error) {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            log("Remote audio track error: " + error);
        });

        auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
        track->chainMediaHandler(depacketizer);

        auto ctx = it->second.audioContext;
        int peerChannels = channelCount_;

        auto decodeAndQueue = [this, ctx, peerChannels](const uint8_t* encoded,
                                     size_t encodedSize,
                                     bool fromFrame,
                                     const char* sourceTag) -> bool {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return false;
            }
            if (!ctx->active.load(std::memory_order_acquire)) {
                return false;
            }

            int frameSamples = 0;
            std::vector<float> decodeBuffer;
            std::optional<std::string> telemetryLog;
            {
                std::lock_guard<SpinLock> lock(ctx->mutex);
                if (!ctx->decoder) {
                    return false;
                }

                if (!fromFrame && ctx->onFrameDecodeSeen) {
                    return false;
                }
                decodeBuffer.resize(kFrameSizeSamples * static_cast<size_t>(peerChannels));
                frameSamples = opus_decode_float(ctx->decoder,
                                                 encoded,
                                                 static_cast<opus_int32>(encodedSize),
                                                 decodeBuffer.data(),
                                                 static_cast<int>(kFrameSizeSamples),
                                                 0);
                if (frameSamples > 0) {
                    ++ctx->frameCount;
                    if (fromFrame) {
                        ctx->onFrameDecodeSeen = true;
                    }
                    if (!ctx->loggedFirstFrame) {
                        ctx->loggedFirstFrame = true;
                        telemetryLog = std::string("First decoded audio frame received via ") + sourceTag;
                    } else if ((ctx->frameCount % 1000) == 0) {
                        telemetryLog = "Decoded audio frame count: " + std::to_string(ctx->frameCount);
                    }
                } else {
                    ++ctx->decodeErrorCount;
                    if (ctx->decodeErrorCount <= 3 || (ctx->decodeErrorCount % 100) == 0) {
                        telemetryLog = std::string("Opus decode error ") + std::to_string(frameSamples) +
                                       " via " + sourceTag +
                                       ", total decode errors: " + std::to_string(ctx->decodeErrorCount);
                    }
                }
            }
            if (telemetryLog) {
                log(*telemetryLog);
            }
            if (frameSamples <= 0) {
                return false;
            }
            if (!receivingAudio_.exchange(true, std::memory_order_acq_rel)) {
                emitStatus("Receiving audio");
            }

            std::vector<float> resampled;
            size_t outFrames;
            {
                std::lock_guard<SpinLock> lock(ctx->mutex);
                outFrames = ctx->resampler.processInterleaved(decodeBuffer.data(),
                                                              static_cast<size_t>(frameSamples),
                                                              peerChannels,
                                                              resampled);
            }
            if (outFrames == 0 || resampled.empty()) {
                return false;
            }

            std::vector<std::vector<float>> planar(peerChannels, std::vector<float>(outFrames));
            for (size_t frame = 0; frame < outFrames; ++frame) {
                for (int ch = 0; ch < peerChannels; ++ch) {
                    planar[ch][frame] = resampled[frame * static_cast<size_t>(peerChannels) + ch];
                }
            }

            std::vector<const float*> channelPtrs(peerChannels);
            for (int ch = 0; ch < peerChannels; ++ch) {
                channelPtrs[ch] = planar[ch].data();
            }

            ctx->buffer->push(channelPtrs.data(), outFrames, peerChannels);

            // Mark pre-fill ready once buffer has enough data for smooth playback
            if (!ctx->preFillReady.load(std::memory_order_relaxed)) {
                if (ctx->buffer->availableFrames(peerChannels) >= kJitterPreFillFrames) {
                    ctx->preFillReady.store(true, std::memory_order_release);
                }
            }
            return true;
        };

        track->onFrame([decodeAndQueue](rtc::binary data, rtc::FrameInfo) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
            decodeAndQueue(bytes, data.size(), true, "onFrame");
        });

        track->onMessage([this, ctx, decodeAndQueue](rtc::message_variant message) {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }

            std::visit([this, &ctx, &decodeAndQueue](auto&& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (!std::is_same_v<T, rtc::binary>) {
                    return;
                } else {
                    const auto maybeView = parseRtpPayloadView(payload);
                    if (!maybeView) {
                        return;
                    }

                    bool shouldAttemptFallback = false;
                    {
                        std::lock_guard<SpinLock> lock(ctx->mutex);
                        ++ctx->rtpPacketCount;
                        if (!ctx->loggedFirstRtpPacket) {
                            ctx->loggedFirstRtpPacket = true;
                            log("First RTP packet received on remote track: pt=" +
                                std::to_string(maybeView->payloadType) +
                                ", bytes=" + std::to_string(maybeView->size));
                        } else if ((ctx->rtpPacketCount % 2000) == 0) {
                            log("RTP packet count on remote track: " + std::to_string(ctx->rtpPacketCount));
                        }

                        shouldAttemptFallback = !ctx->onFrameDecodeSeen && ctx->rtpPacketCount > 20;
                    }

                    if (!shouldAttemptFallback) {
                        return;
                    }

                    const auto* packetBytes = reinterpret_cast<const uint8_t*>(payload.data());
                    const auto* audioPayload = packetBytes + maybeView->offset;
                    const size_t audioPayloadSize = maybeView->size;

                    if (decodeAndQueue(audioPayload, audioPayloadSize, false, "rtp")) {
                        return;
                    }

                    if (auto redPrimary = extractRedPrimaryPayload(audioPayload, audioPayloadSize)) {
                        const auto* redBytes = reinterpret_cast<const uint8_t*>(redPrimary->data());
                        decodeAndQueue(redBytes, redPrimary->size(), false, "rtp-red");
                    }
                }
            }, message);
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

            dc->onMessage([this, keyCopy, dc](auto data) {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                // Handle incoming datachannel messages
                std::visit([this, keyCopy, dc](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, std::string>) {
                        constexpr size_t kMaxLogChars = 512;
                        if (arg.size() <= kMaxLogChars) {
                            log("Datachannel message: " + arg);
                        } else {
                            log("Datachannel message: " + arg.substr(0, kMaxLogChars) + "...(truncated)");
                        }

                        try {
                            auto msg = nlohmann::json::parse(arg);

                            const bool hasSignalingPayload =
                                msg.contains("description") ||
                                msg.contains("candidate") ||
                                msg.contains("candidates") ||
                                msg.contains("request");
                            if (!hasSignalingPayload) {
                                return;
                            }

                            const std::string dcUuid = msg.value("UUID", msg.value("uuid", std::string{}));
                            const std::string dcSession = msg.value("session", std::string{});
                            std::string dcDescType;
                            if (msg.contains("description") && msg["description"].is_object()) {
                                dcDescType = msg["description"].value("type", std::string{});
                            }
                            log("Datachannel signaling payload: uuid=" + (dcUuid.empty() ? "<none>" : dcUuid) +
                                ", session=" + (dcSession.empty() ? "<none>" : dcSession) +
                                ", descType=" + (dcDescType.empty() ? "<none>" : dcDescType) +
                                ", hasCandidates=" + (msg.contains("candidates") ? "true" : "false"));

                            {
                                std::lock_guard<SpinLock> lock(mutex_);
                                auto sourceIt = peerSessions_.find(keyCopy);
                                if (sourceIt == peerSessions_.end()) {
                                    return;
                                }

                                // Preserve identifiers carried by publisher datachannel signaling;
                                // only fill fields if absent.
                                if (!msg.contains("UUID") && !msg.contains("uuid")) {
                                    msg["UUID"] = sourceIt->second.uuid;
                                }
                                if (!msg.contains("session") && !sourceIt->second.sessionId.empty()) {
                                    msg["session"] = sourceIt->second.sessionId;
                                }
                                if (!msg.contains("streamID") && !sourceIt->second.streamId.empty()) {
                                    msg["streamID"] = sourceIt->second.streamId;
                                }

                                // Publisher signaling for additional peers often arrives over the initial
                                // datachannel; bind this datachannel to the target peer so our answer/candidates
                                // are sent back over the same path instead of bouncing through WebSocket.
                                const std::string targetUuid = msg.value("UUID", msg.value("uuid", std::string{}));
                                if (!targetUuid.empty()) {
                                    std::string targetSession = msg.value("session", std::string{});
                                    if (targetSession.empty()) {
                                        targetSession = sourceIt->second.sessionId;
                                    }
                                    PeerSession* targetSessionState =
                                        ensurePeerSession(targetUuid, targetSession, false);
                                    if (targetSessionState && targetSessionState->dataChannel.get() != dc.get()) {
                                        targetSessionState->dataChannel = dc;
                                        log("Mapped publisher datachannel to peer session " +
                                            makePeerKey(targetSessionState->uuid, targetSessionState->sessionId));
                                    }
                                }
                            }

                            handleSignalingMessage(msg);
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

    return &session;
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
    intentionalDisconnect_.store(false, std::memory_order_release);
    reconnectAttempts_ = 0;
    isReconnecting_.store(false, std::memory_order_release);

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

        outgoingFifo_.clear();
        peerSessions_.clear();
        sessionByUuid_.clear();
        pendingGlobalIce_.clear();
        roomJoined_ = false;
        roleAnnounced_ = false;
        lastPlayRefreshAt_ = {};
        selfUuid_ = generateUUID();
        log("Generated self UUID: " + selfUuid_);

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
                bool shouldReconnect = false;
                bool idlePlayMode = false;
                {
                    std::lock_guard<SpinLock> innerLock(mutex_);
                    notifyDisconnect = started_;
                    const bool hadPeers = !peerSessions_.empty();
                    resetAllPeerConnections();
                    roomJoined_ = false;
                    roleAnnounced_ = false;
                    idlePlayMode = (config_.mode == ConnectionMode::Play) && !hadPeers;
                    shouldReconnect = started_ &&
                                      !intentionalDisconnect_.load(std::memory_order_acquire) &&
                                      config_.enableAutoReconnect;
                }
                publishingAudio_.store(false, std::memory_order_relaxed);
                receivingAudio_.store(false, std::memory_order_relaxed);
                if (shouldReconnect) {
                    attemptReconnect(idlePlayMode);
                } else if (notifyDisconnect) {
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

    // Set intentional disconnect to prevent auto-reconnect
    intentionalDisconnect_.store(true, std::memory_order_release);

    // Set shutdown flag FIRST to stop all callbacks immediately
    shuttingDown_.store(true, std::memory_order_release);

    // Wait for any in-flight reconnect thread
    if (reconnectThread_ && reconnectThread_->joinable()) {
        reconnectThread_->join();
    }
    reconnectThread_.reset();
    reconnectAttempts_ = 0;
    isReconnecting_.store(false, std::memory_order_release);

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

        selfUuid_.clear();
        hashedRoomId_.clear();
        hashedStreamId_.clear();
        lastPlayRefreshAt_ = {};

        outgoingResampler_.reset();
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
            const std::string alertMessage = message.value("message", std::string{});
            log("Alert from server: " + alertMessage);
            if (!alertMessage.empty()) {
                std::string lowered = alertMessage;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (config_.mode == ConnectionMode::Seed &&
                    lowered.find("already in use") != std::string::npos) {
                    publishingAudio_.store(false, std::memory_order_relaxed);
                    emitStatus("Error: Stream ID already in use");
                } else {
                    emitStatus("Alert: " + alertMessage);
                }
            } else {
                emitStatus("Alert from signaling server");
            }
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
    PeerSession* session = ensurePeerSession(uuid, std::string{}, true);
    if (!session) {
        return;
    }
    session->negotiationReady = false;

    if (session->connection) {
        session->connection->setLocalDescription();
    }
}

void WebRTCSession::handleRemoteDescription(const nlohmann::json& message) {
    if (!message.contains("description")) {
        return;
    }

    const std::string uuid = message.value("UUID", message.value("uuid", std::string{}));
    const std::string sessionId = message.value("session", std::string{});
    const auto descJson = message["description"];
    if (!descJson.is_object() || !descJson.contains("type") || !descJson.contains("sdp")) {
        return;
    }

    const std::string type = descJson["type"].get<std::string>();
    const std::string sdp = descJson["sdp"].get<std::string>();

    if (type == "offer") {
        if (uuid.empty()) {
            log("Ignoring remote offer without UUID");
            return;
        }

        PeerKey key;
        std::shared_ptr<rtc::PeerConnection> connection;
        {
            std::lock_guard<SpinLock> lock(mutex_);
            PeerSession* session = ensurePeerSession(uuid, sessionId, false);
            if (!session) {
                return;
            }
            session->streamId = message.value("streamID", session->streamId);
            session->negotiationReady = false;
            key = makePeerKey(session->uuid, session->sessionId);
            connection = session->connection;
        }

        if (!connection) {
            return;
        }

        try {
            connection->setRemoteDescription(rtc::Description(sdp, type));
        } catch (const std::exception& ex) {
            log(std::string("Failed to apply remote offer for ") + key + ": " + ex.what());
            return;
        }

        {
            std::lock_guard<SpinLock> lock(mutex_);
            auto it = peerSessions_.find(key);
            if (it == peerSessions_.end()) {
                return;
            }

            it->second.remoteDescriptionSet = true;
            flushPendingIceLocked(it->second);
        }
    } else if (type == "answer") {
        if (config_.mode == ConnectionMode::Play) {
            log("Ignoring remote answer in play mode for " + makePeerKey(uuid, sessionId));
            return;
        }

        PeerKey key;
        std::shared_ptr<rtc::PeerConnection> connection;
        {
            std::lock_guard<SpinLock> lock(mutex_);
            const PeerKey directKey = makePeerKey(uuid, sessionId);
            auto it = peerSessions_.find(directKey);
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
            key = makePeerKey(it->second.uuid, it->second.sessionId);
            connection = it->second.connection;
        }

        if (!connection) {
            return;
        }

        try {
            connection->setRemoteDescription(rtc::Description(sdp, type));
        } catch (const std::exception& ex) {
            log(std::string("Failed to apply remote answer for ") + key + ": " + ex.what());
            return;
        }

        std::lock_guard<SpinLock> lock(mutex_);
        auto it = peerSessions_.find(key);
        if (it == peerSessions_.end()) {
            return;
        }

        it->second.remoteDescriptionSet = true;
        flushPendingIceLocked(it->second);
        it->second.negotiationReady = true;
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

void WebRTCSession::attemptReconnect(bool idlePlayMode) {
    if (isReconnecting_.exchange(true, std::memory_order_acq_rel)) {
        return; // Already reconnecting
    }

    int delayMs = kReconnectBaseDelayMs;
    if (idlePlayMode) {
        reconnectAttempts_ = 0;
        delayMs = kIdlePlayReconnectDelayMs;
        log("Idle Play mode signaling disconnect; scheduling reconnect in 15 minutes");
        emitStatus("Idle Play mode; retrying signaling in 15m");
    } else {
        reconnectAttempts_++;
        if (reconnectAttempts_ > kMaxReconnectAttempts) {
            log("Reconnection failed: max attempts (" + std::to_string(kMaxReconnectAttempts) + ") exhausted");
            emitStatus("Reconnection failed");
            isReconnecting_.store(false, std::memory_order_release);
            reconnectAttempts_ = 0;
            return;
        }

        // Exponential backoff: delay * 2^(attempt-1), capped at 30s
        delayMs = std::min(
            kReconnectBaseDelayMs * (1 << (reconnectAttempts_ - 1)),
            kReconnectMaxDelayMs);

        log("Reconnecting (attempt " + std::to_string(reconnectAttempts_) + "/" +
            std::to_string(kMaxReconnectAttempts) + ") in " + std::to_string(delayMs) + "ms");
        emitStatus("Reconnecting (" + std::to_string(reconnectAttempts_) + "/" +
                   std::to_string(kMaxReconnectAttempts) + ")...");
    }

    // Clean up any previous reconnect thread object before replacing it.
    if (reconnectThread_ && reconnectThread_->joinable()) {
        reconnectThread_->join();
    }
    reconnectThread_.reset();

    reconnectThread_ = std::make_unique<std::thread>([this, delayMs]() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (shuttingDown_.load(std::memory_order_acquire) ||
                intentionalDisconnect_.load(std::memory_order_acquire)) {
                isReconnecting_.store(false, std::memory_order_release);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        if (shuttingDown_.load(std::memory_order_acquire) ||
            intentionalDisconnect_.load(std::memory_order_acquire)) {
            isReconnecting_.store(false, std::memory_order_release);
            return;
        }

        reconnectInternal();
    });
}

void WebRTCSession::reconnectInternal() {
    // Tear down old signaling client
    std::unique_ptr<VDONinjaSignalingClient> clientToDestroy;
    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (signalingClient_) {
            signalingClient_->setCallbacks({});
            clientToDestroy = std::move(signalingClient_);
            // Release lock before disconnect to avoid deadlock
        }
    }
    if (clientToDestroy) {
        clientToDestroy->disconnect();
        clientToDestroy.reset();
    }

    if (shuttingDown_.load(std::memory_order_acquire) ||
        intentionalDisconnect_.load(std::memory_order_acquire)) {
        isReconnecting_.store(false, std::memory_order_release);
        return;
    }

    // Generate new UUID (server assigns its own, but we need a fresh "from" field)
    {
        std::lock_guard<SpinLock> lock(mutex_);
        selfUuid_ = generateUUID();
        outgoingFifo_.clear();
        lastSentSignalingJson_.reset();
        lastReceivedSignalingJson_.reset();
        suppressingSentDuplicate_ = false;
        suppressingReceivedDuplicate_ = false;
    }

    log("Reconnect: new UUID " + selfUuid_);

    // Create new signaling client and connect
    auto client = std::make_unique<VDONinjaSignalingClient>(config_.handshakeUrl);
    client->setCallbacks({
        [this]() {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            log("Reconnected to VDO.Ninja signaling server");
            reconnectAttempts_ = 0;
            isReconnecting_.store(false, std::memory_order_release);
            emitStatus("Reconnected");

            // Re-post initial requests (rejoin room + reseed/replay)
            {
                std::lock_guard<SpinLock> lock(mutex_);
                roomJoined_ = false;
                roleAnnounced_ = false;
            }
            postInitialRequests();
        },
        [this]() {
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return;
            }
            log("Signaling connection closed during reconnect");
            bool shouldRetry = false;
            bool idlePlayMode = false;
            {
                std::lock_guard<SpinLock> innerLock(mutex_);
                const bool hadPeers = !peerSessions_.empty();
                resetAllPeerConnections();
                roomJoined_ = false;
                roleAnnounced_ = false;
                idlePlayMode = (config_.mode == ConnectionMode::Play) && !hadPeers;
                shouldRetry = started_ &&
                              !intentionalDisconnect_.load(std::memory_order_acquire) &&
                              config_.enableAutoReconnect;
            }
            publishingAudio_.store(false, std::memory_order_relaxed);
            receivingAudio_.store(false, std::memory_order_relaxed);
            isReconnecting_.store(false, std::memory_order_release);
            if (shouldRetry) {
                attemptReconnect(idlePlayMode);
            } else {
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
            log("Signaling error during reconnect: " + error);
        }
    });

    {
        std::lock_guard<SpinLock> lock(mutex_);
        signalingClient_ = std::move(client);
    }

    signalingClient_->connectAsync();
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
            {"streamID", hashedStreamId_.empty() ? config_.streamId : hashedStreamId_},
            {"audio", true},
            {"video", false}
        };
        sendSignalingMessage(playMessage);
        log("Sent play request for stream " + config_.streamId);
    }

    roleAnnounced_ = true;
}

void WebRTCSession::requestPlayRefresh(const std::string& reason) {
    if (!truthyEnvEnabled("WEBRTC_VST_ENABLE_PLAY_REFRESH")) {
        return;
    }

    std::string streamIdToRequest;
    {
        std::lock_guard<SpinLock> lock(mutex_);
        if (shuttingDown_.load(std::memory_order_acquire) || !started_) {
            return;
        }
        if (config_.mode != ConnectionMode::Play) {
            return;
        }
        if (!signalingClient_) {
            return;
        }
        if (!config_.roomName.empty() && !roomJoined_) {
            return;
        }
        if (config_.streamId.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (lastPlayRefreshAt_.time_since_epoch().count() != 0 &&
            (now - lastPlayRefreshAt_) < kPlayRefreshCooldown) {
            return;
        }
        lastPlayRefreshAt_ = now;
        streamIdToRequest = hashedStreamId_.empty() ? config_.streamId : hashedStreamId_;
    }

    nlohmann::json playMessage = {
        {"request", "play"},
        {"streamID", streamIdToRequest},
        {"audio", true},
        {"video", false}
    };
    sendSignalingMessage(playMessage);
    log("Sent play refresh request (" + reason + ") for stream " + config_.streamId);
}

void WebRTCSession::pushOutgoingAudio(const float* const* inputs, size_t frames, int channels) {
    std::lock_guard<SpinLock> lock(mutex_);
    if (!started_ || config_.mode != ConnectionMode::Seed || !opusEncoder_ || peerSessions_.empty()) {
        return;
    }

    if (!publishingAudio_.exchange(true, std::memory_order_acq_rel)) {
        emitStatus("Publishing audio");
    }
    const size_t producedFrames =
        outgoingResampler_.processPlanar(inputs, frames, channels, outgoingInterleavedScratch_);
    if (producedFrames == 0 || outgoingInterleavedScratch_.empty()) {
        return;
    }

    for (float sample : outgoingInterleavedScratch_) {
        outgoingFifo_.push_back(sample);
    }

    // Cap FIFO to prevent unbounded memory growth
    const size_t maxSamples = kOutgoingFifoMaxSamples;
    while (outgoingFifo_.size() > maxSamples) {
        outgoingFifo_.pop_front();
    }

    const size_t frameSamples = kFrameSizeSamples * static_cast<size_t>(channels);
    if (outgoingFrameScratch_.size() < frameSamples) {
        outgoingFrameScratch_.resize(frameSamples);
    }

    while (outgoingFifo_.size() >= frameSamples) {
        for (size_t i = 0; i < frameSamples; ++i) {
            outgoingFrameScratch_[i] = outgoingFifo_.front();
            outgoingFifo_.pop_front();
        }

        const int encodedBytes = opus_encode_float(opusEncoder_,
                                                   outgoingFrameScratch_.data(),
                                                   static_cast<int>(kFrameSizeSamples),
                                                   outgoingEncodedScratch_.data(),
                                                   static_cast<opus_int32>(outgoingEncodedScratch_.size()));
        if (encodedBytes <= 0) {
            continue;
        }

        for (auto& [key, session] : peerSessions_) {
            if (!session.localAudioTrack || !session.rtpConfig || !session.negotiationReady) {
                continue;
            }
            if (!session.localAudioTrack->isOpen()) {
                ++session.outgoingDroppedNotOpenCount;
                if (session.outgoingDroppedNotOpenCount <= 3 ||
                    (session.outgoingDroppedNotOpenCount % 500) == 0) {
                    log("Outgoing audio drop: track not open for peer " + key +
                        " (count=" + std::to_string(session.outgoingDroppedNotOpenCount) + ")");
                }
                continue;
            }
            session.rtpConfig->timestamp = session.nextTimestamp;
            const bool sent = session.localAudioTrack->send(
                toBinary(outgoingEncodedScratch_.data(), static_cast<size_t>(encodedBytes)));
            if (!sent) {
                log("Outgoing audio send returned false for peer " + key);
                continue;
            }
            ++session.outgoingSendCount;
            if (!session.loggedFirstOutgoingFrame) {
                session.loggedFirstOutgoingFrame = true;
                log("First encoded audio frame sent to peer " + key +
                    ", bytes=" + std::to_string(encodedBytes));
            } else if ((session.outgoingSendCount % 1000) == 0) {
                log("Outgoing audio frame count for peer " + key + ": " + std::to_string(session.outgoingSendCount));
            }
            session.nextTimestamp += static_cast<uint32_t>(kFrameSizeSamples);
        }
    }
}

size_t WebRTCSession::pullIncomingAudio(float* const* outputs, size_t frames, int channels) {
    // Zero output buffers
    for (int ch = 0; ch < channels; ++ch) {
        std::fill_n(outputs[ch], frames, 0.0f);
    }

    // Snapshot peer audio buffers/contexts under lock (only peers that have pre-filled)
    auto& peerSources = pullPeerSourcesScratch_;
    peerSources.clear();
    {
        std::lock_guard<SpinLock> lock(mutex_);
        peerSources.reserve(peerSessions_.size());
        for (auto& [key, session] : peerSessions_) {
            if (session.audioContext && session.audioContext->buffer &&
                session.audioContext->preFillReady.load(std::memory_order_acquire)) {
                peerSources.push_back({session.audioContext->buffer, session.audioContext});
            }
        }
    }

    if (peerSources.empty()) {
        return 0;
    }

    const size_t tempSampleCount = static_cast<size_t>(channels) * frames;
    if (pullTempSamplesScratch_.size() < tempSampleCount) {
        pullTempSamplesScratch_.resize(tempSampleCount);
    }
    if (pullTempChannelPtrsScratch_.size() < static_cast<size_t>(channels)) {
        pullTempChannelPtrsScratch_.resize(static_cast<size_t>(channels));
    }
    for (int ch = 0; ch < channels; ++ch) {
        pullTempChannelPtrsScratch_[static_cast<size_t>(ch)] =
            pullTempSamplesScratch_.data() + (static_cast<size_t>(ch) * frames);
    }

    size_t maxFramesRead = 0;

    for (auto& source : peerSources) {
        std::fill_n(pullTempSamplesScratch_.data(), tempSampleCount, 0.0f);

        size_t peerFramesMixed = source.buffer->pop(pullTempChannelPtrsScratch_.data(), frames, channels);

        // Sum buffered audio into output
        for (int ch = 0; ch < channels; ++ch) {
            for (size_t i = 0; i < peerFramesMixed; ++i) {
                outputs[ch][i] += pullTempChannelPtrsScratch_[static_cast<size_t>(ch)][i];
            }
        }

        // Generate Opus PLC on underrun to avoid abrupt silence.
        if (peerFramesMixed < frames && source.context) {
            size_t framesNeeded = frames - peerFramesMixed;
            size_t mixOffset = peerFramesMixed;

            while (framesNeeded > 0) {
                int plcSamples = 0;
                size_t plcOutFrames = 0;
                bool plcLogged = false;
                {
                    std::lock_guard<SpinLock> ctxLock(source.context->mutex);
                    if (!source.context->decoder || !source.context->active.load(std::memory_order_acquire)) {
                        break;
                    }

                    const size_t decodeSamples = kFrameSizeSamples * static_cast<size_t>(channelCount_);
                    if (plcDecodeScratch_.size() < decodeSamples) {
                        plcDecodeScratch_.resize(decodeSamples);
                    }

                    plcSamples = opus_decode_float(source.context->decoder,
                                                   nullptr,
                                                   0,
                                                   plcDecodeScratch_.data(),
                                                   static_cast<int>(kFrameSizeSamples),
                                                   0);
                    if (plcSamples <= 0) {
                        break;
                    }

                    plcOutFrames = source.context->resampler.processInterleaved(plcDecodeScratch_.data(),
                                                                                 static_cast<size_t>(plcSamples),
                                                                                 channelCount_,
                                                                                 plcResampledScratch_);
                    if (plcOutFrames == 0 || plcResampledScratch_.empty()) {
                        break;
                    }

                    ++source.context->plcFrameCount;
                    if (!source.context->loggedFirstPlcFrame) {
                        source.context->loggedFirstPlcFrame = true;
                        plcLogged = true;
                    }
                }

                if (plcLogged) {
                    log("Generated first Opus PLC frame for underrun concealment");
                }

                const size_t channelsToMix = static_cast<size_t>(std::min(channels, channelCount_));
                const size_t framesToMix = std::min(plcOutFrames, framesNeeded);
                for (size_t i = 0; i < framesToMix; ++i) {
                    const size_t srcBase = i * static_cast<size_t>(channelCount_);
                    for (size_t ch = 0; ch < channelsToMix; ++ch) {
                        outputs[static_cast<int>(ch)][mixOffset + i] += plcResampledScratch_[srcBase + ch];
                    }
                }

                mixOffset += framesToMix;
                framesNeeded -= framesToMix;
                peerFramesMixed += framesToMix;
            }
        }

        if (peerFramesMixed > maxFramesRead) {
            maxFramesRead = peerFramesMixed;
        }
    }

    return maxFramesRead;
}

} // namespace webrtc_vst
