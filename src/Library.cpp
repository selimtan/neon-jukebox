#include "neon/Library.hpp"

#include <Windows.h>

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
#include "neon/BackgroundIo.hpp"

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

std::wstring browseName(std::string_view value) {
    auto wide = fromUtf8(trimmed(std::string(value)));
    // The A-Z index groups both Turkish forms of I with the I key.
    for (auto& letter : wide) if (letter == L'ı' || letter == L'İ') letter = L'I';
    return wide;
}

int compareNames(std::wstring_view left, std::wstring_view right) {
    if (left.empty() || right.empty()) return left.empty() ? (right.empty() ? 0 : -1) : 1;
    const int result = CompareStringEx(LOCALE_NAME_INVARIANT,
        NORM_IGNORECASE | NORM_IGNORENONSPACE | SORT_DIGITSASNUMBERS,
        left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()),
        nullptr, nullptr, 0);
    return result ? result - CSTR_EQUAL : left.compare(right);
}

bool matchesArtistInitial(std::wstring_view artist, char initial) {
    if (!initial) return true;
    if (artist.empty()) return false;
    const wchar_t first = artist.front();
    if (initial == '#') return first >= L'0' && first <= L'9';
    const wchar_t letter = initial;
    return compareNames(artist.substr(0, 1), std::wstring_view(&letter, 1)) == 0;
}

}  // namespace

TrackLabel trackLabel(const Track& track) {
    if (track.mediaKind == MediaKind::Radio) return {track.title, track.title};
    if (track.mediaKind != MediaKind::Video) return {track.artist, track.title};
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    auto artist = trim(track.artist);
    auto title = trim(track.title.empty() ? pathToUtf8(track.path.stem()) : track.title);
    const bool unknown = artist.empty() || normalizeForSearch(artist) == "unknown artist";
    // Untagged music videos commonly keep "Artist - Clip" in the file title.
    // Tagged artist metadata wins; only remove a matching repeated prefix.
    for (const std::string_view separator : {" - ", " – ", " — ", " _ ", "_ "}) {
        const auto split = title.find(separator);
        if (split == std::string::npos) continue;
        const auto prefix = trim(title.substr(0, split));
        const auto clip = trim(title.substr(split + separator.size()));
        if (!prefix.empty() && !clip.empty() &&
            (unknown || normalizeForSearch(prefix) == normalizeForSearch(artist))) {
            if (unknown) artist = prefix;
            title = clip;
        }
        break;
    }
    // Keep mix/live/version names, but omit upload-format annotations from the label.
    for (;;) {
        const auto upper = uppercaseForDisplay(title);
        bool removed = false;
        for (const std::string_view suffix : {" (OFFICIAL MUSIC VIDEO)", " (OFFICIAL VIDEO)", " (OFFICIAL)",
             " [OFFICIAL MUSIC VIDEO]", " [OFFICIAL VIDEO]", " (HD)", " [HD]", " (4K)", " [4K]"}) {
            if (upper.size() > suffix.size() && upper.ends_with(suffix)) {
                title = trim(title.substr(0, title.size() - suffix.size()));
                removed = true;
                break;
            }
        }
        if (!removed) break;
    }
    return {artist.empty() ? "Unknown Artist" : artist, title};
}

