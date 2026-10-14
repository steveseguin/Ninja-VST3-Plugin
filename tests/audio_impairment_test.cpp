// Deterministic impairment of real Opus packets at the production receive boundary.
// This deliberately excludes ICE, SRTP, signaling and operating-system scheduling.
#include "../webrtc_vst/src/WebRTCSession.h"
#include "../webrtc_vst/src/PeerAudioBufferSizing.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace webrtc_vst {
struct WebRTCSessionTestAccess {
    static void configure(WebRTCSession& session, double rate, int channels) {
        session.sampleRate_ = rate;
        session.channelCount_ = channels;
    }
    static void addPeer(WebRTCSession& session, const std::string& name) {
        auto ctx = std::make_shared<WebRTCSession::PeerAudioContext>();
        int error = 0;
        ctx->decoder = opus_decoder_create(48000, session.channelCount_, &error);
        if (error != OPUS_OK) throw std::runtime_error("Cannot create decoder");
        ctx->resampler.configure(48000, session.sampleRate_, session.channelCount_);
        const auto sizing = peerAudioBufferSizing(session.sampleRate_);
        ctx->preFillFrames = sizing.preFillFrames;
        ctx->buffer = std::make_shared<AudioRingBuffer>(sizing.capacityFrames, session.channelCount_);
        session.peerSessions_[name].audioContext = std::move(ctx);
    }
    static bool deliver(WebRTCSession& session, const std::string& name,
                        const std::vector<unsigned char>& packet) {
        return session.decodeAndQueueAudio(session.peerSessions_.at(name).audioContext,
            session.channelCount_, packet.data(), packet.size(), true, "impairment-test");
    }
    static size_t available(const WebRTCSession& session, const std::string& name) {
        const auto& ctx = session.peerSessions_.at(name).audioContext;
        return ctx->preFillReady ? ctx->buffer->availableFrames(session.channelCount_) : 0;
    }
    static uint64_t plc(const WebRTCSession& session, const std::string& name) {
        return session.peerSessions_.at(name).audioContext->plcFrameCount;
    }
};
}

namespace {
using Access = webrtc_vst::WebRTCSessionTestAccess;
using Packet = std::vector<unsigned char>;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<Packet> encode(int channels, int count, int packetFrames = 960) {
    int error = 0;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
        opus_encoder_create(48000, channels, OPUS_APPLICATION_AUDIO, &error), opus_encoder_destroy);
    require(error == OPUS_OK, "Cannot create encoder");
    std::vector<Packet> packets;
    std::vector<float> pcm(static_cast<size_t>(packetFrames) * channels);
    for (int packet = 0; packet < count; ++packet) {
        for (int i = 0; i < packetFrames; ++i) {
            const double time = (packet * packetFrames + i) / 48000.0;
            for (int ch = 0; ch < channels; ++ch) {
                const double hz = ch ? 733.0 : 440.0;
                // Distinct stereo tones with slow amplitude changes expose ordering errors.
                pcm[static_cast<size_t>(i) * channels + ch] = static_cast<float>(
                    (0.2 + 0.04 * std::sin(time * 3.0)) * std::sin(2 * std::numbers::pi * hz * time));
            }
        }
        Packet bytes(4000);
        const int size = opus_encode_float(encoder.get(), pcm.data(), packetFrames, bytes.data(), 4000);
        require(size > 0, "Cannot encode packet");
        bytes.resize(static_cast<size_t>(size));
        packets.push_back(std::move(bytes));
    }
    return packets;
}

struct Receiver {
    webrtc_vst::AudioRingBuffer unused{2048, 2};
    webrtc_vst::WebRTCSession session{unused};
    int channels;
    explicit Receiver(double rate = 48000, int count = 2) : channels(count) {
        Access::configure(session, rate, channels);
        Access::addPeer(session, "a");
    }
    void deliver(const Packet& packet, const std::string& peer = "a") {
        require(Access::deliver(session, peer, packet), "Production decoder rejected valid Opus");
    }
    std::vector<float> pull(size_t frames) {
        std::vector<float> planar(frames * channels);
        std::vector<float*> pointers(static_cast<size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) pointers[ch] = planar.data() + ch * frames;
        session.pullIncomingAudio(pointers.data(), frames, channels);
        std::vector<float> result(frames * channels);
        for (size_t i = 0; i < frames; ++i)
            for (int ch = 0; ch < channels; ++ch) result[i * channels + ch] = pointers[ch][i];
        return result;
    }
    void warm(const std::vector<Packet>& packets) {
        for (int i = 0; i < 20; ++i) {
            deliver(packets[i]);
            const auto frames = Access::available(session, "a");
            if (frames) pull(frames);
        }
    }
};

