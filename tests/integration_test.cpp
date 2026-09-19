// Integration Test for WebRTC VST Plugin
// Simulates real VST3 host behavior (like Audacity) to catch real-world issues
// Uses the same PlugProvider pattern as the CLI host for reliable hosting.

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstprefetchablesupport.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vsttypes.h>

#include "../webrtc_vst/src/ParameterIDs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

// ============================================================================
// Test Configuration
// ============================================================================

constexpr double kSampleRate = 48000.0;
constexpr int32 kBlockSize = 512;  // Audacity uses 512
constexpr int32 kNumChannels = 2;

class StringStream : public IBStream {
public:
    explicit StringStream(const std::string& data) : data_(data) {}

    tresult PLUGIN_API queryInterface(const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override {
        if (--refCount_ == 0) {
            delete this;
            return 0;
        }
        return refCount_;
    }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        const int32 available = static_cast<int32>(data_.size()) - position_;
        const int32 toRead = std::min(numBytes, available);
        if (toRead > 0) {
            std::memcpy(buffer, data_.data() + position_, static_cast<size_t>(toRead));
        }
        position_ += toRead;
        if (numBytesRead) {
            *numBytesRead = toRead;
        }
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 count, int32* written) override {
        if (count < 0 || position_ < 0) return kInvalidArgument;
        data_.resize(std::max(data_.size(), static_cast<size_t>(position_ + count)));
        std::memcpy(data_.data() + position_, buffer, static_cast<size_t>(count));
        position_ += count;
        if (written) *written = count;
        return kResultOk;
    }

    const std::string& data() const { return data_; }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        if (mode == kIBSeekSet) {
            position_ = static_cast<int32>(pos);
        } else if (mode == kIBSeekCur) {
            position_ += static_cast<int32>(pos);
        } else if (mode == kIBSeekEnd) {
            position_ = static_cast<int32>(data_.size()) + static_cast<int32>(pos);
        }
        if (result) {
            *result = position_;
        }
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) override {
        if (pos) {
            *pos = position_;
        }
        return kResultOk;
    }

private:
    std::string data_;
    int32 position_{0};
    std::atomic<uint32> refCount_{1};
};

std::string string128ToAscii(const String128 text) {
    std::string result;
    for (int i = 0; i < 128 && text[i] != 0; ++i) {
        result.push_back(static_cast<uint32_t>(text[i]) <= 0x7F ? static_cast<char>(text[i]) : '?');
    }
    return result;
}

// ============================================================================
// Test Result Tracking
// ============================================================================

struct TestResult {
    std::string name;
    bool passed{false};
    std::string error;
    double duration_ms{0.0};
};

class TestSuite {
public:
    void addResult(TestResult result) {
        results_.push_back(result);
        if (result.passed) {
            std::cout << "  PASS " << result.name << " (" << result.duration_ms << "ms)" << std::endl;
        } else {
            std::cout << "  FAIL " << result.name << ": " << result.error << std::endl;
            failed_++;
        }
    }

    void printSummary() {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "TEST SUMMARY" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        std::cout << "Total:  " << results_.size() << std::endl;
        std::cout << "Passed: " << (results_.size() - failed_) << std::endl;
        std::cout << "Failed: " << failed_ << std::endl;

        if (failed_ > 0) {
            std::cout << "\nFailed tests:" << std::endl;
            for (const auto& r : results_) {
                if (!r.passed) {
                    std::cout << "  - " << r.name << ": " << r.error << std::endl;
                }
            }
        }
        std::cout << std::string(60, '=') << std::endl;
    }

    bool allPassed() const { return failed_ == 0; }

private:
    std::vector<TestResult> results_;
    size_t failed_{0};
};

// ============================================================================
// VST3 Host Simulator using PlugProvider (same pattern as CLI host)
// ============================================================================

class VST3HostSimulator {
public:
    explicit VST3HostSimulator(const std::string& pluginPath)
        : pluginPath_(pluginPath) {}

    ~VST3HostSimulator() {
        cleanup();
    }

    bool loadPlugin() {
        std::string error;
        module_ = VST3::Hosting::Module::create(pluginPath_, error);
        if (!module_) {
            lastError_ = "Failed to load plugin: " + error;
            return false;
        }

        const VST3::Hosting::PluginFactory& factory = module_->getFactory();
        auto classInfos = factory.classInfos();
        if (classInfos.empty()) {
            lastError_ = "No classes available in module";
            return false;
        }

        provider_ = std::make_unique<PlugProvider>(factory, classInfos.front(), true);
        if (!provider_->initialize()) {
            lastError_ = "Failed to initialize plug provider";
            return false;
        }

        component_ = provider_->getComponentPtr();
        if (!component_) {
            lastError_ = "Component creation failed";
            return false;
        }

        IAudioProcessor* processorRaw = nullptr;
        if (component_->queryInterface(IAudioProcessor::iid,
                                       reinterpret_cast<void**>(&processorRaw)) != kResultOk) {
            lastError_ = "Component does not expose IAudioProcessor";
            return false;
        }
        processor_ = IPtr<IAudioProcessor>(processorRaw, false);

        controller_ = provider_->getControllerPtr();

        return true;
    }

