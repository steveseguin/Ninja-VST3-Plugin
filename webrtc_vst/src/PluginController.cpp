#include "PluginController.h"
#include "qrcodegen.hpp"
#include "StreamIdGenerator.h"
#include "ConfigState.h"
#include <cmath>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <base/source/fstring.h>
#include <pluginterfaces/base/ibstream.h>
#include <vstgui/lib/cclipboard.h>
#include <vstgui/lib/controls/ccontrol.h>
#include <vstgui/plugin-bindings/vst3editor.h>
#include <vstgui/lib/platform/platformfactory.h>
#if SMTG_OS_WINDOWS
#include <vstgui/lib/platform/win32/win32factory.h>
#include <windows.h>
#include <shellapi.h>
#endif
#if SMTG_OS_MACOS
#include "MacPlatform.h"
#endif

namespace webrtc_vst {

namespace {

class WebRTCEditor final : public VSTGUI::VST3Editor {
public:
    using VST3Editor::VST3Editor;
    static bool isExternalAction(const VSTGUI::CControl* control) {
        return control && (control->getTag() == kParamCopyPushLink || control->getTag() == kParamShowPushQr);
    }
    void valueChanged(VSTGUI::CControl* control) override {
        if (!isExternalAction(control)) { VST3Editor::valueChanged(control); return; }
        // Parameter listeners also invoke valueChanged during host updates.
        // Only an actual editing gesture may touch clipboard / launch a URL.
        if (control->isEditing() && control->getValueNormalized() >= 0.5f) {
            static_cast<WebRTCController*>(getController())->performUserAction(control->getTag());
            control->setValueNormalized(0.f);
            control->invalid();
        }
    }
    void controlBeginEdit(VSTGUI::CControl* control) override {
        if (!isExternalAction(control)) VST3Editor::controlBeginEdit(control);
    }
    void controlEndEdit(VSTGUI::CControl* control) override {
        if (!isExternalAction(control)) VST3Editor::controlEndEdit(control);
    }
    VSTGUI::CView* verifyView(VSTGUI::CView* view, const VSTGUI::UIAttributes& attributes,
                            const VSTGUI::IUIDescription* description) override {
        if (auto* control = dynamic_cast<VSTGUI::CControl*>(view); control && control->getTag() == 1000) {
            // UIViewSwitchContainer observes this local control directly. Binding
            // an unregistered tag to VST3Editor causes recursive valueChanged().
            control->setListener(nullptr);
        }
        return VST3Editor::verifyView(view, attributes, description);
    }
};

std::string toUtf8(const Steinberg::Vst::TChar* text) {
    if (!text) return {};
    Steinberg::String converted(text);
    if (!converted.toMultiByte(Steinberg::kCP_Utf8)) return {};
    return converted.text8();
}

void copyUtf8ToTChar(const std::string& text, Steinberg::Vst::String128 dest) {
    std::fill_n(dest, 128, 0);
    Steinberg::String converted(text.c_str(), Steinberg::kCP_Utf8);
    converted.copyTo16(dest, 0, 127);
    dest[127] = 0;
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

std::string urlEncode(const std::string& text) {
    auto isUnreserved = [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
    };

    std::string encoded;
    encoded.reserve(text.size() * 3);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char ch : text) {
        if (isUnreserved(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4) & 0x0F]);
        encoded.push_back(kHex[ch & 0x0F]);
    }
    return encoded;
}

std::string encodeURIComponentCompat(const std::string& text) {
    auto isUnescaped = [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '!' ||
               ch == '~' || ch == '*' || ch == '\'' || ch == '(' || ch == ')';
    };

    std::string encoded;
    encoded.reserve(text.size() * 3);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char ch : text) {
        if (isUnescaped(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4) & 0x0F]);
        encoded.push_back(kHex[ch & 0x0F]);
    }
    return encoded;
}

