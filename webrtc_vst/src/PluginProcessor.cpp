#include "PluginProcessor.h"

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


WebRTCProcessor::WebRTCProcessor()
    : receiveBuffer_(kDefaultBufferFrames, 2),
      session_(receiveBuffer_, [this](const std::string& line) {
          SMTG_DBPRT1("[WebRTC] %s\n", line.c_str());
      }) {
    config_.streamId = "vst-stream";
    config_.handshakeUrl = "wss://wss0.vdo.ninja";
    config_.mode = ConnectionMode::Play;
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
    configDirty_ = true;
    return kResultOk;
}

tresult PLUGIN_API WebRTCProcessor::terminate() {
    stopSession();
    return AudioEffect::terminate();
}

tresult PLUGIN_API WebRTCProcessor::setupProcessing(ProcessSetup& setup) {
    processSetup_ = setup;
    return AudioEffect::setupProcessing(setup);
}

void WebRTCProcessor::updateConfigFromEnvironment() {
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

    bool autoDisable = passwordImpliesDisableEncryption(config_.password);
    bool passwordBlank = trimCopy(config_.password).empty();
    if (const char* password = std::getenv("WEBRTC_VST_PASSWORD")) {
        config_.password = password;
        autoDisable = passwordImpliesDisableEncryption(config_.password);
        passwordBlank = trimCopy(config_.password).empty();
    }

    bool disableExplicit = false;
    if (const char* disableEnc = std::getenv("WEBRTC_VST_DISABLE_ENCRYPTION")) {
        std::string value(disableEnc);
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        config_.disableEncryption = (value == "1" || value == "true" || value == "yes");
        disableExplicit = true;
    }

    if (autoDisable) {
        config_.disableEncryption = true;
    } else if (!disableExplicit && passwordBlank) {
        config_.disableEncryption = false;
    }

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
        config_.streamId = "vst-stream";
    }

    if (config_.handshakeUrl.empty()) {
        config_.handshakeUrl = "wss://wss0.vdo.ninja";
    }
}


