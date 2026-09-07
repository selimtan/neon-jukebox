#include "neon/Artwork.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <fstream>

#include <taglib/fileref.h>
#include <taglib/flac/flacpicture.h>

#include "neon/Utils.hpp"
#include "neon/VideoThumbnail.hpp"
#include "neon/BackgroundIo.hpp"
#include "neon/ArtworkFileCache.hpp"

namespace neon {
namespace {

std::vector<std::uint8_t> copyBytes(const TagLib::ByteVector& value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(value.data());
    return {begin, begin + value.size()};
}

std::optional<SDL_Rect> spotifyBrandingCrop(SDL_Surface* surface) {
    if (!surface || surface->w != surface->h || surface->w < 128) return std::nullopt;
    const int artworkSide = surface->w * 78 / 100;
    const int margin = (surface->w - artworkSide) / 2;
    if (margin <= 0 || artworkSide >= surface->h) return std::nullopt;

    struct PixelCounts {
        std::uint64_t black{};
        std::uint64_t light{};
        std::uint64_t total{};
    };
    const auto count = [surface](int x0, int y0, int x1, int y1) {
        PixelCounts counts;
        const int step = std::max(1, surface->w / 150);
        for (int y = y0; y < y1; y += step) {
            for (int x = x0; x < x1; x += step) {
                Uint8 red{}, green{}, blue{}, alpha{};
                if (!SDL_ReadSurfacePixel(surface, x, y, &red, &green, &blue, &alpha)) continue;
                ++counts.total;
                if (red < 48 && green < 48 && blue < 48) ++counts.black;
                if (red > 180 && green > 180 && blue > 180) ++counts.light;
            }
        }
        return counts;
    };
    const auto left = count(0, 0, margin, artworkSide);
    const auto right = count(margin + artworkSide, 0, surface->w, artworkSide);
    const auto bottom = count(0, artworkSide, surface->w, surface->h);
    const auto ratio = [](std::uint64_t value, std::uint64_t total) {
        return total == 0 ? 0.0 : static_cast<double>(value) / total;
    };
    if (ratio(left.black, left.total) <= 0.85 || ratio(right.black, right.total) <= 0.85 ||
        ratio(bottom.black, bottom.total) <= 0.55 ||
        ratio(bottom.light, bottom.total) <= 0.07) return std::nullopt;
    return SDL_Rect{margin, 0, artworkSide, artworkSide};
}

SDL_Surface* cropSpotifyBranding(SDL_Surface* surface) {
    const auto crop = spotifyBrandingCrop(surface);
    if (!crop) return surface;
    SDL_Surface* cropped = SDL_CreateSurface(crop->w, crop->h, surface->format);
    if (!cropped) return surface;
    SDL_Rect destination{0, 0, crop->w, crop->h};
    if (!SDL_BlitSurface(surface, &*crop, cropped, &destination)) {
        SDL_DestroySurface(cropped);
        return surface;
    }
    SDL_DestroySurface(surface);
    return cropped;
}

}  // namespace

ArtworkCache::ArtworkCache(std::size_t capacity, Loader loader, std::size_t decoderCount, Kind kind)
    : capacity_(std::max<std::size_t>(1, capacity)),
      decoderCount_(std::clamp<std::size_t>(decoderCount, 1, std::min<std::size_t>(4, capacity_))),
      kind_(kind),
      loader_(std::move(loader)), defaultLoader_(!loader_) {}

ArtworkCache::~ArtworkCache() { clear(); }

std::string ArtworkCache::key(const Track& track) {
    std::string cacheKey = track.id;
    cacheKey += ':' + std::to_string(track.modifiedTicks) + ':' + std::to_string(track.fileSize);
    cacheKey += track.hasEmbeddedArtwork ? ":embedded" : ":external";
    if (track.sidecarArtwork) cacheKey += ':' + pathToUtf8(*track.sidecarArtwork);
    if (track.onlineArtwork) cacheKey += ':' + pathToUtf8(*track.onlineArtwork);
    return cacheKey;
}

bool ArtworkCache::ready(const Track& track) const {
    if (!hasSource(track)) return true;
    const auto found = entries_.find(key(track));
    return found != entries_.end() && found->second.ready;
}

bool ArtworkCache::hasSource(const Track& track) const {
    return track.mediaKind == MediaKind::Video || (kind_ != Kind::VideoFrame && (
        track.hasEmbeddedArtwork || track.sidecarArtwork || track.onlineArtwork));
}

std::uint64_t ArtworkCache::revision(const Track& track) const {
    const auto found = entries_.find(key(track));
    return found == entries_.end() ? 0 : found->second.revision;
}

void ArtworkCache::startWorkers() {
    if (workers_.empty()) {
        for (std::size_t i = 0; i < decoderCount_; ++i) {
            ++activeWorkers_;
            try {
                workers_.emplace_back([this](std::stop_token stop) {
                    struct Done { std::atomic_size_t& count; ~Done() { --count; } } done{activeWorkers_};
                    BackgroundIoCancellation cancelIo([stop] { return stop.stop_requested(); });
                    work(stop);
                });
            } catch (...) { --activeWorkers_; throw; }
        }
    }
    wake_.notify_all();
}

void ArtworkCache::prepare(std::span<const Track* const> tracks) {
    std::unordered_set<std::string> wanted;
    std::vector<Request> ordered;
    ordered.reserve(std::min(tracks.size(), capacity_));
    for (const auto* track : tracks) {
        if (!track || wanted.size() >= capacity_) continue;
        auto cacheKey = key(*track);
        if (!wanted.insert(cacheKey).second) continue;
        auto [entry, inserted] = entries_.try_emplace(cacheKey);
        if (inserted) {
            entry->second.ticket = ++nextTicket_;
            entry->second.ready = !hasSource(*track);
        }
        ordered.push_back({std::move(cacheKey), *track, entry->second.ticket});
    }
    // Keep the most important entries newest when enforcing the memory limit.
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it)
        entries_.at(it->key).lastUsed = ++clock_;
    {
        std::scoped_lock lock(mutex_);
        requests_.clear();
        for (const auto& request : ordered) {
            if (!entries_.at(request.key).ready && !inFlight_.contains(request.ticket))
                requests_.push_back(request);
        }
        // A changed filter/page must not leave obsolete work ahead of the new page.
        std::erase_if(entries_, [&](auto& item) {
            if (item.second.ready || wanted.contains(item.first)) return false;
            SDL_DestroyTexture(item.second.texture);
            return true;
        });
        std::erase_if(results_, [&](const Result& result) {
            const auto found = entries_.find(result.key);
            if (found != entries_.end() && found->second.ticket == result.ticket) return false;
            inFlight_.erase(result.ticket);
            return true;
        });
    }
    evictIfNeeded();
    if (!ordered.empty()) startWorkers();
}

