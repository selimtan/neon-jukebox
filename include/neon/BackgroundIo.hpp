#pragma once

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace neon {

// Own the thread handle until the operation has unwound. In particular, never
// cancel a pooled std::async thread after it has returned to the pool.
class BackgroundIoCancellation {
public:
    explicit BackgroundIoCancellation(std::function<bool()> cancelled) {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &thread_, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
        watcher_ = std::jthread([this, cancelled = std::move(cancelled)](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (cancelled()) CancelSynchronousIo(thread_);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    ~BackgroundIoCancellation() {
        if (watcher_.joinable()) { watcher_.request_stop(); watcher_.join(); }
        if (thread_) CloseHandle(thread_);
    }
    BackgroundIoCancellation(const BackgroundIoCancellation&) = delete;
    BackgroundIoCancellation& operator=(const BackgroundIoCancellation&) = delete;
private:
    HANDLE thread_{};
    std::jthread watcher_;
};

// Teardown can cause native media/COM components to send window messages.
// Keep servicing them while waiting, without dispatching new application actions.
template<class Ready>
void waitWithWindowMessages(Ready ready) {
    while (!ready()) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

} // namespace neon
