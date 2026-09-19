#pragma once

#include "ParameterIDs.h"
#include "PluginConfig.h"
#include "PrivateQrPages.h"
#include <optional>

#include <public.sdk/source/vst/vsteditcontroller.h>
#include <public.sdk/source/vst/vstparameters.h>
#include <base/source/timer.h>

namespace Steinberg { namespace Vst { class IMessage; } }

namespace webrtc_vst {

class StringParameter : public Steinberg::Vst::Parameter {
public:
    StringParameter(const Steinberg::char16* title,
                    Steinberg::Vst::ParamID tag);

    void setString(const std::string& value);
    void setDefaultString(const std::string& value);
    std::string getString() const;
    bool setNormalized(Steinberg::Vst::ParamValue value) override;

    void toString(Steinberg::Vst::ParamValue normValue, Steinberg::Vst::String128 string) const override;
    bool fromString(const Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& normValue) const override;
private:
    std::string text_;
    mutable std::optional<std::string> pendingText_;
    mutable Steinberg::Vst::ParamValue pendingValue_{0};
    mutable uint32_t serial_{0};
};

class WebRTCController final : public Steinberg::Vst::EditControllerEx1, public Steinberg::ITimerCallback {
public:
    WebRTCController();
    ~WebRTCController() override;
    Steinberg::tresult PLUGIN_API terminate() override;
    void onTimer(Steinberg::Timer*) override;

    static Steinberg::FUnknown* createInstance(void* context);

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag,
                                                     Steinberg::Vst::ParamValue value) override;
    Steinberg::IPlugView* PLUGIN_API createView(const char* name) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;

    // Native editor gesture only; never called by host parameter/state writes.
    void performUserAction(Steinberg::Vst::ParamID tag);

private:
    bool applyStateJson(const std::string& jsonString);
    std::string exportStateJson(bool includeDraft = false) const;
    bool sendConfig(bool includeDraft = false);

    StringParameter* findStringParameter(Steinberg::Vst::ParamID id) const;
    void updateDisableEncryptionFromPassword();
    void updateShareLinks();
    void setStringParameterAndNotify(Steinberg::Vst::ParamID id, const std::string& value);
    void handleActionButton(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value);
    void postControllerStatus(const std::string& status);
    bool suppressDisableEdit_{false};
    PrivateQrPages qrPages_;
    PluginConfig committedAdvanced_;
    bool advancedDirty_{false};
    Steinberg::Timer* statusTimer_{nullptr};
};

extern const Steinberg::FUID kWebRTCControllerUID;

} // namespace webrtc_vst
