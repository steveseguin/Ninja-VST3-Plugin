// Fuzz Test for WebRTC VST Plugin
// Tests mode toggling, reconnection during streaming, and duplicate stream IDs.
// Exercises the real signaling path against wss://wss.vdo.ninja.

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/vsttypes.h>

#include "../../webrtc_vst/src/ParameterIDs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// ============================================================================
// Helpers
// ============================================================================

constexpr double kSampleRate = 48000.0;
constexpr int32 kBlockSize = 256;
constexpr int32 kNumChannels = 2;
constexpr double kTwoPi = 6.283185307179586476925286766559;

class StringStream : public IBStream {
public:
    explicit StringStream(const std::string& data) : data_(data) {}
    tresult PLUGIN_API queryInterface(const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override {
        if (--refCount_ == 0) { delete this; return 0; }
        return refCount_;
    }
    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        int32 avail = static_cast<int32>(data_.size()) - pos_;
        int32 toRead = std::min(numBytes, avail);
        if (toRead > 0) std::memcpy(buffer, data_.data() + pos_, static_cast<size_t>(toRead));
        pos_ += toRead;
        if (numBytesRead) *numBytesRead = toRead;
        return kResultOk;
    }
    tresult PLUGIN_API write(void*, int32, int32*) override { return kNotImplemented; }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        if (mode == kIBSeekSet) pos_ = static_cast<int32>(pos);
        else if (mode == kIBSeekCur) pos_ += static_cast<int32>(pos);
        else if (mode == kIBSeekEnd) pos_ = static_cast<int32>(data_.size()) + static_cast<int32>(pos);
        if (result) *result = pos_;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (pos) *pos = pos_;
        return kResultOk;
    }
private:
    std::string data_;
    int32 pos_{0};
    std::atomic<uint32> refCount_{1};
};

// Minimal IParamValueQueue to inject a single parameter change per block
class SingleParamQueue : public IParamValueQueue {
public:
    SingleParamQueue(ParamID id, ParamValue value)
        : id_(id), value_(value) {}

    tresult PLUGIN_API queryInterface(const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override {
        if (index != 0) return kResultFalse;
        sampleOffset = 0;
        value = value_;
        return kResultTrue;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kNotImplemented; }

private:
    ParamID id_;
    ParamValue value_;
};

// Minimal IParameterChanges holding up to 4 queues
class ParamChanges : public IParameterChanges {
public:
    tresult PLUGIN_API queryInterface(const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    int32 PLUGIN_API getParameterCount() override { return count_; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index < 0 || index >= count_) return nullptr;
        return &queues_[static_cast<size_t>(index)];
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override { return nullptr; }

    void add(ParamID id, ParamValue value) {
        if (count_ < 4) {
            queues_[static_cast<size_t>(count_)] = SingleParamQueue(id, value);
            ++count_;
        }
    }

    void clear() { count_ = 0; }

private:
    std::array<SingleParamQueue, 4> queues_{
        SingleParamQueue{0, 0.0},
        SingleParamQueue{0, 0.0},
        SingleParamQueue{0, 0.0},
        SingleParamQueue{0, 0.0}
    };
    int32 count_{0};
};

std::string string128ToAscii(const String128 text) {
    std::string result;
    for (int i = 0; i < 128 && text[i] != 0; ++i) {
        result.push_back(static_cast<uint32_t>(text[i]) <= 0x7F
                             ? static_cast<char>(text[i])
                             : '?');
    }
    return result;
}

// ============================================================================
// Extended Host Simulator with parameter injection and status reading
// ============================================================================

class FuzzHost {
public:
    explicit FuzzHost(const std::string& pluginPath)
        : pluginPath_(pluginPath) {}

    ~FuzzHost() { cleanup(); }

