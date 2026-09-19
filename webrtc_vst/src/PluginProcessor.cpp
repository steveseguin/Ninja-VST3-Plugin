#include "PluginProcessor.h"
#include "ConfigState.h"

#include "StreamIdGenerator.h"
#include "ParameterIDs.h"

#include <base/source/fdebug.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <iostream>

#include <nlohmann/json.hpp>

namespace webrtc_vst {

using namespace Steinberg;
using namespace Steinberg::Vst;

const Steinberg::FUID kWebRTCProcessorUID(0x63A34A7C, 0xBE214208, 0x9DB2E0D1, 0x1173D962);

namespace {
constexpr double kDefaultSampleRate = 48000.0;
constexpr int kDefaultBufferFrames = 2048;
constexpr int kMaxProcessChannels = 8;

std::string trimCopy(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\n\r");
    return text.substr(first, last - first + 1);
}

bool passwordImpliesDisableEncryption(const std::string& value) {
    const auto trimmed = trimCopy(value);
    if (trimmed.empty()) {
        return false;
    }

    std::string lowered;
    lowered.reserve(trimmed.size());
    for (char ch : trimmed) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return lowered == "0" || lowered == "off" || lowered == "false";
}
}
bool shouldLogToStdout() {
    const char* env = std::getenv("WEBRTC_VST_LOG_STDOUT");
    if (!env) {
        return false;
    }
    if (env[0] == '0' && env[1] == '\0') {
        return false;
    }
    return true;
}

bool shouldLogSignaling() {
    const char* env = std::getenv("WEBRTC_VST_LOG_SIGNALING");
    if (!env) {
        return false;
    }
    if (env[0] == '0' && env[1] == '\0') {
        return false;
    }
    return true;
}

WebRTCProcessor::WebRTCProcessor()
    : receiveBuffer_(kDefaultBufferFrames, 2),
      session_(receiveBuffer_,
               [logToStdout = shouldLogToStdout()](const std::string& line) {
                   SMTG_DBPRT1("[WebRTC] %s\n", line.c_str());
                   if (logToStdout) {
                       std::cout << "[WebRTC] " << line << std::endl;
                   }
               },
               [this](const PluginConfig& sanitized) {
                   handleSanitizedConfig(sanitized);
               },
               [this](const std::string& status) {
                   queueStatus(status);
               }) {
    SMTG_DBPRT0("[WebRTC] WebRTCProcessor() constructor\n");
    session_.setLogSignalingMessages(shouldLogSignaling());
    std::lock_guard<std::mutex> lock(configMutex_);
    config_.streamId = generateRandomStreamId();
    config_.handshakeUrl = "wss://wss.vdo.ninja";
    config_.mode = ConnectionMode::Play;
    modeAtomic_.store(config_.mode, std::memory_order_release);
}

WebRTCProcessor::~WebRTCProcessor() {
    SMTG_DBPRT0("[WebRTC] ~WebRTCProcessor() destructor - stopping session\n");
    audioWorkerExit_.store(true, std::memory_order_release);
    if (audioWorker_.joinable()) audioWorker_.join();
    configThreadExit_.store(true, std::memory_order_release);
    configCv_.notify_one();
    if (configThread_.joinable()) {
        configThread_.join();
    }
    // Stop the session BEFORE member destruction to prevent callbacks with dangling 'this'
    stopSession();
    SMTG_DBPRT0("[WebRTC] ~WebRTCProcessor() destructor - complete\n");
}

FUnknown* WebRTCProcessor::createInstance(void* context) {
                return static_cast<Vst::IAudioProcessor*>(new WebRTCProcessor());
}

tresult PLUGIN_API WebRTCProcessor::initialize(FUnknown* context) {
    auto result = AudioEffect::initialize(context);
    if (result != kResultOk) {
        return result;
    }

    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);

    updateConfigFromEnvironment();
    syncConfigToController();
    configDirty_.store(true, std::memory_order_release);
    configThreadExit_.store(false, std::memory_order_release);
    configThread_ = std::thread(&WebRTCProcessor::configThreadMain, this);
    audioWorkerExit_.store(false, std::memory_order_release);
    audioWorker_ = std::thread(&WebRTCProcessor::audioWorkerMain, this);
    return kResultOk;
}

