#include "neon/SingleInstance.hpp"

namespace neon {
namespace {
struct WindowSearch {
    const wchar_t* property;
    HWND window{};
};

BOOL CALLBACK findMainWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    if (GetPropW(window, search.property)) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}
}  // namespace

SingleInstance::SingleInstance(const wchar_t* name) noexcept {
    mutex_ = CreateMutexW(nullptr, FALSE, name);
    if (!mutex_) {
        error_ = GetLastError();
        return;
    }
    // Acquiring ownership (rather than checking a process list or file) is
    // atomic across simultaneous launches and recovers after a crashed owner.
    const DWORD result = WaitForSingleObject(mutex_, 0);
    ownsMutex_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    if (result == WAIT_FAILED) error_ = GetLastError();
}

SingleInstance::~SingleInstance() {
    if (ownsMutex_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
}

bool SingleInstance::markWindow(HWND window, const wchar_t* property) noexcept {
    return window && SetPropW(window, property, reinterpret_cast<HANDLE>(1));
}

bool SingleInstance::activateExistingWindow(DWORD timeoutMs, const wchar_t* property) noexcept {
    const ULONGLONG started = GetTickCount64();
    do {
        WindowSearch search{property};
        EnumWindows(findMainWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window) {
            // Do not block on the primary window while it is loading its library.
            ShowWindowAsync(search.window, IsIconic(search.window) ? SW_RESTORE : SW_SHOW);
            if (!SetForegroundWindow(search.window)) {
                FLASHWINFO flash{sizeof(FLASHWINFO), search.window, FLASHW_TRAY, 2, 0};
                FlashWindowEx(&flash);
            }
            return true;
        }
        if (GetTickCount64() - started >= timeoutMs) return false;
        // A second launch can arrive before SDL has created the first window.
        Sleep(50);
    } while (true);
}

}  // namespace neon
