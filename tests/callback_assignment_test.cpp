// Regression for libdatachannel's stored-callback assignment under teardown.
#include <rtc/utils.hpp>
#include <atomic>
#include <functional>
#include <iostream>
#include <thread>

int main() {
    rtc::synchronized_stored_callback<int> callback;
    std::atomic<int> calls{0};
    const std::function<void(int)> sink = [&](int) { calls.fetch_add(1, std::memory_order_relaxed); };
    std::thread assign([&] {
        for (int i=0; i<10000; ++i) { callback = sink; callback = nullptr; }
    });
    std::thread invoke([&] {
        for (int i=0; i<10000; ++i) { callback(i); if (i % 3 == 0) callback = nullptr; }
    });
    assign.join(); invoke.join();
    callback = nullptr;
    callback(42);
    int replay = 0;
    callback = std::function<void(int)>([&](int value) { replay = value; });
    if (replay != 42) { std::cerr << "Missed callback was not replayed\n"; return 1; }
    callback = nullptr;
    std::cout << "PASS: concurrent stored-callback assignment/reset and missed-event replay\n";
}
