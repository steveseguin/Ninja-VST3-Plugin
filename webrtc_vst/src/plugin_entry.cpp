#include "PluginController.h"
#include "PluginProcessor.h"
#include "Version.h"

#include <public.sdk/source/main/pluginfactory.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
constexpr auto kVendor = "Open Source";
constexpr auto kUrl = "https://vdo.ninja";
constexpr auto kEmail = "support@vdo.ninja";
constexpr auto kCategory = "Fx";
constexpr auto kPluginName = "VDO.Ninja WebRTC Bridge";

class AtomicReleaseFactory final : public CPluginFactory {
public:
    using CPluginFactory::CPluginFactory;

    uint32 PLUGIN_API release() override {
        // SDK 3.8's default release performs a plain refcount read after the
        // atomic decrement. Another release can race that read (or delete the
        // object first). Return the decrement result without touching it again.
        const auto remaining = FUnknownPrivate::atomicAdd(__funknownRefCount, -1);
        if (remaining == 0) delete this;
        return static_cast<uint32>(remaining);
    }
};
}

BEGIN_FACTORY_CLASS(AtomicReleaseFactory, kVendor, kUrl, kEmail, Vst::kDefaultFactoryFlags)

DEF_CLASS2(INLINE_UID_FROM_FUID(webrtc_vst::kWebRTCProcessorUID),
           PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           kPluginName,
           Vst::kDistributable,
           kCategory,
           WEBRTC_VST_VERSION_STRING,
           kVstVersionString,
           webrtc_vst::WebRTCProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(webrtc_vst::kWebRTCControllerUID),
           PClassInfo::kManyInstances,
           kVstComponentControllerClass,
           "VDO.Ninja WebRTC Bridge Controller",
           0,
           "",
           WEBRTC_VST_VERSION_STRING,
           kVstVersionString,
           webrtc_vst::WebRTCController::createInstance)

END_FACTORY