void append(std::vector<float>& target, const std::vector<float>& source) {
    target.insert(target.end(), source.begin(), source.end());
}
double maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    require(a.size() == b.size(), "Different output lengths");
    double maximum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        require(std::isfinite(a[i]) && std::isfinite(b[i]), "Nonfinite output");
        maximum = std::max(maximum, std::abs(static_cast<double>(a[i]) - b[i]));
    }
    return maximum;
}

nlohmann::json plcPartition(double rate, int channels, size_t block) {
    const auto packets = encode(channels, 25);
    Receiver whole(rate, channels), split(rate, channels);
    whole.warm(packets);
    split.warm(packets);
    const size_t duration = static_cast<size_t>(rate / 10); // Same 100 ms outage.
    const auto reference = whole.pull(duration);
    std::vector<float> actual;
    for (size_t offset = 0; offset < duration; offset += block)
        append(actual, split.pull(std::min(block, duration - offset)));
    const auto difference = maxDifference(reference, actual);
    const auto wholePlc = Access::plc(whole.session, "a"), splitPlc = Access::plc(split.session, "a");
    // Reception resumes at the same media time; decoder state must also match.
    whole.deliver(packets[24]);
    split.deliver(packets[24]);
    const auto recoveryDifference = maxDifference(whole.pull(block), split.pull(block));
    return {{"rate", rate}, {"channels", channels}, {"block", block},
        {"wholePlcPackets", wholePlc}, {"splitPlcPackets", splitPlc},
        {"maxSampleDifference", difference}, {"recoveryDifference", recoveryDifference},
        {"passed", wholePlc == 5 && wholePlc == splitPlc && difference < 1e-6 && recoveryDifference < 1e-6}};
}

void checkCleanDecode() {
    const auto packets = encode(2, 100);
    Receiver receiver;
    int error = 0;
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
        opus_decoder_create(48000, 2, &error), opus_decoder_destroy);
    require(error == OPUS_OK, "Cannot create reference decoder");
    std::vector<float> expected(1920);
    for (const auto& packet : packets) {
        require(opus_decode_float(decoder.get(), packet.data(), static_cast<int>(packet.size()),
            expected.data(), 960, 0) == 960, "Reference decode failed");
        receiver.deliver(packet);
        require(maxDifference(expected, receiver.pull(960)) < 1e-6, "Clean audio changed");
    }
    require(Access::plc(receiver.session, "a") == 0, "Clean stream needed concealment");
}

void checkRecoveryTail() {
    const auto packets = encode(2, 22);
    Receiver expected, actual;
    expected.warm(packets);
    actual.warm(packets);
    auto reference = expected.pull(960);
    expected.deliver(packets[21]);
    append(reference, expected.pull(960));
    auto split = actual.pull(256);
    actual.deliver(packets[21]); // New audio arrives while 704 concealed samples remain.
    append(split, actual.pull(704));
    append(split, actual.pull(960));
    require(maxDifference(reference, split) < 1e-6,
        "Concealment tail was lost or played after the newly arrived packet");
}

void checkPeerIsolation() {
    const auto packets = encode(2, 30);
    Receiver combined, a, b;
    Access::addPeer(combined.session, "b");
    for (int i = 0; i < 30; ++i) {
        combined.deliver(packets[i]); a.deliver(packets[i]);
        if (i != 21 && i != 22) { combined.deliver(packets[i], "b"); b.deliver(packets[i]); }
        for (size_t offset = 0; offset < 960; offset += 256) {
            const auto frames = std::min<size_t>(256, 960 - offset);
            auto expected = a.pull(frames);
            const auto other = b.pull(frames);
            for (size_t sample = 0; sample < expected.size(); ++sample) expected[sample] += other[sample];
            require(maxDifference(expected, combined.pull(frames)) < 1e-6, "Peer concealment leaked between streams");
        }
    }
}

void checkConcurrentReceiveAndPull() {
    const auto packets = encode(2, 50);
    Receiver receiver;
    receiver.deliver(packets.front());
    std::atomic<bool> start{false}, valid{true};
    std::thread incoming([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 4000; ++i) {
            if (!Access::deliver(receiver.session, "a", packets[i % packets.size()])) valid = false;
            if (i % 8 == 0) std::this_thread::yield();
        }
    });
    std::thread playback([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 4000; ++i) {
            for (const float sample : receiver.pull(128))
                if (!std::isfinite(sample) || std::abs(sample) >= 1) valid = false;
            if (Access::available(receiver.session, "a") > 2048) valid = false;
        }
    });
    start.store(true, std::memory_order_release);
    incoming.join();
    playback.join();
    require(valid, "Concurrent receive/pull produced invalid audio or exceeded the buffer bound");
}