std::string buildPasswordHash(const std::string& password, const std::string& salt) {
    const auto trimmedPassword = trimCopy(password);
    if (trimmedPassword.empty() || passwordImpliesDisableEncryption(trimmedPassword)) {
        return {};
    }

    const auto payload = encodeURIComponentCompat(trimmedPassword) + salt;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest.data());

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hash;
    hash.reserve(4);
    for (int i = 0; i < 2; ++i) {
        hash.push_back(kHex[(digest[i] >> 4) & 0x0F]);
        hash.push_back(kHex[digest[i] & 0x0F]);
    }
    return hash;
}

std::string buildVdoLink(bool push,
                         const std::string& streamId,
                         const std::string& roomName,
                         const std::string& password,
                         const std::string& webBaseUrl,
                         const std::string& saltOverride,
                         const std::string& handshakeUrl) {
    const auto trimmedStream = trimCopy(streamId);
    if (trimmedStream.empty()) {
        return {};
    }

    std::ostringstream url;
    url << webBaseUrl << "?";
    if (push) {
        url << "push=" << urlEncode(trimmedStream);
    } else {
        url << "view=" << urlEncode(trimmedStream) << "&style=2";
    }

    const auto trimmedRoom = trimCopy(roomName);
    if (!trimmedRoom.empty()) {
        url << "&room=" << urlEncode(trimmedRoom);
        if (!push) {
            url << "&solo";
        }
    }

    const auto salt = effectiveSalt(saltOverride, webBaseUrl);
    const auto passwordHash = buildPasswordHash(password, salt);
    if (!passwordHash.empty()) {
        url << "&hash=" << passwordHash;
    }

    if (passwordImpliesDisableEncryption(password)) url << "&password=false";
    if (salt != saltForUrl(webBaseUrl)) url << "&salt=" << urlEncode(salt);
    // wss2 changes the endpoint while retaining the VDO.Ninja signaling
    // protocol. wss would switch the browser to its generic-relay protocol.
    // Carry the endpoint even if the custom web front end has other defaults.
    // ws:// is for CLI tests only.
    if (handshakeUrl.starts_with("ws://")) return {};
    if (handshakeUrl != kDefaultHandshakeUrl || webBaseUrl != kDefaultWebBaseUrl)
        url << "&wss2=" << urlEncode(handshakeUrl);

    return url.str();
}

std::string htmlEscape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&#39;";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

std::string buildQrSvg(const std::string& targetUrl) {
    const auto qr = qrcodegen::QrCode::encodeText(targetUrl.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int border = 4;
    const int dimension = qr.getSize() + border * 2;

    std::ostringstream modules;
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (qr.getModule(x, y)) {
                modules << "M" << (x + border) << "," << (y + border) << "h1v1h-1z ";
            }
        }
    }

    std::ostringstream svg;
    svg << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 "
        << dimension << " " << dimension
        << "' shape-rendering='crispEdges' aria-hidden='true'>"
        << "<rect width='100%' height='100%' fill='#ffffff'/>"
        << "<path d='" << modules.str() << "' fill='#000000'/>"
        << "</svg>";
    return svg.str();
}

std::string buildQrViewerHtml(const std::string& label, const std::string& svgMarkup) {
    std::ostringstream html;
    html << "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
         << "<meta name='viewport' content='width=device-width, initial-scale=1'>"
         << "<title>" << htmlEscape(label) << " QR</title>"
         << "<style>"
         << ":root{color-scheme:light;font-family:Segoe UI,Arial,sans-serif;}"
         << "body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0f1115;color:#f5f7fa;}"
         << ".card{width:min(92vw,520px);padding:24px;border-radius:20px;background:#1a1e26;"
         << "box-shadow:0 24px 80px rgba(0,0,0,.45);text-align:center;}"
         << ".qr{background:#fff;border-radius:16px;padding:20px;display:inline-block;line-height:0;}"
         << ".qr svg{width:min(70vw,360px);height:auto;display:block;}"
         << "h1{margin:0 0 10px;font-size:28px;}p{margin:10px 0 0;color:#c8d0da;line-height:1.5;}"
         << "</style></head><body><main class='card'><h1>" << htmlEscape(label)
         << "</h1><div class='qr'>" << svgMarkup
         << "</div><p>Generated locally by WebRTC VST. No third-party QR service is used.</p>"
         << "</main></body></html>";
    return html.str();
}

