#include "neon/Storage.hpp"

#include <Windows.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace neon {
namespace {

template <typename T>
T readJson(const std::filesystem::path& path, T fallback) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return fallback;
        nlohmann::json json;
        stream >> json;
        return json.get<T>();
    } catch (...) {
        return fallback;
    }
}

}  // namespace

Storage::Storage(std::filesystem::path root, std::filesystem::path libraryRoot)
    : root_(std::move(root)), libraryRoot_(libraryRoot.empty() ? root_ : std::move(libraryRoot)) {
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    std::filesystem::create_directories(libraryRoot_, error);
    writer_ = std::thread([this] { writePending(); });
}

Storage::~Storage() {
    flush();
    { std::lock_guard lock(mutex_); stopping_ = true; }
    changed_.notify_all();
    if (writer_.joinable()) writer_.join();
}

void Storage::saveSettingsAsync(const Settings& settings) const {
    { std::lock_guard lock(mutex_); pendingSettings_ = settings; }
    changed_.notify_all();
}

void Storage::saveLibraryAsync(const LibraryIndex& index) const {
    { std::lock_guard lock(mutex_); pendingLibrary_ = index; }
    changed_.notify_all();
}

bool Storage::flush() const {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return !writing_ && !pendingSettings_ && !pendingLibrary_; });
    return !writeFailed_;
}

void Storage::writePending() {
    for (;;) {
        std::optional<Settings> settings;
        std::optional<LibraryIndex> library;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || pendingSettings_ || pendingLibrary_; });
            if (stopping_) return;
            settings.swap(pendingSettings_);
            library.swap(pendingLibrary_);
            writing_ = true;
        }
        bool success = true;
        try {
            if (settings) success &= atomicWrite(root_ / L"settings.json", nlohmann::json(*settings).dump(2));
            if (library) success &= atomicWrite(libraryRoot_ / L"library.json", nlohmann::json(*library).dump(2));
        } catch (...) { success = false; }
        {
            std::lock_guard lock(mutex_);
            writeFailed_ |= !success;
            writing_ = false;
        }
        changed_.notify_all();
    }
}

Settings Storage::loadSettings() const { flush(); return readJson(root_ / L"settings.json", Settings{}); }

LibraryIndex Storage::loadLibrary() const {
    flush();
    std::error_code error;
    const auto path = libraryRoot_ / L"library.json";
    // Upgrade existing installations without losing cached tags and artwork paths.
    if (std::filesystem::is_regular_file(path, error)) return readJson(path, LibraryIndex{});
    return readJson(root_ / L"library.json", LibraryIndex{});
}

std::vector<QueueItem> Storage::loadQueue() const {
    return readJson(root_ / L"queue.json", std::vector<QueueItem>{});
}

bool Storage::saveSettings(const Settings& settings) const {
    flush();
    return atomicWrite(root_ / L"settings.json", nlohmann::json(settings).dump(2));
}

bool Storage::saveLibrary(const LibraryIndex& index) const {
    flush();
    return atomicWrite(libraryRoot_ / L"library.json", nlohmann::json(index).dump(2));
}

bool Storage::saveQueue(const std::vector<QueueItem>& queue) const {
    return atomicWrite(root_ / L"queue.json", nlohmann::json(queue).dump(2));
}

bool Storage::atomicWrite(const std::filesystem::path& target, const std::string& contents) const {
    const auto temporary = target.wstring() + L".tmp";
    try {
        {
            std::ofstream stream(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
            if (!stream) return false;
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.flush();
            if (!stream) return false;
        }
        return MoveFileExW(temporary.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) {
        DeleteFileW(temporary.c_str());
        return false;
    }
}

}  // namespace neon
