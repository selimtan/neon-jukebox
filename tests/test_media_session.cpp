#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "neon/MediaSession.hpp"

namespace {

std::uint32_t pngDimension(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (bytes.size() < offset + 4) return 0;
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

}  // namespace

int main() {
    neon::MediaSessionInfo identityA;
    identityA.sourceAppId = L"Spotify.exe";
    identityA.title = L"Aynı Parça";
    identityA.artist = L"İlk sanatçı bildirimi";
    auto identityB = identityA;
    identityB.artist = L"Güncellenmiş sanatçı bildirimi";
    if (neon::mediaTrackIdentity(identityA) != neon::mediaTrackIdentity(identityB)) {
        std::cerr << "Artist-only metadata update incorrectly changes track identity.\n";
        return 1;
    }
    identityB.title = L"Yeni Parça";
    if (neon::mediaTrackIdentity(identityA) == neon::mediaTrackIdentity(identityB)) {
        std::cerr << "Title change did not change track identity.\n";
        return 1;
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool changed{};
    neon::MediaSessionWatcher watcher;
    watcher.setSource(neon::MediaSource::Spotify);
    watcher.start([&] {
        std::lock_guard lock(mutex);
        changed = true;
        condition.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(8), [&] { return changed; });
    }
    const auto snapshot = watcher.latest();
    watcher.stop();
    if (!changed) {
        std::cerr << "Media session watcher timed out.\n";
        return 1;
    }
    if (!snapshot.apiAvailable) {
        std::wcerr << L"Media session API unavailable: " << snapshot.error << L'\n';
        return 1;
    }
    std::cout << "Spotify artwork: bytes=" << snapshot.artwork.size()
              << ", mime=" << snapshot.artworkMimeType
              << ", width=" << pngDimension(snapshot.artwork, 16)
              << ", height=" << pngDimension(snapshot.artwork, 20) << '\n';
    std::wcout << L"Media session API ready; session=" << snapshot.sessionAvailable
               << L", playing=" << snapshot.playing << L", source=" << snapshot.sourceAppId
               << L", title=" << snapshot.title
               << L", artworkBytes=" << snapshot.artwork.size()
               << L", artworkMime="
               << std::wstring(snapshot.artworkMimeType.begin(), snapshot.artworkMimeType.end())
               << L'\n';
    return 0;
}
