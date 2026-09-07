#include <SDL3/SDL_main.h>

#include <string>

#include "neon/App.hpp"
#include "neon/SingleInstance.hpp"

int main(int, char**) {
    neon::SingleInstance instance;
    if (instance.error()) {
        const auto message = L"Unable to check whether Neon Jukebox is already running. Windows error: " +
                             std::to_wstring(instance.error());
        MessageBoxW(nullptr, message.c_str(), L"Neon Jukebox", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!instance.isPrimary()) {
        neon::SingleInstance::activateExistingWindow();
        return 0;
    }
    neon::App app;
    return app.run();
}
