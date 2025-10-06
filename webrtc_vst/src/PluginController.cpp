#include "PluginController.h"
#include "StreamIdGenerator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

#include <base/source/fstring.h>
#include <pluginterfaces/base/ibstream.h>
#include <vstgui/plugin-bindings/vst3editor.h>
#include <vstgui/lib/platform/platformfactory.h>
#if SMTG_OS_WINDOWS
#include <vstgui/lib/platform/win32/win32factory.h>
#endif

namespace webrtc_vst {

namespace {

constexpr auto kDefaultHandshakeUrl = "wss://wss0.vdo.ninja";

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

    auto* mode = new Steinberg::Vst::StringListParameter(STR16("Connection Mode"), kParamMode);
    mode->appendString(STR16("Play"));
    mode->appendString(STR16("Seed"));
    mode->setNormalized(0.0);
    parameters.addParameter(mode);

    auto* streamIdParam = new StringParameter(STR16("Stream ID"), kParamStreamId);
    parameters.addParameter(streamIdParam);
    const auto initialStreamId = generateRandomStreamId();
    streamIdParam->setDefaultString(initialStreamId);
    streamIdParam->setString(initialStreamId);

    auto* roomNameParam = new StringParameter(STR16("Room Name"), kParamRoomName);
    parameters.addParameter(roomNameParam);
    roomNameParam->setDefaultString("");

    auto* handshakeUrlParam = new StringParameter(STR16("Handshake URL"), kParamHandshakeUrl);
    parameters.addParameter(handshakeUrlParam);
    handshakeUrlParam->setDefaultString(kDefaultHandshakeUrl);

    auto* statusParam = new StringParameter(STR16("Status"), kParamStatus);
    auto& statusInfo = const_cast<Steinberg::Vst::ParameterInfo&>(statusParam->getInfo());
    statusInfo.flags |= Steinberg::Vst::ParameterInfo::kIsReadOnly;
    statusInfo.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;
    parameters.addParameter(statusParam);
    statusParam->setDefaultString("Idle");

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
    auto& disableInfo = const_cast<Steinberg::Vst::ParameterInfo&>(disableEnc->getInfo());
    disableInfo.flags |= Steinberg::Vst::ParameterInfo::kIsReadOnly;
    disableInfo.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;

