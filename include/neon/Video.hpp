#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <SDL3/SDL_rect.h>

#include "neon/Audio.hpp"
#include "neon/Models.hpp"

struct SDL_Window;

namespace neon {

class VideoEngine {
public:
    VideoEngine();
    ~VideoEngine();
    VideoEngine(const VideoEngine&) = delete;
    VideoEngine& operator=(const VideoEngine&) = delete;

    bool initialize(SDL_Window* window, std::string& error);
    void shutdown();
    bool play(const Track& track, std::int64_t startMs, std::string& error);
    void stop();
    bool pause();
    bool resume();
    bool seek(std::int64_t milliseconds);
    void setVolume(float volume);
    void render(const SDL_FRect& logicalDestination);
    void hide();

    [[nodiscard]] bool initialized() const;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool paused() const;
    [[nodiscard]] bool takeFinished();
    [[nodiscard]] bool takeSurfaceTouch();
    [[nodiscard]] std::int64_t positionMs() const;
    [[nodiscard]] std::int64_t durationMs() const;
    [[nodiscard]] float volume() const;
    [[nodiscard]] AudioVisualizationFrame visualization();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neon
