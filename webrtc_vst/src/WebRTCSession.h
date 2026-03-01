#pragma once

#include "AudioRingBuffer.h"
#include "PluginConfig.h"
#include "VDONinjaSignalingClient.h"
#include "SpinLock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstring>

#include <nlohmann/json.hpp>
#include <opus.h>

namespace rtc {
class PeerConnection;
class Track;
class RtpPacketizationConfig;
class DataChannel;
} // namespace rtc

namespace webrtc_vst {

class WebRTCSession {
public:
    using LogSink = std::function<void(const std::string&)>;
    using ConfigSink = std::function<void(const PluginConfig&)>;
    using StatusSink = std::function<void(const std::string&)>;

    WebRTCSession(AudioRingBuffer& receiveBuffer,
                  LogSink logSink = {},
                  ConfigSink configSink = {},
                  StatusSink statusSink = {});
    ~WebRTCSession();

    void start(const PluginConfig& config, double sampleRate, int channels);
    void stop();
    void setConfigUpdateSink(ConfigSink sink);
    void setStatusSink(StatusSink sink);
    void setLogSignalingMessages(bool enable);

    void pushOutgoingAudio(const float* const* inputs, size_t frames, int channels);
    size_t pullIncomingAudio(float* const* outputs, size_t frames, int channels);

    bool isConnected() const;

private:
    class LinearResampler {
    public:
        void configure(double inputRate, double outputRate, int channels);
        void reset();
        bool isPassthrough() const { return passthrough_; }
        size_t processPlanar(const float* const* inputs, size_t frames, int channels, std::vector<float>& outputInterleaved);
        size_t processInterleaved(const float* data, size_t frames, int channels, std::vector<float>& outputInterleaved);
        size_t processInterleaved(const std::vector<float>& data, int channels, std::vector<float>& outputInterleaved);

    private:
        size_t processBuffer(const float* source, size_t frames, int channels, std::vector<float>& outputInterleaved);

        double inputRate_{48000.0};
        double outputRate_{48000.0};
        double step_{1.0};
        double phase_{0.0};
        bool configured_{false};
        bool passthrough_{true};
        bool havePrev_{false};
        int channels_{0};
        std::vector<float> prevSamples_;
    };

    struct PendingIce {
        std::string type;
        nlohmann::json payload;
    };

    struct PeerAudioContext {
        ::OpusDecoder* decoder{nullptr};
        LinearResampler resampler;
        std::shared_ptr<AudioRingBuffer> buffer;
        std::atomic<bool> active{true};
        std::atomic<bool> preFillReady{false}; // true once buffer has enough data
        uint64_t frameCount{0};
        uint64_t decodeErrorCount{0};
        bool loggedFirstFrame{false};
        bool onFrameDecodeSeen{false};
        uint64_t rtpPacketCount{0};
        bool loggedFirstRtpPacket{false};
        SpinLock mutex;

        ~PeerAudioContext() {
            if (decoder) {
                opus_decoder_destroy(decoder);
                decoder = nullptr;
            }
        }
    };

    struct PeerSession {
        std::shared_ptr<rtc::PeerConnection> connection;
        std::shared_ptr<rtc::Track> localAudioTrack;
        std::shared_ptr<rtc::Track> remoteAudioTrack;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
        std::shared_ptr<rtc::DataChannel> dataChannel;
        std::shared_ptr<PeerAudioContext> audioContext;
        std::string uuid;
        std::string sessionId;
        std::string streamId;
        std::vector<PendingIce> pendingRemoteIce;
        uint32_t nextTimestamp{0};
        bool negotiationReady{false};
        bool remoteDescriptionSet{false};
        bool localDescriptionSet{false};
        bool loggedFirstOutgoingFrame{false};
        uint64_t outgoingSendCount{0};
        uint64_t outgoingDroppedNotOpenCount{0};
    };

    using PeerKey = std::string;

    void resetAllPeerConnections();
    PeerKey makePeerKey(const std::string& uuid, const std::string& session) const;
    PeerSession& ensurePeerSession(const std::string& uuid,
                                   const std::string& session,
                                   bool createLocalTracks);
    void closePeerSession(const PeerKey& key);

