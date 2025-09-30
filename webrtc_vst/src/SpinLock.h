#pragma once

#include <atomic>
#include <thread>

namespace webrtc_vst {

class SpinLock {
public:
    SpinLock() noexcept { flag_.clear(std::memory_order_relaxed); }
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

} // namespace webrtc_vst