    return Steinberg::kResultOk;
}

void WebRTCController::applyStateJson(const std::string& jsonString) {
    if (jsonString.empty()) {
        return;
    }

    try {
        const auto json = nlohmann::json::parse(jsonString);

        if (auto it = json.find("streamId"); it != json.end()) {
            const auto streamId = it->get<std::string>();
            if (auto* param = findStringParameter(kParamStreamId)) {
                param->setDefaultString(streamId);
                param->setString(streamId);
            }
        }

        if (auto it = json.contains("roomName") ? json.find("roomName") : json.find("roomId"); it != json.end()) {
            if (auto* param = findStringParameter(kParamRoomName)) {
                param->setString(it->get<std::string>());
            }
        }

        if (auto it = json.contains("handshakeUrl") ? json.find("handshakeUrl") : json.find("signalingUrl"); it != json.end()) {
            if (auto* param = findStringParameter(kParamHandshakeUrl)) {
                param->setString(it->get<std::string>());
            }
        }

        if (auto it = json.find("password"); it != json.end()) {
            if (auto* param = findStringParameter(kParamPassword)) {
                param->setString(it->get<std::string>());
            }
        }

        if (auto it = json.find("mode"); it != json.end()) {
            if (auto* param = parameters.getParameter(kParamMode)) {
                const auto mode = it->get<std::string>();
                param->setNormalized((mode == "seed") ? 1.0 : 0.0);
            }
        }

        updateDisableEncryptionFromPassword();
    } catch (...) {
        // ignore malformed state
    }
}


std::string WebRTCController::exportStateJson() const {
    auto* modeParam = parameters.getParameter(kParamMode);
    const bool isSeed = modeParam && modeParam->getNormalized() >= 0.5;

    const auto streamId = [this]() {
        if (auto* param = findStringParameter(kParamStreamId)) {
            return param->getString();
        }
        return std::string{};
    }();

    const auto roomName = [this]() {
        if (auto* param = findStringParameter(kParamRoomName)) {
            return param->getString();
        }
        return std::string{};
    }();

    const auto handshakeUrl = [this]() {
        if (auto* param = findStringParameter(kParamHandshakeUrl)) {
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

    const bool disableEncryption = passwordImpliesDisableEncryption(password);

    nlohmann::json json = {
        {"streamId", streamId},
        {"roomName", roomName},
        {"handshakeUrl", handshakeUrl},
        {"password", password},
        {"mode", isSeed ? "seed" : "play"},
        {"disableEncryption", disableEncryption}
    };

    json["roomId"] = roomName;
    json["signalingUrl"] = handshakeUrl;

    return json.dump();
}


StringParameter* WebRTCController::findStringParameter(Steinberg::Vst::ParamID id) const {
    return dynamic_cast<StringParameter*>(parameters.getParameter(id));
}

void WebRTCController::updateDisableEncryptionFromPassword() {
    if (suppressDisableEdit_) {
        return;
    }

    auto* passwordParam = findStringParameter(kParamPassword);
    auto* disableParam = parameters.getParameter(kParamDisableEncryption);
    if (!passwordParam || !disableParam) {
        return;
    }

    const bool shouldDisable = passwordImpliesDisableEncryption(passwordParam->getString());
    const double normalizedValue = shouldDisable ? 1.0 : 0.0;
    if (disableParam->getNormalized() == normalizedValue) {
        suppressDisableEdit_ = false;
        return;
    }

    suppressDisableEdit_ = true;
    disableParam->setNormalized(normalizedValue);
    if (componentHandler) {
        componentHandler->beginEdit(kParamDisableEncryption);
        componentHandler->performEdit(kParamDisableEncryption, normalizedValue);
        componentHandler->endEdit(kParamDisableEncryption);
    } else {
        suppressDisableEdit_ = false;
    }
}

Steinberg::tresult PLUGIN_API WebRTCController::setParamNormalized(Steinberg::Vst::ParamID tag,
                                                                   Steinberg::Vst::ParamValue value) {
    const auto result = EditControllerEx1::setParamNormalized(tag, value);
    if (result != Steinberg::kResultOk) {
        return result;
    }

    if (tag == kParamDisableEncryption) {
        if (suppressDisableEdit_) {
            suppressDisableEdit_ = false;
        } else {
            updateDisableEncryptionFromPassword();
        }
        return result;
    }

    if (tag == kParamPassword) {
        updateDisableEncryptionFromPassword();
    }

    return result;
}

Steinberg::IPlugView* PLUGIN_API WebRTCController::createView(const char* name) {
    Steinberg::ConstString viewName(name);
    if (viewName == Steinberg::Vst::ViewType::kEditor) {
#if SMTG_OS_WINDOWS
        if (auto* factory = VSTGUI::getPlatformFactory().asWin32Factory()) {
            factory->disableDirectComposition();
        }
#endif
        return new VSTGUI::VST3Editor(this, "view", "webrtc_vst.uidesc");
    }
    return nullptr;
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


Steinberg::tresult PLUGIN_API WebRTCController::notify(Steinberg::Vst::IMessage* message) {
    if (!message) {
        return EditControllerEx1::notify(message);
    }

    if (std::strcmp(message->getMessageID(), "ConfigSync") == 0) {
        const void* data = nullptr;
        Steinberg::uint32 size = 0;
        if (auto* attributes = message->getAttributes();
            attributes && attributes->getBinary("config", data, size) == Steinberg::kResultTrue && data && size > 0) {
            const char* charData = static_cast<const char*>(data);
            std::string serialized(charData, (size > 0 && charData[size - 1] == '\0') ? size - 1 : size);
            applyStateJson(serialized);
        }
        return Steinberg::kResultOk;
    }
    if (std::strcmp(message->getMessageID(), "StatusUpdate") == 0) {
        const void* data = nullptr;
        Steinberg::uint32 size = 0;
        if (auto* attributes = message->getAttributes(); attributes && attributes->getBinary("status", data, size) == Steinberg::kResultTrue && data && size > 0) {
            const char* charData = static_cast<const char*>(data);
            std::string status(charData, (size > 0 && charData[size - 1] == '\0') ? size - 1 : size);
            if (auto* statusParam = findStringParameter(kParamStatus)) {
                statusParam->setString(status);
                const auto normalized = statusParam->getNormalized();
                if (componentHandler) {
                    componentHandler->beginEdit(kParamStatus);
                    componentHandler->performEdit(kParamStatus, normalized);
                    componentHandler->endEdit(kParamStatus);
                }
            }
        }
        return Steinberg::kResultOk;
    }

    return EditControllerEx1::notify(message);
}

} // namespace webrtc_vst