tresult PLUGIN_API WebRTCProcessor::terminate() {
    // Ensure plugin is deactivated before cleanup to stop audio processing
    SMTG_DBPRT0("[WebRTC] terminate() called - deactivating\n");
    setActive(false);
    audioWorkerExit_.store(true, std::memory_order_release);
    if (audioWorker_.joinable()) audioWorker_.join();
    configThreadExit_.store(true, std::memory_order_release);
    configCv_.notify_one();
    if (configThread_.joinable()) {
        configThread_.join();
    }
    // Stop session to clean up WebRTC resources
    SMTG_DBPRT0("[WebRTC] terminate() - stopping session\n");
    stopSession();
    SMTG_DBPRT0("[WebRTC] terminate() - calling parent terminate\n");
    auto result = AudioEffect::terminate();
    SMTG_DBPRT0("[WebRTC] terminate() - complete\n");
    return result;
}

tresult PLUGIN_API WebRTCProcessor::setBusArrangements(SpeakerArrangement* inputs, int32 numInputs,
    SpeakerArrangement* outputs, int32 numOutputs) {
    auto supported = [](SpeakerArrangement value) { return value == SpeakerArr::kMono || value == SpeakerArr::kStereo; };
    if (!inputs || !outputs || numInputs != 1 || numOutputs != 1 || !supported(inputs[0]) || !supported(outputs[0]))
        return kResultFalse;
    return AudioEffect::setBusArrangements(inputs, numInputs, outputs, numOutputs);
}

tresult PLUGIN_API WebRTCProcessor::setupProcessing(ProcessSetup& setup) {
    bool changed = (setup.sampleRate != processSetup_.sampleRate) ||
                   (setup.maxSamplesPerBlock != processSetup_.maxSamplesPerBlock) ||
                   (setup.symbolicSampleSize != processSetup_.symbolicSampleSize);
    if (!std::isfinite(setup.sampleRate) || setup.sampleRate < 8000 || setup.sampleRate > 384000 ||
        setup.maxSamplesPerBlock < 1 || setup.symbolicSampleSize != kSample32) return kInvalidArgument;
    { std::lock_guard<std::mutex> lock(configMutex_); processSetup_ = setup; }
    offline_.store(setup.processMode == kOffline, std::memory_order_release);
    if (changed) {
        requestConfigApply();
    }
    return AudioEffect::setupProcessing(setup);
}

void WebRTCProcessor::updateConfigFromEnvironment() {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (const char* stream = std::getenv("WEBRTC_VST_STREAM_ID")) {
        config_.streamId = stream;
    }

    if (const char* room = std::getenv("WEBRTC_VST_ROOM_NAME")) {
        config_.roomName = room;
    } else if (const char* roomLegacy = std::getenv("WEBRTC_VST_ROOM_ID")) {
        config_.roomName = roomLegacy;
    }

    if (const char* url = std::getenv("WEBRTC_VST_HANDSHAKE_URL")) {
        config_.handshakeUrl = url;
    } else if (const char* urlLegacy = std::getenv("WEBRTC_VST_SIGNALING_URL")) {
        config_.handshakeUrl = urlLegacy;
    }

    if (const char* password = std::getenv("WEBRTC_VST_PASSWORD")) {
        config_.password = password;
    }

    if (const char* web = std::getenv("WEBRTC_VST_WEB_BASE_URL")) config_.webBaseUrl = web;
    if (const char* salt = std::getenv("WEBRTC_VST_SALT")) config_.salt = salt;
    else if (!std::getenv("WEBRTC_VST_WEB_BASE_URL") &&
             (std::getenv("WEBRTC_VST_HANDSHAKE_URL") || std::getenv("WEBRTC_VST_SIGNALING_URL")))
        config_.salt = saltForUrl(config_.handshakeUrl);
    if (auto web = normalizeEndpoint(config_.webBaseUrl, true)) config_.webBaseUrl = *web;
    if (auto wss = normalizeEndpoint(config_.handshakeUrl, false)) config_.handshakeUrl = *wss;

    config_.disableEncryption = passwordImpliesDisableEncryption(config_.password);

    if (const char* modeEnv = std::getenv("WEBRTC_VST_MODE")) {
        std::string modeStr(modeEnv);
        std::transform(modeStr.begin(), modeStr.end(), modeStr.begin(), ::tolower);
        if (modeStr == "seed" || modeStr == "publish" || modeStr == "send") {
            config_.mode = ConnectionMode::Publish;
        } else {
            config_.mode = ConnectionMode::Play;
        }
    }

    if (config_.streamId.empty()) {
        config_.streamId = generateRandomStreamId();
    }

    if (config_.handshakeUrl.empty()) {
        config_.handshakeUrl = "wss://wss.vdo.ninja";
    }
    modeAtomic_.store(config_.mode, std::memory_order_release);
}


