#pragma once

#include <SDL3/SDL_surface.h>
#include <stop_token>
#include "neon/Models.hpp"

namespace neon {
// Call from a background thread. Uses Windows' video thumbnail provider/cache;
// it never opens a player or changes the current playback session.
std::filesystem::path videoThumbnailCachePath(const Track& track, const std::filesystem::path& cacheRoot = {});
SDL_Surface* loadVideoThumbnail(const Track& track, const std::filesystem::path& cacheRoot = {},
                                std::stop_token stop = {});
}
