// Manual timing characterization. Scheduling measurements are not a CI gate.
#include "../webrtc_vst/src/AudioWorkerWait.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <vector>
#include <nlohmann/json.hpp>

template <typename Wait>
nlohmann::json measure(Wait wait) {
    std::vector<double> samples;
    for (int i = 0; i < 200; ++i) {
        const auto start = std::chrono::steady_clock::now();
        wait();
        samples.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return {{"samples", samples.size()}, {"requestedMs", 1},
        {"meanMs", std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size()},
        {"medianMs", samples[99]}, {"p95Ms", samples[189]}, {"maxMs", samples.back()}};
}

int main(int argc, char** argv) {
    const auto ordinary = measure([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    webrtc_vst::AudioWorkerWait wait;
    const auto precise = measure([&] { wait.wait(); });
    const nlohmann::json report = {{"highResolutionTimerAvailable", wait.hasHighResolutionTimer()},
        {"ordinarySleep", ordinary}, {"audioWorkerWait", precise},
        {"scope", "Observed local scheduling only; no hard real-time guarantee or timing assertion"}};
    std::cout << report.dump(2) << '\n';
    if (argc > 1) { std::ofstream output(argv[1]); output << report.dump(2) << '\n'; }
}
