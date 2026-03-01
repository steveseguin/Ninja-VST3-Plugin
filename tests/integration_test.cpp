// Integration Test for WebRTC VST Plugin
// Simulates real VST3 host behavior (like Audacity) to catch real-world issues
// Uses the same PlugProvider pattern as the CLI host for reliable hosting.

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// ============================================================================
// Test Configuration
// ============================================================================

constexpr double kSampleRate = 48000.0;
constexpr int32 kBlockSize = 512;  // Audacity uses 512
constexpr int32 kNumChannels = 2;

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

// ============================================================================
// Main Test Runner
// ============================================================================

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
        // Try default location
        pluginPath = "build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3";
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

    // Run tests
    suite.addResult(test_basic_load_unload(pluginPath));
    suite.addResult(test_activate_deactivate(pluginPath));
    suite.addResult(test_process_audio(pluginPath));
    suite.addResult(test_rapid_open_close(pluginPath));
    suite.addResult(test_process_while_deactivating(pluginPath));
    suite.addResult(test_long_running_session(pluginPath));

    // Print summary
    suite.printSummary();

    return suite.allPassed() ? 0 : 1;
}