SDL_Texture* ArtworkCache::get(SDL_Renderer* renderer, const Track& track) {
    // A cover must never make the event/render thread wait for disk or decoding.
    const bool sourceAvailable = hasSource(track);
    const auto cacheKey = key(track);
    auto [found, inserted] = entries_.try_emplace(cacheKey);
    auto& entry = found->second;
    entry.lastUsed = ++clock_;
    if (inserted) {
        entry.ticket = ++nextTicket_;
        entry.ready = !sourceAvailable;
        if (sourceAvailable) {
            std::scoped_lock lock(mutex_);
            requests_.push_front({cacheKey, track, entry.ticket});
        }
    }
    if (!entry.texture) entry.texture = generatedTexture(renderer, track, sourceAvailable ? 96 : 384);
    auto* texture = entry.texture;
    evictIfNeeded();
    if (inserted && sourceAvailable) startWorkers();
    return texture;
}

void ArtworkCache::work(std::stop_token stop) {
    while (!stop.stop_requested()) {
        Request request;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return !requests_.empty(); })) return;
            request = std::move(requests_.front());
            requests_.pop_front();
            inFlight_.insert(request.ticket);
        }
        Surface surface;
        try {
            if (defaultLoader_ && !diskCacheRoot_.empty()) {
                const auto path = request.track.mediaKind == MediaKind::Video
                    ? videoThumbnailCachePath(request.track, diskCacheRoot_ / L"video-thumbnails")
                    : coverCachePath(request.track, diskCacheRoot_ / L"music-covers");
                surface.reset(IMG_Load(pathToUtf8(path).c_str()));
            } else {
                surface.reset(defaultLoader_
                    ? (request.track.mediaKind == MediaKind::Video ? loadVideoThumbnail(request.track, {}, stop) : loadCachedCover(request.track, {}, stop))
                    : loader_(request.track));
            }
        }
        catch (...) { /* Unreadable artwork keeps its generated cover. */ }
        if (stop.stop_requested()) return;
        if (!surface) surface.reset(generatedSurface(request.track, 384, kind_ == Kind::VideoFrame));
        std::scoped_lock lock(mutex_);
        if (results_.size() >= capacity_) {
            inFlight_.erase(results_.front().ticket);
            results_.pop_front();
        }
        results_.push_back({std::move(request.key), std::move(surface), request.ticket});
    }
}

