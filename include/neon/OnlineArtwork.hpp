#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "neon/Models.hpp"

namespace neon {

struct OnlineArtworkProgress {
    std::size_t discovered{};
    std::size_t processed{};
    std::size_t found{};
    std::size_t unavailable{};
    std::size_t retryPending{};
    std::string current;
};

struct OnlineArtworkMatch {
    std::vector<std::string> trackIds;
    std::filesystem::path imagePath;
    std::string artist;
    std::string album;
    std::string genre;
    int albumYear{};
};

class OnlineArtworkFetcher {
public:
    using ProgressCallback = std::function<void(const OnlineArtworkProgress&)>;
    using MatchCallback = std::function<void(OnlineArtworkMatch)>;

    void run(std::span<const Track> tracks,
             const std::filesystem::path& cacheRoot,
             const ProgressCallback& progress,
             const MatchCallback& match,
             const std::atomic_bool* cancel = nullptr) const;
};

}  // namespace neon
