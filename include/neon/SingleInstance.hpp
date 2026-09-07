#pragma once

#include <Windows.h>

namespace neon {

// Keep this guard on the main thread until the application has fully shut down.
class SingleInstance {
public:
    static constexpr wchar_t mutexName[] =
        L"Local\\NeonJukebox.SingleInstance.{32604A91-E821-43B8-A41D-CA057A6485CA}";
    static constexpr wchar_t windowProperty[] =
        L"NeonJukebox.MainWindow.{32604A91-E821-43B8-A41D-CA057A6485CA}";

    explicit SingleInstance(const wchar_t* name = mutexName) noexcept;
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    [[nodiscard]] bool isPrimary() const noexcept { return ownsMutex_; }
    [[nodiscard]] DWORD error() const noexcept { return error_; }

    static bool markWindow(HWND window, const wchar_t* property = windowProperty) noexcept;
    static bool activateExistingWindow(DWORD timeoutMs = 3000,
                                      const wchar_t* property = windowProperty) noexcept;

private:
    HANDLE mutex_{};
    bool ownsMutex_{};
    DWORD error_{};
};

}  // namespace neon
