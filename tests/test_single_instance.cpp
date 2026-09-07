#include "neon/SingleInstance.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE handle) : value(handle) { require(value != nullptr, "Missing Windows handle"); }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Child {
    HANDLE process{};
    explicit Child(const std::wstring& arguments) {
        std::array<wchar_t, 32768> executable{};
        require(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) != 0,
                "Cannot resolve test executable");
        auto command = L"\"" + std::wstring(executable.data()) + L"\" " + arguments;
        STARTUPINFOW startup{sizeof(STARTUPINFOW)};
        PROCESS_INFORMATION info{};
        require(CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE,
                "Cannot start test process");
        process = info.hProcess;
        CloseHandle(info.hThread);
    }
    ~Child() {
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process, 99);
            WaitForSingleObject(process, 5000);
        }
        CloseHandle(process);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    DWORD exitCode() const {
        DWORD code{};
        require(GetExitCodeProcess(process, &code) != FALSE, "Cannot read child status");
        return code;
    }
};

int worker(const wchar_t* name, const wchar_t* startName,
           const wchar_t* readyName, const wchar_t* releaseName) {
    Handle start(OpenEventW(SYNCHRONIZE, FALSE, startName));
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName));
    Handle release(OpenEventW(SYNCHRONIZE, FALSE, releaseName));
    require(WaitForSingleObject(start.value, 10000) == WAIT_OBJECT_0, "Worker start timed out");
    neon::SingleInstance instance(name);
    SetEvent(ready.value);
    if (instance.error()) return 3;
    if (!instance.isPrimary()) return 2;
    require(WaitForSingleObject(release.value, 10000) == WAIT_OBJECT_0, "Worker release timed out");
    return 0;
}

void checkInstances(const std::wstring& name) {
    const auto startName = name + L".start";
    const auto releaseName = name + L".release";
    Handle start(CreateEventW(nullptr, TRUE, FALSE, startName.c_str()));
    Handle release(CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str()));
    std::vector<std::unique_ptr<Handle>> ready;
    std::vector<std::unique_ptr<Child>> children;
    std::vector<HANDLE> readyHandles;
    for (int i = 0; i < 8; ++i) {
        const auto readyName = name + L".ready" + std::to_wstring(i);
        ready.push_back(std::make_unique<Handle>(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str())));
        readyHandles.push_back(ready.back()->value);
        children.push_back(std::make_unique<Child>(L"--worker " + name + L" " + startName +
                                                   L" " + readyName + L" " + releaseName));
    }
    SetEvent(start.value);
    require(WaitForMultipleObjects(static_cast<DWORD>(readyHandles.size()), readyHandles.data(),
                                   TRUE, 10000) == WAIT_OBJECT_0, "Concurrent launches timed out");
    SetEvent(release.value);
    int owners = 0;
    int duplicates = 0;
    for (const auto& child : children) {
        require(WaitForSingleObject(child->process, 5000) == WAIT_OBJECT_0, "Child did not exit");
        owners += child->exitCode() == 0;
        duplicates += child->exitCode() == 2;
    }
    require(owners == 1 && duplicates == 7, "Concurrent launches did not select exactly one owner");
    {
        neon::SingleInstance reopened(name.c_str());
        require(reopened.isPrimary() && !reopened.error(), "Normal exit left a stale lock");
    }

    ResetEvent(release.value);
    ResetEvent(ready.front()->value);
    Child crashing(L"--worker " + name + L" " + startName + L" " + name + L".ready0 " + releaseName);
    require(WaitForSingleObject(ready.front()->value, 5000) == WAIT_OBJECT_0, "Crash worker not ready");
    require(crashing.exitCode() == STILL_ACTIVE, "Crash worker did not own the lock");
    // Keep the object alive to exercise abandoned ownership, not just recreation.
    Handle retained(OpenMutexW(SYNCHRONIZE, FALSE, name.c_str()));
    require(TerminateProcess(crashing.process, 77) != FALSE, "Cannot simulate a crash");
    require(WaitForSingleObject(crashing.process, 5000) == WAIT_OBJECT_0, "Crash worker did not stop");
    neon::SingleInstance recovered(name.c_str());
    require(recovered.isPrimary() && !recovered.error(), "Crash left the application locked out");
}

void checkWindow(const std::wstring& property) {
    HWND window = CreateWindowExW(0, L"STATIC", L"Neon Jukebox", WS_OVERLAPPEDWINDOW,
                                  0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr, "Cannot create focus test window");
    const bool ignoredTitle = !neon::SingleInstance::activateExistingWindow(0, property.c_str());
    const bool marked = neon::SingleInstance::markWindow(window, property.c_str());
    ShowWindow(window, SW_MINIMIZE);
    const bool found = neon::SingleInstance::activateExistingWindow(0, property.c_str());
    MSG message{};
    const auto deadline = GetTickCount64() + 2000;
    while (IsIconic(window) && GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
    const bool restored = !IsIconic(window);
    DestroyWindow(window);
    require(ignoredTitle, "Activation matched an unrelated window by title");
    require(marked && found && restored, "Existing minimized window was not restored");
    require(!neon::SingleInstance::activateExistingWindow(0, property.c_str()), "Destroyed window still found");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 6 && std::wstring(argv[1]) == L"--worker") {
            return worker(argv[2], argv[3], argv[4], argv[5]);
        }
        const auto name = L"Local\\NeonJukebox.Test." + std::to_wstring(GetCurrentProcessId()) +
                          L"." + std::to_wstring(GetTickCount64());
        checkInstances(name);
        const auto collisionName = name + L".collision";
        Handle collision(CreateEventW(nullptr, TRUE, FALSE, collisionName.c_str()));
        neon::SingleInstance failed(collisionName.c_str());
        require(failed.error() != 0 && !failed.isPrimary(), "Guard failure must prevent startup");
        checkWindow(name + L".window");
        std::cout << "Single instance: 8 concurrent processes, duplicate rejection, normal exit, "
                     "crash recovery, fail-closed startup and window restoration passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