bool copyToClipboard(const std::string& text) {
    if (text.empty()) {
        return false;
    }
#if SMTG_OS_WINDOWS
    const int wideChars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wideChars <= 0) {
        return false;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wideChars) * sizeof(wchar_t));
    if (!memory) {
        return false;
    }

    auto* buffer = static_cast<wchar_t*>(GlobalLock(memory));
    if (!buffer) {
        GlobalFree(memory);
        return false;
    }

    if (MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer, wideChars) <= 0) {
        GlobalUnlock(memory);
        GlobalFree(memory);
        return false;
    }
    GlobalUnlock(memory);

    bool opened = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr)) {
            opened = true;
            break;
        }
        Sleep(20);
    }
    if (!opened) {
        GlobalFree(memory);
        return false;
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }

    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }

    CloseClipboard();
    return true;
#else
    return VSTGUI::CClipboard::setString(text.c_str());
#endif
}

#if SMTG_OS_WINDOWS
std::wstring asciiToWide(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (unsigned char ch : text) {
        wide.push_back(static_cast<wchar_t>(ch));
    }
    return wide;
}
#endif

bool openExternalUrl(const std::string& url) {
    if (url.empty()) {
        return false;
    }
#if SMTG_OS_WINDOWS
    const auto wideUrl = asciiToWide(url);
    const auto result = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr,
                                                                 L"open",
                                                                 wideUrl.c_str(),
                                                                 nullptr,
                                                                 nullptr,
                                                                 SW_SHOWNORMAL));
    return result > 32;
#elif SMTG_OS_MACOS
    return openMacExternalUrl(url);
#elif SMTG_OS_LINUX
    std::string escapedUrl;
    escapedUrl.reserve(url.size());
    for (char ch : url) {
        if (ch == '"' || ch == '\\') {
            escapedUrl.push_back('\\');
        }
        escapedUrl.push_back(ch);
    }
    const auto command = "xdg-open \"" + escapedUrl + "\" >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
#else
    return false;
#endif
}
} // namespace

const Steinberg::FUID kWebRTCControllerUID(0x2A1D4B56, 0x5E7341FA, 0x8C00C53F, 0x1D0B6572);

StringParameter::StringParameter(const Steinberg::char16* title,
                                 Steinberg::Vst::ParamID tag)
    : Steinberg::Vst::Parameter(title, tag) {
    getInfo().flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;
}

void StringParameter::setString(const std::string& value) {
    if (text_ == value) return;
    text_ = value;
    pendingText_.reset();
    Parameter::setNormalized(static_cast<double>((++serial_ & 0xffffff) + 1) / 16777216.0);
}

void StringParameter::setDefaultString(const std::string& value) {
    setString(value);
    getInfo().defaultNormalizedValue = getNormalized();
}

std::string StringParameter::getString() const {
    return text_;
}

bool StringParameter::setNormalized(Steinberg::Vst::ParamValue value) {
    if (!std::isfinite(value)) return false;
    if (pendingText_ && value == pendingValue_) {
        text_ = std::move(*pendingText_);
        pendingText_.reset();
        return Parameter::setNormalized(value);
    }
    return false;
}

void StringParameter::toString(Steinberg::Vst::ParamValue normValue, Steinberg::Vst::String128 string) const {
    copyUtf8ToTChar(pendingText_ && normValue == pendingValue_ ? *pendingText_ : text_, string);
}