void WebRTCProcessor::startSession(const PluginConfig& config) {
    std::lock_guard<std::mutex> workLock(audioWorkMutex_);
    if (sessionActive_.load(std::memory_order_acquire)) {
        return;
    }

    if (shouldLogToStdout()) {
        std::cout << "[WebRTC] startSession requested"
                  << " mode=" << (config.mode == ConnectionMode::Publish ? "Publish" : "Play")
                  << std::endl;
    }

    const double sampleRate = activeSampleRate_;
    const int channels = 2; // Internal network audio is stereo; bridge adapts mono host buses.

    receiveBuffer_.reset(static_cast<size_t>(kDefaultBufferFrames), channels);
    // stop() clears these sinks defensively; rebind them on every start.
    session_.setConfigUpdateSink([this](const PluginConfig& sanitized) {
        handleSanitizedConfig(sanitized);
    });
    session_.setStatusSink([this](const std::string& status) {
        queueStatus(status);
    });
    session_.start(config, sampleRate, channels);
    sessionActive_.store(true, std::memory_order_release);
    if (shouldLogToStdout()) {
        std::cout << "[WebRTC] sessionActive=true" << std::endl;
    }
    configDirty_.store(false, std::memory_order_release);
}

void WebRTCProcessor::stopSession() {
    sessionActive_.store(false, std::memory_order_release);
    if (shouldLogToStdout()) {
        std::cout << "[WebRTC] sessionActive=false (stopSession)" << std::endl;
    }
    std::lock_guard<std::mutex> workLock(audioWorkMutex_);
    session_.stop();
    audioBridge_.reset();
}

void WebRTCProcessor::applyParameterChange(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) {
    // Text arrives through bounded ConfigUpdate messages, not process-local IDs.
    // Numeric automation only publishes a lock-free mailbox for the worker.
    if (id == kParamMode && std::isfinite(value) && value >= 0 && value <= 1) {
        pendingMode_.store(value >= 0.5 ? 1 : 0, std::memory_order_release);
        configDirty_.store(true, std::memory_order_release);
    }
}

tresult PLUGIN_API WebRTCProcessor::notify(IMessage* message) {
    if (!message || !message->getMessageID()) return kInvalidArgument;
    // Controller polls on its UI timer. No host/UI calls originate in process().
    if (std::strcmp(message->getMessageID(), "PollStatus") == 0) {
        if (controllerSyncPending_.exchange(false, std::memory_order_acq_rel)) syncConfigToController();
        flushPendingStatus();
        return kResultOk;
    }
    if (std::strcmp(message->getMessageID(), "ConfigUpdate") != 0) return AudioEffect::notify(message);
    const void* data = nullptr;
    Steinberg::uint32 size = 0;
    if (!message->getAttributes() || message->getAttributes()->getBinary("config", data, size) != kResultOk ||
        !data || !size || size > kMaxStateBytes) return kResultFalse;
    try {
        const std::string text(static_cast<const char*>(data), size);
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = parseConfigState(text, config_);
        pendingMode_.store(-1, std::memory_order_release);
        modeAtomic_.store(config_.mode, std::memory_order_release);
    } catch (...) { return kResultFalse; }
    requestConfigApply();
    return kResultOk;
}

void WebRTCProcessor::requestConfigApply() {
    configPending_.store(true, std::memory_order_release);
    configCv_.notify_one();
}

