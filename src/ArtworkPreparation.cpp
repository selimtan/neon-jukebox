#include "neon/ArtworkPreparation.hpp"

#include <array>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "neon/Artwork.hpp"
#include "neon/BackgroundIo.hpp"
#include "neon/VideoThumbnail.hpp"

namespace neon {
namespace {
std::string versionKey(const Track& track) {
    return track.mediaKind == MediaKind::Video ? track.id + ":video:" + std::to_string(track.fileSize) + ":" +
        std::to_string(track.modifiedTicks) : ArtworkCache::versionKey(track);
}
std::filesystem::path cachePath(const Track& track, const std::filesystem::path& root) {
    return track.mediaKind == MediaKind::Video ? videoThumbnailCachePath(track, root / L"video-thumbnails") :
        ArtworkCache::coverCachePath(track, root / L"music-covers");
}
bool completed(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) ||
        std::filesystem::is_regular_file(path.wstring() + L".missing", ec);
}
}

struct LibraryArtworkPreparer::State {
    struct Request { Track track; std::string version; };
    std::filesystem::path root;
    Prepare prepare;
    std::stop_source stop;
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::array<std::deque<Request>, 2> queues;
    std::unordered_map<std::string, std::string> latest;
    std::vector<std::pair<std::string, std::string>> priority;
    std::vector<Track> finished;
    Progress progress;

    void work(std::size_t lane) {
        const auto token = stop.get_token();
        BackgroundIoCancellation cancelIo([token] { return token.stop_requested(); });
        const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
        struct Priority {
            bool background;
            ~Priority() { if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); }
        } priority{background};
        if (!background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (!token.stop_requested()) {
            Request request;
            {
                std::unique_lock lock(mutex);
                if (!wake.wait(lock, token, [&] { return !queues[lane].empty(); })) return;
                request = std::move(queues[lane].front());
                queues[lane].pop_front();
                --progress.pending;
                if (latest[request.track.id] != request.version) continue;
                ++progress.active;
            }
            bool reused = false, saved = false;
            try {
                const auto path = cachePath(request.track, root);
                reused = completed(path);
                if (!reused && !token.stop_requested()) prepare(request.track, root, token);
                saved = reused || completed(path);
            } catch (...) { /* An inaccessible source remains pending on disk, never a completed marker. */ }
            {
                std::lock_guard lock(mutex);
                --progress.active;
                if (token.stop_requested()) return;
                if (reused) ++progress.reused;
                else if (saved) { ++progress.prepared; finished.push_back(std::move(request.track)); }
                else if (latest[request.track.id] == request.version) latest.erase(request.track.id);
            }
        }
    }
};

LibraryArtworkPreparer::LibraryArtworkPreparer(Prepare prepare) : prepare_(std::move(prepare)) {
    if (!prepare_) prepare_ = [](const Track& track, const std::filesystem::path& root, std::stop_token stop) {
        auto* surface = track.mediaKind == MediaKind::Video
            ? loadVideoThumbnail(track, root / L"video-thumbnails", stop)
            : ArtworkCache::loadCachedCover(track, root / L"music-covers", stop);
        SDL_DestroySurface(surface); // Never create a renderer/texture on these workers.
    };
}
LibraryArtworkPreparer::~LibraryArtworkPreparer() { stop(); }

void LibraryArtworkPreparer::start(const std::filesystem::path& libraryRoot) {
    if (state_) return;
    state_ = std::make_shared<State>();
    state_->root = libraryRoot;
    state_->prepare = prepare_;
    // Independent music/video lanes let a slow video leave music preparation
    // moving. Jobs own copies and no App/UI references, including during Exit.
    for (std::size_t lane = 0; lane < 2; ++lane)
        std::thread([state = state_, lane] { state->work(lane); }).detach();
}

void LibraryArtworkPreparer::enqueue(std::span<const Track> tracks) {
    if (!state_ || state_->stop.stop_requested()) return;
    std::lock_guard lock(state_->mutex);
    for (const auto& track : tracks) {
        if (track.mediaKind == MediaKind::Radio) continue;
        const auto version = versionKey(track);
        const auto found = state_->latest.find(track.id);
        if (found != state_->latest.end() && found->second == version) continue;
        state_->latest[track.id] = version;
        const auto lane = track.mediaKind == MediaKind::Video ? 1 : 0;
        state_->queues[lane].push_back({track, version});
        ++state_->progress.pending;
    }
    state_->wake.notify_all();
}

std::vector<Track> LibraryArtworkPreparer::takeCompleted() {
    std::vector<Track> result;
    if (state_) { std::lock_guard lock(state_->mutex); result.swap(state_->finished); }
    return result;
}

void LibraryArtworkPreparer::prioritize(std::span<const Track* const> tracks) {
    if (!state_ || state_->stop.stop_requested()) return;
    std::lock_guard lock(state_->mutex);
    std::vector<std::pair<std::string, std::string>> priority;
    for (const auto* track : tracks) {
        if (!track) continue;
        const auto found = state_->latest.find(track->id);
        if (found != state_->latest.end()) priority.emplace_back(found->first, found->second);
    }
    if (priority == state_->priority) return;
    state_->priority = priority;
    for (auto it = priority.rbegin(); it != priority.rend(); ++it) {
        for (auto& queue : state_->queues) {
            const auto found = std::find_if(queue.begin(), queue.end(), [&](const auto& request) {
                return request.track.id == it->first && request.version == it->second;
            });
            if (found == queue.end()) continue;
            auto request = std::move(*found);
            queue.erase(found);
            queue.push_front(std::move(request));
        }
    }
}
LibraryArtworkPreparer::Progress LibraryArtworkPreparer::progress() const {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->progress;
}
void LibraryArtworkPreparer::stop() {
    if (!state_) return;
    state_->stop.request_stop();
    state_->wake.notify_all();
    // Some USB drivers acknowledge cancellation late. Workers retain their own
    // state until they unwind; the window and App can shut down immediately.
    state_.reset();
}

} // namespace neon