    bool setupProcessing() {
        if (!processor_) {
            lastError_ = "Processor not available";
            return false;
        }

        ProcessSetup setup{};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = kBlockSize;
        setup.sampleRate = kSampleRate;

        if (processor_->setupProcessing(setup) != kResultOk) {
            lastError_ = "setupProcessing failed";
            return false;
        }

        // Allocate buffers
        inputBuffers_.resize(kNumChannels, std::vector<float>(kBlockSize, 0.0f));
        outputBuffers_.resize(kNumChannels, std::vector<float>(kBlockSize, 0.0f));

        inputPtrs_.resize(kNumChannels);
        outputPtrs_.resize(kNumChannels);
        for (int32 i = 0; i < kNumChannels; ++i) {
            inputPtrs_[i] = inputBuffers_[i].data();
            outputPtrs_[i] = outputBuffers_[i].data();
        }

        return true;
    }

    bool activate() {
        if (!component_) {
            lastError_ = "Component not loaded";
            return false;
        }

        if (component_->setActive(true) != kResultOk) {
            lastError_ = "setActive(true) failed";
            return false;
        }

        active_ = true;
        return true;
    }

    bool deactivate() {
        if (!component_ || !active_) {
            return true;
        }

        if (component_->setActive(false) != kResultOk) {
            lastError_ = "setActive(false) failed";
            return false;
        }

        active_ = false;
        return true;
    }

    bool injectState(const std::string& json) {
        if (!component_ || !controller_) {
            lastError_ = "Plugin not fully loaded";
            return false;
        }

        auto* componentState = new StringStream(json);
        component_->setState(componentState);
        componentState->release();

        auto* controllerState = new StringStream(json);
        const auto result = controller_->setComponentState(controllerState);
        controllerState->release();
        if (result != kResultOk) {
            lastError_ = "Controller state sync failed";
            return false;
        }

        return true;
    }

    std::string savedState(bool controller = false) {
        StringStream stream("");
        if (controller) controller_->getState(&stream);
        else component_->getState(&stream);
        return stream.data();
    }

    bool rejectsShortStateWrites() {
        struct ShortStream : StringStream {
            ShortStream() : StringStream("") {}
            tresult PLUGIN_API write(void*, int32 count, int32* written) override {
                if (written) *written = count > 0 ? count - 1 : 0;
                return kResultOk;
            }
        } stream;
        return component_->getState(&stream) != kResultOk && controller_->getState(&stream) != kResultOk;
    }

    bool requestsRealtimeHostProcessing() {
        IPrefetchableSupport* support = nullptr;
        if (component_->queryInterface(IPrefetchableSupport::iid, reinterpret_cast<void**>(&support)) != kResultOk || !support) return false;
        PrefetchableSupport mode = kIsYetPrefetchable;
        const auto result = support->getPrefetchableSupport(mode);
        support->release();
        return result == kResultOk && mode == kIsNeverPrefetchable;
    }

    bool acceptsText(ParamID id, const char16* text) {
        ParamValue value{};
        return controller_->getParamValueByString(id, const_cast<char16*>(text), value) == kResultOk;
    }

    bool editText(ParamID id, const std::string& text) {
        std::u16string wide(text.begin(), text.end());
        ParamValue value{};
        if (controller_->getParamValueByString(id, wide.data(), value) != kResultOk) return false;
        String128 roundTrip{}, expected{};
        controller_->getParamStringByValue(id, value, expected);
        if (controller_->getParamStringByValue(id, static_cast<float>(value), roundTrip) != kResultOk ||
            std::u16string(roundTrip) != std::u16string(expected)) return false;
        return controller_->setParamNormalized(id, value) == kResultOk;
    }

    bool action(ParamID id) { return controller_->setParamNormalized(id, 1.0) == kResultOk; }

    bool readStringParam(ParamID id, std::string& value) {
        if (!controller_) {
            lastError_ = "Controller not available";
            return false;
        }

        String128 text{};
        const auto normalized = controller_->getParamNormalized(id);
        if (controller_->getParamStringByValue(id, normalized, text) != kResultOk) {
            lastError_ = "Failed to read controller parameter";
            return false;
        }

        value = string128ToAscii(text);
        return true;
    }

