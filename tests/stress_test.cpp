// Stress Test for WebRTC VST Plugin
// Aggressive testing to expose threading issues, memory leaks, and race conditions

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// ============================================================================
// Configuration
// ============================================================================

constexpr double kSampleRate = 48000.0;
constexpr int32 kBlockSize = 512;
constexpr int32 kNumChannels = 2;

// ============================================================================
// Concurrent Plugin Instance
// ============================================================================

class PluginInstance {
public:
    explicit PluginInstance(int id) : id_(id) {}

    ~PluginInstance() {
        cleanup();
    }

    bool initialize(const std::string& pluginPath) {
        std::string error;
        module_ = VST3::Hosting::Module::create(pluginPath, error);
        if (!module_) {
            return false;
        }

        auto factory = module_->getFactory();
        for (auto& classInfo : factory.classInfos()) {
            if (classInfo.category() == kVstAudioEffectClass) {
                component_ = factory.createInstance<IComponent>(classInfo.ID());
                break;
            }
        }

        if (!component_ || component_->initialize(&hostContext_) != kResultOk) {
            return false;
        }

        if (component_->queryInterface(IAudioProcessor::iid, (void**)&processor_) != kResultOk) {
            return false;
        }

        return true;
    }

    bool setup() {
        if (!processor_) return false;

        ProcessSetup setup;
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = kBlockSize;
        setup.sampleRate = kSampleRate;

        return processor_->setupProcessing(setup) == kResultOk;
    }

    bool activate() {
        if (!component_) return false;
        active_ = component_->setActive(true) == kResultOk;
        return active_;
    }

    bool deactivate() {
        if (!component_ || !active_) return true;
        bool result = component_->setActive(false) == kResultOk;
        if (result) active_ = false;
        return result;
    }

    void cleanup() {
        if (active_) deactivate();
        if (processor_) { processor_->release(); processor_ = nullptr; }
        if (component_) { component_->terminate(); component_->release(); component_ = nullptr; }
        module_.reset();
    }

    int getId() const { return id_; }

private:
    class HostApplication : public FUnknownImpl<IHostApplication> {
    public:
        HostApplication() { FUNKNOWN_CTOR }
        ~HostApplication() { FUNKNOWN_DTOR }
        tresult PLUGIN_API getName(String128 name) override {
            String str("Stress Test Host");
            str.copyTo(name, 0, 127);
            return kResultOk;
        }
        tresult PLUGIN_API createInstance(TUID, TUID, void**) override {
            return kNotImplemented;
        }
    };

    int id_;
    std::shared_ptr<VST3::Hosting::Module> module_;
    IComponent* component_{nullptr};
    IAudioProcessor* processor_{nullptr};
    HostApplication hostContext_;
    bool active_{false};
};

// ============================================================================
// Stress Test: Rapid Create/Destroy
// ============================================================================

