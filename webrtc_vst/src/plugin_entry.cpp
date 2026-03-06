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
}

BEGIN_FACTORY_DEF(kVendor, kUrl, kEmail)

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