bool StringParameter::fromString(const Steinberg::Vst::TChar* text, Steinberg::Vst::ParamValue& normValue) const {
    std::string ascii = toUtf8(text);
#if SMTG_OS_MACOS
    // VSTGUI decomposes Cocoa text into NFD, unlike ordinary browser password
    // entry. Compose UI edits back to NFC; preset/config bytes remain untouched.
    ascii = normalizeMacEditedText(ascii);
#endif
    try { validateSettingText(ascii); } catch (...) { return false; }
    if (getInfo().id == kParamWebBaseUrl || getInfo().id == kParamHandshakeUrl) {
        auto normalized = normalizeEndpoint(ascii, getInfo().id == kParamWebBaseUrl);
        if (!normalized || normalized->size() > 127 || normalized->starts_with("ws://")) return false;
        ascii = *normalized;
    }
    pendingText_ = std::move(ascii);
    // VSTGUI's text conversion passes through float. Use exactly representable
    // binary fractions or toString would discard the draft during that round trip.
    pendingValue_ = static_cast<double>((++serial_ & 0xffffff) + 1) / 16777216.0;
    normValue = pendingValue_;
    return true;
}

WebRTCController::WebRTCController() = default;

WebRTCController::~WebRTCController() {
    if (statusTimer_) statusTimer_->release();
}

Steinberg::tresult PLUGIN_API WebRTCController::terminate() {
    if (statusTimer_) { statusTimer_->release(); statusTimer_ = nullptr; }
    return EditControllerEx1::terminate();
}

void WebRTCController::onTimer(Steinberg::Timer*) {
    if (auto* message = allocateMessage()) {
        message->setMessageID("PollStatus");
        sendMessage(message);
        message->release();
    }
}

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
    mode->appendString(STR16("Publish"));
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
    roomNameParam->setString("");

    auto* handshakeUrlParam = new StringParameter(STR16("Handshake URL"), kParamHandshakeUrl);
    parameters.addParameter(handshakeUrlParam);
    handshakeUrlParam->setDefaultString(kDefaultHandshakeUrl);
    handshakeUrlParam->setString(kDefaultHandshakeUrl);

    auto& handshakeInfo = const_cast<Steinberg::Vst::ParameterInfo&>(handshakeUrlParam->getInfo());
    handshakeInfo.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;

    auto* statusParam = new StringParameter(STR16("Status"), kParamStatus);
    auto& statusInfo = const_cast<Steinberg::Vst::ParameterInfo&>(statusParam->getInfo());
    statusInfo.flags |= Steinberg::Vst::ParameterInfo::kIsReadOnly;
    statusInfo.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;
    parameters.addParameter(statusParam);
    statusParam->setDefaultString("Idle");
    statusParam->setString("Idle");

    auto* passwordParam = new StringParameter(STR16("Password"), kParamPassword);
    parameters.addParameter(passwordParam);
    passwordParam->setDefaultString("");
    passwordParam->setString("");

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

    auto* pushLinkParam = new StringParameter(STR16("VDO.Ninja Link"), kParamPushLink);
    parameters.addParameter(pushLinkParam);
    pushLinkParam->setDefaultString("");
    pushLinkParam->setString("");
    auto& pushLinkInfo = const_cast<Steinberg::Vst::ParameterInfo&>(pushLinkParam->getInfo());
    pushLinkInfo.flags |= Steinberg::Vst::ParameterInfo::kIsReadOnly;
    pushLinkInfo.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;

    auto* copyPushParam = new Steinberg::Vst::RangeParameter(STR16("Copy VDO.Ninja Link"),
                                                              kParamCopyPushLink,
                                                              STR16(""),
                                                              0.0,
                                                              1.0,
                                                              0.0);
    copyPushParam->setPrecision(0);
    parameters.addParameter(copyPushParam);
    auto& copyPushInfo = const_cast<Steinberg::Vst::ParameterInfo&>(copyPushParam->getInfo());
    copyPushInfo.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;

    auto* showPushQrParam = new Steinberg::Vst::RangeParameter(STR16("Show VDO.Ninja Link QR"),
                                                                kParamShowPushQr,
                                                                STR16(""),
                                                                0.0,
                                                                1.0,
                                                                0.0);
    showPushQrParam->setPrecision(0);
    parameters.addParameter(showPushQrParam);
    auto& showPushQrInfo = const_cast<Steinberg::Vst::ParameterInfo&>(showPushQrParam->getInfo());
    showPushQrInfo.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;

    // Append, preserving existing host parameter indices as well as stable IDs.
    for (const auto& item : {
             std::pair{kParamWebBaseUrl, STR16("Web domain / URL")},
             std::pair{kParamSalt, STR16("Custom salt")}}) {
        auto* param = new StringParameter(item.second, item.first);
        param->setDefaultString(item.first == kParamWebBaseUrl ? kDefaultWebBaseUrl : "");
        auto& info = const_cast<Steinberg::Vst::ParameterInfo&>(param->getInfo());
        info.flags &= ~Steinberg::Vst::ParameterInfo::kCanAutomate;
        parameters.addParameter(param);
    }

    parameters.addParameter(STR16("Apply advanced settings"), nullptr, 1, 0,
        0, kParamApplyAdvanced);

    updateShareLinks();

    statusTimer_ = Steinberg::Timer::create(this, 100);
    return Steinberg::kResultOk;
}