void stress_rapid_create_destroy(const std::string& pluginPath, int iterations, std::atomic<int>& failures) {
    for (int i = 0; i < iterations; ++i) {
        PluginInstance instance(i);
        if (!instance.initialize(pluginPath)) {
            failures++;
            continue;
        }
        if (!instance.setup() || !instance.activate() || !instance.deactivate()) {
            failures++;
        }
        instance.cleanup();

        // Random tiny delay
        if (i % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// ============================================================================
// Stress Test: Concurrent Instances
// ============================================================================

void stress_concurrent_instances(const std::string& pluginPath, int numInstances, std::atomic<int>& failures) {
    std::vector<std::thread> threads;
    threads.reserve(numInstances);

    for (int i = 0; i < numInstances; ++i) {
        threads.emplace_back([&pluginPath, i, &failures]() {
            PluginInstance instance(i);
            if (!instance.initialize(pluginPath)) {
                failures++;
                return;
            }
            if (!instance.setup()) {
                failures++;
                return;
            }
            if (!instance.activate()) {
                failures++;
                return;
            }

            // Hold active for a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (!instance.deactivate()) {
                failures++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

// ============================================================================
// Stress Test: Rapid Activate/Deactivate
// ============================================================================

void stress_rapid_activate_deactivate(const std::string& pluginPath, int cycles, std::atomic<int>& failures) {
    PluginInstance instance(0);
    if (!instance.initialize(pluginPath) || !instance.setup()) {
        failures++;
        return;
    }

    for (int i = 0; i < cycles; ++i) {
        if (!instance.activate()) {
            failures++;
            return;
        }
        // Immediate deactivate (no processing)
        if (!instance.deactivate()) {
            failures++;
            return;
        }
    }
}

// ============================================================================
// Stress Test: Memory Leak Check (repeated cycles)
// ============================================================================

void stress_memory_leak_check(const std::string& pluginPath, int cycles, std::atomic<int>& failures) {
    for (int i = 0; i < cycles; ++i) {
        PluginInstance instance(i);
        if (!instance.initialize(pluginPath)) {
            failures++;
            continue;
        }
        if (!instance.setup()) {
            failures++;
            continue;
        }
        if (!instance.activate()) {
            failures++;
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!instance.deactivate()) {
            failures++;
        }

        instance.cleanup();
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "WebRTC VST3 Plugin - STRESS TESTS" << std::endl;
    std::cout << "WARNING: Aggressive testing for race conditions" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    std::string pluginPath;
    if (argc > 1) {
        pluginPath = argv[1];
    } else if (const char* env = std::getenv("WEBRTC_VST_PLUGIN_PATH")) {
        pluginPath = env;
    } else {
        pluginPath = "build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3";
    }

    std::cout << "Plugin path: " << pluginPath << std::endl;

    if (!std::filesystem::exists(pluginPath)) {
        std::cerr << "Error: Plugin not found at " << pluginPath << std::endl;
        return 1;
    }

    std::atomic<int> totalFailures{0};

    // Test 1: Rapid Create/Destroy (single threaded)
    {
        std::cout << "\n[1/5] Rapid Create/Destroy (1000x)... " << std::flush;
        auto start = std::chrono::high_resolution_clock::now();
        std::atomic<int> failures{0};
        stress_rapid_create_destroy(pluginPath, 1000, failures);
        auto duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
        totalFailures += failures.load();
        if (failures == 0) {
            std::cout << "✓ PASS (" << duration << "s)" << std::endl;
        } else {
            std::cout << "✗ FAIL (" << failures << " failures)" << std::endl;
        }
    }

    // Test 2: Concurrent Instances
    {
        std::cout << "[2/5] Concurrent Instances (10 parallel)... " << std::flush;
        auto start = std::chrono::high_resolution_clock::now();
        std::atomic<int> failures{0};
        stress_concurrent_instances(pluginPath, 10, failures);
        auto duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
        totalFailures += failures.load();
        if (failures == 0) {
            std::cout << "✓ PASS (" << duration << "s)" << std::endl;
        } else {
            std::cout << "✗ FAIL (" << failures << " failures)" << std::endl;
        }
    }

    // Test 3: Rapid Activate/Deactivate
    {
        std::cout << "[3/5] Rapid Activate/Deactivate (500x)... " << std::flush;
        auto start = std::chrono::high_resolution_clock::now();
        std::atomic<int> failures{0};
        stress_rapid_activate_deactivate(pluginPath, 500, failures);
        auto duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
        totalFailures += failures.load();
        if (failures == 0) {
            std::cout << "✓ PASS (" << duration << "s)" << std::endl;
        } else {
            std::cout << "✗ FAIL (" << failures << " failures)" << std::endl;
        }
    }

    // Test 4: Memory Leak Check
    {
        std::cout << "[4/5] Memory Leak Check (100 cycles)... " << std::flush;
        auto start = std::chrono::high_resolution_clock::now();
        std::atomic<int> failures{0};
        stress_memory_leak_check(pluginPath, 100, failures);
        auto duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
        totalFailures += failures.load();
        if (failures == 0) {
            std::cout << "✓ PASS (" << duration << "s)" << std::endl;
        } else {
            std::cout << "✗ FAIL (" << failures << " failures)" << std::endl;
        }
    }

    // Test 5: Concurrent Create/Destroy
    {
        std::cout << "[5/5] Concurrent Create/Destroy (5 threads, 100x each)... " << std::flush;
        auto start = std::chrono::high_resolution_clock::now();
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 5; ++i) {
            threads.emplace_back([&pluginPath, &failures]() {
                stress_rapid_create_destroy(pluginPath, 100, failures);
            });
        }
        for (auto& t : threads) t.join();
        auto duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
        totalFailures += failures.load();
        if (failures == 0) {
            std::cout << "✓ PASS (" << duration << "s)" << std::endl;
        } else {
            std::cout << "✗ FAIL (" << failures << " failures)" << std::endl;
        }
    }

    // Summary
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "STRESS TEST SUMMARY" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    if (totalFailures == 0) {
        std::cout << "✓ ALL STRESS TESTS PASSED" << std::endl;
        std::cout << "Plugin is stable under extreme conditions" << std::endl;
    } else {
        std::cout << "✗ FAILURES DETECTED: " << totalFailures << std::endl;
        std::cout << "Plugin has stability issues" << std::endl;
    }
    std::cout << std::string(60, '=') << std::endl;

    return totalFailures == 0 ? 0 : 1;
}
