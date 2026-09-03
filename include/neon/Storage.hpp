#pragma once

#include <filesystem>
#include <vector>

#include "neon/Models.hpp"

namespace neon {

class Storage {
public:
    explicit Storage(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] Settings loadSettings() const;
    [[nodiscard]] LibraryIndex loadLibrary() const;
    [[nodiscard]] std::vector<QueueItem> loadQueue() const;

    bool saveSettings(const Settings& settings) const;
    bool saveLibrary(const LibraryIndex& index) const;
    bool saveQueue(const std::vector<QueueItem>& queue) const;

private:
    bool atomicWrite(const std::filesystem::path& target, const std::string& contents) const;
    std::filesystem::path root_;
};

}  // namespace neon
