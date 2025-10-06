#pragma once

#include "AudioRingBuffer.h"
#include "PluginConfig.h"
#include "VDONinjaSignalingClient.h"
#include "SpinLock.h"

#include <deque>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

    WebRTCSession(AudioRingBuffer& receiveBuffer,
                  LogSink logSink = {},
                  ConfigSink configSink = {});
    ~WebRTCSession();

    void start(const PluginConfig& config, double sampleRate, int channels);
    void stop();
    void setConfigUpdateSink(ConfigSink sink);
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

    struct PeerSession {
        std::shared_ptr<rtc::PeerConnection> connection;
        std::shared_ptr<rtc::Track> localAudioTrack;
        std::shared_ptr<rtc::Track> remoteAudioTrack;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
        std::shared_ptr<rtc::DataChannel> dataChannel;  // For sending signaling messages in play mode
        std::string uuid;
        std::string sessionId;
        std::string streamId;
        std::vector<PendingIce> pendingRemoteIce;
        uint32_t nextTimestamp{0};
        bool negotiationReady{false};
        bool remoteDescriptionSet{false};
        bool localDescriptionSet{false};
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
    void processCandidateMessage(PeerSession& session, const nlohmann::json& candidateMessage);
    void queueOrApplyCandidate(PeerSession& session, const nlohmann::json& candidateObject);
    void flushPendingIceLocked(PeerSession& session);
    void sendSignalingMessage(const nlohmann::json& payload);

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
    bool started_{false};

    ::OpusEncoder* opusEncoder_{nullptr};
    ::OpusDecoder* opusDecoder_{nullptr};
    std::deque<float> outgoingFifo_;

    LogSink logSink_;
    ConfigSink configUpdateSink_;
    mutable std::mutex signalingLogMutex_;

    std::unordered_map<PeerKey, PeerSession> peerSessions_;
    std::unordered_map<std::string, PeerKey> sessionByUuid_;
    std::vector<PendingIce> pendingGlobalIce_;
    std::string salt_;
    bool logSignalingMessages_{false};
    std::string lastSentSignalingJson_;
    std::string lastReceivedSignalingJson_;
    bool suppressingSentDuplicate_{false};
    bool suppressingReceivedDuplicate_{false};
    std::string hashedStreamId_;
    std::string hashedRoomId_;
    mutable std::optional<std::string> cachedPassword_;
    bool roomJoined_{false};
    bool roleAnnounced_{false};
    LinearResampler outgoingResampler_;
    LinearResampler incomingResampler_;
};

} // namespace webrtc_vst