void WebRTCProcessor::configThreadMain() {
    SMTG_DBPRT0("[WebRTC] config thread started\n");
    while (true) {
        std::unique_lock<std::mutex> lock(configMutex_);
        configCv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
            return configThreadExit_.load(std::memory_order_acquire) || configPending_.load(std::memory_order_acquire);
        });

        if (configThreadExit_.load(std::memory_order_acquire)) {
            break;
        }
        if (!configPending_.load(std::memory_order_acquire)) {
            continue;  // Timeout, not a real wakeup
        }

        configPending_.store(false, std::memory_order_release);
        const int pendingMode = pendingMode_.exchange(-1, std::memory_order_acq_rel);
        if (pendingMode >= 0) {
            config_.mode = pendingMode ? ConnectionMode::Publish : ConnectionMode::Play;
            modeAtomic_.store(config_.mode, std::memory_order_release);
        }
        const bool shouldActivate = hostActive_.load(std::memory_order_acquire);
        const bool ready = processingReady_.load(std::memory_order_acquire);
        PluginConfig configCopy = config_;

        double sampleRate = processSetup_.sampleRate > 0.0 ? processSetup_.sampleRate : kDefaultSampleRate;
        const int channels = 2;
        lock.unlock();

        if (!shouldActivate || !ready) {
            stopSession();
            continue;
        }

        // Validate on the configuration worker, never in process(). Invalid
        // overrides must not silently connect to the public default server.
        try {
            for (const auto* value : {&configCopy.streamId, &configCopy.roomName, &configCopy.password, &configCopy.salt})
                validateSettingText(*value);
        } catch (...) {
            stopSession();
            queueStatus("Error: invalid or oversized setting");
            continue;
        }
        const auto web = normalizeEndpoint(configCopy.webBaseUrl, true);
        const auto wss = normalizeEndpoint(configCopy.handshakeUrl, false);
        if (!web || !wss) {
            stopSession();
            queueStatus("Error: invalid advanced endpoint");
            continue;
        }
        configCopy.webBaseUrl = *web;
        configCopy.handshakeUrl = *wss;

        bool configChanged = (configCopy.streamId != activeConfig_.streamId) ||
                             (configCopy.roomName != activeConfig_.roomName) ||
                             (configCopy.handshakeUrl != activeConfig_.handshakeUrl) ||
                             (configCopy.webBaseUrl != activeConfig_.webBaseUrl) ||
                             (configCopy.salt != activeConfig_.salt) ||
                             (configCopy.mode != activeConfig_.mode) ||
                             (configCopy.password != activeConfig_.password) ||
                             (configCopy.disableEncryption != activeConfig_.disableEncryption);

        bool audioParamsChanged = (sampleRate != activeSampleRate_) || (channels != activeChannels_);

        if (sessionActive_.load(std::memory_order_acquire) && !configChanged && !audioParamsChanged) {
            continue; // Nothing significant changed
        }

        stopSession();
        activeConfig_ = configCopy;
        activeSampleRate_ = sampleRate;
        activeChannels_ = channels;

        try {
            startSession(configCopy);
        } catch (...) {
            // Swallow exceptions to avoid crashing the host; emit status for visibility
            queueStatus("Error: failed to start session");
        }
    }
    SMTG_DBPRT0("[WebRTC] config thread exiting\n");
}

std::string WebRTCProcessor::serializeConfigToJson() const {
    PluginConfig copy;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        copy = config_;
    }

    nlohmann::json json = {
        {"streamId", copy.streamId},
        {"roomName", copy.roomName},
        {"handshakeUrl", copy.handshakeUrl},
        {"webBaseUrl", copy.webBaseUrl},
        {"salt", copy.salt},
        {"mode", copy.mode == ConnectionMode::Publish ? "seed" : "play"},
        {"password", copy.password},
        {"disableEncryption", copy.disableEncryption}
    };

    json["roomId"] = copy.roomName;
    json["signalingUrl"] = copy.handshakeUrl;

    return json.dump();
}

void WebRTCProcessor::syncConfigToController() {
    const auto serialized = serializeConfigToJson();
    if (serialized.empty()) {
        return;
    }

    if (auto* message = allocateMessage()) {
        message->setMessageID("ConfigSync");
        if (auto* attributes = message->getAttributes()) {
            attributes->setBinary("config",
                                  serialized.c_str(),
                                  static_cast<Steinberg::uint32>(serialized.size() + 1));
        }
        sendMessage(message);
        message->release();
    }
}

