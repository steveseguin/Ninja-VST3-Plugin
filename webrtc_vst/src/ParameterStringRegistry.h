#pragma once
#include "SpinLock.h"
#include <mutex>
#include <thread>

#include <cstdint>
#include <atomic>
#include <string>
#include <unordered_map>

#include <pluginterfaces/vst/vsttypes.h>

namespace webrtc_vst {

class ParameterStringRegistry {
public:
    static ParameterStringRegistry& instance();

    uint32_t registerValue(const std::string& value);
    std::string lookup(uint32_t id) const;

private:
    ParameterStringRegistry() = default;
    ParameterStringRegistry(const ParameterStringRegistry&) = delete;
    ParameterStringRegistry& operator=(const ParameterStringRegistry&) = delete;

    mutable SpinLock mutex_;
    std::unordered_map<uint32_t, std::string> values_;
    std::unordered_map<std::string, uint32_t> reverse_;
    uint32_t nextId_{1};
};

uint32_t normalizedToId(Steinberg::Vst::ParamValue value);
Steinberg::Vst::ParamValue idToNormalized(uint32_t id);

} // namespace webrtc_vst


