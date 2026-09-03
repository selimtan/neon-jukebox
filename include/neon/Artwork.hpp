#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_render.h>

#include "neon/Models.hpp"

namespace neon {

class ArtworkCache {
public:
    explicit ArtworkCache(std::size_t capacity = 64) : capacity_(capacity) {}
    ~ArtworkCache();
    ArtworkCache(const ArtworkCache&) = delete;
    ArtworkCache& operator=(const ArtworkCache&) = delete;

    SDL_Texture* get(SDL_Renderer* renderer, const Track& track);
    void clear();

private:
    struct Entry {
        SDL_Texture* texture{};
        std::uint64_t lastUsed{};
    };

    static std::vector<std::uint8_t> embeddedBytes(const Track& track);
    static SDL_Texture* loadTexture(SDL_Renderer* renderer, const Track& track);
    static SDL_Texture* generatedTexture(SDL_Renderer* renderer, const Track& track);
    void evictIfNeeded();

    std::size_t capacity_;
    std::uint64_t clock_{};
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace neon
