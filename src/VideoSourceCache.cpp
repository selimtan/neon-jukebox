#include "neon/VideoSourceCache.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <vector>

#include "neon/BackgroundIo.hpp"
#include "neon/Utils.hpp"

namespace neon {
namespace {
struct FileHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~FileHandle() { close(); }
    void close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};

struct PartialFile {
    std::filesystem::path path;
    ~PartialFile() { std::error_code ec; if (!path.empty()) std::filesystem::remove(path, ec); }
};

// Only our versioned files directly inside the dedicated cache are eligible.
bool ownedCacheFile(const std::filesystem::path& path) {
    const auto stem = path.stem().wstring();
    return stem.starts_with(L"video-") && stem.size() == 38 &&
        std::all_of(stem.begin() + 6, stem.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        });
}

void makeRoom(const std::filesystem::path& root, const std::filesystem::path& keep,
              std::uintmax_t incoming, std::uintmax_t budget, const std::function<bool()>& cancelled) {
    struct Entry { std::filesystem::path path; std::filesystem::file_time_type used; std::uintmax_t size; };
    std::vector<Entry> entries;
    std::uintmax_t total = incoming;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (cancelled()) return;
        const auto& item = *it;
        if (item.path() == keep || !ownedCacheFile(item.path()) || item.is_symlink(ec) ||
            !item.is_regular_file(ec)) continue;
        const auto size = item.file_size(ec);
        if (ec) { ec.clear(); continue; }
        const auto used = item.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        total += size;
        entries.push_back({item.path(), used, size});
    }
    std::ranges::sort(entries, {}, &Entry::used);
    for (const auto& item : entries) {
        if (cancelled() || total <= std::max(budget, incoming)) break;
        if (std::filesystem::remove(item.path, ec)) total -= item.size;
        ec.clear();
    }
}
} // namespace

bool videoSourceNeedsCache(const std::filesystem::path& source) {
    if (source.native().starts_with(L"\\\\")) return true;
    const auto drive = GetDriveTypeW(source.root_path().c_str());
    return drive == DRIVE_REMOVABLE || drive == DRIVE_REMOTE || drive == DRIVE_CDROM;
}

std::filesystem::path videoSourceCachePath(const Track& track, const std::filesystem::path& root) {
    const auto key = makeStableId(pathToUtf8(track.path.lexically_normal()) + "\n" +
        std::to_string(track.fileSize) + "\n" + std::to_string(track.modifiedTicks));
    return root / pathFromUtf8("video-" + key + pathToUtf8(track.path.extension()));
}

