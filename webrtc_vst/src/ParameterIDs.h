#pragma once

#include <pluginterfaces/vst/vsttypes.h>

namespace webrtc_vst {

constexpr Steinberg::Vst::ParamID kParamMode = 0;
constexpr Steinberg::Vst::ParamID kParamStreamId = 1;
constexpr Steinberg::Vst::ParamID kParamRoomName = 2;
constexpr Steinberg::Vst::ParamID kParamHandshakeUrl = 3;
constexpr Steinberg::Vst::ParamID kParamPassword = 4;
constexpr Steinberg::Vst::ParamID kParamDisableEncryption = 5;
constexpr Steinberg::Vst::ParamID kParamStatus = 6;
constexpr Steinberg::Vst::ParamID kParamPushLink = 7;
constexpr Steinberg::Vst::ParamID kParamViewLink = 8;
constexpr Steinberg::Vst::ParamID kParamCopyPushLink = 9;
constexpr Steinberg::Vst::ParamID kParamCopyViewLink = 10;
constexpr Steinberg::Vst::ParamID kParamShowPushQr = 11;
constexpr Steinberg::Vst::ParamID kParamShowViewQr = 12;

} // namespace webrtc_vst

