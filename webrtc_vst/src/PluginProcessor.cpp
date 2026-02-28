#include "PluginProcessor.h"

#include "StreamIdGenerator.h"
#include "ParameterIDs.h"
#include "ParameterStringRegistry.h"

#include <base/source/fdebug.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <algorithm>
#include <array>
#include <cctype>
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
    config_.handshakeUrl = "wss://wss0.vdo.ninja";
    config_.mode = ConnectionMode::Play;
    modeAtomic_.store(config_.mode, std::memory_order_release);
}

WebRTCProcessor::~WebRTCProcessor() {
    SMTG_DBPRT0("[WebRTC] ~WebRTCProcessor() destructor - stopping session\n");
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
    return kResultOk;
}

tresult PLUGIN_API WebRTCProcessor::terminate() {
    // Ensure plugin is deactivated before cleanup to stop audio processing
    SMTG_DBPRT0("[WebRTC] terminate() called - deactivating\n");
    setActive(false);
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

tresult PLUGIN_API WebRTCProcessor::setupProcessing(ProcessSetup& setup) {
    processSetup_ = setup;
    requestConfigApply();
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

    config_.disableEncryption = passwordImpliesDisableEncryption(config_.password);

    if (const char* modeEnv = std::getenv("WEBRTC_VST_MODE")) {
        std::string modeStr(modeEnv);
        std::transform(modeStr.begin(), modeStr.end(), modeStr.begin(), ::tolower);
        if (modeStr == "seed" || modeStr == "publish" || modeStr == "send") {
            config_.mode = ConnectionMode::Seed;
        } else {
            config_.mode = ConnectionMode::Play;
        }
    }

    if (config_.streamId.empty()) {
        config_.streamId = generateRandomStreamId();
    }

    if (config_.handshakeUrl.empty()) {
        config_.handshakeUrl = "wss://wss0.vdo.ninja";
    }
    modeAtomic_.store(config_.mode, std::memory_order_release);
}


void WebRTCProcessor::startSession(const PluginConfig& config) {
    if (sessionActive_.load(std::memory_order_acquire)) {
        return;
    }

    if (shouldLogToStdout()) {
        std::cout << "[WebRTC] startSession requested"
                  << " mode=" << (config.mode == ConnectionMode::Seed ? "Seed" : "Play")
                  << std::endl;
    }

    double sampleRate = processSetup_.sampleRate > 0.0 ? processSetup_.sampleRate : kDefaultSampleRate;
    int channels = 2;
    if (!audioInputs.empty()) {
        BusInfo info;
        if (audioInputs[0]->getInfo(info)) {
            channels = info.channelCount;
        }
    } else if (!audioOutputs.empty()) {
        BusInfo info;
        if (audioOutputs[0]->getInfo(info)) {
            channels = info.channelCount;
        }
    }

    receiveBuffer_.reset(static_cast<size_t>(kDefaultBufferFrames), channels);
    session_.start(config, sampleRate, channels);
    sessionActive_.store(true, std::memory_order_release);
    if (shouldLogToStdout()) {
        std::cout << "[WebRTC] sessionActive=true" << std::endl;
    }
    configDirty_.store(false, std::memory_order_release);
}

void WebRTCProcessor::stopSession() {
    if (!sessionActive_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (shouldLogToStdout()) {
        std::cout << "[WebRTC] sessionActive=false (stopSession)" << std::endl;
    }
    session_.stop();
}

void WebRTCProcessor::applyParameterChange(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) {
    std::lock_guard<std::mutex> lock(configMutex_);
    switch (id) {
        case kParamMode:
            config_.mode = (value >= 0.5) ? ConnectionMode::Seed : ConnectionMode::Play;
            modeAtomic_.store(config_.mode, std::memory_order_release);
            configDirty_.store(true, std::memory_order_release);
            break;
        case kParamDisableEncryption:
            config_.disableEncryption = passwordImpliesDisableEncryption(config_.password);
            break;
        case kParamStreamId: {
            const auto idValue = normalizedToId(value);
            const auto text = ParameterStringRegistry::instance().lookup(idValue);
            if (!text.empty()) {
                config_.streamId = text;
                configDirty_.store(true, std::memory_order_release);
            }
            break;
        }
        case kParamRoomName: {
            const auto idValue = normalizedToId(value);
            config_.roomName = ParameterStringRegistry::instance().lookup(idValue);
            configDirty_.store(true, std::memory_order_release);
            break;
        }
        case kParamHandshakeUrl: {
            const auto idValue = normalizedToId(value);
            const auto url = ParameterStringRegistry::instance().lookup(idValue);
            if (!url.empty()) {
                config_.handshakeUrl = url;
                configDirty_.store(true, std::memory_order_release);
            }
            break;
        }
        case kParamPassword: {
            const auto idValue = normalizedToId(value);
            config_.password = ParameterStringRegistry::instance().lookup(idValue);
            config_.disableEncryption = passwordImpliesDisableEncryption(config_.password);
            configDirty_.store(true, std::memory_order_release);
            break;
        }
        default:
            break;
    }
}

void WebRTCProcessor::requestConfigApply() {
    configPending_.store(true, std::memory_order_release);
    configCv_.notify_one();
}

void WebRTCProcessor::configThreadMain() {
    SMTG_DBPRT0("[WebRTC] config thread started\n");
    while (true) {
        std::unique_lock<std::mutex> lock(configMutex_);
        configCv_.wait(lock, [this]() {
            return configThreadExit_.load(std::memory_order_acquire) || configPending_.load(std::memory_order_acquire);
        });

        if (configThreadExit_.load(std::memory_order_acquire)) {
            break;
        }

        configPending_.store(false, std::memory_order_release);
        const bool shouldActivate = hostActive_.load(std::memory_order_acquire);
        const bool ready = processingReady_.load(std::memory_order_acquire);
        PluginConfig configCopy = config_;
        lock.unlock();

        if (!shouldActivate || !ready) {
            stopSession();
            continue;
        }

        stopSession();
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
        {"mode", copy.mode == ConnectionMode::Seed ? "seed" : "play"},
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
        sendMessage(message);
        // Only update lastSentStatus_ if message was actually sent
        lastSentStatus_ = status;
    }
    // If message allocation fails, we'll retry on next flush (status remains dirty)
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

tresult PLUGIN_API WebRTCProcessor::process(ProcessData& data) {
    if (data.symbolicSampleSize == kSample64) {
        if (data.numOutputs > 0 && data.outputs[0].channelBuffers64 != nullptr) {
            const int channels = data.outputs[0].numChannels;
            for (int ch = 0; ch < channels; ++ch) {
                std::fill_n(data.outputs[0].channelBuffers64[ch], data.numSamples, 0.0);
            }
        }
        flushPendingStatus();
        return kResultOk; // 64-bit audio not handled in this prototype yet
    }

    if (!processingReady_.load(std::memory_order_acquire)) {
        processingReady_.store(true, std::memory_order_release);
        if (hostActive_.load(std::memory_order_acquire) && !sessionActive_.load(std::memory_order_acquire)) {
            requestConfigApply();
        }
    }

    if (auto* paramChanges = data.inputParameterChanges) {
        const int32 numParams = paramChanges->getParameterCount();
        for (int32 i = 0; i < numParams; ++i) {
            if (auto* queue = paramChanges->getParameterData(i)) {
                Steinberg::int32 index = queue->getPointCount() - 1;
                Steinberg::int32 sampleOffset = 0;
                Steinberg::Vst::ParamValue value = 0.0;
                if (queue->getPoint(index, sampleOffset, value) == kResultTrue) {
                    applyParameterChange(queue->getParameterId(), value);
                }
            }
        }
    }

    if (configDirty_.exchange(false, std::memory_order_acq_rel)) {
        requestConfigApply();
    }
    if (controllerSyncPending_.exchange(false, std::memory_order_acq_rel)) {
        syncConfigToController();
    }

    const int32 numSamples = data.numSamples;
    if (numSamples <= 0) {
        flushPendingStatus();
        return kResultOk;
    }

    const bool hasInput = data.numInputs > 0 && data.inputs[0].channelBuffers32 != nullptr;
    const bool hasOutput = data.numOutputs > 0 && data.outputs[0].channelBuffers32 != nullptr;

    const int inputChannels = hasInput ? data.inputs[0].numChannels : 0;
    const int outputChannels = hasOutput ? data.outputs[0].numChannels : 0;

    if (hasInput && (inputChannels < 1 || inputChannels > 2)) {
        SMTG_DBPRT1("[WebRTC] Warning: Unexpected input channel count %d\n", inputChannels);
    }
    if (hasOutput && (outputChannels < 1 || outputChannels > 2)) {
        SMTG_DBPRT1("[WebRTC] Warning: Unexpected output channel count %d\n", outputChannels);
    }

    const auto mode = modeAtomic_.load(std::memory_order_acquire);
    const bool sessionRunning = sessionActive_.load(std::memory_order_acquire);
    const int modeValue = (mode == ConnectionMode::Seed) ? 1 : 0;
    const int previousMode = lastLoggedMode_.exchange(modeValue, std::memory_order_acq_rel);
    if (previousMode != modeValue && shouldLogToStdout()) {
        std::cout << "[WebRTC] Process mode now "
                  << ((mode == ConnectionMode::Seed) ? "Seed" : "Play")
                  << " sessionRunning=" << (sessionRunning ? "true" : "false")
                  << std::endl;
    }

    if (mode == ConnectionMode::Seed && !loggedSeedProcessState_.exchange(true, std::memory_order_acq_rel)) {
        if (shouldLogToStdout()) {
            std::cout << "[WebRTC] Seed process path active"
                      << " hasInput=" << (hasInput ? "true" : "false")
                      << " inputChannels=" << inputChannels
                      << " sessionRunning=" << (sessionRunning ? "true" : "false")
                      << std::endl;
        }
    }
    if (mode == ConnectionMode::Play && !loggedPlayProcessState_.exchange(true, std::memory_order_acq_rel)) {
        if (shouldLogToStdout()) {
            std::cout << "[WebRTC] Play process path active"
                      << " hasOutput=" << (hasOutput ? "true" : "false")
                      << " outputChannels=" << outputChannels
                      << " sessionRunning=" << (sessionRunning ? "true" : "false")
                      << std::endl;
        }
    }

    if (mode == ConnectionMode::Seed && hasInput) {
        std::vector<const float*> inPtrs(inputChannels);
        for (int ch = 0; ch < inputChannels; ++ch) {
            inPtrs[ch] = data.inputs[0].channelBuffers32[ch];
        }
        if (sessionRunning) {
            if (!loggedSeedPushAttempt_.exchange(true, std::memory_order_acq_rel) && shouldLogToStdout()) {
                std::cout << "[WebRTC] Seed path calling pushOutgoingAudio for first time" << std::endl;
            }
            session_.pushOutgoingAudio(inPtrs.data(), static_cast<size_t>(numSamples), inputChannels);
        }

        if (hasOutput) {
            for (int ch = 0; ch < std::min(inputChannels, outputChannels); ++ch) {
                std::copy_n(data.inputs[0].channelBuffers32[ch], numSamples, data.outputs[0].channelBuffers32[ch]);
            }
            for (int ch = inputChannels; ch < outputChannels; ++ch) {
                std::fill_n(data.outputs[0].channelBuffers32[ch], numSamples, 0.0f);
            }
        }
    }

    if (mode == ConnectionMode::Play && hasOutput) {
        if (sessionRunning) {
            std::vector<float*> outPtrs(outputChannels);
            for (int ch = 0; ch < outputChannels; ++ch) {
                outPtrs[ch] = data.outputs[0].channelBuffers32[ch];
            }
            session_.pullIncomingAudio(outPtrs.data(), static_cast<size_t>(numSamples), outputChannels);
        } else {
            for (int ch = 0; ch < outputChannels; ++ch) {
                std::fill_n(data.outputs[0].channelBuffers32[ch], numSamples, 0.0f);
            }
        }
    }

    if (!hasOutput) {
        flushPendingStatus();
        return kResultOk;
    }

    if (mode != ConnectionMode::Play) {
        // ensure outputs beyond mirrored input are zeroed
        for (int ch = 0; ch < outputChannels; ++ch) {
            if (!hasInput || ch >= inputChannels) {
                std::fill_n(data.outputs[0].channelBuffers32[ch], numSamples, 0.0f);
            }
        }
    }

    flushPendingStatus();
    return kResultOk;
}

tresult PLUGIN_API WebRTCProcessor::setState(IBStream* state) {
    if (!state) {
        return kInvalidArgument;
    }

    std::string buffer;
    buffer.resize(4096);
    Steinberg::int32 bytesRead = 0;
    std::string serialized;

    do {
        const auto status = state->read(buffer.data(), static_cast<int32>(buffer.size()), &bytesRead);
        if (bytesRead > 0) {
            serialized.append(buffer.data(), static_cast<size_t>(bytesRead));
        }
        if (status != kResultTrue) {
            break;
        }
    } while (bytesRead > 0);

    if (serialized.empty()) {
        return kResultOk;
    }

    try {
        const auto json = nlohmann::json::parse(serialized);
        std::lock_guard<std::mutex> lock(configMutex_);
        if (auto it = json.find("streamId"); it != json.end()) {
            config_.streamId = it->get<std::string>();
        }
        if (auto it = json.contains("roomName") ? json.find("roomName") : json.find("roomId"); it != json.end()) {
            config_.roomName = it->get<std::string>();
        }
        if (auto it = json.contains("handshakeUrl") ? json.find("handshakeUrl") : json.find("signalingUrl"); it != json.end()) {
            config_.handshakeUrl = it->get<std::string>();
        }
        if (auto it = json.find("mode"); it != json.end()) {
            const auto mode = it->get<std::string>();
            config_.mode = (mode == "seed" ? ConnectionMode::Seed : ConnectionMode::Play);
            modeAtomic_.store(config_.mode, std::memory_order_release);
        }
        if (auto it = json.find("password"); it != json.end()) {
            config_.password = it->get<std::string>();
        }
        config_.disableEncryption = passwordImpliesDisableEncryption(config_.password);
    } catch (...) {
        // ignore malformed state chunks
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
    state->write(mutableData, static_cast<int32>(serialized.size()), &written);
    return kResultOk;
}


Steinberg::tresult PLUGIN_API WebRTCProcessor::getControllerClassId(Steinberg::TUID classId) {
    if (!classId) {
        return Steinberg::kInvalidArgument;
    }

    kWebRTCControllerUID.toTUID(classId);
    return Steinberg::kResultTrue;
}

} // namespace webrtc_vst








