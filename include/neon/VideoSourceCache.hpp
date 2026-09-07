#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "neon/Models.hpp"

namespace neon {

struct VideoSourceResult {
    std::filesystem::path path;
    std::string error;
};

// Called on the media worker only. Local fixed disks can be decoded directly.
bool videoSourceNeedsCache(const std::filesystem::path& source);
std::filesystem::path videoSourceCachePath(const Track& track, const std::filesystem::path& root);

// Publish only complete copies, keyed by the indexed file version. The budget
// retains recent videos (or one oversized video), never the entire library.
VideoSourceResult cacheVideoSource(const Track& track, const std::filesystem::path& root,
    const std::function<bool()>& cancelled, const std::function<void(int)>& progress = {},
    std::uintmax_t budgetBytes = 2ULL * 1024 * 1024 * 1024);

} // namespace neon