    void handleSignalingMessage(const nlohmann::json& message);
    void handleOfferRequest(const nlohmann::json& message);
    void handleRemoteDescription(const nlohmann::json& message);
    void handleRemoteCandidateBundle(const nlohmann::json& message);
    void handleRemoteCandidate(const nlohmann::json& message);
    void handleListingMessage(const nlohmann::json& message);

    void sendPeerDescription(PeerSession& session,
                             const std::string& type,
                             const std::string& sdp);
    void sendIceCandidate(PeerSession& session,
                          const nlohmann::json& candidateJson,
                          const std::string& type);

    void postInitialRequests();
    void announceRoleIfReady();
    void attemptReconnect();
    void reconnectInternal();
    void processCandidateMessage(PeerSession& session, const nlohmann::json& candidateMessage);
    void queueOrApplyCandidate(PeerSession& session, const nlohmann::json& candidateObject);
    void flushPendingIceLocked(PeerSession& session);
    void sendSignalingMessage(const nlohmann::json& payload);

    void emitStatus(const std::string& status) const;
    void log(const std::string& line) const;

    std::optional<std::string> effectivePassword() const;
    std::string deriveSalt(const std::string& url) const;
    std::string hashRoom(const std::string& room, const std::string& password) const;
    std::string hashStreamIdSuffix(const std::string& password) const;
    nlohmann::json decryptFieldIfNeeded(const nlohmann::json& message) const;
    void maybeDecryptDescription(nlohmann::json& message) const;
    void maybeDecryptCandidates(nlohmann::json& message) const;
    std::string buildHashedStreamId() const;
    std::pair<std::string, std::string> encryptPayload(const std::string& payload) const;
    std::string decryptPayload(const std::string& payload, const std::string& vector) const;

    PluginConfig config_;
    double sampleRate_{48000.0};
    int channelCount_{2};

    AudioRingBuffer& receiveBuffer_;
    AudioRingBuffer outgoingBuffer_{48000, 2};

    std::unique_ptr<VDONinjaSignalingClient> signalingClient_;

    std::string selfUuid_;

    mutable SpinLock mutex_;
    std::atomic<bool> shuttingDown_{false};
    bool started_{false};
    bool intentionalDisconnect_{false};
    std::atomic<bool> isReconnecting_{false};
    int reconnectAttempts_{0};
    std::unique_ptr<std::thread> reconnectThread_;

    ::OpusEncoder* opusEncoder_{nullptr};
    std::deque<float> outgoingFifo_;

    LogSink logSink_;
    ConfigSink configUpdateSink_;
    StatusSink statusSink_;
    mutable SpinLock statusSinkMutex_;
    std::atomic<bool> receivingAudio_{false};
    std::atomic<bool> publishingAudio_{false};
    mutable std::mutex signalingLogMutex_;

    std::unordered_map<PeerKey, PeerSession> peerSessions_;
    std::unordered_map<std::string, PeerKey> sessionByUuid_;
    std::vector<PendingIce> pendingGlobalIce_;
    std::string salt_;
    bool logSignalingMessages_{false};
    struct SignalingJsonCache {
        std::array<char, 1024> storage{};
        size_t length{0};

        void reset() {
            length = 0;
            storage[0] = '\0';
        }

        void assign(std::string_view text) {
            const size_t maxCopy = storage.size() - 1;
            length = std::min(text.size(), maxCopy);
            std::memcpy(storage.data(), text.data(), length);
            storage[length] = '\0';
        }

        bool equals(std::string_view text) const {
            return length == text.size() &&
                   std::memcmp(storage.data(), text.data(), length) == 0;
        }

        std::string_view view() const {
            return std::string_view(storage.data(), length);
        }
    };

    SignalingJsonCache lastSentSignalingJson_;
    SignalingJsonCache lastReceivedSignalingJson_;
    bool suppressingSentDuplicate_{false};
    bool suppressingReceivedDuplicate_{false};
    std::string hashedStreamId_;
    std::string hashedRoomId_;
    mutable std::optional<std::string> cachedPassword_;
    bool roomJoined_{false};
    bool roleAnnounced_{false};
    LinearResampler outgoingResampler_;
};

} // namespace webrtc_vst