nlohmann::json simulate(const std::string& scenario, size_t block) {
    constexpr int count = 500;
    const auto packets = encode(2, count);
    struct Delivery { int64_t time; int packet; };
    std::vector<Delivery> schedule;
    int dropped = 0, duplicates = 0, delayed = 0;
    for (int i = 0; i < count; ++i) {
        int64_t time = i * 960;
        if ((scenario == "loss-5pct" && i % 20 == 10) ||
            (scenario == "burst-100ms" && i % 100 >= 50 && i % 100 < 55) ||
            (scenario == "outage-1s" && i >= 200 && i < 250)) { ++dropped; continue; }
        if (scenario == "jitter-12ms") { time += (i * 137) % 577; ++delayed; }
        if (scenario == "reorder-30ms" && i % 10 == 5) { time += 1440; ++delayed; }
        schedule.push_back({time, i});
        if (scenario == "duplicates" && i % 50 == 25) { schedule.push_back({time + 120, i}); ++duplicates; }
    }
    std::stable_sort(schedule.begin(), schedule.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    Receiver receiver;
    size_t next = 0, silentBlocks = 0, clippedSamples = 0, nonfinite = 0;
    double peak = 0, tailEnergy = 0;
    size_t tailSamples = 0;
    for (int64_t time = 0; time < count * 960; time += static_cast<int64_t>(block)) {
        while (next < schedule.size() && schedule[next].time <= time) receiver.deliver(packets[schedule[next++].packet]);
        const auto frames = std::min<size_t>(block, count * 960 - time);
        const auto audio = receiver.pull(frames);
        double energy = 0;
        for (const auto sample : audio) {
            nonfinite += !std::isfinite(sample);
            clippedSamples += std::abs(sample) >= 1;
            peak = std::max(peak, static_cast<double>(std::abs(sample)));
            energy += sample * sample;
        }
        silentBlocks += energy / audio.size() < 1e-8;
        if (time >= 9 * 48000) { tailEnergy += energy; tailSamples += audio.size(); }
    }
    const double tailRms = std::sqrt(tailEnergy / tailSamples);
    const auto plc = Access::plc(receiver.session, "a");
    // At these fixed packet intervals at most one packet is needed to align
    // startup to a DAW block. Loss must not multiply concealment by block count.
    const bool fixedCadence = scenario == "clean" || scenario == "loss-5pct" ||
        scenario == "burst-100ms" || scenario == "outage-1s";
    const bool plcBudget = !fixedCadence || plc <= static_cast<uint64_t>(dropped + 1);
    return {{"scenario", scenario}, {"block", block}, {"seconds", 10},
        {"droppedPackets", dropped}, {"duplicatePackets", duplicates}, {"delayedPackets", delayed},
        {"plcPackets", plc}, {"concealmentBudgetPassed", plcBudget}, {"silentBlocks", silentBlocks},
        {"nonfiniteSamples", nonfinite}, {"clippedSamples", clippedSamples}, {"peak", peak},
        {"recoveredTailRms", tailRms}, {"passed", nonfinite == 0 && clippedSamples == 0 && tailRms > 0.08 && plcBudget}};
}
}

int main(int argc, char** argv) {
    nlohmann::json report = {{"scope", "Virtual clock; production Opus receive, resampler, ring and pull; excludes network transport and DAW"},
        {"partitionTests", nlohmann::json::array()}, {"scenarios", nlohmann::json::array()},
        {"checks", nlohmann::json::array()}};
    size_t failed = 0;
    const auto check = [&](const char* name, auto function) {
        try { function(); report["checks"].push_back({{"name", name}, {"passed", true}}); }
        catch (const std::exception& e) {
            ++failed; report["checks"].push_back({{"name", name}, {"passed", false}, {"error", e.what()}});
        }
    };
    check("clean decode equals independent Opus decoder", checkCleanDecode);
    check("arrival during partially consumed concealment", checkRecoveryTail);
    check("independent peer concealment", checkPeerIsolation);
    check("concurrent receive and pull remain finite and bounded", checkConcurrentReceiveAndPull);
    for (double rate : {8000., 16000., 22050., 44100., 48000., 88200., 96000., 192000., 384000.})
        for (int channels : {1, 2})
            for (size_t block : {32, 64, 128, 256, 512, 1024, 4096}) {
                auto result = plcPartition(rate, channels, block);
                failed += !result["passed"].get<bool>();
                report["partitionTests"].push_back(std::move(result));
            }
    for (const auto* scenario : {"clean", "loss-5pct", "burst-100ms", "jitter-12ms", "reorder-30ms", "duplicates", "outage-1s"})
        for (size_t block : {64, 256, 1024}) {
            auto result = simulate(scenario, block);
            failed += !result["passed"].get<bool>();
            report["scenarios"].push_back(std::move(result));
        }
    report["failedChecks"] = failed;
    report["passed"] = failed == 0;
    if (argc > 1) { std::ofstream output(argv[1]); output << report.dump(2) << '\n'; }
    std::cout << "Audio impairment: " << report["partitionTests"].size() << " partition cases, "
              << report["scenarios"].size() << " scenarios, " << failed << " failures\n";
    for (const auto& result : report["checks"]) std::cout << result.dump() << '\n';
    return failed ? 1 : 0;
}
