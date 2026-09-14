#pragma once

#include <memory>
#include <string>

#include "neon/Audio.hpp"

namespace neon {

// HTTP(S) radio through Windows MediaPlayer. No seeking, duration, or file cache.
// Calls and state queries are thread-safe; destruction requires exclusive ownership.
// The host keeps Windows Runtime initialized until application shutdown.
class RadioEngine {
public:
    RadioEngine();
    ~RadioEngine();
    RadioEngine(const RadioEngine&) = delete;
    RadioEngine& operator=(const RadioEngine&) = delete;

    bool initialize(std::string& error);
    // Invalidates callbacks immediately; waits at most 250 ms for worker cleanup.
    void shutdown();
    // True means accepted, not yet playing. Open/decoder/network errors are
    // delivered once through takeError(), with takeFinished() also set.
    // Invalid requests leave any current station untouched.
    bool play(const Track& track, std::string& error);
    void stop();
    // Pause/resume also work while opening. True means a change was accepted.
    bool pause();
    bool resume();
    // Clamped to [0, 1], default 0.8. Non-finite values become silence.
    // Preserved across stop/shutdown/initialize; set before play to avoid a burst.
    void setVolume(float volume);

    [[nodiscard]] bool initialized() const;
    // True only after WinRT reports actual playback, and false while paused.
    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool loading() const;
    [[nodiscard]] bool paused() const;
    [[nodiscard]] bool takeFinished();
    [[nodiscard]] std::string takeError();
    [[nodiscard]] float volume() const;
    [[nodiscard]] AudioVisualizationFrame visualization();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neon
