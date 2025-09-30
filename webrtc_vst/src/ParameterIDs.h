#pragma once

#include <pluginterfaces/vst/vsttypes.h>

namespace webrtc_vst {

constexpr Steinberg::Vst::ParamID kParamMode = 0;
constexpr Steinberg::Vst::ParamID kParamStreamId = 1;
constexpr Steinberg::Vst::ParamID kParamRoomName = 2;
constexpr Steinberg::Vst::ParamID kParamHandshakeUrl = 3;
constexpr Steinberg::Vst::ParamID kParamPassword = 4;
constexpr Steinberg::Vst::ParamID kParamDisableEncryption = 5;

} // namespace webrtc_vst

