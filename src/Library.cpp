#include "neon/Library.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <taglib/tag.h>
#include <taglib.h>
#include <taglib/fileref.h>
#include <tfile.h>

#include "neon/Utils.hpp"

namespace neon {
namespace {

std::string tagString(const TagLib::String& value) {
    return value.isEmpty() ? std::string{} : value.to8Bit(true);
}

std::int64_t modifiedTicks(const std::filesystem::path& path) {
    std::error_code error;
    const auto value = std::filesystem::last_write_time(path, error);
    return error ? 0 : value.time_since_epoch().count();
}

std::string normalizedPathKey(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return normalizeForSearch(pathToUtf8(error ? path.lexically_normal() : canonical));
}

bool contains(std::string_view haystack, std::string_view needle) {
    return needle.empty() || haystack.find(needle) != std::string_view::npos;
}

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool embeddedArtworkAvailable(const TagLib::FileRef& reference) {
    for (const auto& picture : reference.complexProperties("PICTURE")) {
        bool valid{};
        const auto data = picture.value("data").toByteVector(&valid);
        if (valid && !data.isEmpty()) return true;
    }
    return false;
}

}  // namespace

LibraryIndex LibraryScanner::scan(std::span<const std::filesystem::path> musicRoots,
                                  std::span<const std::filesystem::path> videoRoots,
                                  const LibraryIndex& cached,
                                  const std::vector<std::string>& favoriteIds,
                                  const ProgressCallback& progress,
                                  const std::atomic_bool* cancel,
                                  const ErrorCallback& onError,
                                  const TrackCallback& onTrack) const {
    LibraryIndex result;

    std::unordered_map<std::string, const Track*> cachedByPath;
    cachedByPath.reserve(cached.tracks.size());
    for (const auto& track : cached.tracks) cachedByPath.emplace(normalizedPathKey(track.path), &track);
    const std::unordered_set<std::string> favorites(favoriteIds.begin(), favoriteIds.end());

    struct Candidate { std::filesystem::path path; MediaKind mediaKind; };
    std::vector<Candidate> files;
    std::unordered_set<std::string> seenFiles;
    const auto discover = [&](std::span<const std::filesystem::path> roots, MediaKind mediaKind,
                              std::vector<std::filesystem::path>& acceptedRoots) {
        std::unordered_set<std::string> seenRoots;
        for (const auto& requestedRoot : roots) {
            if (cancel && cancel->load(std::memory_order_relaxed)) break;
            if (requestedRoot.empty()) continue;

            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(requestedRoot, error);
            const auto root = error ? requestedRoot.lexically_normal() : canonical;
            error.clear();
            if (!seenRoots.insert(normalizedPathKey(root)).second) continue;
            acceptedRoots.push_back(root);

            if (!std::filesystem::is_directory(root, error) || error) {
                if (onError) onError(root, error ? error.message() : "Media source is not a directory");
                continue;
            }
            for (std::filesystem::recursive_directory_iterator it(
                     root, std::filesystem::directory_options::skip_permission_denied, error), end;
                 it != end; it.increment(error)) {
                if (cancel && cancel->load(std::memory_order_relaxed)) break;
                if (error) { error.clear(); continue; }
                const bool supported = mediaKind == MediaKind::Music
                    ? isSupportedAudioFile(it->path()) : isSupportedVideoFile(it->path());
                if (it->is_regular_file(error) && !error && supported &&
                    seenFiles.insert(normalizedPathKey(it->path())).second) {
                    files.push_back({it->path(), mediaKind});
                }
            }
        }
    };
    discover(musicRoots, MediaKind::Music, result.musicRoots);
    discover(videoRoots, MediaKind::Video, result.videoRoots);
    std::ranges::sort(files, [](const auto& left, const auto& right) {
        return normalizeForSearch(pathToUtf8(left.path)) < normalizeForSearch(pathToUtf8(right.path));
    });

    result.tracks.reserve(files.size());
    ScanProgress state{files.size(), 0, {}};
    for (const auto& candidate : files) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        const auto& file = candidate.path;
        state.currentFile = pathToUtf8(file.filename());
        if (progress) progress(state);
        try {
            const auto size = std::filesystem::file_size(file);
            const auto ticks = modifiedTicks(file);
            const auto found = cachedByPath.find(normalizedPathKey(file));
            if (found != cachedByPath.end() && found->second->fileSize == size &&
                found->second->modifiedTicks == ticks) {
                Track track = *found->second;
                track.mediaKind = candidate.mediaKind;
                track.favorite = favorites.contains(track.id) || track.favorite;
                // Artwork files can be added or removed without changing the audio file.
                track.sidecarArtwork = findSidecar(file);
                result.tracks.push_back(std::move(track));
                if (onTrack) onTrack(result.tracks.back());
            } else {
                Track track = readTrack(file, false, candidate.mediaKind);
                track.favorite = favorites.contains(track.id);
                result.tracks.push_back(std::move(track));
                if (onTrack) onTrack(result.tracks.back());
            }
        } catch (const std::exception& exception) {
            // A malformed or concurrently removed file must not abort the scan.
            if (onError) onError(file, exception.what());
        } catch (...) {
            if (onError) onError(file, "Unknown metadata error");
        }
        ++state.processed;
    }