    bool process(int32 numBlocks) {
        if (!processor_ || !active_) {
            lastError_ = "Not ready for processing";
            return false;
        }

        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = kBlockSize;
        data.numInputs = 1;
        data.numOutputs = 1;

        AudioBusBuffers inputBus{};
        inputBus.numChannels = kNumChannels;
        inputBus.channelBuffers32 = inputPtrs_.data();
        data.inputs = &inputBus;

        AudioBusBuffers outputBus{};
        outputBus.numChannels = kNumChannels;
        outputBus.channelBuffers32 = outputPtrs_.data();
        data.outputs = &outputBus;

        // Generate test tone in input
        double phase = 0.0;
        constexpr double freq = 440.0;  // A4
        constexpr double phaseIncrement = 2.0 * 3.14159265359 * freq / kSampleRate;

        for (int32 block = 0; block < numBlocks; ++block) {
            // Fill input with test tone
            for (int32 sample = 0; sample < kBlockSize; ++sample) {
                float value = 0.25f * static_cast<float>(std::sin(phase));
                for (int32 ch = 0; ch < kNumChannels; ++ch) {
                    inputBuffers_[ch][sample] = value;
                }
                phase += phaseIncrement;
            }

            // Clear output
            for (int32 ch = 0; ch < kNumChannels; ++ch) {
                std::fill(outputBuffers_[ch].begin(), outputBuffers_[ch].end(), 0.0f);
            }

            // Process
            tresult result = processor_->process(data);
            if (result != kResultOk) {
                lastError_ = "process() returned error on block " + std::to_string(block);
                return false;
            }

            // Simulate real-time processing delay
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        return true;
    }

    bool hasOutputSignal() const {
        for (const auto& channel : outputBuffers_) {
            for (float sample : channel) {
                if (std::abs(sample) > 0.001f) return true;
            }
        }
        return false;
    }

    bool checkPublishSilenceFlags(bool inPlace) {
        AudioBusBuffers inputBus{};
        inputBus.numChannels = kNumChannels;
        inputBus.channelBuffers32 = inputPtrs_.data();
        AudioBusBuffers outputBus{};
        outputBus.numChannels = kNumChannels;
        outputBus.channelBuffers32 = inPlace ? inputPtrs_.data() : outputPtrs_.data();

        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = kBlockSize;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inputBus;
        data.outputs = &outputBus;

        // Hosts may reuse a bus previously marked silent. Alternate silence and
        // signal while retaining the output flags between calls.
        outputBus.silenceFlags = 3;
        for (float level : {0.0f, 0.25f, 0.0f, -0.5f}) {
            inputBus.silenceFlags = level == 0.0f ? 3 : 0;
            for (auto& channel : inputBuffers_) {
                std::fill(channel.begin(), channel.end(), level);
            }
            if (processor_->process(data) != kResultOk) {
                lastError_ = "Publish processing failed";
                return false;
            }
            for (int ch = 0; ch < kNumChannels; ++ch) {
                const auto* samples = outputBus.channelBuffers32[ch];
                if (!std::all_of(samples, samples + kBlockSize,
                                 [level](float sample) { return sample == level; })) {
                    lastError_ = "Publish passthrough changed audio samples";
                    return false;
                }
                if (level != 0.0f && (outputBus.silenceFlags & (uint64{1} << ch))) {
                    lastError_ = "Nonzero output is still marked silent";
                    return false;
                }
            }
        }
        return true;
    }

    bool checkAudioFormats() {
        if (processor_->canProcessSampleSize(kSample64) == kResultTrue) { lastError_ = "Unexpected float64 support"; return false; }
        double worstUs = 0;
        for (double rate : {8000., 16000., 22050., 44100., 48000., 88200., 96000., 192000.}) {
            for (int32 frames : {1, 16, 64, 127, 256, 960, 1024, 4096}) {
                for (int32 channels : {1, 2}) {
                    ProcessSetup setup{};
                    setup.processMode = kOffline; setup.sampleRate = rate;
                    setup.symbolicSampleSize = kSample32; setup.maxSamplesPerBlock = frames;
                    if (processor_->setupProcessing(setup) != kResultOk) { lastError_ = "Valid audio setup rejected"; return false; }
                    SpeakerArrangement arrangement = channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
                    if (processor_->setBusArrangements(&arrangement, 1, &arrangement, 1) != kResultOk) return false;
                    std::vector<float> left(frames + 2, 0.25f), right(frames + 2, -0.5f);
                    float* pointers[] = {left.data() + 1, right.data() + 1};
                    AudioBusBuffers bus{}; bus.numChannels = channels; bus.channelBuffers32 = pointers;
                    ProcessData data{}; data.processMode = kOffline; data.symbolicSampleSize = kSample32;
                    data.numSamples = frames; data.numInputs = data.numOutputs = 1; data.inputs = data.outputs = &bus;
                    const auto began = std::chrono::steady_clock::now();
                    for (int n = 0; n < 100; ++n) {
                        pointers[0][0] = std::numeric_limits<float>::quiet_NaN();
                        if (channels == 2) pointers[1][0] = std::numeric_limits<float>::infinity();
                        if (processor_->process(data) != kResultOk) return false;
                        for (int ch = 0; ch < channels; ++ch)
                            for (int i = 0; i < frames; ++i)
                                if (!std::isfinite(pointers[ch][i])) { lastError_ = "Nonfinite output escaped"; return false; }
                    }
                    worstUs = std::max(worstUs, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count() / 100);
                    if (left.front() != 0.25f || left.back() != 0.25f || right.front() != -0.5f || right.back() != -0.5f) {
                        lastError_ = "Audio write exceeded block bounds"; return false;
                    }
                    data.numSamples = 0;
                    if (processor_->process(data) != kResultOk) return false;
                    data.numSamples = -1;
                    if (processor_->process(data) == kResultOk) { lastError_ = "Negative block accepted"; return false; }
                    data.numSamples = frames; data.symbolicSampleSize = kSample64;
                    if (processor_->process(data) == kResultOk) return false;
                }
            }
        }
        SpeakerArrangement surround = SpeakerArr::k51;
        if (processor_->setBusArrangements(&surround, 1, &surround, 1) == kResultOk) {
            lastError_ = "Unsupported surround accepted"; return false;
        }
        ProcessSetup invalid{}; invalid.sampleRate = std::numeric_limits<double>::quiet_NaN(); invalid.maxSamplesPerBlock = 256;
        if (processor_->setupProcessing(invalid) == kResultOk) return false;
        std::cout << "  128 format/block/channel combinations, worst average process+check=" << worstUs << "us\n";
        return true;
    }

    void cleanup() {
        if (active_) {
            deactivate();
        }

        processor_ = nullptr;

        if (provider_) {
            auto* componentRaw = component_.take();
            auto* controllerRaw = controller_.take();
            provider_->releasePlugIn(componentRaw, controllerRaw);
            provider_.reset();
        }

        module_.reset();
    }

    const std::string& getLastError() const { return lastError_; }

private:
    std::string pluginPath_;
    std::string lastError_;
    std::shared_ptr<VST3::Hosting::Module> module_;
    std::unique_ptr<PlugProvider> provider_;
    IPtr<IComponent> component_;
    IPtr<IAudioProcessor> processor_;
    IPtr<IEditController> controller_;
    bool active_{false};

    std::vector<std::vector<float>> inputBuffers_;
    std::vector<std::vector<float>> outputBuffers_;
    std::vector<float*> inputPtrs_;
    std::vector<float*> outputPtrs_;
};

// ============================================================================
// Test Cases
// ============================================================================

TestResult test_basic_load_unload(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) {
        return {"BasicLoadUnload", false, host.getLastError()};
    }

