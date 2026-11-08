#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace webrtc_vst {

// A message-loop timer, owned and dispatched by its creating UI thread.
// Different controllers may belong to different threads. SDK 3.8's Windows
// Timer instead shares an unsynchronized list across all of those threads.
class WindowsUiTimer final {
public:
    WindowsUiTimer(UINT interval, std::function<void()> callback) {
        id_ = SetTimer(nullptr, 0, interval, dispatch);
        if (!id_) return;
        try {
            std::lock_guard lock(registryMutex());
            registry().emplace(id_, std::move(callback));
        } catch (...) {
            KillTimer(nullptr, id_);
            throw;
        }
    }

    ~WindowsUiTimer() {
        if (!id_) return;
        {
            std::lock_guard lock(registryMutex());
            registry().erase(id_);
        }
        KillTimer(nullptr, id_);
    }

    WindowsUiTimer(const WindowsUiTimer&) = delete;
    WindowsUiTimer& operator=(const WindowsUiTimer&) = delete;
    bool valid() const { return id_ != 0; }

private:
    static auto& registry() {
        static std::unordered_map<UINT_PTR, std::function<void()>> callbacks;
        return callbacks;
    }
    static auto& registryMutex() {
        static std::mutex mutex;
        return mutex;
    }
    static void CALLBACK dispatch(HWND, UINT, UINT_PTR id, DWORD) {
        std::function<void()> callback;
        {
            std::lock_guard lock(registryMutex());
            const auto found = registry().find(id);
            if (found != registry().end()) callback = found->second;
        }
        // Host calls can destroy this timer or create another one. Never hold
        // the registry lock while calling the host, or access the timer after.
        if (callback) callback();
    }
    UINT_PTR id_ = 0;
};

} // namespace webrtc_vst
#endif