void ArtworkCache::update(SDL_Renderer* renderer) {
    const auto started = SDL_GetTicksNS();
    for (int upload = 0; upload < 8; ++upload) {
        Result result;
        {
            std::scoped_lock lock(mutex_);
            if (results_.empty()) return;
            result = std::move(results_.front());
            results_.pop_front();
            inFlight_.erase(result.ticket);
        }
        const auto found = entries_.find(result.key);
        if (found == entries_.end() || found->second.ticket != result.ticket) continue;
        if (auto* texture = result.surface ? SDL_CreateTextureFromSurface(renderer, result.surface.get()) : nullptr) {
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            SDL_DestroyTexture(found->second.texture);
            found->second.texture = texture;
        }
        // Failed files/uploads also finish: the generated cover is the final fallback.
        found->second.ready = true;
        found->second.revision = ++revision_;
        if (SDL_GetTicksNS() - started >= 4000000) break;
    }
}

void ArtworkCache::clear() {
    requestStop();
    waitWithWindowMessages([this] { return activeWorkers_.load() == 0; });
    workers_.clear();
    requests_.clear();
    results_.clear();
    inFlight_.clear();
    for (auto& [_, entry] : entries_) if (entry.texture) SDL_DestroyTexture(entry.texture);
    entries_.clear();
    ++revision_;
}

void ArtworkCache::requestStop() {
    for (auto& worker : workers_) worker.request_stop();
    wake_.notify_all();
}

void ArtworkCache::invalidate(const Track& track) {
    const auto found = entries_.find(key(track));
    if (found == entries_.end()) return;
    SDL_DestroyTexture(found->second.texture);
    entries_.erase(found);
    ++revision_;
}

void ArtworkCache::useDiskCache(const std::filesystem::path& libraryRoot) {
    SDL_assert(workers_.empty());
    diskCacheRoot_ = libraryRoot;
}

std::filesystem::path ArtworkCache::coverCachePath(const Track& track, const std::filesystem::path& root) {
    const auto directory = root.empty() ? pathFromUtf8(SDL_GetBasePath()) / L"library" / L"music-covers" : root;
    return directory / pathFromUtf8(makeStableId(key(track)) + ".png");
}

