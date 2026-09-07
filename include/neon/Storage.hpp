#pragma once

#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "neon/Models.hpp"

namespace neon {

class Storage {
public:
    explicit Storage(std::filesystem::path root, std::filesystem::path libraryRoot = {});
    ~Storage();

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] const std::filesystem::path& libraryRoot() const { return libraryRoot_; }
    [[nodiscard]] Settings loadSettings() const;
    [[nodiscard]] LibraryIndex loadLibrary() const;
    [[nodiscard]] std::vector<QueueItem> loadQueue() const;

    bool saveSettings(const Settings& settings) const;
    bool saveLibrary(const LibraryIndex& index) const;
    bool saveQueue(const std::vector<QueueItem>& queue) const;
    void saveSettingsAsync(const Settings& settings) const;
    void saveLibraryAsync(const LibraryIndex& index) const;
    bool flush() const;

private:
    bool atomicWrite(const std::filesystem::path& target, const std::string& contents) const;
    std::filesystem::path root_;
    std::filesystem::path libraryRoot_;
    void writePending();
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    mutable std::optional<Settings> pendingSettings_;
    mutable std::optional<LibraryIndex> pendingLibrary_;
    bool stopping_{};
    bool writing_{};
    bool writeFailed_{};
    std::thread writer_;
};

}  // namespace neon
