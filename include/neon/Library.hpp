#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "neon/Models.hpp"

namespace neon {

struct ScanProgress {
    std::size_t discovered{};
    std::size_t processed{};
    std::string currentFile;
};

class LibraryScanner {
public:
    using ProgressCallback = std::function<void(const ScanProgress&)>;
    using ErrorCallback = std::function<void(const std::filesystem::path&, std::string_view)>;
    using TrackCallback = std::function<void(const Track&)>;

    LibraryIndex scan(std::span<const std::filesystem::path> musicRoots,
                      std::span<const std::filesystem::path> videoRoots,
                      const LibraryIndex& cached,
                      const std::vector<std::string>& favoriteIds,
                      const ProgressCallback& progress = {},
                      const std::atomic_bool* cancel = nullptr,
                      const ErrorCallback& onError = {},
                      const TrackCallback& onTrack = {}) const;

    static std::vector<std::size_t> filter(const LibraryIndex& library,
                                           std::string_view query,
                                           LibraryFilter filter,
                                           std::string_view genre = {});
    static std::vector<std::string> genres(const LibraryIndex& library);
    static const Track* find(const LibraryIndex& library, std::string_view id);

private:
    static Track readTrack(const std::filesystem::path& path, bool favorite, MediaKind mediaKind);
    static std::optional<std::filesystem::path> findSidecar(const std::filesystem::path& audioPath);
};

}  // namespace neon
