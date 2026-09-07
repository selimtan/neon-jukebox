#pragma once

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL_render.h>

#include "neon/Models.hpp"

namespace neon {

class ArtworkCache {
public:
    enum class Kind { Cover, VideoFrame };
    using Loader = std::function<SDL_Surface*(const Track&)>;
    explicit ArtworkCache(std::size_t capacity = 128, Loader loader = {}, std::size_t decoderCount = 4,
                          Kind kind = Kind::Cover);
    ~ArtworkCache();
    ArtworkCache(const ArtworkCache&) = delete;
    ArtworkCache& operator=(const ArtworkCache&) = delete;

    SDL_Texture* get(SDL_Renderer* renderer, const Track& track);
    // Ordered working set: visible/destination covers first, nearby pages after.
    // Decoding stays off-thread; preparing an offscreen cover creates no placeholder.
    void prepare(std::span<const Track* const> tracks);
    [[nodiscard]] bool ready(const Track& track) const;
    [[nodiscard]] std::uint64_t revision(const Track& track) const;
    // Upload a bounded number of decoded covers, on the render thread only.
    void update(SDL_Renderer* renderer);
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    void clear();
    void requestStop();
    void invalidate(const Track& track);
    // Configure once, before prepare/get: the App's library worker owns extraction.
    void useDiskCache(const std::filesystem::path& libraryRoot);
    static std::filesystem::path coverCachePath(const Track& track, const std::filesystem::path& root = {});
    static std::string versionKey(const Track& track) { return key(track); }
    static SDL_Surface* loadCachedCover(const Track& track, const std::filesystem::path& root = {},
                                       std::stop_token stop = {});

private:
    struct Entry {
        SDL_Texture* texture{};
        std::uint64_t lastUsed{};
        std::uint64_t ticket{};
        std::uint64_t revision{};
        bool ready{};
    };
    struct SurfaceDeleter { void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); } };
    using Surface = std::unique_ptr<SDL_Surface, SurfaceDeleter>;
    struct Request { std::string key; Track track; std::uint64_t ticket{}; };
    struct Result { std::string key; Surface surface; std::uint64_t ticket{}; };

    static std::vector<std::uint8_t> embeddedBytes(const Track& track);
    static SDL_Surface* loadSurface(const Track& track, std::stop_token stop);
    static SDL_Surface* generatedSurface(const Track& track, int size, bool video = false);
    SDL_Texture* generatedTexture(SDL_Renderer* renderer, const Track& track, int size = 384) const;
    [[nodiscard]] bool hasSource(const Track& track) const;
    static std::string key(const Track& track);
    void startWorkers();
    void evictIfNeeded();
    void work(std::stop_token stop);

    std::size_t capacity_;
    std::size_t decoderCount_;
    Kind kind_;
    std::uint64_t clock_{};
    std::uint64_t nextTicket_{};
    std::uint64_t revision_{};
    std::unordered_map<std::string, Entry> entries_;
    Loader loader_;
    bool defaultLoader_{};
    std::filesystem::path diskCacheRoot_;
    std::atomic_size_t activeWorkers_{};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Request> requests_;
    std::deque<Result> results_;
    std::unordered_set<std::uint64_t> inFlight_;
    std::vector<std::jthread> workers_;
};

}  // namespace neon
