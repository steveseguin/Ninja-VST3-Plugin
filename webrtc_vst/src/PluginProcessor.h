#pragma once

#include "PluginConfig.h"
#include "WebRTCSession.h"

#include <public.sdk/source/vst/vstaudioeffect.h>

namespace webrtc_vst {

class WebRTCProcessor final : public Steinberg::Vst::AudioEffect {
public:
    WebRTCProcessor();

    static Steinberg::FUnknown* createInstance(void* context);

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getControllerClassId(Steinberg::TUID classId) override;

private:
    void startSession();
    void stopSession();
    void updateConfigFromEnvironment();
    void applyParameterChange(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);
    void restartSessionIfNeeded();

    AudioRingBuffer receiveBuffer_;
    WebRTCSession session_;
    PluginConfig config_;
    Steinberg::Vst::ProcessSetup processSetup_{};
    bool sessionActive_{false};
    bool configDirty_{false};
};

extern const Steinberg::FUID kWebRTCProcessorUID;
extern const Steinberg::FUID kWebRTCControllerUID;

} // namespace webrtc_vst
