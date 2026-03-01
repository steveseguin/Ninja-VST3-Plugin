#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vsttypes.h>

#include "../../webrtc_vst/src/ParameterIDs.h"

namespace {

class StringStream : public Steinberg::IBStream {
public:
    explicit StringStream(const std::string& data) : data_(data) {}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++refCount_; }
    Steinberg::uint32 PLUGIN_API release() override {
        if (--refCount_ == 0) { delete this; return 0; }
        return refCount_;
    }
    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        Steinberg::int32 avail = static_cast<Steinberg::int32>(data_.size()) - pos_;
        Steinberg::int32 toRead = std::min(numBytes, avail);
        if (toRead > 0) std::memcpy(buffer, data_.data() + pos_, static_cast<size_t>(toRead));
        pos_ += toRead;
        if (numBytesRead) *numBytesRead = toRead;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void*, Steinberg::int32, Steinberg::int32*) override { return Steinberg::kNotImplemented; }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override {
        if (mode == kIBSeekSet) pos_ = static_cast<Steinberg::int32>(pos);
        else if (mode == kIBSeekCur) pos_ += static_cast<Steinberg::int32>(pos);
        else if (mode == kIBSeekEnd) pos_ = static_cast<Steinberg::int32>(data_.size()) + static_cast<Steinberg::int32>(pos);
        if (result) *result = pos_;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (pos) *pos = pos_;
        return Steinberg::kResultOk;
    }
private:
    std::string data_;
    Steinberg::int32 pos_{0};
    std::atomic<Steinberg::uint32> refCount_{1};
};

constexpr double kSampleRate = 48000.0;
constexpr Steinberg::int32 kBlockSize = 256;
constexpr int kDefaultProcessIterations = 200;
constexpr double kDefaultToneFrequency = 1000.0;
constexpr double kToneAmplitude = 0.25;
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct BusBuffers {
    Steinberg::Vst::AudioBusBuffers buffers {};
    std::vector<std::vector<float>> storage;
    std::vector<float*> channelPtrs;
};

std::string defaultPluginPath() {
#if defined(_WIN32)
    const auto candidate = std::filesystem::path("build") /
                           "webrtc_vst_win" /
                           "VST3" /
                           "Release" /
                           "webrtc_vst.vst3" /
                           "Contents" /
                           "x86_64-win" /
                           "webrtc_vst.vst3";
#elif defined(__APPLE__)
    const auto candidate = std::filesystem::path("build") /
                           "webrtc_vst_mac" /
                           "VST3" /
                           "Release" /
                           "webrtc_vst.vst3" /
                           "Contents" /
                           "MacOS" /
                           "webrtc_vst";
#else
    const auto candidate = std::filesystem::path("build") /
                           "webrtc_vst_linux" /
                           "VST3" /
                           "Release" /
                           "webrtc_vst.vst3";
#endif
    return candidate.string();
}