    std::ranges::sort(result.tracks, [](const Track& left, const Track& right) {
        const auto leftKey = normalizeForSearch(left.artist + "\n" + left.album + "\n" + left.title);
        const auto rightKey = normalizeForSearch(right.artist + "\n" + right.album + "\n" + right.title);
        return leftKey < rightKey;
    });
    result.scannedAtMs = nowUnixMs();
    if (progress) { state.currentFile.clear(); progress(state); }
    return result;
}

std::vector<std::size_t> LibraryScanner::filter(const LibraryIndex& library,
                                                std::string_view query,
                                                LibraryFilter filter,
                                                std::string_view genre) {
    const auto needle = normalizeForSearch(query);
    const auto genreNeedle = normalizeForSearch(genre);
    std::vector<std::size_t> output;
    output.reserve(library.tracks.size());
    for (std::size_t i = 0; i < library.tracks.size(); ++i) {
        const auto& track = library.tracks[i];
        if (filter == LibraryFilter::Favorites && !track.favorite) continue;
        if (filter == LibraryFilter::Music && track.mediaKind != MediaKind::Music) continue;
        if (filter == LibraryFilter::Video && track.mediaKind != MediaKind::Video) continue;
        if (!genreNeedle.empty() && normalizeForSearch(track.genre) != genreNeedle) continue;
        if (needle.empty() || contains(normalizeForSearch(track.title), needle) ||
            contains(normalizeForSearch(track.artist), needle) ||
            contains(normalizeForSearch(track.album), needle) ||
            contains(normalizeForSearch(track.genre), needle) ||
            (track.albumYear > 0 && contains(std::to_string(track.albumYear), needle))) output.push_back(i);
    }
    return output;
}

std::vector<std::string> LibraryScanner::genres(const LibraryIndex& library) {
    std::vector<std::string> output;
    std::unordered_set<std::string> seen;
    output.reserve(std::min<std::size_t>(library.tracks.size(), 128));
    for (const auto& track : library.tracks) {
        auto display = trimmed(track.genre);
        const auto key = normalizeForSearch(display);
        if (key.empty() || key == normalizeForSearch("Unknown Genre")) continue;
        if (seen.insert(key).second) output.push_back(std::move(display));
    }
    std::ranges::sort(output, [](const std::string& left, const std::string& right) {
        return normalizeForSearch(left) < normalizeForSearch(right);
    });
    return output;
}

const Track* LibraryScanner::find(const LibraryIndex& library, std::string_view id) {
    const auto found = std::ranges::find(library.tracks, id, &Track::id);
    return found == library.tracks.end() ? nullptr : &*found;
}

Track LibraryScanner::readTrack(const std::filesystem::path& path, bool favorite,
                                MediaKind mediaKind) {
    Track track;
    track.path = path;
    track.id = makeStableId(normalizedPathKey(path));
    track.fileSize = std::filesystem::file_size(path);
    track.modifiedTicks = modifiedTicks(path);
    track.favorite = favorite;
    track.mediaKind = mediaKind;
    track.sidecarArtwork = findSidecar(path);

    TagLib::FileRef reference(TagLib::FileName(path.c_str()), true, TagLib::AudioProperties::Accurate);
    if (reference.isNull() || !reference.file() || !reference.file()->isValid()) {
        if (mediaKind == MediaKind::Music) throw std::runtime_error("Unsupported or damaged audio file");
    } else {
        if (const auto* tag = reference.tag()) {
            track.title = tagString(tag->title());
            track.artist = tagString(tag->artist());
            track.album = tagString(tag->album());
            track.genre = tagString(tag->genre());
            track.albumYear = static_cast<int>(tag->year());
            track.trackNumber = static_cast<int>(tag->track());
        }
        const auto* properties = reference.audioProperties();
        if (properties && properties->lengthInMilliseconds() > 0) {
            track.durationMs = properties->lengthInMilliseconds();
        } else if (mediaKind == MediaKind::Music) {
            throw std::runtime_error("Audio stream has no valid duration");
        }
        track.hasEmbeddedArtwork = mediaKind == MediaKind::Music && embeddedArtworkAvailable(reference);
    }

    if (track.title.empty()) track.title = pathToUtf8(path.stem());
    if (track.artist.empty()) track.artist = "Unknown Artist";
    if (track.album.empty()) track.album = "Unknown Album";
    if (track.genre.empty()) track.genre = "Unknown Genre";
    return track;
}

std::optional<std::filesystem::path> LibraryScanner::findSidecar(const std::filesystem::path& audioPath) {
    static constexpr std::array<std::wstring_view, 6> names{
        L"cover.jpg", L"cover.png", L"folder.jpg", L"folder.png", L"front.jpg", L"front.png"};
    std::error_code error;
    for (const auto name : names) {
        const auto candidate = audioPath.parent_path() / name;
        if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
        error.clear();
    }
    return std::nullopt;
}

}  // namespace neon