    host.cleanup();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    return {"BasicLoadUnload", true, "", duration};
}

TestResult test_activate_deactivate(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) {
        return {"ActivateDeactivate", false, host.getLastError()};
    }

    if (!host.setupProcessing()) {
        return {"ActivateDeactivate", false, host.getLastError()};
    }

    if (!host.activate()) {
        return {"ActivateDeactivate", false, host.getLastError()};
    }

    if (!host.deactivate()) {
        return {"ActivateDeactivate", false, host.getLastError()};
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    return {"ActivateDeactivate", true, "", duration};
}

TestResult test_process_audio(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) {
        return {"ProcessAudio", false, host.getLastError()};
    }

    if (!host.setupProcessing()) {
        return {"ProcessAudio", false, host.getLastError()};
    }

    if (!host.activate()) {
        return {"ProcessAudio", false, host.getLastError()};
    }

    // Process 100 blocks (~1 second of audio)
    if (!host.process(100)) {
        return {"ProcessAudio", false, host.getLastError()};
    }

    if (!host.deactivate()) {
        return {"ProcessAudio", false, host.getLastError()};
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    return {"ProcessAudio", true, "", duration};
}

TestResult test_rapid_open_close(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    constexpr int iterations = 50;

    for (int i = 0; i < iterations; ++i) {
        VST3HostSimulator host(pluginPath);
        if (!host.loadPlugin()) {
            return {"RapidOpenClose", false, "Failed on iteration " + std::to_string(i) + ": " + host.getLastError()};
        }

        if (!host.setupProcessing()) {
            return {"RapidOpenClose", false, "setupProcessing failed on iteration " + std::to_string(i)};
        }

        if (!host.activate()) {
            return {"RapidOpenClose", false, "activate failed on iteration " + std::to_string(i)};
        }

        if (!host.deactivate()) {
            return {"RapidOpenClose", false, "deactivate failed on iteration " + std::to_string(i)};
        }

        host.cleanup();

        // Small delay between iterations
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    return {"RapidOpenClose(" + std::to_string(iterations) + "x)", true, "", duration};
}

TestResult test_process_while_deactivating(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) {
        return {"ProcessWhileDeactivating", false, host.getLastError()};
    }

    if (!host.setupProcessing()) {
        return {"ProcessWhileDeactivating", false, host.getLastError()};
    }

    if (!host.activate()) {
        return {"ProcessWhileDeactivating", false, host.getLastError()};
    }

    // Process a few blocks
    if (!host.process(5)) {
        return {"ProcessWhileDeactivating", false, host.getLastError()};
    }

    // Immediately deactivate (simulates closing plugin while audio is playing)
    if (!host.deactivate()) {
        return {"ProcessWhileDeactivating", false, host.getLastError()};
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    return {"ProcessWhileDeactivating", true, "", duration};
}

TestResult test_long_running_session(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) {
        return {"LongRunningSession", false, host.getLastError()};
    }

    if (!host.setupProcessing()) {
        return {"LongRunningSession", false, host.getLastError()};
    }

    if (!host.activate()) {
        return {"LongRunningSession", false, host.getLastError()};
    }

    // Process 1000 blocks (~10 seconds of audio)
    if (!host.process(1000)) {
        return {"LongRunningSession", false, host.getLastError()};
    }

    if (!host.deactivate()) {
        return {"LongRunningSession", false, host.getLastError()};
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    return {"LongRunningSession(10s)", true, "", duration};
}

TestResult test_room_view_link_uses_solo(const std::string& pluginPath) {
    auto start = std::chrono::high_resolution_clock::now();

    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) {
        return {"RoomViewLinkUsesSolo", false, host.getLastError()};
    }

    if (!host.injectState(R"({"mode":"publish","streamId":"room-seed","roomName":"mix-room","handshakeUrl":"wss://wss.vdo.ninja","webBaseUrl":"https://vdo.ninja/","salt":""})")) {
        return {"RoomViewLinkUsesSolo", false, host.getLastError()};
    }

    std::string viewLink;
    if (!host.readStringParam(webrtc_vst::kParamPushLink, viewLink)) {
        return {"RoomViewLinkUsesSolo", false, host.getLastError()};
    }

    const std::string expectedViewLink = "https://vdo.ninja/?view=room-seed&style=2&room=mix-room&solo";
    if (viewLink != expectedViewLink) {
        return {"RoomViewLinkUsesSolo", false, "Unexpected view link: " + viewLink};
    }

    if (!host.injectState(R"({"mode":"play","streamId":"return-feed","roomName":"mix-room","handshakeUrl":"wss://wss.vdo.ninja","webBaseUrl":"https://vdo.ninja/","salt":""})")) {
        return {"RoomViewLinkUsesSolo", false, host.getLastError()};
    }

    std::string pushLink;
    if (!host.readStringParam(webrtc_vst::kParamPushLink, pushLink)) {
        return {"RoomViewLinkUsesSolo", false, host.getLastError()};
    }

    const std::string expectedPushLink = "https://vdo.ninja/?push=return-feed&room=mix-room";
    if (pushLink != expectedPushLink) {
        return {"RoomViewLinkUsesSolo", false, "Unexpected push link: " + pushLink};
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    return {"RoomViewLinkUsesSolo", true, "", duration};
}

// ============================================================================
// Main Test Runner
// ============================================================================

TestResult test_advanced_settings(const std::string& pluginPath) {
    const std::string name = "AdvancedSettingsAndLinks";
    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) return {name, false, host.getLastError()};
    struct Case { const char* state; const char* link; };
    const Case cases[] = {
        {R"({"streamId":"s","mode":"publish","password":"secret","webBaseUrl":"a.test","salt":"z","handshakeUrl":"wss://w.test"})",
         "https://a.test/?view=s&style=2&hash=ce58&salt=z&wss2=wss%3A%2F%2Fw.test"},
        {R"({"streamId":"s","mode":"play","password":"secret","webBaseUrl":"a.test","salt":"","handshakeUrl":"wss://w.test"})",
         "https://a.test/?push=s&hash=b739&wss2=wss%3A%2F%2Fw.test"},
        {R"({"streamId":"s","password":"secret","webBaseUrl":"a.test","salt":"a &+/#","handshakeUrl":"wss://w.test"})",
         "https://a.test/?push=s&hash=d3d4&salt=a%20%26%2B%2F%23&wss2=wss%3A%2F%2Fw.test"},
        {R"({"streamId":"s","password":"off","webBaseUrl":"","salt":"","handshakeUrl":""})",
         "https://vdo.ninja/?push=s&password=false"},
        // A legacy project must retain WSS-derived salt, not the last loaded salt.
        {R"({"streamId":"s","password":"secret","signalingUrl":"wss://a.test"})",
         "https://vdo.ninja/?push=s&hash=b739&salt=a.test&wss2=wss%3A%2F%2Fa.test"},
        {R"({"streamId":"s","password":"secret","handshakeUrl":"wss://wss.vdo.ninja"})",
         "https://vdo.ninja/?push=s&hash=ecdb"},
        // Official backup servers use the same signaling protocol; &wss would
        // incorrectly select the browser's generic-relay protocol instead.
        {R"({"streamId":"s","mode":"publish","password":"secret","webBaseUrl":"backup.vdo.ninja","salt":"SALT","handshakeUrl":"wss://apibackup.vdo.ninja/"})",
         "https://backup.vdo.ninja/?view=s&style=2&hash=fd9d&salt=SALT&wss2=wss%3A%2F%2Fapibackup.vdo.ninja%2F"},
        {R"({"streamId":"s","mode":"play","password":"secret","webBaseUrl":"backup.vdo.ninja","salt":"SALT","handshakeUrl":"wss://apibackup.vdo.ninja/"})",
         "https://backup.vdo.ninja/?push=s&hash=fd9d&salt=SALT&wss2=wss%3A%2F%2Fapibackup.vdo.ninja%2F"}
    };
    for (const auto& item : cases) {
        host.injectState(item.state);
        std::string link;
        if (!host.readStringParam(webrtc_vst::kParamPushLink, link) || link != item.link)
            return {name, false, "Unexpected link: " + link};
        const auto componentState = host.savedState();
        const auto controllerState = host.savedState(true);
        for (const auto& state : {componentState, controllerState}) {
            if (state.find("\"salt\":") == std::string::npos || state.find("\"webBaseUrl\":") == std::string::npos)
                return {name, false, "Advanced fields missing from saved state"};
            VST3HostSimulator restored(pluginPath);
            if (!restored.loadPlugin() || !restored.injectState(state) ||
                !restored.readStringParam(webrtc_vst::kParamPushLink, link) || link != item.link)
                return {name, false, "State round-trip changed link: " + link};
        }
    }
    if (host.acceptsText(webrtc_vst::kParamWebBaseUrl, u"javascript:alert(1)") ||
        host.acceptsText(webrtc_vst::kParamWebBaseUrl, u"https://a.test/?view=evil") ||
        host.acceptsText(webrtc_vst::kParamHandshakeUrl, u"https://w.test") ||
        !host.acceptsText(webrtc_vst::kParamWebBaseUrl, u"a.test") ||
        !host.acceptsText(webrtc_vst::kParamHandshakeUrl, u"w.test:443/socket"))
        return {name, false, "Endpoint editor validation failed"};
    const auto before = host.savedState();
    host.injectState(R"({"webBaseUrl":"file:///tmp","salt":"changed","handshakeUrl":"wss://other.test"})");
    if (host.savedState() != before) return {name, false, "Invalid endpoint changed saved settings"};
    return {name, true, "Custom/automatic/legacy salts, links, state round trips and input validation"};
}

TestResult test_publish_silence_flags(const std::string& pluginPath, bool inPlace) {
    const std::string name = inPlace ? "PublishSilenceFlagsInPlace" : "PublishSilenceFlags";
    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin() ||
        !host.injectState(R"({"mode":"publish","streamId":"silence-test","handshakeUrl":"ws://127.0.0.1:1"})") ||
        !host.setupProcessing() || !host.activate() ||
        !host.checkPublishSilenceFlags(inPlace) || !host.deactivate()) {
        return {name, false, host.getLastError()};
    }
    return {name, true};
}

TestResult test_atomic_invalid_state(const std::string& pluginPath) {
    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin() || !host.injectState(R"({"streamId":"original","password":"secret","webBaseUrl":"https://vdo.ninja/","salt":"original-salt","handshakeUrl":"wss://wss.vdo.ninja"})"))
        return {"AtomicInvalidState", false, "Setup failed"};
    const auto componentBefore = host.savedState();
    if (!host.requestsRealtimeHostProcessing()) return {"AtomicInvalidState", false, "Live bridge did not opt out of host prefetch"};
    const auto controllerBefore = host.savedState(true);
    if (!host.rejectsShortStateWrites()) return {"AtomicInvalidState", false, "Short preset write reported success"};
    const std::vector<std::string> invalid = {
        R"({"streamId":"changed","webBaseUrl":"https://other.test/","salt":"new","handshakeUrl":"wss://other.test/","password":[]})",
        R"({"streamId":"changed","roomName":42})",
        R"({"streamId":"changed","mode":false})",
        R"({"streamId":"changed","mode":"typo"})",
        "{\"streamId\":\"changed\",\"salt\":\"" + std::string(100000, 'x') + "\"}",
        R"({"streamId":"changed","handshakeUrl":"wss://a..test"})",
        R"({"streamId":"changed","handshakeUrl":"wss://[abcd]"})"
    };
    for (size_t i = 0; i < invalid.size(); ++i) {
        host.injectState(invalid[i]);
        if (host.savedState() != componentBefore || host.savedState(true) != controllerBefore)
            return {"AtomicInvalidState", false, "Invalid case " + std::to_string(i) + " partially applied"};
    }
    return {"AtomicInvalidState", true};
}

TestResult test_edit_memory(const std::string& pluginPath) {
    VST3HostSimulator host(pluginPath);
    if (!host.loadPlugin()) return {"BoundedStringEdits", false, "Cannot load"};
#if defined(__APPLE__)
    auto resident = []() -> uint64_t {
        mach_task_basic_info info{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return 0;
        return info.resident_size;
    };
    for (int i = 0; i < 1000; ++i) host.editText(webrtc_vst::kParamStreamId, "warmup" + std::to_string(i));
    const auto before = resident();
    for (int i = 0; i < 30000; ++i) {
        if (!host.editText(webrtc_vst::kParamStreamId, "churn" + std::string(85, 'x') + std::to_string(i)))
            return {"BoundedStringEdits", false, "Text edit failed"};
    }
    const auto after = resident();
    const auto growth = after > before ? after - before : 0;
    std::cout << "  String edit RSS growth=" << growth << " bytes\n";
    if (!before || !after || growth > 12 * 1024 * 1024)
        return {"BoundedStringEdits", false, "String edit memory grew beyond 12 MiB"};
#endif
    return {"BoundedStringEdits", true};
}

TestResult test_atomic_advanced_apply(const std::string& pluginPath) {
    VST3HostSimulator host(pluginPath), other(pluginPath);
    if (!host.loadPlugin() || !other.loadPlugin() ||
        !host.injectState(R"({"streamId":"s","mode":"seed","password":"secret","salt":"old","webBaseUrl":"https://vdo.ninja/","handshakeUrl":"wss://wss.vdo.ninja"})"))
        return {"AtomicAdvancedApply", false, "Cannot load baseline"};
    const auto before = host.savedState(), otherBefore = other.savedState();
    std::string linkBefore;
    host.readStringParam(webrtc_vst::kParamPushLink, linkBefore);
    if (!host.editText(webrtc_vst::kParamWebBaseUrl, "backup.vdo.ninja") ||
        !host.editText(webrtc_vst::kParamSalt, "SALT") ||
        !host.editText(webrtc_vst::kParamHandshakeUrl, "wss://apibackup.vdo.ninja/"))
        return {"AtomicAdvancedApply", false, "Editor rejected valid draft"};
    std::string draftLink;
    host.readStringParam(webrtc_vst::kParamPushLink, draftLink);
    if (host.savedState() != before || draftLink != linkBefore)
        return {"AtomicAdvancedApply", false, "Draft changed active settings/link before Apply"};
    if (!host.action(webrtc_vst::kParamApplyAdvanced)) return {"AtomicAdvancedApply", false, "Apply failed"};
    const auto after = host.savedState();
    if (after.find("\"salt\":\"SALT\"") == std::string::npos ||
        after.find("https://backup.vdo.ninja/") == std::string::npos ||
        after.find("wss://apibackup.vdo.ninja/") == std::string::npos)
        return {"AtomicAdvancedApply", false, "Config message did not update the processor"};
    if (!host.editText(webrtc_vst::kParamStreamId, "edited") || host.savedState().find("\"streamId\":\"edited\"") == std::string::npos)
        return {"AtomicAdvancedApply", false, "Normal text edit did not reach processor"};
    if (other.savedState() != otherBefore) return {"AtomicAdvancedApply", false, "Other instance was affected"};
    return {"AtomicAdvancedApply", true};
}

TestResult test_live_peer_teardown(const std::string& pluginPath) {
    const char* value = std::getenv("WEBRTC_VST_TEST_LIVE_STREAM");
    const std::string streamId = value ? value : "";
    if (streamId.empty() || !std::all_of(streamId.begin(), streamId.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_';
        })) {
        return {"LivePeerTeardown", false, "Set WEBRTC_VST_TEST_LIVE_STREAM to an owned tone publisher ID"};
    }
    // A DAW retains the module while replacing live instances. Keep the code
    // loaded while asynchronous peer teardown finishes between replacements.
    std::string error;
    const auto module = VST3::Hosting::Module::create(pluginPath, error);
    if (!module) return {"LivePeerTeardown", false, error};
    for (int cycle = 0; cycle < 4; ++cycle) {
        VST3HostSimulator host(pluginPath);
        if (!host.loadPlugin() || !host.injectState(
                "{\"mode\":\"play\",\"streamId\":\"" + streamId +
                "\",\"handshakeUrl\":\"wss://wss.vdo.ninja\",\"password\":\"\"}") ||
            !host.setupProcessing() || !host.activate()) {
            return {"LivePeerTeardown", false, host.getLastError()};
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        int audibleBlocks = 0;
        while (std::chrono::steady_clock::now() < deadline && audibleBlocks < 50) {
            if (!host.process(1)) return {"LivePeerTeardown", false, host.getLastError()};
            if (host.hasOutputSignal()) ++audibleBlocks;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (audibleBlocks < 50) return {"LivePeerTeardown", false, "Live audio was not sustained before teardown"};
        host.cleanup();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Live peer teardown cycle " << cycle + 1 << "/4 passed" << std::endl;
    }
    return {"LivePeerTeardown", true};
}

int main(int argc, char** argv) {
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "WebRTC VST3 Plugin - Integration Tests" << std::endl;
    std::cout << "Simulates real VST3 host behavior (Audacity-like)" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    // Register host context (required for plugin initialization)
    static HostApplication hostApp;
    PluginContextFactory::instance().setPluginContext(&hostApp);

    // Find plugin path
    std::string pluginPath;
    if (argc > 1) {
        pluginPath = argv[1];
    } else if (const char* env = std::getenv("WEBRTC_VST_PLUGIN_PATH")) {
        pluginPath = env;
    } else {
        pluginPath = WEBRTC_VST_DEFAULT_PLUGIN_PATH;
    }

    std::cout << "Plugin path: " << pluginPath << std::endl;

    if (!std::filesystem::exists(pluginPath)) {
        std::cerr << "Error: Plugin not found at " << pluginPath << std::endl;
        std::cerr << "Usage: " << argv[0] << " [plugin_path]" << std::endl;
        std::cerr << "   or set WEBRTC_VST_PLUGIN_PATH environment variable" << std::endl;
        return 1;
    }

    std::cout << "\nRunning tests..." << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    TestSuite suite;

    if (argc > 2 && std::string(argv[2]) == "--live-teardown") {
        suite.addResult(test_live_peer_teardown(pluginPath));
        suite.printSummary();
        return suite.allPassed() ? 0 : 1;
    }

    // Run tests
    suite.addResult(test_basic_load_unload(pluginPath));
    suite.addResult(test_activate_deactivate(pluginPath));
    suite.addResult(test_process_audio(pluginPath));
    suite.addResult(test_rapid_open_close(pluginPath));
    suite.addResult(test_process_while_deactivating(pluginPath));
    suite.addResult(test_long_running_session(pluginPath));
    suite.addResult(test_room_view_link_uses_solo(pluginPath));
    suite.addResult(test_advanced_settings(pluginPath));
    suite.addResult(test_atomic_invalid_state(pluginPath));
    suite.addResult(test_edit_memory(pluginPath));
    suite.addResult(test_atomic_advanced_apply(pluginPath));
    {
        VST3HostSimulator host(pluginPath);
        const bool passed = host.loadPlugin() && host.injectState(R"({"mode":"seed","streamId":"format_owned","password":"off"})") && host.checkAudioFormats();
        suite.addResult({"AudioFormatMatrix", passed, passed ? "" : host.getLastError()});
    }
    suite.addResult(test_publish_silence_flags(pluginPath, false));
    suite.addResult(test_publish_silence_flags(pluginPath, true));

    // Print summary
    suite.printSummary();

    return suite.allPassed() ? 0 : 1;
}
