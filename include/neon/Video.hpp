#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <SDL3/SDL_rect.h>

#include "neon/Audio.hpp"
#include "neon/Models.hpp"
#include "neon/VideoSourceCache.hpp"

struct SDL_Window;
struct SDL_Surface;

namespace neon {

class VideoEngine {
public:
    enum class PreloadState { None, Preparing, Ready, Failed };
    using SourceLoader = std::function<VideoSourceResult(const Track&, const std::filesystem::path&,
        const std::function<bool()>&, const std::function<void(int)>&)>;
    explicit VideoEngine(SourceLoader sourceLoader = {});
    ~VideoEngine();
    VideoEngine(const VideoEngine&) = delete;
    VideoEngine& operator=(const VideoEngine&) = delete;

    bool initialize(SDL_Window* window, std::string& error);
    void shutdown();
    void requestShutdown();
    // Queues a load; decoder failures arrive through takeError/takeFinished.
    bool play(const Track& track, std::int64_t startMs, std::string& error);
    // Prepare one next source without replacing the currently playing video.
    void preload(const Track& track);
    void cancelPreload();
    [[nodiscard]] PreloadState preloadState() const;
    [[nodiscard]] std::string preloadError() const;
    void stop(); // Leaves the next prepared source available for a clip transition.
    bool pause();
    bool resume();
    bool seek(std::int64_t milliseconds);
    void setVolume(float volume);
    void render(const SDL_FRect& logicalDestination);
    // Premultiplied text pixels, uploaded only when their revision changes.
    void renderOverlay(SDL_Surface* image, std::uint64_t revision);
    void hideOverlay();
    void hide();

    [[nodiscard]] bool initialized() const;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool loading() const;
    [[nodiscard]] std::string loadingStatus() const;
    [[nodiscard]] bool paused() const;
    [[nodiscard]] bool takeFinished();
    [[nodiscard]] std::string takeError();
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
