#pragma once

#include "PluginConfig.h"
#include "WebRTCSession.h"
#include "SpinLock.h"

#include <public.sdk/source/vst/vstaudioeffect.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

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
    void startSession(const PluginConfig& config);
    void stopSession();
    void updateConfigFromEnvironment();
    void applyParameterChange(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);
    void requestConfigApply();
    void configThreadMain();
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
    mutable std::mutex configMutex_;
    std::condition_variable configCv_;
    std::thread configThread_;
    std::atomic<bool> configThreadExit_{false};
    std::atomic<bool> configPending_{false};
    std::atomic<bool> hostActive_{false};
    std::atomic<ConnectionMode> modeAtomic_{ConnectionMode::Play};
    std::atomic<bool> processingReady_{false};
    std::atomic<bool> controllerSyncPending_{false};

    Steinberg::Vst::ProcessSetup processSetup_{};
    std::atomic<bool> sessionActive_{false};
    std::atomic<bool> configDirty_{false};
    std::atomic<bool> loggedSeedProcessState_{false};
    std::atomic<bool> loggedPlayProcessState_{false};
    std::atomic<bool> loggedSeedPushAttempt_{false};
    std::atomic<int> lastLoggedMode_{-1};
};

extern const Steinberg::FUID kWebRTCProcessorUID;
extern const Steinberg::FUID kWebRTCControllerUID;

} // namespace webrtc_vst

