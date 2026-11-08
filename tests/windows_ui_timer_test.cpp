#include "../webrtc_vst/src/WindowsUiTimer.h"
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using webrtc_vst::WindowsUiTimer;

int main() {
    constexpr int workers = 10;
    std::barrier gate(workers);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker) threads.emplace_back([&] {
        for (int n = 0; n < 10000; ++n) {
            gate.arrive_and_wait();
            WindowsUiTimer first(100, [] {});
            WindowsUiTimer second(100, [] {});
            if (!first.valid() || !second.valid()) ++failures;
            gate.arrive_and_wait();
        }

        const auto owner = std::this_thread::get_id();
        int calls = 0;
        bool returned = false;
        std::unique_ptr<WindowsUiTimer> self;
        self = std::make_unique<WindowsUiTimer>(10, [&] {
            if (std::this_thread::get_id() != owner) ++failures;
            ++calls;
            self.reset(); // Dispatch must permit cancellation from the callback.
            WindowsUiTimer nested(10, [] {});
            if (!nested.valid()) ++failures;
            returned = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline && !returned) {
            MSG message;
            while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessage(&message);
            Sleep(1);
        }
        // Flush queued timer messages after cancellation; no second callback.
        Sleep(30);
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessage(&message);
        if (!returned || self || calls != 1) ++failures;
    });
    for (auto& thread : threads) thread.join();
    std::printf("Windows UI timer: 200000 concurrent lifetimes, owner-thread callbacks, "
                "self-cancellation: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
