#include <mutex>
#include <thread>
#include <vector>
#include <iostream>

int main() {
    std::mutex m;
    int counter = 0;
    auto worker = [&]() {
        for (int i = 0; i < 10000; ++i) {
            std::lock_guard<std::mutex> lock(m);
            ++counter;
        }
    };
    std::vector<std::thread> threads(4);
    for (auto& t : threads) {
        t = std::thread(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
    std::cout << counter << std::endl;
    return 0;
}
