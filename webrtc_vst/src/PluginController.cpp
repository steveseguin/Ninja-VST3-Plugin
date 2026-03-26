#include "PluginController.h"
#include "qrcodegen.hpp"
#include "StreamIdGenerator.h"

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
#include <vstgui/plugin-bindings/vst3editor.h>
#include <vstgui/lib/platform/platformfactory.h>
#if SMTG_OS_WINDOWS
#include <vstgui/lib/platform/win32/win32factory.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace webrtc_vst {

namespace {

constexpr auto kDefaultHandshakeUrl = "wss://wss.vdo.ninja";
constexpr auto kDefaultVdoHost = "vdo.ninja";

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

std::string buildPasswordHash(const std::string& password) {
    const auto trimmedPassword = trimCopy(password);
    if (trimmedPassword.empty() || passwordImpliesDisableEncryption(trimmedPassword)) {
        return {};
    }

    const auto payload = encodeURIComponentCompat(trimmedPassword) + kDefaultVdoHost;
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
                         const std::string& password) {
    const auto trimmedStream = trimCopy(streamId);
    if (trimmedStream.empty()) {
        return {};
    }

    std::ostringstream url;
    url << "https://" << kDefaultVdoHost << "/?";
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

    const auto passwordHash = buildPasswordHash(password);
    if (!passwordHash.empty()) {
        url << "&hash=" << passwordHash;
    }

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

std::string writeLocalQrViewer(const std::string& label, const std::string& targetUrl) {
    try {
        namespace fs = std::filesystem;
        fs::path qrDir = fs::temp_directory_path() / "webrtc_vst_qr";
        fs::create_directories(qrDir);

        std::string fileStem = "share_qr";
        if (label.find("push") != std::string::npos) {
            fileStem = "push_qr";
        } else if (label.find("view") != std::string::npos) {
            fileStem = "view_qr";
        }

        fs::path htmlPath = qrDir / ("webrtc_vst_" + fileStem + ".html");
        std::ofstream output(htmlPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            return {};
        }

        output << buildQrViewerHtml(label, buildQrSvg(targetUrl));
        output.close();
        if (!output) {
            return {};
        }
        return htmlPath.string();
    } catch (...) {
        return {};
    }
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

    updateShareLinks();

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
                param->setNormalized((mode == "seed" || mode == "publish") ? 1.0 : 0.0);
            }
        }

        updateDisableEncryptionFromPassword();
        updateShareLinks();
    } catch (...) {
        // ignore malformed state
    }
}


std::string WebRTCController::exportStateJson() const {
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
        {"handshakeUrl", handshakeUrl},
        {"password", password},
        {"mode", isPublish ? "publish" : "play"},
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

    setStringParameterAndNotify(kParamPushLink, buildVdoLink(!isPublish, streamId, roomName, password));
}

void WebRTCController::postControllerStatus(const std::string& status) {
    setStringParameterAndNotify(kParamStatus, status);
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

            const auto viewerPath = writeLocalQrViewer(label, link);
            if (viewerPath.empty() || !openExternalUrl(viewerPath)) {
                postControllerStatus(std::string("Error: failed to open ") + label + " QR");
            }
        };

        switch (tag) {
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
    const auto result = EditControllerEx1::setParamNormalized(tag, value);
    if (result != Steinberg::kResultOk) {
        return result;
    }

    if (tag == kParamCopyPushLink || tag == kParamShowPushQr) {
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
        return result;
    }

    if (tag == kParamMode || tag == kParamStreamId || tag == kParamRoomName) {
        updateShareLinks();
    }

    return result;
}

Steinberg::IPlugView* PLUGIN_API WebRTCController::createView(const char* name) {
    Steinberg::ConstString viewName(name);
    if (viewName == Steinberg::Vst::ViewType::kEditor) {
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

