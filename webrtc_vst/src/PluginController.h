#pragma once

#include "ParameterIDs.h"
#include "ParameterStringRegistry.h"

#include <public.sdk/source/vst/vsteditcontroller.h>
#include <public.sdk/source/vst/vstparameters.h>

namespace Steinberg { namespace Vst { class IMessage; } }

namespace webrtc_vst {

class StringParameter : public Steinberg::Vst::Parameter {
public:
    StringParameter(const Steinberg::char16* title,
                    Steinberg::Vst::ParamID tag);

    void setString(const std::string& value);
    void setDefaultString(const std::string& value);
    std::string getString() const;

    void toString(Steinberg::Vst::ParamValue normValue, Steinberg::Vst::String128 string) const override;
    bool fromString(const Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& normValue) const override;
};

class WebRTCController final : public Steinberg::Vst::EditControllerEx1 {
public:
    WebRTCController();

    static Steinberg::FUnknown* createInstance(void* context);

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag,
                                                     Steinberg::Vst::ParamValue value) override;
    Steinberg::IPlugView* PLUGIN_API createView(const char* name) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;

private:
    void applyStateJson(const std::string& jsonString);
    std::string exportStateJson() const;

    StringParameter* findStringParameter(Steinberg::Vst::ParamID id) const;
    void updateDisableEncryptionFromPassword();
    bool suppressDisableEdit_{false};
};

extern const Steinberg::FUID kWebRTCControllerUID;

} // namespace webrtc_vst