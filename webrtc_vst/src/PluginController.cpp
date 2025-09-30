#include "PluginController.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include <base/source/fstring.h>
#include <pluginterfaces/base/ibstream.h>

namespace webrtc_vst {

namespace {
constexpr auto kDefaultStreamId = "vst-stream";
constexpr auto kDefaultSignalingUrl = "wss://wss0.vdo.ninja";

std::string toAscii(const Steinberg::Vst::TChar* text) {
    if (!text) {
        return {};
    }
    std::string result;
    while (*text) {
        auto code = static_cast<uint32_t>(*text++);
        result.push_back(code <= 0x7F ? static_cast<char>(code) : '?');
    }
    return result;
}

void copyAsciiToTChar(const std::string& text, Steinberg::Vst::String128 dest) {
    size_t count = std::min<size_t>(text.size(), 127);
    for (size_t i = 0; i < count; ++i) {
        dest[i] = static_cast<Steinberg::Vst::TChar>(text[i]);
    }
    dest[count] = 0;
    for (size_t i = count + 1; i < 128; ++i) {
        dest[i] = 0;
    }
}
} // namespace

const Steinberg::FUID kWebRTCControllerUID(0x2A1D4B56, 0x5E7341FA, 0x8C00C53F, 0x1D0B6572);

StringParameter::StringParameter(const Steinberg::char16* title,
                                 Steinberg::Vst::ParamID tag)
    : Steinberg::Vst::Parameter(title, tag) {}

void StringParameter::setString(const std::string& value) {
    uint32_t id = ParameterStringRegistry::instance().registerValue(value);
    setNormalized(idToNormalized(id));
}

void StringParameter::setDefaultString(const std::string& value) {
    const uint32_t id = ParameterStringRegistry::instance().registerValue(value);
    const auto normalized = idToNormalized(id);
    getInfo().defaultNormalizedValue = normalized;
    setNormalized(normalized);
}

std::string StringParameter::getString() const {
    return ParameterStringRegistry::instance().lookup(normalizedToId(getNormalized()));
}

void StringParameter::toString(Steinberg::Vst::ParamValue normValue, Steinberg::Vst::String128 string) const {
    auto text = ParameterStringRegistry::instance().lookup(normalizedToId(normValue));
    copyAsciiToTChar(text, string);
}

bool StringParameter::fromString(const Steinberg::Vst::TChar* text, Steinberg::Vst::ParamValue& normValue) const {
    std::string ascii = toAscii(text);
    uint32_t id = ParameterStringRegistry::instance().registerValue(ascii);
    normValue = idToNormalized(id);
    return true;
}

WebRTCController::WebRTCController() = default;

Steinberg::FUnknown* WebRTCController::createInstance(void* /*context*/) {
    return static_cast<Steinberg::Vst::IEditController*>(new WebRTCController());
}

Steinberg::tresult PLUGIN_API WebRTCController::initialize(Steinberg::FUnknown* context) {
    const auto result = EditControllerEx1::initialize(context);
    if (result != Steinberg::kResultOk) {
        return result;
    }

    auto* mode = new Steinberg::Vst::StringListParameter(STR16("Mode"), kParamMode);
    mode->appendString(STR16("Play"));
    mode->appendString(STR16("Seed"));
    mode->setNormalized(0.0);
    parameters.addParameter(mode);

    auto* streamIdParam = new StringParameter(STR16("Stream ID"), kParamStreamId);
    parameters.addParameter(streamIdParam);
    streamIdParam->setDefaultString(kDefaultStreamId);

    auto* roomIdParam = new StringParameter(STR16("Room ID"), kParamRoomId);
    parameters.addParameter(roomIdParam);
    roomIdParam->setDefaultString("");

    auto* signalingUrlParam = new StringParameter(STR16("Signaling URL"), kParamSignalingUrl);
    parameters.addParameter(signalingUrlParam);
    signalingUrlParam->setDefaultString(kDefaultSignalingUrl);

    auto* passwordParam = new StringParameter(STR16("Password"), kParamPassword);
    parameters.addParameter(passwordParam);
    passwordParam->setDefaultString("");

    auto* disableEnc = new Steinberg::Vst::RangeParameter(STR16("Disable Encryption"),
                                                          kParamDisableEncryption,
                                                          STR16(""),
                                                          0.0,
                                                          1.0,
                                                          0.0);
    disableEnc->setPrecision(0);
    parameters.addParameter(disableEnc);

    return Steinberg::kResultOk;
}

void WebRTCController::applyStateJson(const std::string& jsonString) {
    if (jsonString.empty()) {
        return;
    }

    try {
        const auto json = nlohmann::json::parse(jsonString);

        if (json.contains("streamId")) {
            if (auto* param = findStringParameter(kParamStreamId)) {
                param->setString(json["streamId"].get<std::string>());
            }
        }
        if (json.contains("roomId")) {
            if (auto* param = findStringParameter(kParamRoomId)) {
                param->setString(json["roomId"].get<std::string>());
            }
        }
        if (json.contains("signalingUrl")) {
            if (auto* param = findStringParameter(kParamSignalingUrl)) {
                param->setString(json["signalingUrl"].get<std::string>());
            }
        }
        if (json.contains("password")) {
            if (auto* param = findStringParameter(kParamPassword)) {
                param->setString(json["password"].get<std::string>());
            }
        }
        if (json.contains("disableEncryption")) {
            if (auto* param = parameters.getParameter(kParamDisableEncryption)) {
                param->setNormalized(json["disableEncryption"].get<bool>() ? 1.0 : 0.0);
            }
        }
        if (json.contains("mode")) {
            if (auto* param = parameters.getParameter(kParamMode)) {
                const auto mode = json["mode"].get<std::string>();
                param->setNormalized((mode == "seed") ? 1.0 : 0.0);
            }
        }
    } catch (...) {
        // ignore malformed state
    }
}

std::string WebRTCController::exportStateJson() const {
    auto* modeParam = parameters.getParameter(kParamMode);
    const bool isSeed = modeParam && modeParam->getNormalized() >= 0.5;

    auto* encParam = parameters.getParameter(kParamDisableEncryption);
    const bool disableEncryption = encParam && encParam->getNormalized() >= 0.5;

    const auto streamId = [this]() {
        if (auto* param = findStringParameter(kParamStreamId)) {
            return param->getString();
        }
        return std::string{};
    }();

    const auto roomId = [this]() {
        if (auto* param = findStringParameter(kParamRoomId)) {
            return param->getString();
        }
        return std::string{};
    }();

    const auto signalingUrl = [this]() {
        if (auto* param = findStringParameter(kParamSignalingUrl)) {
            return param->getString();
        }
        return std::string{};
    }();

    const auto password = [this]() {
        if (auto* param = findStringParameter(kParamPassword)) {
            return param->getString();
        }
        return std::string{};
    }();

    nlohmann::json json = {
        {"streamId", streamId},
        {"roomId", roomId},
        {"signalingUrl", signalingUrl},
        {"password", password},
        {"mode", isSeed ? "seed" : "play"},
        {"disableEncryption", disableEncryption}
    };

    return json.dump();
}

StringParameter* WebRTCController::findStringParameter(Steinberg::Vst::ParamID id) const {
    return dynamic_cast<StringParameter*>(parameters.getParameter(id));
}

Steinberg::tresult PLUGIN_API WebRTCController::setComponentState(Steinberg::IBStream* state) {
    if (!state) {
        return Steinberg::kInvalidArgument;
    }

    std::string buffer;
    buffer.resize(4096);
    Steinberg::int32 bytesRead = 0;
    std::string serialized;

    do {
        const auto status = state->read(buffer.data(), static_cast<Steinberg::int32>(buffer.size()), &bytesRead);
        if (bytesRead > 0) {
            serialized.append(buffer.data(), static_cast<size_t>(bytesRead));
        }
        if (status != Steinberg::kResultTrue) {
            break;
        }
    } while (bytesRead > 0);

    applyStateJson(serialized);
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API WebRTCController::setState(Steinberg::IBStream* state) {
    return setComponentState(state);
}

Steinberg::tresult PLUGIN_API WebRTCController::getState(Steinberg::IBStream* state) {
    if (!state) {
        return Steinberg::kInvalidArgument;
    }

    const auto serialized = exportStateJson();
    Steinberg::int32 written = 0;
    auto* mutableData = const_cast<char*>(serialized.data());
    state->write(mutableData, static_cast<Steinberg::int32>(serialized.size()), &written);
    return Steinberg::kResultOk;
}

} // namespace webrtc_vst