void WebRTCProcessor::handleSanitizedConfig(const PluginConfig& sanitizedConfig) {
    std::lock_guard<std::mutex> lock(configMutex_);
    bool updated = false;

    if (config_.streamId != sanitizedConfig.streamId) {
        config_.streamId = sanitizedConfig.streamId;
        updated = true;
    }

    if (config_.roomName != sanitizedConfig.roomName) {
        config_.roomName = sanitizedConfig.roomName;
        updated = true;
    }

    if (config_.password != sanitizedConfig.password) {
        config_.password = sanitizedConfig.password;
        config_.disableEncryption = passwordImpliesDisableEncryption(config_.password);
        updated = true;
    }

    if (!updated) {
        return;
    }

    controllerSyncPending_.store(true, std::memory_order_release);
}

void WebRTCProcessor::queueStatus(const std::string& status) {
    if (status.empty()) {
        return;
    }

    std::string sanitized = status;
    if (sanitized.size() > 256) {
        sanitized.resize(253);
        sanitized.append("...");
    }

    {
        std::lock_guard<SpinLock> lock(statusMutex_);
        pendingStatus_ = sanitized;
    }
    statusDirty_.store(true, std::memory_order_release);
}

void WebRTCProcessor::flushPendingStatus() {
    if (!statusDirty_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    std::string status;
    {
        std::lock_guard<SpinLock> lock(statusMutex_);
        status = pendingStatus_;
    }

    if (status.empty() || status == lastSentStatus_) {
        return;
    }

    sendStatusToController(status);
}

void WebRTCProcessor::sendStatusToController(const std::string& status) {
    if (auto* message = allocateMessage()) {
        message->setMessageID("StatusUpdate");
        if (auto* attributes = message->getAttributes()) {
            attributes->setBinary("status", status.c_str(), static_cast<Steinberg::uint32>(status.size() + 1));
        }
        const auto result = sendMessage(message);
        message->release();
        if (result == kResultOk) lastSentStatus_ = status;
        else statusDirty_.store(true, std::memory_order_release);
    }
    else statusDirty_.store(true, std::memory_order_release);
}


tresult PLUGIN_API WebRTCProcessor::setActive(TBool state) {
    if (state) {
        hostActive_.store(true, std::memory_order_release);
        configDirty_.store(true, std::memory_order_release);
        requestConfigApply();
    } else {
        hostActive_.store(false, std::memory_order_release);
        requestConfigApply();
        flushPendingStatus();
    }
    return AudioEffect::setActive(state);
}

tresult PLUGIN_API WebRTCProcessor::canProcessSampleSize(int32 symbolicSampleSize) {
    return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

void WebRTCProcessor::audioWorkerMain() {
    std::array<float, RealtimeAudioBridge::chunk> left{}, right{};
    float* output[] = {left.data(), right.data()};
    const float* input[] = {left.data(), right.data()};
    while (!audioWorkerExit_.load(std::memory_order_acquire)) {
        bool worked = false;
        {
            std::lock_guard<std::mutex> lock(audioWorkMutex_);
            if (sessionActive_.load(std::memory_order_acquire) && !offline_.load(std::memory_order_acquire)) {
                try {
                    if (const auto frames = audioBridge_.takeInput(output)) {
                        session_.pushOutgoingAudio(input, frames, 2);
                        worked = true;
                    }
                    if (const auto frames = audioBridge_.takeDemand()) {
                        session_.pullIncomingAudio(output, frames, 2);
                        audioBridge_.supplyOutput(input, frames);
                        worked = true;
                    }
                } catch (...) {
                    // A track may close between isOpen() and send(). A transport
                    // exception must not escape this thread and terminate a DAW.
                    audioBridge_.reset();
                    queueStatus("Audio transport error; retrying");
                }
            }
        }
        if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

tresult PLUGIN_API WebRTCProcessor::process(ProcessData& data) {
    if (data.numSamples < 0 || data.symbolicSampleSize != kSample32) return kResultFalse;
    // Prefetch is still live playback, not an offline bounce. Tell supporting
    // hosts not to prefetch via IPrefetchableSupport; remain audible if ignored.
    const bool realtime = data.processMode != kOffline;
    offline_.store(!realtime, std::memory_order_release);
    if (!processingReady_.exchange(true, std::memory_order_acq_rel)) {
        configPending_.store(true, std::memory_order_release);
    }
    if (auto* changes = data.inputParameterChanges) {
        for (int32 i = 0; i < changes->getParameterCount(); ++i) {
            if (auto* queue = changes->getParameterData(i); queue && queue->getPointCount() > 0) {
                int32 offset = 0;
                ParamValue value = 0;
                if (queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk)
                    applyParameterChange(queue->getParameterId(), value);
            }
        }
    }
    // The configuration worker polls this atomic mailbox. No mutex, condition
    // variable notification, logging, messages, codec or network calls here.
    if (configDirty_.exchange(false, std::memory_order_acq_rel))
        configPending_.store(true, std::memory_order_release);
    if (data.numSamples == 0) return kResultOk;
    const bool hasInput = data.numInputs > 0 && data.inputs && data.inputs[0].channelBuffers32;
    const bool hasOutput = data.numOutputs > 0 && data.outputs && data.outputs[0].channelBuffers32;
    const int inChannels = hasInput ? std::max(0, data.inputs[0].numChannels) : 0;
    const int outChannels = hasOutput ? std::max(0, data.outputs[0].numChannels) : 0;
    const bool publish = modeAtomic_.load(std::memory_order_acquire) == ConnectionMode::Publish;
    // Read/enqueue before writing any in-place output buffers.
    if (publish && realtime && hasInput && sessionActive_.load(std::memory_order_acquire))
        audioBridge_.transfer(data.inputs[0].channelBuffers32, nullptr, data.numSamples, inChannels, 0, true);
    if (hasOutput) {
        data.outputs[0].silenceFlags = 0;
        for (int ch = 0; ch < outChannels; ++ch) {
            float* output = data.outputs[0].channelBuffers32[ch];
            if (!output) continue;
            const float* input = publish && ch < inChannels ? data.inputs[0].channelBuffers32[ch] : nullptr;
            for (int32 i = 0; i < data.numSamples; ++i)
                output[i] = input && std::isfinite(input[i]) ? input[i] : 0;
        }
        if (!publish && realtime && sessionActive_.load(std::memory_order_acquire))
            audioBridge_.transfer(nullptr, data.outputs[0].channelBuffers32, data.numSamples, 0, outChannels, false);
    }
    return kResultOk;
}

tresult PLUGIN_API WebRTCProcessor::setState(IBStream* state) {
    if (!state) {
        return kInvalidArgument;
    }

    std::string serialized;
    if (!readBoundedState(state, serialized)) return kResultFalse;

    if (serialized.empty()) {
        return kResultOk;
    }

    try {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = parseConfigState(serialized, config_);
        pendingMode_.store(-1, std::memory_order_release);
        modeAtomic_.store(config_.mode, std::memory_order_release);
    } catch (...) {
        return kResultFalse; // Nothing was committed; do not reconnect or sync.
    }

    configDirty_.store(true, std::memory_order_release);
    requestConfigApply();
    syncConfigToController();

    return kResultOk;
}


tresult PLUGIN_API WebRTCProcessor::getState(IBStream* state) {
    if (!state) {
        return kInvalidArgument;
    }

    const auto serialized = serializeConfigToJson();
    Steinberg::int32 written = 0;
    auto* mutableData = const_cast<char*>(serialized.data());
    const auto result = state->write(mutableData, static_cast<int32>(serialized.size()), &written);
    return result == kResultOk && written == static_cast<int32>(serialized.size()) ? kResultOk : kResultFalse;
}


Steinberg::tresult PLUGIN_API WebRTCProcessor::getControllerClassId(Steinberg::TUID classId) {
    if (!classId) {
        return Steinberg::kInvalidArgument;
    }

    kWebRTCControllerUID.toTUID(classId);
    return Steinberg::kResultTrue;
}

} // namespace webrtc_vst

