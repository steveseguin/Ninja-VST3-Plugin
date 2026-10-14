#pragma once

#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace webrtc_vst {

// Used only on the background audio worker, never in the DAW process callback.
// A normal Windows Sleep(1) can take a full ~15.6 ms clock tick. A private timer
// preserves the intended polling interval without changing system timer policy.
class AudioWorkerWait {
public:
    AudioWorkerWait() {
#if defined(_WIN32)
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
    }
    ~AudioWorkerWait() {
#if defined(_WIN32)
        if (timer_) CloseHandle(timer_);
#endif
    }
    AudioWorkerWait(const AudioWorkerWait&) = delete;
    AudioWorkerWait& operator=(const AudioWorkerWait&) = delete;

    void wait() {
#if defined(_WIN32)
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -10000; // One millisecond, in relative 100 ns units.
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(timer_, 100) == WAIT_OBJECT_0) return;
        }
#endif
        // Older Windows versions and other platforms retain their normal wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool hasHighResolutionTimer() const {
#if defined(_WIN32)
        return timer_ != nullptr;
#else
        return false;
#endif
    }

private:
#if defined(_WIN32)
    HANDLE timer_{nullptr};
#endif
};

} // namespace webrtc_vst