bool WebRTCController::applyStateJson(const std::string& jsonString) {
    if (jsonString.empty()) {
        return true;
    }

    try {
        PluginConfig previous;
        previous.streamId = findStringParameter(kParamStreamId)->getString();
        previous.roomName = findStringParameter(kParamRoomName)->getString();
        previous.password = findStringParameter(kParamPassword)->getString();
        previous.handshakeUrl = findStringParameter(kParamHandshakeUrl)->getString();
        previous.mode = parameters.getParameter(kParamMode)->getNormalized() >= 0.5 ? ConnectionMode::Publish : ConnectionMode::Play;
        const auto next = parseConfigState(jsonString, previous);
        committedAdvanced_ = next;
        advancedDirty_ = false;
        findStringParameter(kParamStreamId)->setString(next.streamId);
        findStringParameter(kParamRoomName)->setString(next.roomName);
        findStringParameter(kParamHandshakeUrl)->setString(next.handshakeUrl);
        findStringParameter(kParamWebBaseUrl)->setString(next.webBaseUrl);
        findStringParameter(kParamSalt)->setString(next.salt);
        findStringParameter(kParamPassword)->setString(next.password);
        parameters.getParameter(kParamMode)->setNormalized(next.mode == ConnectionMode::Publish ? 1.0 : 0.0);

        updateDisableEncryptionFromPassword();
        updateShareLinks();
        return true;
    } catch (...) {
        return false;
    }
}


std::string WebRTCController::exportStateJson(bool includeDraft) const {
    auto* modeParam = parameters.getParameter(kParamMode);
    const bool isPublish = modeParam && modeParam->getNormalized() >= 0.5;

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
        {"handshakeUrl", includeDraft ? handshakeUrl : committedAdvanced_.handshakeUrl},
        {"webBaseUrl", includeDraft ? findStringParameter(kParamWebBaseUrl)->getString() : committedAdvanced_.webBaseUrl},
        {"salt", includeDraft ? findStringParameter(kParamSalt)->getString() : committedAdvanced_.salt},
        {"password", password},
        {"mode", isPublish ? "publish" : "play"},
        {"disableEncryption", disableEncryption}
    };

    json["roomId"] = roomName;
    json["signalingUrl"] = json["handshakeUrl"];

    return json.dump();
}


