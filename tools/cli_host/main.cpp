#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vsttypes.h>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr Steinberg::int32 kBlockSize = 256;
constexpr int kProcessIterations = 200;

struct BusBuffers {
    Steinberg::Vst::AudioBusBuffers buffers {};
    std::vector<std::vector<float>> storage;
    std::vector<float*> channelPtrs;
};

std::string defaultPluginPath() {
    const auto candidate = std::filesystem::path("build") /
                           "webrtc_vst_win" /
                           "VST3" /
                           "Release" /
                           "webrtc_vst.vst3" /
                           "Contents" /
                           "x86_64-win" /
                           "webrtc_vst.vst3";
    return candidate.string();
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

} // namespace

int main(int argc, char** argv) {
    std::string pluginPath = (argc > 1) ? std::string{argv[1]} : defaultPluginPath();
    if (!std::filesystem::exists(pluginPath)) {
        std::cerr << "Plugin not found at: " << pluginPath << "\n";
        return 1;
    }

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

    if (!inputBuses.empty() && !inputBuses.front().storage.empty()) {
        inputBuses.front().storage.front()[0] = 1.0f;
    }

    for (int iteration = 0; iteration < kProcessIterations; ++iteration) {
        if (processor->process(data) != Steinberg::kResultOk) {
            std::cerr << "process call failed at iteration " << iteration << "\n";
            break;
        }
        context.projectTimeSamples += kBlockSize;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    processor->setProcessing(false);
    component->setActive(false);

    provider.releasePlugIn(component, controller);
    return 0;
}