VideoSourceResult cacheVideoSource(const Track& track, const std::filesystem::path& root,
    const std::function<bool()>& cancelled, const std::function<void(int)>& progress,
    std::uintmax_t budgetBytes) {
    if (cancelled()) return {};
    // A progressing copy has no overall decoder-open deadline. A stalled read
    // is cancellable, as are Skip and Exit, even while Windows is inside ReadFile.
    std::atomic<ULONGLONG> lastActivity{GetTickCount64()};
    const auto stalled = [&] { const auto last = lastActivity.load(); return GetTickCount64() - last > 30'000; };
    BackgroundIoCancellation cancelIo([&] { return cancelled() || stalled(); });
    const auto failed = [&](std::string operation, DWORD code = GetLastError()) -> VideoSourceResult {
        if (cancelled()) return {};
        return {{}, stalled() ? "Video source stopped responding while preparing playback" :
            std::move(operation) + " (Windows error " + std::to_string(code) + ")"};
    };
    Track version = track;
    std::error_code ec;
    if (version.fileSize == 0 || version.modifiedTicks == 0) {
        version.fileSize = std::filesystem::file_size(track.path, ec);
        if (ec) return failed("Video source could not be read", ec.value());
        version.modifiedTicks = std::filesystem::last_write_time(track.path, ec).time_since_epoch().count();
        if (ec) return failed("Video source version could not be read", ec.value());
    }
    if (cancelled()) return {};
    const auto cached = videoSourceCachePath(version, root);
    const auto size = std::filesystem::file_size(cached, ec);
    if (!ec && version.fileSize > 0 && size == version.fileSize) {
        std::filesystem::last_write_time(cached, std::filesystem::file_time_type::clock::now(), ec);
        if (progress) progress(100);
        return {cached, {}};
    }
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) return failed("Video playback cache could not be created", ec.value());
    if (progress) progress(0);
    FileHandle source{CreateFileW(track.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED, nullptr)};
    if (!source) return failed("Video source could not be opened");
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(source.value, &length)) return failed("Video size could not be read");
    if (length.QuadPart <= 0) return {{}, "Video source is empty"};
    // Reconcile a stale catalogue before publishing a copy under its version key.
    FILETIME modified{};
    if (!GetFileTime(source.value, nullptr, nullptr, &modified)) return failed("Video version could not be read");
    version.fileSize = static_cast<std::uintmax_t>(length.QuadPart);
    version.modifiedTicks = static_cast<std::int64_t>((static_cast<ULONGLONG>(modified.dwHighDateTime) << 32) |
                                                    modified.dwLowDateTime);
    const auto destination = videoSourceCachePath(version, root);
    const auto existingSize = std::filesystem::file_size(destination, ec);
    if (!ec && existingSize == version.fileSize) {
        std::filesystem::last_write_time(destination, std::filesystem::file_time_type::clock::now(), ec);
        if (progress) progress(100);
        return {destination, {}};
    }
    ec.clear();
    lastActivity.store(GetTickCount64());
    makeRoom(root, destination, version.fileSize, budgetBytes, cancelled);
    if (cancelled()) return {};
    // Unique temporary file: cancellation, crashes and concurrent diagnostics
    // must never expose an incomplete MP4 to Media Foundation.
    PartialFile partial{destination.wstring() + L"." + fromUtf8(randomId()) + L".partial"};
    FileHandle output{CreateFileW(partial.path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!output) return failed("Video playback cache could not be written");
    std::vector<BYTE> buffer(1024 * 1024);
    FileHandle readEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!readEvent.value) return failed("Video read event could not be created");
    std::uintmax_t copied{};
    while (copied < version.fileSize) {
        if (cancelled()) return {};
        DWORD received{}, written{};
        const auto amount = static_cast<DWORD>(std::min<std::uintmax_t>(buffer.size(), version.fileSize - copied));
        OVERLAPPED read{};
        read.hEvent = readEvent.value;
        read.Offset = static_cast<DWORD>(copied);
        read.OffsetHigh = static_cast<DWORD>(copied >> 32);
        ResetEvent(readEvent.value);
        if (!ReadFile(source.value, buffer.data(), amount, &received, &read)) {
            if (GetLastError() != ERROR_IO_PENDING) return failed("Video source read failed");
            while (WaitForSingleObject(readEvent.value, 20) == WAIT_TIMEOUT) {
                if (cancelled() || stalled()) {
                    CancelIoEx(source.value, &read);
                    // The job owns the buffer until Windows completes cancellation.
                    GetOverlappedResult(source.value, &read, &received, TRUE);
                    return failed("Video source read cancelled", ERROR_OPERATION_ABORTED);
                }
            }
            if (!GetOverlappedResult(source.value, &read, &received, FALSE)) return failed("Video source read failed");
        }
        if (!received) return {{}, "Video source ended before the file was complete"};
        if (cancelled()) return {};
        if (!WriteFile(output.value, buffer.data(), received, &written, nullptr) || written != received)
            return failed("Video playback cache write failed");
        copied += received;
        lastActivity.store(GetTickCount64());
        const auto next = static_cast<int>(100 * copied / version.fileSize);
        if (progress) progress(next); // Also a heartbeat when large files stay on the same percentage.
    }
    if (cancelled()) return {};
    output.close();
    if (!MoveFileExW(partial.path.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return failed("Video playback cache could not be saved");
    partial.path.clear();
    return {destination, {}};
}
} // namespace neon
