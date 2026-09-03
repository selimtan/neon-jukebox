#include "neon/Artwork.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>

#include <taglib/fileref.h>
#include <taglib/flac/flacpicture.h>

#include "neon/Utils.hpp"

namespace neon {
namespace {

std::vector<std::uint8_t> copyBytes(const TagLib::ByteVector& value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(value.data());
    return {begin, begin + value.size()};
}

}  // namespace

ArtworkCache::~ArtworkCache() { clear(); }

SDL_Texture* ArtworkCache::get(SDL_Renderer* renderer, const Track& track) {
    std::string cacheKey = track.id;
    if (track.onlineArtwork) cacheKey += ':' + pathToUtf8(*track.onlineArtwork);
    if (const auto found = entries_.find(cacheKey); found != entries_.end()) {
        found->second.lastUsed = ++clock_;
        return found->second.texture;
    }
    SDL_Texture* texture = loadTexture(renderer, track);
    if (!texture) texture = generatedTexture(renderer, track);
    entries_.emplace(std::move(cacheKey), Entry{texture, ++clock_});
    evictIfNeeded();
    return texture;
}

void ArtworkCache::clear() {
    for (auto& [_, entry] : entries_) if (entry.texture) SDL_DestroyTexture(entry.texture);
    entries_.clear();
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

SDL_Texture* ArtworkCache::loadTexture(SDL_Renderer* renderer, const Track& track) {
    SDL_Surface* surface{};
    auto embedded = embeddedBytes(track);
    if (!embedded.empty()) {
        if (auto* stream = SDL_IOFromConstMem(embedded.data(), embedded.size())) surface = IMG_Load_IO(stream, true);
    }
    if (!surface && track.sidecarArtwork) surface = IMG_Load(pathToUtf8(*track.sidecarArtwork).c_str());
    if (!surface && track.onlineArtwork) surface = IMG_Load(pathToUtf8(*track.onlineArtwork).c_str());
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

SDL_Texture* ArtworkCache::generatedTexture(SDL_Renderer* renderer, const Track& track) {
    constexpr int size = 384;
    SDL_Surface* surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return nullptr;
    const auto digest = hexDecode(track.id);
    const std::uint8_t accentR = digest.empty() ? 255 : static_cast<std::uint8_t>(128 + digest[0] / 2);
    const std::uint8_t accentG = digest.size() < 2 ? 40 : static_cast<std::uint8_t>(20 + digest[1] / 3);
    const std::uint8_t accentB = digest.size() < 3 ? 220 : static_cast<std::uint8_t>(128 + digest[2] / 2);
    const auto* format = SDL_GetPixelFormatDetails(surface->format);
    auto* pixels = static_cast<std::uint32_t*>(surface->pixels);
    const int stride = surface->pitch / static_cast<int>(sizeof(std::uint32_t));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x - size / 2);
            const float dy = static_cast<float>(y - size / 2);
            const float radius = std::sqrt(dx * dx + dy * dy) / (size * 0.5F);
            std::uint8_t red = 8, green = 9, blue = 22;
            if (radius < 0.86F) {
                const float groove = 0.72F + 0.18F * std::sin(radius * 150.0F);
                red = static_cast<std::uint8_t>(12 + accentR * 0.12F * groove);
                green = static_cast<std::uint8_t>(12 + accentG * 0.10F * groove);
                blue = static_cast<std::uint8_t>(20 + accentB * 0.13F * groove);
            }
            if (radius < 0.31F) {
                red = accentR;
                green = accentG;
                blue = accentB;
            }
            if (radius < 0.055F) red = green = blue = 5;
            pixels[y * stride + x] = SDL_MapRGBA(format, nullptr, red, green, blue, 255);
        }
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

void ArtworkCache::evictIfNeeded() {
    while (entries_.size() > capacity_) {
        const auto oldest = std::ranges::min_element(entries_, {}, [](const auto& item) { return item.second.lastUsed; });
        if (oldest == entries_.end()) break;
        SDL_DestroyTexture(oldest->second.texture);
        entries_.erase(oldest);
    }
}

}  // namespace neon