bool WebRTCController::sendConfig(bool includeDraft) {
    const auto serialized = exportStateJson(includeDraft);
    try { (void)parseConfigState(serialized, committedAdvanced_); } catch (...) { return false; }
    auto* message = allocateMessage();
    if (!message) return false;
    message->setMessageID("ConfigUpdate");
    message->getAttributes()->setBinary("config", serialized.data(), static_cast<Steinberg::uint32>(serialized.size()));
    const bool sent = sendMessage(message) == Steinberg::kResultOk;
    message->release();
    if (sent && includeDraft) {
        committedAdvanced_.handshakeUrl = findStringParameter(kParamHandshakeUrl)->getString();
        committedAdvanced_.webBaseUrl = findStringParameter(kParamWebBaseUrl)->getString();
        committedAdvanced_.salt = findStringParameter(kParamSalt)->getString();
        advancedDirty_ = false;
    }
    if (!sent) postControllerStatus("Error: host did not accept settings");
    return sent;
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

void WebRTCController::setStringParameterAndNotify(Steinberg::Vst::ParamID id, const std::string& value) {
    auto* param = findStringParameter(id);
    if (!param || param->getString() == value) {
        return;
    }

    param->setString(value);
    if (componentHandler) {
        componentHandler->beginEdit(id);
        componentHandler->performEdit(id, param->getNormalized());
        componentHandler->endEdit(id);
    }
}

void WebRTCController::updateShareLinks() {
    auto* modeParam = parameters.getParameter(kParamMode);
    const bool isPublish = modeParam && modeParam->getNormalized() >= 0.5;

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

    const auto password = [this]() {
        if (auto* param = findStringParameter(kParamPassword)) {
            return param->getString();
        }
        return std::string{};
    }();

    setStringParameterAndNotify(kParamPushLink, buildVdoLink(!isPublish, streamId, roomName, password,
        committedAdvanced_.webBaseUrl, committedAdvanced_.salt, committedAdvanced_.handshakeUrl));
}

void WebRTCController::postControllerStatus(const std::string& status) {
    setStringParameterAndNotify(kParamStatus, status);
}

void WebRTCController::performUserAction(Steinberg::Vst::ParamID tag) {
    if (tag == kParamCopyPushLink || tag == kParamShowPushQr) handleActionButton(tag, 1.0);
}

void WebRTCController::handleActionButton(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) {
    auto* actionParam = parameters.getParameter(tag);
    if (!actionParam) {
        return;
    }

    if (value >= 0.5) {
        const auto runCopyAction = [this](Steinberg::Vst::ParamID linkParamId, const char* label) {
            auto* linkParam = findStringParameter(linkParamId);
            if (!linkParam) {
                postControllerStatus(std::string("Error: missing ") + label);
                return;
            }

            const auto link = linkParam->getString();
            if (!copyToClipboard(link)) {
                postControllerStatus(std::string("Error: failed to copy ") + label);
            }
        };

        const auto runQrAction = [this](Steinberg::Vst::ParamID linkParamId, const char* label) {
            auto* linkParam = findStringParameter(linkParamId);
            if (!linkParam) {
                postControllerStatus(std::string("Error: missing ") + label);
                return;
            }

            const auto link = linkParam->getString();
            if (link.empty()) {
                postControllerStatus(std::string("Error: ") + label + " is empty");
                return;
            }

            const auto viewerPath = qrPages_.write(buildQrViewerHtml(label, buildQrSvg(link)));
            if (viewerPath.empty() || !openExternalUrl(viewerPath)) {
                postControllerStatus(std::string("Error: failed to open ") + label + " QR");
            }
        };

        switch (tag) {
            case kParamApplyAdvanced:
                if (sendConfig(true)) { updateShareLinks(); postControllerStatus("Advanced settings applied"); }
                break;
            case kParamCopyPushLink:
                runCopyAction(kParamPushLink, "VDO.Ninja link");
                break;
            case kParamShowPushQr:
                runQrAction(kParamPushLink, "VDO.Ninja link");
                break;
            default:
                break;
        }
    }

    if (actionParam->getNormalized() != 0.0) {
        actionParam->setNormalized(0.0);
        if (componentHandler) {
            componentHandler->beginEdit(tag);
            componentHandler->performEdit(tag, 0.0);
            componentHandler->endEdit(tag);
        }
    }
}

Steinberg::tresult PLUGIN_API WebRTCController::setParamNormalized(Steinberg::Vst::ParamID tag,
                                                                   Steinberg::Vst::ParamValue value) {
    if (!std::isfinite(value) || value < 0 || value > 1) return Steinberg::kInvalidArgument;
    // Non-automatable is advisory: validators and hosts may still write these
    // IDs. Keep them inert and off; side effects require a native UI gesture.
    if (tag == kParamCopyPushLink || tag == kParamShowPushQr)
        return EditControllerEx1::setParamNormalized(tag, 0.0);
    const auto result = EditControllerEx1::setParamNormalized(tag, value);
    if (result != Steinberg::kResultOk) {
        return result;
    }

    if (tag == kParamApplyAdvanced) {
        handleActionButton(tag, value);
        return result;
    }

    if (tag == kParamDisableEncryption) {
        if (suppressDisableEdit_) {
            suppressDisableEdit_ = false;
        } else {
            updateDisableEncryptionFromPassword();
        }
        updateShareLinks();
        return result;
    }

    if (tag == kParamPassword) {
        updateDisableEncryptionFromPassword();
        updateShareLinks();
        sendConfig();
        return result;
    }

    if (tag == kParamWebBaseUrl || tag == kParamSalt || tag == kParamHandshakeUrl) {
        advancedDirty_ = true;
        postControllerStatus("Advanced changes pending: click Apply");
    } else if (tag == kParamMode || tag == kParamStreamId || tag == kParamRoomName) {
        updateShareLinks();
        sendConfig();
    }

    return result;
}

Steinberg::IPlugView* PLUGIN_API WebRTCController::createView(const char* name) {
    Steinberg::ConstString viewName(name);
    if (viewName == Steinberg::Vst::ViewType::kEditor) {
        return new WebRTCEditor(this, "view", "webrtc_vst.uidesc");
    }
    return nullptr;
}

Steinberg::tresult PLUGIN_API WebRTCController::setComponentState(Steinberg::IBStream* state) {
    if (!state) {
        return Steinberg::kInvalidArgument;
    }

    std::string serialized;
    if (!readBoundedState(state, serialized)) return Steinberg::kResultFalse;
    return applyStateJson(serialized) ? Steinberg::kResultOk : Steinberg::kResultFalse;
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
    const auto result = state->write(mutableData, static_cast<Steinberg::int32>(serialized.size()), &written);
    return result == Steinberg::kResultOk && written == static_cast<Steinberg::int32>(serialized.size())
        ? Steinberg::kResultOk : Steinberg::kResultFalse;
}


Steinberg::tresult PLUGIN_API WebRTCController::notify(Steinberg::Vst::IMessage* message) {
    if (!message || !message->getMessageID()) {
        return EditControllerEx1::notify(message);
    }

    if (std::strcmp(message->getMessageID(), "ConfigSync") == 0) {
        const void* data = nullptr;
        Steinberg::uint32 size = 0;
        if (auto* attributes = message->getAttributes();
            attributes && attributes->getBinary("config", data, size) == Steinberg::kResultTrue && data && size > 0 && size <= kMaxStateBytes) {
            const char* charData = static_cast<const char*>(data);
            std::string serialized(charData, (size > 0 && charData[size - 1] == '\0') ? size - 1 : size);
            const bool hadDraft = advancedDirty_;
            const auto draftWeb = findStringParameter(kParamWebBaseUrl)->getString();
            const auto draftSalt = findStringParameter(kParamSalt)->getString();
            const auto draftWss = findStringParameter(kParamHandshakeUrl)->getString();
            if (!applyStateJson(serialized)) return Steinberg::kResultFalse;
            if (hadDraft) {
                findStringParameter(kParamWebBaseUrl)->setString(draftWeb);
                findStringParameter(kParamSalt)->setString(draftSalt);
                findStringParameter(kParamHandshakeUrl)->setString(draftWss);
                advancedDirty_ = true;
            }
        }
        return Steinberg::kResultOk;
    }
    if (std::strcmp(message->getMessageID(), "StatusUpdate") == 0) {
        const void* data = nullptr;
        Steinberg::uint32 size = 0;
        if (auto* attributes = message->getAttributes(); attributes && attributes->getBinary("status", data, size) == Steinberg::kResultTrue && data && size > 0 && size <= 512) {
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
