#include "../webrtc_vst/src/ConfigState.h"
#include "../webrtc_vst/src/PrivateQrPages.h"
#include "../webrtc_vst/src/RealtimeAudioBridge.h"
#include <limits>
#include <chrono>
#include <random>
#include <thread>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::string read(const std::string& file) {
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main() {
    try {
        using namespace webrtc_vst;
        require(encodeBrowserPassword("a &é🔊") == "a%20%26%C3%A9%F0%9F%94%8A", "Browser password escaping mismatch");
        require(browserAesKeyBytes("plainASCII") == "plainASCII", "ASCII AES material changed");
        require(browserAesKeyBytes("é🔊") == std::string("\xe9\x3d\x0a", 3), "Browser UTF-16 low-byte encoding mismatch");
        for (const auto& invalid : {std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe9", 1)}) {
            bool rejected = false;
            try { validateSettingText(invalid); } catch (...) { rejected = true; }
            require(rejected, "Invalid UTF-8 accepted");
        }
        {
            RealtimeAudioBridge bridge;
            std::array<float, 512> left{}, right{}, outLeft{}, outRight{};
            left.fill(0.25f); right.fill(-0.5f);
            left[12] = std::numeric_limits<float>::quiet_NaN();
            right[33] = std::numeric_limits<float>::infinity();
            const float* input[] = {left.data(), right.data()};
            float* output[] = {outLeft.data(), outRight.data()};
            bridge.transfer(input, nullptr, 512, 2, 0, true);
            require(bridge.takeInput(output) == 512, "Bridge lost queued input");
            require(outLeft[0] == 0.25f && outRight[0] == -0.5f && outLeft[12] == 0 && outRight[33] == 0, "Bridge stereo/finite sanitization failed");
            for (int n = 0; n < 100; ++n) bridge.transfer(input, nullptr, 512, 2, 0, true);
            size_t frames = 0;
            while (const auto count = bridge.takeInput(output)) frames += count;
            require(frames == RealtimeAudioBridge::capacity, "Bridge queue is not bounded");
            bridge.transfer(nullptr, output, 512, 0, 2, false);
            require(bridge.takeDemand() == 512, "Bridge demand clock failed");
            bridge.supplyOutput(input, 512);
            require(bridge.takeDemand() == 512, "Bridge scheduling reserve missing");
            bridge.supplyOutput(input, 512);
            bridge.supplyOutput(input, 512);
            bridge.supplyOutput(input, 512);
            require(bridge.takeDemand() == 0, "Bridge ran ahead of bounded demand");
            // A delayed host can deliver several blocks back-to-back. There
            // must be real queued samples, not a lock-contention silence gap.
            for (int n = 0; n < 4; ++n) {
                outLeft.fill(0); outRight.fill(0);
                bridge.transfer(nullptr, output, 512, 0, 2, false);
                require(outLeft[0] == 0.25f && outRight[0] == -0.5f, "Scheduling reserve failed a four-block host burst");
            }
            bridge.supplyOutput(input, 512);
            bridge.transfer(nullptr, output, 512, 0, 2, false);
            require(outLeft[0] == 0.25f && outRight[0] == -0.5f, "Bridge playback channels crossed");
            bridge.reset();
            require(bridge.takeInput(output) == 0 && bridge.takeDemand() == 0, "Bridge reset retained old-session audio");
            std::atomic<bool> finished{false};
            std::thread worker([&] {
                float* scratch[] = {outLeft.data(), outRight.data()};
                while (!finished.load()) { bridge.takeInput(scratch); bridge.reset(); }
            });
            const auto began = std::chrono::steady_clock::now();
            for (int n = 0; n < 100000; ++n) bridge.transfer(input, nullptr, 512, 2, 0, true);
            finished.store(true); worker.join();
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            require(elapsed < 10, "Bridge contention stalled audio callback");
            std::cout << "PASS: bounded audio bridge, finite stereo, reset, 100000 contended transfers in " << elapsed << "s\n";
        }
        {
            // Independently exercise the receive ring under a concurrent worker
            // and session resets. Run under TSan as well as the native suite.
            RealtimeAudioBridge bridge;
            std::array<float, 512> left{}, right{};
            left.fill(0.25f); right.fill(-0.5f);
            const float* source[] = {left.data(), right.data()};
            std::atomic<bool> finished{false};
            std::thread worker([&] {
                unsigned supplies = 0;
                while (!finished.load()) {
                    if (const auto count = bridge.takeDemand()) {
                        bridge.supplyOutput(source, count);
                        if (++supplies % 17 == 0) bridge.reset();
                    } else std::this_thread::yield();
                }
            });
            std::array<float, 256> outLeft{}, outRight{};
            float* output[] = {outLeft.data(), outRight.data()};
            bool valid = true;
            size_t received = 0;
            for (int block = 0; block < 100000; ++block) {
                outLeft.fill(0); outRight.fill(0);
                bridge.transfer(nullptr, output, 256, 0, 2, false);
                for (size_t i = 0; i < outLeft.size(); ++i) {
                    valid &= (outLeft[i] == 0 && outRight[i] == 0) ||
                             (outLeft[i] == 0.25f && outRight[i] == -0.5f);
                    received += outLeft[i] != 0;
                }
                if (block % 64 == 0) std::this_thread::yield();
            }
            finished.store(true); worker.join();
            require(valid && received > 0, "Concurrent RX/reset corrupted stereo samples or never made progress");
            std::cout << "PASS: 100000 concurrent receive/reset blocks; intact stereo samples=" << received << '\n';
        }
        std::filesystem::path owned;
        {
            PrivateQrPages a, b;
            const auto first = a.write("first"), second = b.write("second"), third = a.write("third");
            require(!first.empty() && first != second && first != third, "QR paths collide");
            require(read(first) == "first" && read(second) == "second", "QR page overwritten");
            owned = std::filesystem::path(first).parent_path();
            for (int i = 0; i < 20; ++i) require(!a.write(std::to_string(i)).empty(), "QR write failed");
            size_t count = 0;
            for (const auto& item : std::filesystem::directory_iterator(owned)) { (void)item; ++count; }
            require(count == 8, "QR history is not bounded");
            require(read(second) == "second", "Other controller page affected");
        }
        require(!std::filesystem::exists(owned), "QR private directory not cleaned");
        for (const auto* invalid : {"wss://a..test", "wss://-a.test", "wss://a-.test", "wss://[abcd]", "wss://[:::1]", "wss://[1:2:3]", "wss://[12345::1]", "wss://a.test:0", "wss://a.test:65536"})
            require(!normalizeEndpoint(invalid, false), "Invalid endpoint accepted");
        for (const auto* valid : {"wss://a.test:443/path?x=1", "wss://[::1]:4443/path", "wss://[2001:db8::1]/"})
            require(normalizeEndpoint(valid, false).has_value(), "Valid endpoint rejected");
        require(normalizeEndpoint("https://[2001:0DB8:0:0:0:0:0:1]/", true) == "https://[2001:db8::1]/", "IPv6 browser hostname mismatch");
        for (const auto* invalid : {"https://127.1/", "https://2130706433/", "https://0x7f000001/", "https://127.000.0.1/", "https://256.1.1.1/"})
            require(!normalizeEndpoint(invalid, true), "Ambiguous or invalid numeric hostname accepted");
        PluginConfig config;
        const auto previous = config;
        try { config = parseConfigState(R"({"streamId":"changed","password":[]})", config); require(false, "Malformed state accepted"); }
        catch (const nlohmann::json::exception&) {}
        require(config.streamId == previous.streamId, "State partially applied");
        bool rejectedDepth = false;
        try { (void)parseConfigState(std::string(4000, '[') + "0" + std::string(4000, ']'), config); }
        catch (...) { rejectedDepth = true; }
        require(rejectedDepth, "Excessive JSON nesting accepted");
        // Reproducible mutation fuzz corpus: arbitrary bytes and structured state
        // mutations. A successful parse must round-trip all relevant fields.
        const uint32_t seed = std::getenv("WEBRTC_FUZZ_SEED") ? static_cast<uint32_t>(std::stoul(std::getenv("WEBRTC_FUZZ_SEED"))) : 0x56444f;
        std::mt19937 random(seed);
        for (int iteration = 0; iteration < 10000; ++iteration) {
            std::string text = R"({"streamId":"owned","webBaseUrl":"https://vdo.ninja/","salt":"test","password":"pw"})";
            for (unsigned changes = 1 + random() % 20; changes > 0; --changes) {
                if (random() % 3 == 0 && !text.empty()) text.erase(random() % text.size(), 1);
                else text.insert(text.begin() + random() % (text.size() + 1), static_cast<char>(random() & 255));
            }
            std::optional<PluginConfig> parsed;
            try { parsed = parseConfigState(text, config); }
            catch (const std::exception&) { /* rejection is expected for malformed corpus */ }
            if (parsed) {
                validateSettingText(parsed->streamId); validateSettingText(parsed->salt); validateSettingText(parsed->password);
                require(normalizeEndpoint(parsed->webBaseUrl, true).has_value(), "Fuzz accepted invalid endpoint");
            }
        }
        std::cout << "PASS: 10000 bounded state mutations, seed=" << seed << '\n';
        std::cout << "PASS: private QR isolation/cleanup, bounded history, endpoints, atomic parsing\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