SDL_Surface* ArtworkCache::loadCachedCover(const Track& track, const std::filesystem::path& root, std::stop_token stop) {
    if (stop.stop_requested()) return nullptr;
    const auto cached = coverCachePath(track, root);
    ArtworkFileLease lease(cached, stop);
    if (!lease || stop.stop_requested()) return nullptr;
    std::error_code ec;
    if (std::filesystem::is_regular_file(cached, ec))
        if (auto* surface = IMG_Load(pathToUtf8(cached).c_str())) return surface;
    const auto missing = std::filesystem::path(cached.wstring() + L".missing");
    if (std::filesystem::is_regular_file(missing, ec)) return nullptr;
    auto* surface = loadSurface(track, stop);
    if (stop.stop_requested()) { SDL_DestroySurface(surface); return nullptr; }
    std::filesystem::create_directories(cached.parent_path(), ec);
    if (!ec) {
        const auto temporary = cached.wstring() + L"." + fromUtf8(randomId()) + L".tmp";
        if (surface) {
            if (IMG_SavePNG(surface, pathToUtf8(temporary).c_str()))
                MoveFileExW(temporary.c_str(), cached.c_str(), MOVEFILE_REPLACE_EXISTING);
        } else {
            // Offline sources stay pending for a later launch/rescan. A genuine
            // no-cover result is remembered for this version/source combination.
            const auto source = track.sidecarArtwork.value_or(track.onlineArtwork.value_or(track.path));
            if ((!track.hasEmbeddedArtwork && !track.sidecarArtwork && !track.onlineArtwork) ||
                std::filesystem::is_regular_file(source, ec)) {
                { std::ofstream marker(temporary, std::ios::binary); marker << "No cover for this file version\n"; }
                MoveFileExW(temporary.c_str(), missing.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        DeleteFileW(temporary.c_str());
    }
    return surface;
}

std::vector<std::uint8_t> ArtworkCache::embeddedBytes(const Track& track) {
    if (!track.hasEmbeddedArtwork) return {};
    TagLib::FileRef reference(TagLib::FileName(track.path.c_str()), false);
    if (reference.isNull()) return {};
    for (const auto& picture : reference.complexProperties("PICTURE")) {
        bool valid{};
        const auto data = picture.value("data").toByteVector(&valid);
        if (valid && !data.isEmpty()) return copyBytes(data);
    }
    return {};
}

SDL_Surface* ArtworkCache::loadSurface(const Track& track, std::stop_token stop) {
    if (stop.stop_requested()) return nullptr;
    SDL_Surface* surface{};
    auto embedded = embeddedBytes(track);
    if (stop.stop_requested()) return nullptr;
    if (!embedded.empty()) {
        if (auto* stream = SDL_IOFromConstMem(embedded.data(), embedded.size())) surface = IMG_Load_IO(stream, true);
    }
    if (surface && !embedded.empty()) surface = cropSpotifyBranding(surface);
    if (!surface && !stop.stop_requested() && track.sidecarArtwork) surface = IMG_Load(pathToUtf8(*track.sidecarArtwork).c_str());
    if (!surface && !stop.stop_requested() && track.onlineArtwork) surface = IMG_Load(pathToUtf8(*track.onlineArtwork).c_str());
    if (!surface) return nullptr;
    // Full-resolution embedded scans are unnecessary for a 340px media panel.
    // Resize off-thread so texture uploads stay small even with large artwork.
    constexpr int maximum = 768;
    const int longest = std::max(surface->w, surface->h);
    if (longest > maximum) {
        SDL_Surface* scaled = SDL_ScaleSurface(surface,
            std::max(1, surface->w * maximum / longest),
            std::max(1, surface->h * maximum / longest), SDL_SCALEMODE_LINEAR);
        if (scaled) { SDL_DestroySurface(surface); surface = scaled; }
    }
    return surface;
}

SDL_Surface* ArtworkCache::generatedSurface(const Track& track, int size, bool video) {
    SDL_Surface* surface = SDL_CreateSurface(size, video ? size * 9 / 16 : size, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return nullptr;
    if (video) {
        // A neutral analogue test card, never a made-up frame or record image.
        constexpr SDL_Color bars[]{{175,177,163,255},{170,161,76,255},{65,143,146,255},
            {67,131,86,255},{138,78,123,255},{147,66,57,255},{57,67,117,255}};
        const auto* details = SDL_GetPixelFormatDetails(surface->format);
        for (int i = 0; i < 7; ++i) {
            SDL_Rect rect{i * size / 7, 0, (i + 1) * size / 7 - i * size / 7, surface->h};
            SDL_FillSurfaceRect(surface, &rect, SDL_MapRGBA(details, nullptr, bars[i].r, bars[i].g, bars[i].b, 255));
        }
        return surface;
    }
    const auto digest = hexDecode(track.id);
    const std::uint8_t accentR = digest.empty() ? 255 : static_cast<std::uint8_t>(128 + digest[0] / 2);
    const std::uint8_t accentG = digest.size() < 2 ? 40 : static_cast<std::uint8_t>(20 + digest[1] / 3);
    const std::uint8_t accentB = digest.size() < 3 ? 220 : static_cast<std::uint8_t>(128 + digest[2] / 2);
    const auto* format = SDL_GetPixelFormatDetails(surface->format);
    auto* pixels = static_cast<std::uint32_t*>(surface->pixels);
    const int stride = surface->pitch / static_cast<int>(sizeof(std::uint32_t));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x + 0.5F - size * 0.5F;
            const float dy = y + 0.5F - size * 0.5F;
            const float radius = std::sqrt(dx * dx + dy * dy) / (size * 0.5F);
            float r = 8, g = 9, b = 22;
            const auto layer = [&](float edge, float red, float green, float blue) {
                const float coverage = std::clamp((edge - radius) * size * 0.5F + 0.5F, 0.0F, 1.0F);
                r += (red - r) * coverage;
                g += (green - g) * coverage;
                b += (blue - b) * coverage;
            };
            const float groove = 0.72F + 0.18F * std::sin(radius * 150.0F);
            layer(0.86F, 12 + accentR * 0.12F * groove, 12 + accentG * 0.10F * groove,
                  20 + accentB * 0.13F * groove);
            layer(0.31F, accentR, accentG, accentB);
            layer(0.055F, 5, 5, 5);
            const auto red = static_cast<std::uint8_t>(std::lround(r));
            const auto green = static_cast<std::uint8_t>(std::lround(g));
            const auto blue = static_cast<std::uint8_t>(std::lround(b));
            pixels[y * stride + x] = SDL_MapRGBA(format, nullptr, red, green, blue, 255);
        }
    }
    return surface;
}

SDL_Texture* ArtworkCache::generatedTexture(SDL_Renderer* renderer, const Track& track, int size) const {
    SDL_Surface* surface = generatedSurface(track, size, kind_ == Kind::VideoFrame);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

void ArtworkCache::evictIfNeeded() {
    while (entries_.size() > capacity_) {
        const auto oldest = std::ranges::min_element(entries_, {}, [](const auto& item) { return item.second.lastUsed; });
        if (oldest == entries_.end()) break;
        {
            std::scoped_lock lock(mutex_);
            const auto ticket = oldest->second.ticket;
            std::erase_if(requests_, [ticket](const Request& request) { return request.ticket == ticket; });
        }
        SDL_DestroyTexture(oldest->second.texture);
        entries_.erase(oldest);
    }
}

}  // namespace neon