    bool load() {
        std::string error;
        module_ = VST3::Hosting::Module::create(pluginPath_, error);
        if (!module_) { err_ = "Load: " + error; return false; }

        const auto& factory = module_->getFactory();
        auto infos = factory.classInfos();
        if (infos.empty()) { err_ = "No classes"; return false; }

        provider_ = std::make_unique<PlugProvider>(factory, infos.front(), true);
        if (!provider_->initialize()) { err_ = "PlugProvider init failed"; return false; }

        component_ = provider_->getComponentPtr();
        if (!component_) { err_ = "No component"; return false; }

        IAudioProcessor* raw = nullptr;
        if (component_->queryInterface(IAudioProcessor::iid,
                                       reinterpret_cast<void**>(&raw)) != kResultOk) {
            err_ = "No IAudioProcessor";
            return false;
        }
        processor_ = IPtr<IAudioProcessor>(raw, false);
        controller_ = provider_->getControllerPtr();
        return true;
    }

    bool setup() {
        ProcessSetup ps{};
        ps.processMode = kRealtime;
        ps.symbolicSampleSize = kSample32;
        ps.maxSamplesPerBlock = kBlockSize;
        ps.sampleRate = kSampleRate;
        if (processor_->setupProcessing(ps) != kResultOk) {
            err_ = "setupProcessing failed";
            return false;
        }

        for (int ch = 0; ch < kNumChannels; ++ch) {
            inputBufs_[ch].assign(kBlockSize, 0.0f);
            outputBufs_[ch].assign(kBlockSize, 0.0f);
            inputPtrs_[ch] = inputBufs_[ch].data();
            outputPtrs_[ch] = outputBufs_[ch].data();
        }
        return true;
    }

    bool activate() {
        if (component_->setActive(true) != kResultOk) {
            err_ = "setActive(true) failed";
            return false;
        }
        active_ = true;
        return true;
    }

    bool deactivate() {
        if (!active_) return true;
        active_ = false;
        return component_->setActive(false) == kResultOk;
    }

    void injectState(const std::string& json) {
        auto* s = new StringStream(json);
        component_->setState(s);
        s->release();
        if (controller_) {
            auto* s2 = new StringStream(json);
            controller_->setComponentState(s2);
            s2->release();
        }
    }

    // Process one block, optionally injecting parameter changes.
    bool processBlock(ParamChanges* changes = nullptr) {
        if (!processor_ || !active_) {
            err_ = "Not ready";
            return false;
        }

        // Fill input with tone
        for (int s = 0; s < kBlockSize; ++s) {
            float v = 0.25f * static_cast<float>(std::sin(tonePhase_));
            for (int ch = 0; ch < kNumChannels; ++ch)
                inputBufs_[ch][s] = v;
            tonePhase_ += kTwoPi * 1000.0 / kSampleRate;
            if (tonePhase_ >= kTwoPi) tonePhase_ -= kTwoPi;
        }

        for (int ch = 0; ch < kNumChannels; ++ch)
            std::fill(outputBufs_[ch].begin(), outputBufs_[ch].end(), 0.0f);

        AudioBusBuffers inBus{};
        inBus.numChannels = kNumChannels;
        inBus.channelBuffers32 = inputPtrs_.data();

        AudioBusBuffers outBus{};
        outBus.numChannels = kNumChannels;
        outBus.channelBuffers32 = outputPtrs_.data();

        ProcessContext ctx{};
        ctx.state = ProcessContext::kPlaying;
        ctx.sampleRate = kSampleRate;
        ctx.projectTimeSamples = projectTime_;

        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = kBlockSize;
        data.numInputs = 1;
        data.inputs = &inBus;
        data.numOutputs = 1;
        data.outputs = &outBus;
        data.processContext = &ctx;
        data.inputParameterChanges = changes;

        auto result = processor_->process(data);
        projectTime_ += kBlockSize;
        if (result != kResultOk) {
            err_ = "process() failed";
            return false;
        }
        return true;
    }

