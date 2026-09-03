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

Storage::Storage(std::filesystem::path root) : root_(std::move(root)) {
    std::error_code error;
    std::filesystem::create_directories(root_, error);
}

Settings Storage::loadSettings() const { return readJson(root_ / L"settings.json", Settings{}); }

LibraryIndex Storage::loadLibrary() const { return readJson(root_ / L"library.json", LibraryIndex{}); }

std::vector<QueueItem> Storage::loadQueue() const {
    return readJson(root_ / L"queue.json", std::vector<QueueItem>{});
}

bool Storage::saveSettings(const Settings& settings) const {
    return atomicWrite(root_ / L"settings.json", nlohmann::json(settings).dump(2));
}

bool Storage::saveLibrary(const LibraryIndex& index) const {
    return atomicWrite(root_ / L"library.json", nlohmann::json(index).dump(2));
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