LibraryIndex LibraryScanner::scan(std::span<const std::filesystem::path> musicRoots,
                                  std::span<const std::filesystem::path> videoRoots,
                                  const LibraryIndex& cached,
                                  const std::vector<std::string>& favoriteIds,
                                  const ProgressCallback& progress,
                                  const std::atomic_bool* cancel,
                                  const ErrorCallback& onError,
                                  const TrackCallback& onTrack) const {
    LibraryIndex result;
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_acquire); };
    if (cancelled()) return result;
    BackgroundIoCancellation cancelIo(cancelled);

    std::unordered_map<std::string, const Track*> cachedByPath;
    cachedByPath.reserve(cached.tracks.size());
    // These paths were resolved when scanned. Indexing the saved catalogue must
    // not touch thousands of files on sleeping/removable/network drives.
    const auto cacheKey = [](const std::filesystem::path& path) {
        return normalizeForSearch(pathToUtf8(path.lexically_normal()));
    };
    for (const auto& track : cached.tracks) {
        if (cancelled()) return result;
        // Radio is merged by App after local scans; never use it as file metadata.
        if (track.mediaKind == MediaKind::Radio) continue;
        cachedByPath.emplace(cacheKey(track.path), &track);
    }
    const std::unordered_set<std::string> favorites(favoriteIds.begin(), favoriteIds.end());

    ScanProgress state;
    const auto processFile = [&](const std::filesystem::path& file, MediaKind mediaKind) {
        if (cancelled()) return;
        ++state.discovered;
        state.currentFile = pathToUtf8(file);
        if (progress) progress(state);
        try {
            const auto size = std::filesystem::file_size(file);
            if (cancelled()) return;
            const auto ticks = modifiedTicks(file);
            if (cancelled()) return;
            const auto found = cachedByPath.find(cacheKey(file));
            Track track;
            if (found != cachedByPath.end() && found->second->fileSize == size &&
                found->second->modifiedTicks == ticks) {
                track = *found->second;
                track.mediaKind = mediaKind;
                track.favorite = favorites.contains(track.id) || track.favorite;
                // Artwork files can be added or removed without changing the audio file.
                track.sidecarArtwork = findSidecar(file);
            } else {
                track = readTrack(file, false, mediaKind);
                track.favorite = favorites.contains(track.id);
            }
            if (cancelled()) return;
            result.tracks.push_back(std::move(track));
            if (onTrack) onTrack(result.tracks.back());
        } catch (const std::exception& exception) {
            // A malformed or concurrently removed file must not abort the scan.
            if (onError && !cancelled()) onError(file, exception.what());
        } catch (...) {
            if (onError && !cancelled()) onError(file, "Unknown metadata error");
        }
        ++state.processed;
        if (progress) progress(state);
    };

    std::unordered_set<std::string> seenFiles;
    const auto discover = [&](std::span<const std::filesystem::path> roots, MediaKind mediaKind,
                              std::vector<std::filesystem::path>& acceptedRoots) {
        std::unordered_set<std::string> seenRoots;
        for (const auto& requestedRoot : roots) {
            if (cancel && cancel->load(std::memory_order_relaxed)) break;
            if (requestedRoot.empty()) continue;

            state.currentFile = pathToUtf8(requestedRoot);
            if (progress) progress(state);
            if (cancelled()) break;

            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(requestedRoot, error);
            if (cancelled()) break;
            const auto root = error ? requestedRoot.lexically_normal() : canonical;
            error.clear();
            if (!seenRoots.insert(cacheKey(root)).second) continue;
            acceptedRoots.push_back(root);

            if (!std::filesystem::is_directory(root, error) || error) {
                if (cancelled()) break;
                if (onError) onError(root, error ? error.message() : "Media source is not a directory");
                continue;
            }
            for (std::filesystem::recursive_directory_iterator it(
                     root, std::filesystem::directory_options::skip_permission_denied, error), end;
                 it != end; it.increment(error)) {
                if (cancel && cancel->load(std::memory_order_relaxed)) break;
                if (error) { error.clear(); continue; }
                // Publish before filesystem/metadata access, including folders
                // with no supported media and slow removable/network sources.
                state.currentFile = pathToUtf8(it->path());
                if (progress) progress(state);
                if (cancelled()) break;
                const bool supported = mediaKind == MediaKind::Music
                    ? isSupportedAudioFile(it->path()) : isSupportedVideoFile(it->path());
                if (it->is_regular_file(error) && !error && supported &&
                    seenFiles.insert(cacheKey(it->path())).second) {
                    // Publish each playable track before walking the remaining folders.
                    processFile(it->path(), mediaKind);
                }
                if (cancelled()) break; // Do not advance the directory iterator after cancellation.
            }
        }
    };
    discover(musicRoots, MediaKind::Music, result.musicRoots);
    discover(videoRoots, MediaKind::Video, result.videoRoots);
    if (cancelled()) {
        if (progress) { state.currentFile.clear(); progress(state); }
        return result;
    }

    const auto ordered = filter(result, {}, LibraryFilter::All);
    std::vector<Track> sorted;
    sorted.reserve(ordered.size());
    for (const auto index : ordered) sorted.push_back(std::move(result.tracks[index]));
    result.tracks = std::move(sorted);
    result.scannedAtMs = nowUnixMs();
    if (progress) { state.currentFile.clear(); progress(state); }
    return result;
}

std::vector<std::size_t> LibraryScanner::filter(const LibraryIndex& library,
                                                std::string_view query,
                                                LibraryFilter filter,
                                                std::string_view genre,
                                                char artistInitial) {
    const auto needle = normalizeForSearch(query);
    const auto genreNeedle = normalizeForSearch(genre);
    struct BrowseEntry {
        std::size_t index;
        std::wstring artist;
        std::wstring title;
        std::wstring album;
    };
    std::vector<BrowseEntry> entries;
    entries.reserve(library.tracks.size());
    std::vector<std::size_t> output;
    output.reserve(library.tracks.size());
    for (std::size_t i = 0; i < library.tracks.size(); ++i) {
        const auto& track = library.tracks[i];
        if (filter == LibraryFilter::Favorites && !track.favorite) continue;
        if (filter == LibraryFilter::Music && track.mediaKind != MediaKind::Music) continue;
        if (filter == LibraryFilter::Video && track.mediaKind != MediaKind::Video) continue;
        if (filter == LibraryFilter::Radio && track.mediaKind != MediaKind::Radio) continue;
        if (!genreNeedle.empty() && normalizeForSearch(track.genre) != genreNeedle) continue;
        const auto label = trackLabel(track);
        auto artist = browseName(label.artist);
        if (!matchesArtistInitial(artist, artistInitial)) continue;
        if (needle.empty() || contains(normalizeForSearch(track.title), needle) ||
            contains(normalizeForSearch(label.title), needle) ||
            contains(normalizeForSearch(label.artist), needle) ||
            contains(normalizeForSearch(track.album), needle) ||
            contains(normalizeForSearch(track.genre), needle) ||
            (track.albumYear > 0 && contains(std::to_string(track.albumYear), needle))) {
            entries.push_back({i, std::move(artist), browseName(label.title), browseName(track.album)});
        }
    }
    // Sort visible indices as well as completed scans: cached libraries and
    // incremental arrivals must use the same artist-first order immediately.
    std::ranges::sort(entries, [&](const BrowseEntry& left, const BrowseEntry& right) {
        if (const int artist = compareNames(left.artist, right.artist)) return artist < 0;
        if (const int title = compareNames(left.title, right.title)) return title < 0;
        if (const int album = compareNames(left.album, right.album)) return album < 0;
        return library.tracks[left.index].id < library.tracks[right.index].id;
    });
    for (const auto& entry : entries) output.push_back(entry.index);
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