    std::string readStatus() const {
        if (!controller_) return "";
        String128 text{};
        auto norm = controller_->getParamNormalized(webrtc_vst::kParamStatus);
        if (controller_->getParamStringByValue(webrtc_vst::kParamStatus, norm, text) == kResultOk) {
            return string128ToAscii(text);
        }
        return "";
    }

    void cleanup() {
        if (active_) deactivate();
        processor_ = nullptr;
        if (provider_) {
            auto* c = component_.take();
            auto* e = controller_.take();
            provider_->releasePlugIn(c, e);
            provider_.reset();
        }
        module_.reset();
    }

    const std::string& err() const { return err_; }

private:
    std::string pluginPath_;
    std::string err_;
    std::shared_ptr<VST3::Hosting::Module> module_;
    std::unique_ptr<PlugProvider> provider_;
    IPtr<IComponent> component_;
    IPtr<IAudioProcessor> processor_;
    IPtr<IEditController> controller_;
    bool active_{false};
    double tonePhase_{0.0};
    int64 projectTime_{0};

    std::array<std::vector<float>, kNumChannels> inputBufs_;
    std::array<std::vector<float>, kNumChannels> outputBufs_;
    std::array<float*, kNumChannels> inputPtrs_{};
    std::array<float*, kNumChannels> outputPtrs_{};
};

// ============================================================================
// Test result
// ============================================================================

struct TestResult {
    std::string name;
    bool passed;
    std::string detail;
    double ms;
};

void printResult(const TestResult& r) {
    if (r.passed) {
        std::cout << "  PASS " << r.name << " (" << r.ms << "ms)" << std::endl;
    } else {
        std::cout << "  FAIL " << r.name << ": " << r.detail << std::endl;
    }
}

// ============================================================================
// Test 1: Rapid mode toggling while processing audio
// Simulates a user clicking Publish/Play back and forth while audio flows.
// ============================================================================

TestResult test_mode_toggle_while_streaming(const std::string& path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    FuzzHost host(path);
    if (!host.load() || !host.setup() || !host.activate()) {
        return {"ModeToggleWhileStreaming", false, host.err(), 0};
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, 30); // blocks between toggles
    bool publishMode = false; // start in Play (0.0)

    int toggleCount = 0;
    constexpr int kTotalBlocks = 4000; // ~21 seconds of audio

    for (int block = 0; block < kTotalBlocks; ++block) {
        // Randomly toggle mode
        if (dist(rng) == 1) {
            publishMode = !publishMode;
            ParamChanges changes;
            changes.add(webrtc_vst::kParamMode, publishMode ? 1.0 : 0.0);
            if (!host.processBlock(&changes)) {
                return {"ModeToggleWhileStreaming", false,
                        "process failed at block " + std::to_string(block) + " after " +
                        std::to_string(toggleCount) + " toggles: " + host.err(), 0};
            }
            ++toggleCount;
        } else {
            if (!host.processBlock()) {
                return {"ModeToggleWhileStreaming", false,
                        "process failed at block " + std::to_string(block) + ": " + host.err(), 0};
            }
        }
    }

    host.deactivate();

    auto dt = std::chrono::duration<double, std::milli>(
                  std::chrono::high_resolution_clock::now() - t0).count();
    return {"ModeToggleWhileStreaming(" + std::to_string(toggleCount) + " toggles, " +
            std::to_string(kTotalBlocks) + " blocks)", true, "", dt};
}

// ============================================================================
// Test 2: Rapid mode toggling at maximum rate (every block)
// Worst-case scenario: toggle every single process() call.
// ============================================================================

TestResult test_mode_toggle_every_block(const std::string& path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    FuzzHost host(path);
    if (!host.load() || !host.setup() || !host.activate()) {
        return {"ModeToggleEveryBlock", false, host.err(), 0};
    }

    constexpr int kBlocks = 2000;
    for (int i = 0; i < kBlocks; ++i) {
        ParamChanges changes;
        changes.add(webrtc_vst::kParamMode, (i % 2 == 0) ? 1.0 : 0.0);
        if (!host.processBlock(&changes)) {
            return {"ModeToggleEveryBlock", false,
                    "process failed at block " + std::to_string(i) + ": " + host.err(), 0};
        }
    }

    host.deactivate();

    auto dt = std::chrono::duration<double, std::milli>(
                  std::chrono::high_resolution_clock::now() - t0).count();
    return {"ModeToggleEveryBlock(" + std::to_string(kBlocks) + " blocks)", true, "", dt};
}

// ============================================================================
// Test 3: Reconnect simulation — deactivate/reactivate mid-stream
// Simulates host stop/start (like Reaper play/stop) while connected.
// ============================================================================

TestResult test_reconnect_during_streaming(const std::string& path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    FuzzHost host(path);
    if (!host.load() || !host.setup()) {
        return {"ReconnectDuringStreaming", false, host.err(), 0};
    }

    // Inject Publish mode with a stream ID
    host.injectState(R"({"mode":"publish","streamId":"fuzz-reconnect-test"})");

    constexpr int kCycles = 20;
    constexpr int kBlocksPerCycle = 200; // ~1 second

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        if (!host.activate()) {
            return {"ReconnectDuringStreaming", false,
                    "activate failed at cycle " + std::to_string(cycle) + ": " + host.err(), 0};
        }

        for (int b = 0; b < kBlocksPerCycle; ++b) {
            if (!host.processBlock()) {
                return {"ReconnectDuringStreaming", false,
                        "process failed at cycle " + std::to_string(cycle) +
                        " block " + std::to_string(b) + ": " + host.err(), 0};
            }
        }

        if (!host.deactivate()) {
            return {"ReconnectDuringStreaming", false,
                    "deactivate failed at cycle " + std::to_string(cycle) + ": " + host.err(), 0};
        }

        // Brief pause simulating user pressing stop then play again
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto dt = std::chrono::duration<double, std::milli>(
                  std::chrono::high_resolution_clock::now() - t0).count();
    return {"ReconnectDuringStreaming(" + std::to_string(kCycles) + " cycles)",
            true, "", dt};
}

// ============================================================================
// Test 4: Dual instance with same stream ID (both publishing)
// Second instance should not crash. We check that both survive and that
// the status is readable (ideally one would report an error/conflict).
// ============================================================================

TestResult test_duplicate_stream_id(const std::string& path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    FuzzHost host1(path);
    FuzzHost host2(path);

    if (!host1.load() || !host1.setup()) {
        return {"DuplicateStreamId", false, "host1 setup: " + host1.err(), 0};
    }
    if (!host2.load() || !host2.setup()) {
        return {"DuplicateStreamId", false, "host2 setup: " + host2.err(), 0};
    }

    const std::string streamId = "fuzz-duplicate-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);

    const std::string state = R"({"mode":"publish","streamId":")" + streamId + R"("})";
    host1.injectState(state);
    host2.injectState(state);

    if (!host1.activate()) {
        return {"DuplicateStreamId", false, "host1 activate: " + host1.err(), 0};
    }
    if (!host2.activate()) {
        return {"DuplicateStreamId", false, "host2 activate: " + host2.err(), 0};
    }

    // Both instances process concurrently for ~3 seconds
    constexpr int kBlocks = 600;
    std::atomic<bool> host1Failed{false};
    std::atomic<bool> host2Failed{false};
    std::string host1Err, host2Err;

    std::thread t1([&]() {
        for (int b = 0; b < kBlocks; ++b) {
            if (!host1.processBlock()) {
                host1Err = host1.err();
                host1Failed = true;
                return;
            }
        }
    });

    std::thread t2([&]() {
        for (int b = 0; b < kBlocks; ++b) {
            if (!host2.processBlock()) {
                host2Err = host2.err();
                host2Failed = true;
                return;
            }
        }
    });

    t1.join();
    t2.join();

    if (host1Failed) {
        return {"DuplicateStreamId", false, "host1 process: " + host1Err, 0};
    }
    if (host2Failed) {
        return {"DuplicateStreamId", false, "host2 process: " + host2Err, 0};
    }

    // Read status from both — log for visibility
    const auto status1 = host1.readStatus();
    const auto status2 = host2.readStatus();

    host1.deactivate();
    host2.deactivate();
    host1.cleanup();
    host2.cleanup();

    auto dt = std::chrono::duration<double, std::milli>(
                  std::chrono::high_resolution_clock::now() - t0).count();

    std::string detail = "stream=" + streamId;
    if (!status1.empty()) detail += " host1_status=\"" + status1 + "\"";
    if (!status2.empty()) detail += " host2_status=\"" + status2 + "\"";

    return {"DuplicateStreamId(" + streamId + ")", true, detail, dt};
}

// ============================================================================
// Test 5: Mode toggle during live connection
// Start in Publish, wait for signaling, toggle to Play then back to Publish.
// Exercises the full teardown-and-rebuild of the WebRTC peer sessions.
// ============================================================================

TestResult test_mode_toggle_live(const std::string& path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    FuzzHost host(path);
    if (!host.load() || !host.setup()) {
        return {"ModeToggleLive", false, host.err(), 0};
    }

    const std::string streamId = "fuzz-toggle-live-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
    host.injectState(R"({"mode":"publish","streamId":")" + streamId + R"("})");

    if (!host.activate()) {
        return {"ModeToggleLive", false, "activate: " + host.err(), 0};
    }

    // Phase 1: Publish for ~2 seconds (wait for signaling connect)
    for (int b = 0; b < 400; ++b) {
        if (!host.processBlock()) {
            return {"ModeToggleLive", false, "publish phase failed: " + host.err(), 0};
        }
    }

    std::string statusAfterPublish = host.readStatus();

    // Phase 2: Switch to Play mid-stream
    {
        ParamChanges changes;
        changes.add(webrtc_vst::kParamMode, 0.0); // Play
        if (!host.processBlock(&changes)) {
            return {"ModeToggleLive", false, "switch to play failed: " + host.err(), 0};
        }
    }

    // Process in Play mode for ~1 second
    for (int b = 0; b < 200; ++b) {
        if (!host.processBlock()) {
            return {"ModeToggleLive", false, "play phase failed: " + host.err(), 0};
        }
    }

    std::string statusAfterPlay = host.readStatus();

    // Phase 3: Switch back to Publish
    {
        ParamChanges changes;
        changes.add(webrtc_vst::kParamMode, 1.0); // Publish
        if (!host.processBlock(&changes)) {
            return {"ModeToggleLive", false, "switch back to publish failed: " + host.err(), 0};
        }
    }

    // Process in Publish mode for ~2 seconds
    for (int b = 0; b < 400; ++b) {
        if (!host.processBlock()) {
            return {"ModeToggleLive", false, "re-publish phase failed: " + host.err(), 0};
        }
    }

    std::string statusFinal = host.readStatus();

    host.deactivate();

    auto dt = std::chrono::duration<double, std::milli>(
                  std::chrono::high_resolution_clock::now() - t0).count();

    std::string detail = "stream=" + streamId;
    if (!statusAfterPublish.empty()) detail += " post_publish=\"" + statusAfterPublish + "\"";
    if (!statusAfterPlay.empty()) detail += " post_play=\"" + statusAfterPlay + "\"";
    if (!statusFinal.empty()) detail += " final=\"" + statusFinal + "\"";

    return {"ModeToggleLive(" + streamId + ")", true, detail, dt};
}

// ============================================================================
// Test 6: setupProcessing storm (simulates Reaper calling it repeatedly)
// ============================================================================

TestResult test_setup_processing_storm(const std::string& path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    FuzzHost host(path);
    if (!host.load()) {
        return {"SetupProcessingStorm", false, host.err(), 0};
    }

    host.injectState(R"({"mode":"publish","streamId":"fuzz-setup-storm"})");

    if (!host.setup() || !host.activate()) {
        return {"SetupProcessingStorm", false, host.err(), 0};
    }

    // Simulate Reaper calling setupProcessing every ~100 blocks
    // while audio is actively being processed.
    constexpr int kTotalBlocks = 2000;
    constexpr int kSetupInterval = 100;
    int setupCount = 0;

    for (int b = 0; b < kTotalBlocks; ++b) {
        if (!host.processBlock()) {
            return {"SetupProcessingStorm", false,
                    "process failed at block " + std::to_string(b) + ": " + host.err(), 0};
        }

        if (b > 0 && (b % kSetupInterval) == 0) {
            // Re-call setup with same params (should be no-op with our fix)
            if (!host.setup()) {
                return {"SetupProcessingStorm", false,
                        "setup failed at block " + std::to_string(b) + ": " + host.err(), 0};
            }
            ++setupCount;
        }
    }

    host.deactivate();

    auto dt = std::chrono::duration<double, std::milli>(
                  std::chrono::high_resolution_clock::now() - t0).count();
    return {"SetupProcessingStorm(" + std::to_string(setupCount) + " re-setups)",
            true, "", dt};
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "WebRTC VST3 Plugin - FUZZ TESTS" << std::endl;
    std::cout << "Mode toggling, reconnection, duplicate streams" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    static HostApplication hostApp;
    PluginContextFactory::instance().setPluginContext(&hostApp);

    std::string pluginPath;
    if (argc > 1) {
        pluginPath = argv[1];
    } else if (const char* env = std::getenv("WEBRTC_VST_PLUGIN_PATH")) {
        pluginPath = env;
    } else {
#if defined(_WIN32)
        pluginPath = "build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3/Contents/x86_64-win/webrtc_vst.vst3";
#else
        pluginPath = "build/webrtc_vst_linux/VST3/Release/webrtc_vst.vst3";
#endif
    }

    std::cout << "Plugin path: " << pluginPath << std::endl;

    if (!std::filesystem::exists(pluginPath)) {
        std::cerr << "Error: Plugin not found at " << pluginPath << std::endl;
        return 1;
    }

    std::vector<TestResult> results;
    int failed = 0;

    auto run = [&](TestResult (*fn)(const std::string&), int index, int total) {
        std::cout << "\n[" << index << "/" << total << "] Running..." << std::flush;
        auto r = fn(pluginPath);
        printResult(r);
        if (!r.detail.empty() && r.passed) {
            std::cout << "         " << r.detail << std::endl;
        }
        if (!r.passed) ++failed;
        results.push_back(r);
    };

    constexpr int kTotal = 6;
    run(test_mode_toggle_while_streaming, 1, kTotal);
    run(test_mode_toggle_every_block, 2, kTotal);
    run(test_reconnect_during_streaming, 3, kTotal);
    run(test_duplicate_stream_id, 4, kTotal);
    run(test_mode_toggle_live, 5, kTotal);
    run(test_setup_processing_storm, 6, kTotal);

    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "FUZZ TEST SUMMARY" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "Total:  " << results.size() << std::endl;
    std::cout << "Passed: " << (results.size() - static_cast<size_t>(failed)) << std::endl;
    std::cout << "Failed: " << failed << std::endl;

    if (failed > 0) {
        std::cout << "\nFailed tests:" << std::endl;
        for (const auto& r : results) {
            if (!r.passed) std::cout << "  - " << r.name << ": " << r.detail << std::endl;
        }
    } else {
        std::cout << "ALL FUZZ TESTS PASSED" << std::endl;
    }
    std::cout << std::string(60, '=') << std::endl;

    return failed == 0 ? 0 : 1;
}
