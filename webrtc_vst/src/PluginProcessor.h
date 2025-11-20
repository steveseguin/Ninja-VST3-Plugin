#pragma once

#include "PluginConfig.h"
#include "WebRTCSession.h"
#include "SpinLock.h"

#include <public.sdk/source/vst/vstaudioeffect.h>

#include <atomic>
#include <string>

namespace webrtc_vst {

class WebRTCProcessor final : public Steinberg::Vst::AudioEffect {
public:
    WebRTCProcessor();
    ~WebRTCProcessor() override;

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
    void syncConfigToController();
    std::string serializeConfigToJson() const;
    void handleSanitizedConfig(const PluginConfig& sanitizedConfig);
    void queueStatus(const std::string& status);
    void flushPendingStatus();
    void sendStatusToController(const std::string& status);

    // CRITICAL: Construction follows declaration order. receiveBuffer_ must be ready before
    // session_ because WebRTCSession stores a reference to it. With receiveBuffer_ declared
    // first, the buffer is constructed before session_ uses it, while session_ still tears
    // down near last thanks to stopSession() clearing callbacks before destruction.
    AudioRingBuffer receiveBuffer_;
    WebRTCSession session_;

    // Status members - safe to destroy before session_ since callbacks are cleared in destructor
    std::atomic<bool> statusDirty_{false};
    mutable SpinLock statusMutex_;
    std::string pendingStatus_;
    std::string lastSentStatus_{"Idle"};

    PluginConfig config_;
    Steinberg::Vst::ProcessSetup processSetup_{};
    bool sessionActive_{false};
    bool configDirty_{false};
};

extern const Steinberg::FUID kWebRTCProcessorUID;
extern const Steinberg::FUID kWebRTCControllerUID;

} // namespace webrtc_vst

