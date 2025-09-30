#include "ParameterStringRegistry.h"

#include <algorithm>
#include <limits>

namespace webrtc_vst {

namespace {
constexpr double kMaxIdAsDouble = static_cast<double>(std::numeric_limits<uint32_t>::max());
}

ParameterStringRegistry& ParameterStringRegistry::instance() {
    static auto* registry = new ParameterStringRegistry();
    return *registry;
}

uint32_t ParameterStringRegistry::registerValue(const std::string& value) {
    std::lock_guard<SpinLock> lock(mutex_);
    if (auto existing = reverse_.find(value); existing != reverse_.end()) {
        return existing->second;
    }

    const uint32_t id = nextId_++;
    values_[id] = value;
    reverse_[value] = id;
    return id;
}

std::string ParameterStringRegistry::lookup(uint32_t id) const {
    std::lock_guard<SpinLock> lock(mutex_);
    if (auto it = values_.find(id); it != values_.end()) {
        return it->second;
    }
    return {};
}

uint32_t normalizedToId(Steinberg::Vst::ParamValue value) {
    value = std::clamp(value, 0.0, 1.0);
    return static_cast<uint32_t>(value * kMaxIdAsDouble + 0.5);
}

Steinberg::Vst::ParamValue idToNormalized(uint32_t id) {
    return static_cast<Steinberg::Vst::ParamValue>(id) / kMaxIdAsDouble;
}

} // namespace webrtc_vst