int parseIntEnv(const char* value, int fallback) {
    if (!value) {
        return fallback;
    }
    try {
        int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

double parseDoubleEnv(const char* value, double fallback) {
    if (!value) {
        return fallback;
    }
    try {
        double parsed = std::stod(value);
        return parsed > 0.0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

int resolveIterationCount() {
    const auto iterationsEnv = std::getenv("WEBRTC_CLI_HOST_ITERATIONS");
    if (iterationsEnv) {
        const int iterations = parseIntEnv(iterationsEnv, kDefaultProcessIterations);
        std::cout << "[config] Using " << iterations << " iterations from WEBRTC_CLI_HOST_ITERATIONS" << std::endl;
        return iterations;
    }

    const auto runtimeEnv = std::getenv("WEBRTC_CLI_HOST_RUNTIME_MS");
    if (!runtimeEnv) {
        std::cout << "[config] Using default " << kDefaultProcessIterations << " iterations" << std::endl;
        return kDefaultProcessIterations;
    }

    const double runtimeMs = parseDoubleEnv(runtimeEnv, 0.0);
    if (runtimeMs <= 0.0) {
        std::cout << "[config] Using default " << kDefaultProcessIterations << " iterations" << std::endl;
        return kDefaultProcessIterations;
    }

    const double blocksPerSecond = kSampleRate / static_cast<double>(kBlockSize);
    const double totalBlocks = runtimeMs / 1000.0 * blocksPerSecond;
    const int computed = static_cast<int>(std::ceil(totalBlocks));
    std::cout << "[config] Using " << computed << " iterations from WEBRTC_CLI_HOST_RUNTIME_MS=" << runtimeMs << "ms" << std::endl;
    return std::max(computed, 1);
}

double resolveToneFrequency() {
    const auto freqEnv = std::getenv("WEBRTC_CLI_HOST_TONE_HZ");
    return parseDoubleEnv(freqEnv, kDefaultToneFrequency);
}

int resolveTimeoutMs() {
    const auto timeoutEnv = std::getenv("WEBRTC_CLI_HOST_TIMEOUT_MS");
    if (!timeoutEnv) {
        return 0;
    }
    return parseIntEnv(timeoutEnv, 0);
}

int resolveBlockSleepMs() {
    const auto sleepEnv = std::getenv("WEBRTC_CLI_HOST_BLOCK_SLEEP_MS");
    if (!sleepEnv) {
        return 0;
    }
    return parseIntEnv(sleepEnv, 0);
}

int resolveWallclockRuntimeMs() {
    const auto runtimeEnv = std::getenv("WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS");
    if (!runtimeEnv) {
        return 0;
    }
    return parseIntEnv(runtimeEnv, 0);
}

class ScopedTimeoutGuard {
public:
    explicit ScopedTimeoutGuard(int timeoutMs) {
        if (timeoutMs <= 0) {
            return;
        }
        timeoutMs_ = timeoutMs;
        worker_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto completed = cv_.wait_for(lock,
                                               std::chrono::milliseconds(timeoutMs_),
                                               [this]() { return cancelled_; });
            if (completed) {
                return;
            }
            std::cerr << "[ERROR] CLI host timed out after " << timeoutMs_ << " ms" << std::endl;
            std::exit(EXIT_FAILURE);
        });
    }

    ~ScopedTimeoutGuard() {
        if (!worker_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
        }
        cv_.notify_one();
        worker_.join();
    }

    ScopedTimeoutGuard(const ScopedTimeoutGuard&) = delete;
    ScopedTimeoutGuard& operator=(const ScopedTimeoutGuard&) = delete;

private:
    int timeoutMs_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool cancelled_ = false;
    std::thread worker_;
};

bool shouldMonitorOutput() {
    const char* env = std::getenv("WEBRTC_CLI_HOST_MONITOR_OUTPUT");
    if (!env) {
        return false;
    }
    if (env[0] == '0' && env[1] == '\0') {
        return false;
    }
    return true;
}

void accumulateOutputMetrics(const std::vector<BusBuffers>& outputBuses,
                             double& energy,
                             size_t& sampleCount) {
    for (const auto& bus : outputBuses) {
        const auto channelCount = bus.buffers.numChannels;
        for (Steinberg::int32 channel = 0; channel < channelCount; ++channel) {
            const auto index = static_cast<size_t>(channel);
            if (index >= bus.storage.size()) {
                continue;
            }
            const auto& storage = bus.storage[index];
            for (float sample : storage) {
                const double value = static_cast<double>(sample);
                energy += value * value;
                sampleCount += 1;
            }
        }
    }
}

std::string string128ToAscii(const Steinberg::Vst::String128 text) {
    std::string result;
    for (int i = 0; i < 128 && text[i] != 0; ++i) {
        const auto code = static_cast<uint32_t>(text[i]);
        result.push_back(code <= 0x7F ? static_cast<char>(code) : '?');
    }
    return result;
}

bool prepareBuses(Steinberg::Vst::IComponent* component,
                  Steinberg::Vst::MediaType type,
                  Steinberg::Vst::BusDirection dir,
                  std::vector<BusBuffers>& outBuffers) {
    const auto busCount = component->getBusCount(type, dir);
    outBuffers.clear();
    outBuffers.resize(static_cast<size_t>(busCount));

    for (Steinberg::int32 busIndex = 0; busIndex < busCount; ++busIndex) {
        if (component->activateBus(type, dir, busIndex, true) != Steinberg::kResultOk) {
            std::cerr << "Failed to activate bus " << busIndex << "\n";
            return false;
        }
        Steinberg::Vst::BusInfo info {};
        if (component->getBusInfo(type, dir, busIndex, info) != Steinberg::kResultOk) {
            std::cerr << "Failed to query bus info for index " << busIndex << "\n";
            return false;
        }
        auto& bus = outBuffers[static_cast<size_t>(busIndex)];
        bus.storage.resize(info.channelCount);
        bus.channelPtrs.resize(info.channelCount);
        for (Steinberg::int32 channel = 0; channel < info.channelCount; ++channel) {
            bus.storage[static_cast<size_t>(channel)].assign(kBlockSize, 0.0f);
            bus.channelPtrs[static_cast<size_t>(channel)] =
                bus.storage[static_cast<size_t>(channel)].data();
        }
        bus.buffers.numChannels = info.channelCount;
        bus.buffers.channelBuffers32 = info.channelCount > 0 ? bus.channelPtrs.data() : nullptr;
        bus.buffers.silenceFlags = 0;
    }

    return true;
}

std::vector<Steinberg::Vst::SpeakerArrangement> stereoArrangements(Steinberg::int32 count) {
    std::vector<Steinberg::Vst::SpeakerArrangement> arrangements(static_cast<size_t>(count),
                                                                 Steinberg::Vst::SpeakerArr::kStereo);
    return arrangements;
}

void populateInputWithTone(std::vector<BusBuffers>& inputBuses,
                           const std::vector<float>& toneBlock) {
    for (auto& bus : inputBuses) {
        for (Steinberg::int32 channel = 0; channel < bus.buffers.numChannels; ++channel) {
            auto& storage = bus.storage[static_cast<size_t>(channel)];
            std::copy(toneBlock.begin(), toneBlock.end(), storage.begin());
        }
    }
}

std::string buildPluginStateJson() {
    std::string json = "{";
    bool hasField = false;
    if (const char* mode = std::getenv("WEBRTC_CLI_HOST_MODE")) {
        json += "\"mode\":\"" + std::string(mode) + "\"";
        hasField = true;
    }
    if (const char* sid = std::getenv("WEBRTC_CLI_HOST_STREAM_ID")) {
        if (hasField) json += ",";
        json += "\"streamId\":\"" + std::string(sid) + "\"";
        hasField = true;
    }
    if (const char* room = std::getenv("WEBRTC_CLI_HOST_ROOM")) {
        if (hasField) json += ",";
        json += "\"roomName\":\"" + std::string(room) + "\"";
        hasField = true;
    }
    if (const char* password = std::getenv("WEBRTC_CLI_HOST_PASSWORD")) {
        if (hasField) json += ",";
        json += "\"password\":\"" + std::string(password) + "\"";
        hasField = true;
    }
    json += "}";
    return hasField ? json : "";
}

bool injectPluginState(Steinberg::Vst::IComponent* component,
                       Steinberg::Vst::IEditController* controller,
                       const std::string& jsonState) {
    if (jsonState.empty()) return true;
    std::cout << "[config] Injecting plugin state: " << jsonState << std::endl;

    auto* stream1 = new StringStream(jsonState);
    component->setState(stream1);
    stream1->release();

    if (controller) {
        auto* stream2 = new StringStream(jsonState);
        controller->setComponentState(stream2);
        stream2->release();
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string pluginPath = (argc > 1) ? std::string{argv[1]} : defaultPluginPath();
    if (!std::filesystem::exists(pluginPath)) {
        std::cerr << "Plugin not found at: " << pluginPath << "\n";
        return 1;
    }

    const int timeoutMs = resolveTimeoutMs();
    ScopedTimeoutGuard timeoutGuard(timeoutMs);

    const int wallclockRuntimeMs = resolveWallclockRuntimeMs();
    const int iterationCount = (wallclockRuntimeMs > 0) ? 0 : resolveIterationCount();
    if (wallclockRuntimeMs > 0) {
        std::cout << "[config] Using wallclock runtime "
                  << wallclockRuntimeMs
                  << "ms from WEBRTC_CLI_HOST_WALLCLOCK_RUNTIME_MS"
                  << std::endl;
    }
    const double toneFrequency = resolveToneFrequency();
    const double tonePhaseIncrement = kTwoPi * toneFrequency / kSampleRate;
    const int blockSleepMs = resolveBlockSleepMs();

    static Steinberg::Vst::HostApplication hostApp;
    Steinberg::Vst::PluginContextFactory::instance().setPluginContext(&hostApp);

    std::string errorDescription;
    auto module = VST3::Hosting::Module::create(pluginPath, errorDescription);
    if (!module) {
        std::cerr << "Failed to load module: " << errorDescription << "\n";
        return 1;
    }

    const VST3::Hosting::PluginFactory& factory = module->getFactory();
    factory.setHostContext(&hostApp);
    const auto classInfos = factory.classInfos();
    for (const auto& info : classInfos) {
        std::cout << "Class: " << info.name() << " (" << info.category() << ")" << std::endl;
    }
    if (classInfos.empty()) {
        std::cerr << "No classes available in module." << std::endl;
        return 1;
    }

    auto selectedClass = classInfos.front();
    Steinberg::Vst::PlugProvider provider(factory, selectedClass, true);
    if (!provider.initialize()) {
        std::cerr << "Failed to initialize plug provider." << std::endl;
        return 1;
    }

    auto component = provider.getComponentPtr();
    if (!component) {
        std::cerr << "Component creation failed." << std::endl;
        return 1;
    }
    auto controller = provider.getControllerPtr();

    Steinberg::Vst::IAudioProcessor* processorRaw = nullptr;
    if (component->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                                  reinterpret_cast<void**>(&processorRaw)) != Steinberg::kResultOk) {
        std::cerr << "Component does not expose IAudioProcessor." << std::endl;
        provider.releasePlugIn(component, controller);
        return 1;
    }
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor(processorRaw, false);

    // Inject plugin state from env vars before activation
    const auto stateJson = buildPluginStateJson();
    if (!stateJson.empty()) {
        injectPluginState(component, controller, stateJson);
    }

    std::vector<BusBuffers> inputBuses;
    std::vector<BusBuffers> outputBuses;
    if (!prepareBuses(component, Steinberg::Vst::kAudio, Steinberg::Vst::kInput, inputBuses) ||
        !prepareBuses(component, Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, outputBuses)) {
        provider.releasePlugIn(component, controller);
        return 1;
    }

    auto inputArrangements = stereoArrangements(static_cast<Steinberg::int32>(inputBuses.size()));
    auto outputArrangements = stereoArrangements(static_cast<Steinberg::int32>(outputBuses.size()));

    if (processor->setBusArrangements(inputArrangements.empty() ? nullptr : inputArrangements.data(),
                                      static_cast<Steinberg::int32>(inputArrangements.size()),
                                      outputArrangements.empty() ? nullptr : outputArrangements.data(),
                                      static_cast<Steinberg::int32>(outputArrangements.size())) !=
        Steinberg::kResultOk) {
        std::cerr << "Failed to set bus arrangements." << std::endl;
        provider.releasePlugIn(component, controller);
        return 1;
    }

    Steinberg::Vst::ProcessSetup setup {};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = kBlockSize;
    setup.sampleRate = kSampleRate;

    if (processor->setupProcessing(setup) != Steinberg::kResultOk) {
        std::cerr << "setupProcessing failed." << std::endl;
        provider.releasePlugIn(component, controller);
        return 1;
    }

    if (component->setActive(true) != Steinberg::kResultOk) {
        std::cerr << "setActive failed." << std::endl;
        provider.releasePlugIn(component, controller);
        return 1;
    }

    const auto processingResult = processor->setProcessing(true);
    if (processingResult != Steinberg::kResultOk &&
        processingResult != Steinberg::kResultTrue &&
        processingResult != Steinberg::kNotImplemented) {
        std::cerr << "setProcessing failed (" << processingResult << ")." << std::endl;
        component->setActive(false);
        provider.releasePlugIn(component, controller);
        return 1;
    }

    Steinberg::Vst::ProcessContext context {};
    context.state = Steinberg::Vst::ProcessContext::kPlaying;
    context.sampleRate = kSampleRate;
    context.projectTimeSamples = 0;

    std::vector<Steinberg::Vst::AudioBusBuffers> inputBusViews;
    inputBusViews.reserve(inputBuses.size());
    for (auto& bus : inputBuses) {
        inputBusViews.push_back(bus.buffers);
    }
    std::vector<Steinberg::Vst::AudioBusBuffers> outputBusViews;
    outputBusViews.reserve(outputBuses.size());
    for (auto& bus : outputBuses) {
        outputBusViews.push_back(bus.buffers);
    }

    Steinberg::Vst::ProcessData data {};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = kBlockSize;
    data.numInputs = static_cast<Steinberg::int32>(inputBusViews.size());
    data.inputs = inputBusViews.empty() ? nullptr : inputBusViews.data();
    data.numOutputs = static_cast<Steinberg::int32>(outputBusViews.size());
    data.outputs = outputBusViews.empty() ? nullptr : outputBusViews.data();
    data.processContext = &context;

    // Prime one block to trigger startup, then warm up for signaling/ICE.
    const int warmupMs = parseIntEnv(std::getenv("WEBRTC_CLI_HOST_WARMUP_MS"), 2000);
    if (warmupMs > 0) {
        if (processor->process(data) != Steinberg::kResultOk) {
            std::cerr << "warmup process call failed." << std::endl;
        } else {
            context.projectTimeSamples += kBlockSize;
        }
        std::cout << "[warmup] Waiting " << warmupMs << "ms for connection..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs));
    }

    std::vector<float> toneBlock(kBlockSize, 0.0f);
    double tonePhase = 0.0;
    const bool monitorOutput = shouldMonitorOutput();
    double outputEnergy = 0.0;
    size_t outputSamples = 0;

    int iteration = 0;
    const auto processStart = std::chrono::steady_clock::now();
    while (true) {
        if (wallclockRuntimeMs > 0) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - processStart).count();
            if (elapsedMs >= wallclockRuntimeMs) {
                break;
            }
        } else if (iteration >= iterationCount) {
            break;
        }

        if (!inputBuses.empty() && !toneBlock.empty()) {
            for (int sample = 0; sample < kBlockSize; ++sample) {
                toneBlock[static_cast<size_t>(sample)] =
                    static_cast<float>(kToneAmplitude * std::sin(tonePhase));
                tonePhase += tonePhaseIncrement;
                if (tonePhase >= kTwoPi) {
                    tonePhase -= kTwoPi;
                }
            }
            populateInputWithTone(inputBuses, toneBlock);
        }
        const auto processResult = processor->process(data);
        if (processResult != Steinberg::kResultOk) {
            std::cerr << "process call failed at iteration " << iteration << "\n";
            break;
        }
        if (monitorOutput && !outputBuses.empty()) {
            accumulateOutputMetrics(outputBuses, outputEnergy, outputSamples);
        }
        context.projectTimeSamples += kBlockSize;
        if (blockSleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(blockSleepMs));
        }
        ++iteration;
    }

    if (monitorOutput) {
        if (outputSamples > 0) {
            const double rms = std::sqrt(outputEnergy / static_cast<double>(outputSamples));
            std::cout << "[monitor] output_rms="
                      << std::fixed << std::setprecision(6) << rms << std::defaultfloat
                      << " samples=" << outputSamples << std::endl;
        } else {
            std::cout << "[monitor] output_rms=0 samples=0" << std::endl;
        }
    }

    if (controller) {
        Steinberg::Vst::String128 status {};
        const auto normalizedStatus = controller->getParamNormalized(webrtc_vst::kParamStatus);
        if (controller->getParamStringByValue(webrtc_vst::kParamStatus, normalizedStatus, status) == Steinberg::kResultOk) {
            const auto statusAscii = string128ToAscii(status);
            if (!statusAscii.empty()) {
                std::cout << "[status] " << statusAscii << std::endl;
            }
        }
    }

    processor->setProcessing(false);
    component->setActive(false);

    processor = nullptr;

    auto* componentRaw = component.take();
    auto* controllerRaw = controller.take();
    provider.releasePlugIn(componentRaw, controllerRaw);
    return 0;
}