void WebRTCProcessor::startSession() {
    if (sessionActive_) {
        return;
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
    session_.start(config_, sampleRate, channels);
    sessionActive_ = true;
    configDirty_ = false;
}

void WebRTCProcessor::stopSession() {
    if (!sessionActive_) {
        return;
    }
    session_.stop();
    sessionActive_ = false;
}

void WebRTCProcessor::applyParameterChange(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) {
    switch (id) {
        case kParamMode:
            config_.mode = (value >= 0.5) ? ConnectionMode::Seed : ConnectionMode::Play;
            configDirty_ = true;
            break;
        case kParamDisableEncryption:
            config_.disableEncryption = value >= 0.5;
            configDirty_ = true;
            break;
        case kParamStreamId: {
            const auto idValue = normalizedToId(value);
            const auto text = ParameterStringRegistry::instance().lookup(idValue);
            if (!text.empty()) {
                config_.streamId = text;
                configDirty_ = true;
            }
            break;
        }
        case kParamRoomName: {
            const auto idValue = normalizedToId(value);
            config_.roomName = ParameterStringRegistry::instance().lookup(idValue);
            configDirty_ = true;
            break;
        }
        case kParamHandshakeUrl: {
            const auto idValue = normalizedToId(value);
            const auto url = ParameterStringRegistry::instance().lookup(idValue);
            if (!url.empty()) {
                config_.handshakeUrl = url;
                configDirty_ = true;
            }
            break;
        }
        case kParamPassword: {
            const auto idValue = normalizedToId(value);
            config_.password = ParameterStringRegistry::instance().lookup(idValue);
            const bool autoDisable = passwordImpliesDisableEncryption(config_.password);
            if (autoDisable && !config_.disableEncryption) {
                config_.disableEncryption = true;
            } else if (!autoDisable && config_.disableEncryption) {
                config_.disableEncryption = false;
            }
            configDirty_ = true;
            break;
        }
        default:
            break;
    }
}

void WebRTCProcessor::restartSessionIfNeeded() {
    if (!configDirty_) {
        return;
    }

    configDirty_ = false;

    if (!sessionActive_) {
        startSession();
        return;
    }

    stopSession();
    startSession();
}

tresult PLUGIN_API WebRTCProcessor::setActive(TBool state) {
    if (state) {
        configDirty_ = true;
    } else {
        stopSession();
    }
    return AudioEffect::setActive(state);
}

tresult PLUGIN_API WebRTCProcessor::process(ProcessData& data) {
    if (data.symbolicSampleSize == kSample64) {
        return kResultOk; // 64-bit audio not handled in this prototype yet
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

    restartSessionIfNeeded();

    const int32 numSamples = data.numSamples;
    if (numSamples <= 0) {
        return kResultOk;
    }

    const bool hasInput = data.numInputs > 0 && data.inputs[0].channelBuffers32 != nullptr;
    const bool hasOutput = data.numOutputs > 0 && data.outputs[0].channelBuffers32 != nullptr;

    const int inputChannels = hasInput ? data.inputs[0].numChannels : 0;
    const int outputChannels = hasOutput ? data.outputs[0].numChannels : 0;

    if (config_.mode == ConnectionMode::Seed && hasInput) {
        std::vector<const float*> inPtrs(inputChannels);
        for (int ch = 0; ch < inputChannels; ++ch) {
            inPtrs[ch] = data.inputs[0].channelBuffers32[ch];
        }
        session_.pushOutgoingAudio(inPtrs.data(), static_cast<size_t>(numSamples), inputChannels);

        if (hasOutput) {
            for (int ch = 0; ch < std::min(inputChannels, outputChannels); ++ch) {
                std::copy_n(data.inputs[0].channelBuffers32[ch], numSamples, data.outputs[0].channelBuffers32[ch]);
            }
            for (int ch = inputChannels; ch < outputChannels; ++ch) {
                std::fill_n(data.outputs[0].channelBuffers32[ch], numSamples, 0.0f);
            }
        }
    }

    if (config_.mode == ConnectionMode::Play && hasOutput) {
        std::vector<float*> outPtrs(outputChannels);
        for (int ch = 0; ch < outputChannels; ++ch) {
            outPtrs[ch] = data.outputs[0].channelBuffers32[ch];
        }
        session_.pullIncomingAudio(outPtrs.data(), static_cast<size_t>(numSamples), outputChannels);
    }

    if (!hasOutput) {
        return kResultOk;
    }

    if (config_.mode != ConnectionMode::Play) {
        // ensure outputs beyond mirrored input are zeroed
        for (int ch = 0; ch < outputChannels; ++ch) {
            if (!hasInput || ch >= inputChannels) {
                std::fill_n(data.outputs[0].channelBuffers32[ch], numSamples, 0.0f);
            }
        }
    }

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
        }
        if (auto it = json.find("password"); it != json.end()) {
            config_.password = it->get<std::string>();
        }
        if (auto it = json.find("disableEncryption"); it != json.end()) {
            config_.disableEncryption = it->get<bool>();
        } else {
            config_.disableEncryption = false;
        }

        if (passwordImpliesDisableEncryption(config_.password)) {
            config_.disableEncryption = true;
        } else if (trimCopy(config_.password).empty() && !json.contains("disableEncryption")) {
            config_.disableEncryption = false;
        }
    } catch (...) {
        // ignore malformed state chunks
    }

    configDirty_ = true;
    restartSessionIfNeeded();

    return kResultOk;
}


tresult PLUGIN_API WebRTCProcessor::getState(IBStream* state) {
    if (!state) {
        return kInvalidArgument;
    }

    nlohmann::json json = {
        {"streamId", config_.streamId},
        {"roomName", config_.roomName},
        {"handshakeUrl", config_.handshakeUrl},
        {"mode", config_.mode == ConnectionMode::Seed ? "seed" : "play"},
        {"password", config_.password},
        {"disableEncryption", config_.disableEncryption}
    };

    json["roomId"] = config_.roomName;
    json["signalingUrl"] = config_.handshakeUrl;

    const auto serialized = json.dump();
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






