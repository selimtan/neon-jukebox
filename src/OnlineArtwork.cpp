#include "neon/OnlineArtwork.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "neon/Utils.hpp"

namespace neon {
namespace {

constexpr std::size_t maxJsonBytes = 2 * 1024 * 1024;
constexpr std::size_t maxImageBytes = 8 * 1024 * 1024;
constexpr std::int64_t negativeCacheMs = 14LL * 24 * 60 * 60 * 1000;
constexpr std::string_view attemptsNamespace = "multi-source-v2:";

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET get() const { return value_; }
    [[nodiscard]] explicit operator bool() const { return value_ != nullptr; }

private:
    HINTERNET value_{};
};

struct HttpResponse {
    bool received{};
    DWORD status{};
    std::vector<std::uint8_t> body;
};

struct AlbumGroup {
    std::string cacheKey;
    std::string artist;
    std::string searchTitle;
    std::string fallbackTrackTitle;
    bool titleIsTrack{};
    std::vector<std::string> trackIds;
    bool needsArtwork{};
    bool needsArtist{};
    bool needsAlbum{};
    bool needsGenre{};
    bool needsYear{};
};

struct MetadataCandidate {
    std::string artist;
    std::string album;
    std::string genre;
    int albumYear{};
};

struct ReleaseLookup {
    std::optional<std::string> id;
    MetadataCandidate metadata;
    bool retryPending{};
};

struct ProviderLookup {
    std::vector<std::string> imageUrls;
    MetadataCandidate metadata;
    bool retryPending{};
};

struct JsonLookup {
    std::optional<nlohmann::json> document;
    bool retryPending{};
};

struct ProviderThrottle {
    std::chrono::steady_clock::time_point lastRequest;
    std::chrono::milliseconds spacing;
};

bool cancelled(const std::atomic_bool* value) {
    return value && value->load(std::memory_order_relaxed);
}

std::string urlEncode(std::string_view value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() * 3);
    for (const unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            output.push_back(static_cast<char>(byte));
        } else {
            output.push_back('%');
            output.push_back(digits[byte >> 4]);
            output.push_back(digits[byte & 0x0F]);
        }
    }
    return output;
}

std::string luceneValue(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    for (const char character : value) {
        if (character == '\\' || character == '"') output.push_back('\\');
        output.push_back(character);
    }
    output.push_back('"');
    return output;
}

std::string simplifiedReleaseTitle(std::string value) {
    static const std::regex bracketedEdition(
        R"(\s*[\(\[]\s*(deluxe|expanded|special|remaster(?:ed)?|anniversary|bonus|disc|disk|cd|single|ep)[^\)\]]*[\)\]]\s*$)",
        std::regex_constants::icase);
    static const std::regex plainEdition(
        R"(\s*[-:]\s*(deluxe|expanded|special|remaster(?:ed)?|anniversary|bonus|disc|disk|cd|single|ep).*$)",
        std::regex_constants::icase);
    value = std::regex_replace(value, bracketedEdition, "");
    value = std::regex_replace(value, plainEdition, "");
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string simplifiedArtist(std::string value) {
    static const std::regex featuredArtist(
        R"(\s*(?:[\(\[]\s*)?(?:feat(?:uring)?|ft)\.?\s+.*$)",
        std::regex_constants::icase);
    value = std::regex_replace(value, featuredArtist, "");
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string canonicalText(std::string_view value) {
    try {
        const std::wstring lowered = fromUtf8(normalizeForSearch(value));
        if (lowered.empty()) return {};
        const int decomposedCount = NormalizeString(NormalizationD, lowered.data(),
                                                      static_cast<int>(lowered.size()), nullptr, 0);
        std::wstring decomposed;
        if (decomposedCount > 0) {
            decomposed.resize(static_cast<std::size_t>(decomposedCount));
            NormalizeString(NormalizationD, lowered.data(), static_cast<int>(lowered.size()),
                            decomposed.data(), decomposedCount);
        } else {
            decomposed = lowered;
        }

        std::vector<WORD> type1(decomposed.size());
        std::vector<WORD> type3(decomposed.size());
        const bool haveTypes = !decomposed.empty() &&
            GetStringTypeW(CT_CTYPE1, decomposed.data(), static_cast<int>(decomposed.size()), type1.data()) &&
            GetStringTypeW(CT_CTYPE3, decomposed.data(), static_cast<int>(decomposed.size()), type3.data());

        std::wstring clean;
        clean.reserve(decomposed.size());
        bool pendingSpace = false;
        for (std::size_t i = 0; i < decomposed.size(); ++i) {
            if (haveTypes && (type3[i] & C3_NONSPACING) != 0) continue;
            const bool alphaNumeric = haveTypes && (type1[i] & (C1_ALPHA | C1_DIGIT)) != 0;
            if (alphaNumeric) {
                if (pendingSpace && !clean.empty()) clean.push_back(L' ');
                clean.push_back(decomposed[i]);
                pendingSpace = false;
            } else {
                pendingSpace = true;
            }
        }
        return toUtf8(clean);
    } catch (...) {
        std::string fallback(value);
        std::transform(fallback.begin(), fallback.end(), fallback.begin(), [](const unsigned char byte) {
            return static_cast<char>(std::tolower(byte));
        });
        return fallback;
    }
}

std::vector<std::string> meaningfulTokens(std::string_view value) {
    static const std::unordered_set<std::string> ignored{
        "a", "an", "and", "the", "feat", "featuring", "ft", "edition", "version",
        "deluxe", "expanded", "special", "remaster", "remastered", "anniversary"
    };
    std::vector<std::string> all;
    std::istringstream stream{std::string(value)};
    for (std::string token; stream >> token;) all.push_back(std::move(token));
    std::vector<std::string> filtered;
    for (const auto& token : all) if (!ignored.contains(token)) filtered.push_back(token);
    return filtered.empty() ? all : filtered;
}

int textMatchScore(std::string_view target, std::string_view candidate, bool simplifyTitle) {
    const std::string targetValue = canonicalText(
        simplifyTitle ? simplifiedReleaseTitle(std::string(target)) : std::string(target));
    const std::string candidateValue = canonicalText(
        simplifyTitle ? simplifiedReleaseTitle(std::string(candidate)) : std::string(candidate));
    if (targetValue.empty() || candidateValue.empty()) return 0;
    if (targetValue == candidateValue) return 100;

    const std::string paddedCandidate = " " + candidateValue + " ";
    if (paddedCandidate.find(" " + targetValue + " ") != std::string::npos) return 88;
    const std::string paddedTarget = " " + targetValue + " ";
    if (targetValue.size() >= 4 && paddedTarget.find(" " + candidateValue + " ") != std::string::npos) return 82;

    const auto targetTokens = meaningfulTokens(targetValue);
    const auto candidateTokens = meaningfulTokens(candidateValue);
    if (targetTokens.empty() || candidateTokens.empty()) return 0;
    const std::unordered_set<std::string> candidateSet(candidateTokens.begin(), candidateTokens.end());
    std::size_t matched{};
    for (const auto& token : targetTokens) if (candidateSet.contains(token)) ++matched;
    const int coverage = static_cast<int>((matched * 100) / targetTokens.size());
    if (coverage == 100) return 80;
    if (coverage >= 75) return 72;
    if (coverage >= 60) return 62;
    return coverage / 2;
}

int structuredMatchScore(std::string_view targetArtist, std::string_view targetTitle,
                         std::string_view candidateArtist, std::string_view candidateTitle) {
    const int titleScore = textMatchScore(targetTitle, candidateTitle, true);
    if (targetArtist.empty()) return titleScore >= 88 ? titleScore * 2 : -1;
    const int artistScore = textMatchScore(simplifiedArtist(std::string(targetArtist)), candidateArtist, false);
    if (artistScore < 62 || titleScore < 62) return -1;
    return artistScore + titleScore * 2;
}

std::string jsonString(const nlohmann::json& object, std::string_view key) {
    if (!object.is_object()) return {};
    const auto found = object.find(std::string(key));
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

bool missingMetadataText(std::string_view value, std::string_view fallback) {
    return value.empty() || normalizeForSearch(value) == normalizeForSearch(fallback);
}

int yearFromText(std::string_view value) {
    if (value.size() < 4 || !std::all_of(value.begin(), value.begin() + 4,
                                         [](unsigned char character) { return std::isdigit(character) != 0; })) {
        return 0;
    }
    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
                     (value[2] - '0') * 10 + (value[3] - '0');
    return year >= 1000 && year <= 3000 ? year : 0;
}

std::string musicBrainzArtist(const nlohmann::json& object) {
    const auto credits = object.find("artist-credit");
    if (credits == object.end() || !credits->is_array()) return {};
    std::string result;
    for (const auto& credit : *credits) {
        std::string name = jsonString(credit, "name");
        const auto artist = credit.find("artist");
        if (name.empty() && artist != credit.end()) name = jsonString(*artist, "name");
        if (!name.empty()) result += name;
        result += jsonString(credit, "joinphrase");
    }
    return result;
}

std::string highestRatedGenre(const nlohmann::json& object) {
    const auto genres = object.find("genres");
    if (genres == object.end() || !genres->is_array()) return {};
    std::string best;
    int bestCount = -1;
    for (const auto& genre : *genres) {
        if (!genre.is_object()) continue;
        const std::string name = jsonString(genre, "name");
        const int count = genre.value("count", 0);
        if (!name.empty() && count > bestCount) {
            best = name;
            bestCount = count;
        }
    }
    return best;
}

void mergeMetadata(MetadataCandidate& target, const MetadataCandidate& candidate) {
    if (target.artist.empty()) target.artist = candidate.artist;
    if (target.album.empty()) target.album = candidate.album;
    if (target.genre.empty()) target.genre = candidate.genre;
    if (target.albumYear == 0) target.albumYear = candidate.albumYear;
}

bool metadataComplete(const AlbumGroup& group, const MetadataCandidate& metadata) {
    return (!group.needsArtist || !metadata.artist.empty()) &&
           (!group.needsAlbum || !metadata.album.empty()) &&
           (!group.needsGenre || !metadata.genre.empty()) &&
           (!group.needsYear || metadata.albumYear > 0);
}

void appendUnique(std::vector<std::string>& values, std::string value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

std::string upgradedAppleArtwork(std::string url) {
    static const std::regex dimensions(R"(/\d+x\d+bb(\.[A-Za-z0-9]+)(?:\?.*)?$)",
                                       std::regex_constants::icase);
    return std::regex_replace(url, dimensions, "/1200x1200bb$1");
}

bool retryable(const HttpResponse& response) {
    return !response.received || response.status == 0 || response.status == 408 ||
           response.status == 425 || response.status == 429 || response.status == 500 ||
           response.status == 502 || response.status == 503 || response.status == 504;
}

void cancellableDelay(std::chrono::milliseconds duration, const std::atomic_bool* cancel) {
    const auto ready = std::chrono::steady_clock::now() + duration;
    while (!cancelled(cancel) && std::chrono::steady_clock::now() < ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void waitForProvider(ProviderThrottle& provider, const std::atomic_bool* cancel) {
    const auto ready = provider.lastRequest + provider.spacing;
    while (!cancelled(cancel) && std::chrono::steady_clock::now() < ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    provider.lastRequest = std::chrono::steady_clock::now();
}

HttpResponse httpGet(HINTERNET session, std::string_view url, std::wstring_view accept,
                     std::size_t maximumBytes, const std::atomic_bool* cancel) {
    HttpResponse response;
    if (!session || cancelled(cancel)) return response;
    const std::wstring wideUrl = fromUtf8(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components)) return response;

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring resource(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    InternetHandle connection(WinHttpConnect(session, host.c_str(), components.nPort, 0));
    if (!connection) return response;
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", resource.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) return response;

    const std::wstring headers = L"Accept: " + std::wstring(accept) + L"\r\n";
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) return response;
    response.received = true;
    DWORD statusBytes = sizeof(response.status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusBytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (response.status != 200) return response;

    std::array<std::uint8_t, 16 * 1024> buffer{};
    while (!cancelled(cancel)) {
        DWORD read{};
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            response.received = false;
            response.body.clear();
            return response;
        }
        if (read == 0) break;
        if (response.body.size() + read > maximumBytes) {
            response.received = false;
            response.body.clear();
            return response;
        }
        response.body.insert(response.body.end(), buffer.begin(), buffer.begin() + read);
    }
    return response;
}

bool validImage(std::span<const std::uint8_t> data) {
    const bool jpeg = data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    const bool png = data.size() >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
    return jpeg || png;
}

bool atomicBinaryWrite(const std::filesystem::path& target, std::span<const std::uint8_t> bytes) {
    const std::filesystem::path temporary = target.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) return false;
    }
    if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temporary.c_str());
    return false;
}

nlohmann::json loadAttempts(const std::filesystem::path& path) {
    try {
        std::ifstream stream(path, std::ios::binary);
        nlohmann::json value;
        if (stream) stream >> value;
        if (value.is_object()) return value;
    } catch (...) {
    }
    return nlohmann::json::object();
}

void saveAttempts(const std::filesystem::path& path, const nlohmann::json& attempts) {
    const std::string text = attempts.dump(2);
    atomicBinaryWrite(path, std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

ReleaseLookup releaseGroup(const nlohmann::json& document, int minimumScore) {
    ReleaseLookup result;
    try {
        const auto groups = document.find("release-groups");
        if (groups == document.end() || !groups->is_array()) return result;
        for (const auto& group : *groups) {
            if (group.value("score", 0) >= minimumScore) {
                const std::string id = jsonString(group, "id");
                if (id.empty()) continue;
                result.id = id;
                result.metadata.artist = musicBrainzArtist(group);
                result.metadata.album = jsonString(group, "title");
                result.metadata.genre = highestRatedGenre(group);
                result.metadata.albumYear = yearFromText(jsonString(group, "first-release-date"));
                return result;
            }
        }
    } catch (...) {
    }
    return result;
}

}  // namespace

void OnlineArtworkFetcher::run(std::span<const Track> tracks,
                               const std::filesystem::path& cacheRoot,
                               const ProgressCallback& onProgress,
                               const MatchCallback& onMatch,
                               const std::atomic_bool* cancel) const {
    std::error_code filesystemError;
    std::filesystem::create_directories(cacheRoot, filesystemError);
    if (filesystemError || cancelled(cancel)) return;

    std::vector<AlbumGroup> groups;
    std::unordered_map<std::string, std::size_t> groupIndices;
    for (const auto& track : tracks) {
        if (track.mediaKind != MediaKind::Music) continue;
        const bool cachedOnline = track.onlineArtwork && std::filesystem::is_regular_file(*track.onlineArtwork, filesystemError);
        filesystemError.clear();
        const bool availableSidecar = track.sidecarArtwork &&
                                      std::filesystem::is_regular_file(*track.sidecarArtwork, filesystemError);
        filesystemError.clear();
        const bool needsArtwork = !track.hasEmbeddedArtwork && !availableSidecar && !cachedOnline;
        const bool needsArtist = missingMetadataText(track.artist, "Unknown Artist");
        const bool needsAlbum = missingMetadataText(track.album, "Unknown Album");
        const bool needsGenre = missingMetadataText(track.genre, "Unknown Genre");
        const bool needsYear = track.albumYear <= 0;
        if (!needsArtwork && !needsArtist && !needsAlbum && !needsGenre && !needsYear) continue;

        const bool titleIsTrack = needsAlbum;
        const std::string title = titleIsTrack ? track.title : track.album;
        if (title.empty()) continue;
        std::string artist = needsArtist ? std::string{} : simplifiedArtist(track.artist);
        if (artist.empty() && !needsArtist) artist = track.artist;
        const std::string groupKey = canonicalText(artist + "\n" + title);
        auto [found, inserted] = groupIndices.emplace(groupKey, groups.size());
        if (inserted) {
            AlbumGroup group;
            group.cacheKey = makeStableId(groupKey);
            group.artist = artist;
            group.searchTitle = title;
            group.fallbackTrackTitle = track.title;
            group.titleIsTrack = titleIsTrack;
            groups.push_back(std::move(group));
        }
        auto& group = groups[found->second];
        group.trackIds.push_back(track.id);
        group.needsArtwork = group.needsArtwork || needsArtwork;
        group.needsArtist = group.needsArtist || needsArtist;
        group.needsAlbum = group.needsAlbum || needsAlbum;
        group.needsGenre = group.needsGenre || needsGenre;
        group.needsYear = group.needsYear || needsYear;
    }

    OnlineArtworkProgress state;
    state.discovered = groups.size();
    if (onProgress) onProgress(state);
    if (groups.empty()) return;

    InternetHandle session(WinHttpOpen(L"NeonJukebox/1.1 (local Windows artwork fetcher)",
                                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        state.processed = groups.size();
        state.retryPending = groups.size();
        if (onProgress) onProgress(state);
        return;
    }
    WinHttpSetTimeouts(session.get(), 5000, 5000, 15000, 20000);

    const auto attemptsPath = cacheRoot / L"attempts.json";
    auto attempts = loadAttempts(attemptsPath);
    const auto now = std::chrono::steady_clock::now();
    ProviderThrottle musicBrainz{now - std::chrono::seconds(2), std::chrono::milliseconds(1100)};
    ProviderThrottle apple{now - std::chrono::seconds(4), std::chrono::milliseconds(3100)};
    ProviderThrottle deezer{now - std::chrono::seconds(1), std::chrono::milliseconds(600)};
    ProviderThrottle audioDb{now - std::chrono::seconds(3), std::chrono::milliseconds(2100)};
    ProviderThrottle wikimedia{now - std::chrono::seconds(1), std::chrono::milliseconds(600)};
    ProviderThrottle internetArchive{now - std::chrono::seconds(2), std::chrono::milliseconds(1100)};

    const auto requestJson = [&](std::string_view url, ProviderThrottle& throttle) -> JsonLookup {
        for (int attempt = 0; attempt < 3 && !cancelled(cancel); ++attempt) {
            waitForProvider(throttle, cancel);
            if (cancelled(cancel)) return {{}, true};
            const auto response = httpGet(session.get(), url, L"application/json", maxJsonBytes, cancel);
            if (response.received && response.status == 200) {
                try {
                    return {nlohmann::json::parse(response.body.begin(), response.body.end()), false};
                } catch (...) {
                    if (attempt == 2) return {{}, true};
                }
            } else if (!retryable(response)) {
                return {};
            } else if (attempt == 2) {
                return {{}, true};
            }
            cancellableDelay(std::chrono::milliseconds(1000 * (1 << attempt)), cancel);
        }
        return {{}, true};
    };

    const auto findReleaseGroup = [&](std::string_view title, std::string_view artist,
                                      int minimumScore) -> ReleaseLookup {
        const std::string query = "release:" + luceneValue(title) +
                                  (artist.empty() ? std::string{} : " AND artist:" + luceneValue(artist));
        const std::string url = "https://musicbrainz.org/ws/2/release-group/?query=" +
                                urlEncode(query) + "&fmt=json&limit=5";
        const auto lookup = requestJson(url, musicBrainz);
        if (!lookup.document) {
            ReleaseLookup result;
            result.retryPending = lookup.retryPending;
            return result;
        }
        return releaseGroup(*lookup.document, minimumScore);
    };

    const auto musicBrainzLookup = [&](const AlbumGroup& group) -> ProviderLookup {
        if (group.artist.empty()) return {};
        ReleaseLookup lookup = findReleaseGroup(group.searchTitle, group.artist, 80);
        const std::string simplifiedTitle = simplifiedReleaseTitle(group.searchTitle);
        if (!lookup.id && !lookup.retryPending && !simplifiedTitle.empty() &&
            canonicalText(simplifiedTitle) != canonicalText(group.searchTitle)) {
            lookup = findReleaseGroup(simplifiedTitle, group.artist, 80);
        }
        ProviderLookup result;
        result.retryPending = lookup.retryPending;
        result.metadata = lookup.metadata;
        if (lookup.id && group.needsGenre && result.metadata.genre.empty()) {
            const auto details = requestJson(
                "https://musicbrainz.org/ws/2/release-group/" + *lookup.id +
                    "?inc=genres%2Bartist-credits&fmt=json",
                musicBrainz);
            result.retryPending = result.retryPending || details.retryPending;
            if (details.document) {
                MetadataCandidate candidate;
                candidate.artist = musicBrainzArtist(*details.document);
                candidate.album = jsonString(*details.document, "title");
                candidate.genre = highestRatedGenre(*details.document);
                candidate.albumYear = yearFromText(jsonString(*details.document, "first-release-date"));
                mergeMetadata(result.metadata, candidate);
            }
        }
        if (lookup.id) appendUnique(result.imageUrls,
            "https://coverartarchive.org/release-group/" + *lookup.id + "/front-1200");
        if (lookup.id) appendUnique(result.imageUrls,
            "https://coverartarchive.org/release-group/" + *lookup.id + "/front-500");
        return result;
    };

    const auto appleLookup = [&](const AlbumGroup& group) -> ProviderLookup {
        const auto searchApple = [&](bool trackSearch, std::string_view searchTitle) {
            const std::string entity = trackSearch ? "song" : "album";
            const std::string searchText = group.artist.empty()
                ? std::string(searchTitle) : group.artist + " " + std::string(searchTitle);
            const std::string url = "https://itunes.apple.com/search?term=" +
                                    urlEncode(searchText) +
                                    "&media=music&entity=" + entity + "&limit=10";
            const auto lookup = requestJson(url, apple);
            ProviderLookup result;
            result.retryPending = lookup.retryPending;
            if (!lookup.document) return result;
            const auto results = lookup.document->find("results");
            if (results == lookup.document->end() || !results->is_array()) return result;

            int bestScore = -1;
            const nlohmann::json* best{};
            for (const auto& candidate : *results) {
                const std::string artist = jsonString(candidate, "artistName");
                const std::string title = jsonString(candidate, trackSearch ? "trackName" : "collectionName");
                const int score = structuredMatchScore(group.artist, searchTitle, artist, title);
                if (score > bestScore) {
                    bestScore = score;
                    best = &candidate;
                }
            }
            if (bestScore >= 0 && best) {
                result.metadata.artist = jsonString(*best, "artistName");
                result.metadata.album = jsonString(*best, "collectionName");
                result.metadata.genre = jsonString(*best, "primaryGenreName");
                result.metadata.albumYear = yearFromText(jsonString(*best, "releaseDate"));
                const std::string bestUrl = jsonString(*best, "artworkUrl100");
                appendUnique(result.imageUrls, upgradedAppleArtwork(bestUrl));
                appendUnique(result.imageUrls, bestUrl);
            }
            return result;
        };

        ProviderLookup result = searchApple(group.titleIsTrack, group.searchTitle);
        if (!group.titleIsTrack && group.needsGenre && result.metadata.genre.empty() &&
            !group.fallbackTrackTitle.empty()) {
            auto fallback = searchApple(true, group.fallbackTrackTitle);
            result.retryPending = result.retryPending || fallback.retryPending;
            mergeMetadata(result.metadata, fallback.metadata);
            for (auto& url : fallback.imageUrls) appendUnique(result.imageUrls, std::move(url));
        }
        return result;
    };

    const auto deezerLookup = [&](const AlbumGroup& group) -> ProviderLookup {
        const std::string kind = group.titleIsTrack ? "track" : "album";
        const std::string titleField = group.titleIsTrack ? "track" : "album";
        const std::string query = "artist:\"" + group.artist + "\" " + titleField +
                                  ":\"" + group.searchTitle + "\"";
        const std::string url = "https://api.deezer.com/search/" + kind + "?q=" +
                                urlEncode(query) + "&limit=10";
        const auto lookup = requestJson(url, deezer);
        ProviderLookup result;
        result.retryPending = lookup.retryPending;
        if (!lookup.document) return result;
        const auto candidates = lookup.document->find("data");
        if (candidates == lookup.document->end() || !candidates->is_array()) return result;

        int bestScore = -1;
        const nlohmann::json* best{};
        for (const auto& candidate : *candidates) {
            const auto artistObject = candidate.find("artist");
            const std::string artist = artistObject != candidate.end() ? jsonString(*artistObject, "name") : std::string{};
            const std::string title = jsonString(candidate, "title");
            const int score = structuredMatchScore(group.artist, group.searchTitle, artist, title);
            if (score > bestScore) {
                bestScore = score;
                best = &candidate;
            }
        }
        if (bestScore < 0 || !best) return result;
        const nlohmann::json* artwork = best;
        if (group.titleIsTrack) {
            const auto albumObject = best->find("album");
            if (albumObject == best->end() || !albumObject->is_object()) return result;
            artwork = &*albumObject;
        }
        const auto artistObject = best->find("artist");
        if (artistObject != best->end()) result.metadata.artist = jsonString(*artistObject, "name");
        result.metadata.album = jsonString(*artwork, "title");
        result.metadata.albumYear = yearFromText(jsonString(*artwork, "release_date"));
        appendUnique(result.imageUrls, jsonString(*artwork, "cover_xl"));
        appendUnique(result.imageUrls, jsonString(*artwork, "cover_big"));
        appendUnique(result.imageUrls, jsonString(*artwork, "cover_medium"));

        const auto albumId = artwork->find("id");
        if ((group.needsGenre || group.needsYear) && albumId != artwork->end() &&
            (albumId->is_number_integer() || albumId->is_number_unsigned())) {
            const auto details = requestJson("https://api.deezer.com/album/" +
                std::to_string(albumId->get<std::uint64_t>()), deezer);
            result.retryPending = result.retryPending || details.retryPending;
            if (details.document && details.document->is_object()) {
                result.metadata.album = result.metadata.album.empty()
                    ? jsonString(*details.document, "title") : result.metadata.album;
                if (result.metadata.albumYear == 0) {
                    result.metadata.albumYear = yearFromText(jsonString(*details.document, "release_date"));
                }
                const auto genres = details.document->find("genres");
                if (genres != details.document->end() && genres->is_object()) {
                    const auto data = genres->find("data");
                    if (data != genres->end() && data->is_array() && !data->empty()) {
                        result.metadata.genre = jsonString(data->front(), "name");
                    }
                }
                appendUnique(result.imageUrls, jsonString(*details.document, "cover_xl"));
                appendUnique(result.imageUrls, jsonString(*details.document, "cover_big"));
            }
        }
        return result;
    };

    const auto audioDbLookup = [&](const AlbumGroup& group) -> ProviderLookup {
        const std::string endpoint = group.titleIsTrack ? "searchtrack.php" : "searchalbum.php";
        const std::string titleParameter = group.titleIsTrack ? "t" : "a";
        const std::string url = "https://www.theaudiodb.com/api/v1/json/123/" + endpoint +
                                "?s=" + urlEncode(group.artist) + "&" + titleParameter + "=" +
                                urlEncode(group.searchTitle);
        const auto lookup = requestJson(url, audioDb);
        ProviderLookup result;
        result.retryPending = lookup.retryPending;
        if (!lookup.document) return result;
        const std::string rootField = group.titleIsTrack ? "track" : "album";
        const auto candidates = lookup.document->find(rootField);
        if (candidates == lookup.document->end() || !candidates->is_array()) return result;

        int bestScore = -1;
        const nlohmann::json* best{};
        for (const auto& candidate : *candidates) {
            const std::string artist = jsonString(candidate, "strArtist");
            const std::string title = jsonString(candidate, group.titleIsTrack ? "strTrack" : "strAlbum");
            const int score = structuredMatchScore(group.artist, group.searchTitle, artist, title);
            if (score > bestScore) {
                bestScore = score;
                best = &candidate;
            }
        }
        if (bestScore < 0 || !best) return result;
        result.metadata.artist = jsonString(*best, "strArtist");
        result.metadata.album = jsonString(*best, "strAlbum");
        result.metadata.genre = jsonString(*best, "strGenre");
        result.metadata.albumYear = yearFromText(jsonString(*best, "intYearReleased"));
        appendUnique(result.imageUrls, jsonString(*best, "strAlbumThumbHQ"));
        appendUnique(result.imageUrls, jsonString(*best, "strAlbumThumb"));
        appendUnique(result.imageUrls, jsonString(*best, "strTrackThumb"));
        return result;
    };

    const auto wikimediaLookup = [&](const AlbumGroup& group) -> ProviderLookup {
        const std::string query = group.artist + " " + group.searchTitle +
                                  (group.titleIsTrack ? " song" : " album cover");
        const std::string url = "https://commons.wikimedia.org/w/api.php?action=query&generator=search&gsrsearch=" +
            urlEncode(query) + "&gsrnamespace=6&gsrlimit=10&prop=imageinfo&iiprop=url%7Cmime&iiurlwidth=1200"
            "&format=json&formatversion=2";
        const auto lookup = requestJson(url, wikimedia);
        ProviderLookup result;
        result.retryPending = lookup.retryPending;
        if (!lookup.document) return result;
        const auto queryObject = lookup.document->find("query");
        if (queryObject == lookup.document->end() || !queryObject->is_object()) return result;
        const auto pages = queryObject->find("pages");
        if (pages == queryObject->end() || !pages->is_array()) return result;

        int bestScore = -1;
        std::string bestUrl;
        for (const auto& page : *pages) {
            const std::string pageTitle = jsonString(page, "title");
            int score = structuredMatchScore(group.artist, group.searchTitle, pageTitle, pageTitle);
            if (score < 0) continue;
            const std::string canonicalTitle = canonicalText(pageTitle);
            if (!group.titleIsTrack && canonicalTitle.find(" album") != std::string::npos) score += 12;
            if (canonicalTitle.find(" cover") != std::string::npos) score += 8;
            if (canonicalTitle.find(" logo") != std::string::npos) score -= 20;
            const auto imageInfo = page.find("imageinfo");
            if (score <= bestScore || imageInfo == page.end() || !imageInfo->is_array() || imageInfo->empty()) continue;
            const auto& info = imageInfo->front();
            const std::string mime = jsonString(info, "mime");
            if (mime != "image/jpeg" && mime != "image/png") continue;
            std::string imageUrl = jsonString(info, "thumburl");
            if (imageUrl.empty()) imageUrl = jsonString(info, "url");
            if (!imageUrl.empty()) {
                bestScore = score;
                bestUrl = std::move(imageUrl);
            }
        }
        if (bestScore >= 0) appendUnique(result.imageUrls, std::move(bestUrl));
        return result;
    };

    const auto archiveLookup = [&](const AlbumGroup& group) -> ProviderLookup {
        const std::string query = "title:" + luceneValue(group.searchTitle) +
                                  " AND creator:" + luceneValue(group.artist) + " AND mediatype:audio";
        const std::string url = "https://archive.org/advancedsearch.php?q=" + urlEncode(query) +
                                "&fl%5B%5D=identifier&fl%5B%5D=title&fl%5B%5D=creator&rows=10&page=1&output=json";
        const auto lookup = requestJson(url, internetArchive);
        ProviderLookup result;
        result.retryPending = lookup.retryPending;
        if (!lookup.document) return result;
        const auto response = lookup.document->find("response");
        if (response == lookup.document->end() || !response->is_object()) return result;
        const auto docs = response->find("docs");
        if (docs == response->end() || !docs->is_array()) return result;

        int bestScore = -1;
        std::string bestIdentifier;
        for (const auto& candidate : *docs) {
            std::string artist = jsonString(candidate, "creator");
            const auto creator = candidate.find("creator");
            if (artist.empty() && creator != candidate.end() && creator->is_array()) {
                for (const auto& entry : *creator) {
                    if (entry.is_string()) {
                        if (!artist.empty()) artist += " ";
                        artist += entry.get<std::string>();
                    }
                }
            }
            const std::string title = jsonString(candidate, "title");
            const std::string identifier = jsonString(candidate, "identifier");
            const int score = structuredMatchScore(group.artist, group.searchTitle, artist, title);
            if (score > bestScore && !identifier.empty()) {
                bestScore = score;
                bestIdentifier = identifier;
            }
        }
        if (bestScore >= 0) appendUnique(result.imageUrls,
            "https://archive.org/services/img/" + urlEncode(bestIdentifier));
        return result;
    };

    const auto downloadImage = [&](const ProviderLookup& lookup,
                                   const std::filesystem::path& imagePath,
                                   bool& transientFailure) {
        transientFailure = transientFailure || lookup.retryPending;
        for (const auto& url : lookup.imageUrls) {
            HttpResponse response;
            for (int attempt = 0; attempt < 3 && !cancelled(cancel); ++attempt) {
                response = httpGet(session.get(), url, L"image/jpeg,image/png", maxImageBytes, cancel);
                if (response.received && response.status == 200) break;
                if (!retryable(response)) break;
                if (attempt < 2) cancellableDelay(std::chrono::milliseconds(1200 * (1 << attempt)), cancel);
            }
            if (response.received && response.status == 200 && validImage(response.body)) {
                if (atomicBinaryWrite(imagePath, response.body)) return true;
                transientFailure = true;
            } else if (retryable(response)) {
                transientFailure = true;
            }
        }
        return false;
    };

    for (const auto& group : groups) {
        if (cancelled(cancel)) break;
        state.current = group.artist.empty() ? group.searchTitle
                                             : group.artist + " — " + group.searchTitle;
        if (onProgress) onProgress(state);
        const auto imagePath = cacheRoot / fromUtf8(group.cacheKey + ".jpg");
        bool foundArtwork = std::filesystem::is_regular_file(imagePath, filesystemError) && !filesystemError;
        filesystemError.clear();

        MetadataCandidate metadata;
        if (foundArtwork && metadataComplete(group, metadata)) {
            ++state.found;
            if (onMatch) onMatch({group.trackIds, imagePath});
            ++state.processed;
            if (onProgress) onProgress(state);
            continue;
        }

        const std::string attemptKey = std::string(attemptsNamespace) + group.cacheKey;
        const auto lastAttempt = attempts.value(attemptKey, std::int64_t{});
        if (lastAttempt > 0 && nowUnixMs() - lastAttempt < negativeCacheMs) {
            if (foundArtwork) ++state.found;
            ++state.unavailable;
            ++state.processed;
            if (onProgress) onProgress(state);
            continue;
        }

        bool transientFailure = false;
        const auto tryProvider = [&](const auto& lookup) {
            const bool artworkComplete = !group.needsArtwork || foundArtwork;
            if ((artworkComplete && metadataComplete(group, metadata)) || cancelled(cancel)) return;
            auto result = lookup();
            mergeMetadata(metadata, result.metadata);
            transientFailure = transientFailure || result.retryPending;
            if (group.needsArtwork && !foundArtwork) {
                foundArtwork = downloadImage(result, imagePath, transientFailure);
            }
        };

        tryProvider([&] { return musicBrainzLookup(group); });
        tryProvider([&] { return appleLookup(group); });
        tryProvider([&] { return deezerLookup(group); });
        tryProvider([&] { return audioDbLookup(group); });
        tryProvider([&] { return wikimediaLookup(group); });
        tryProvider([&] { return archiveLookup(group); });
        if (cancelled(cancel)) break;

        const bool hasMetadata = !metadata.artist.empty() || !metadata.album.empty() ||
                                 !metadata.genre.empty() || metadata.albumYear > 0;
        if (foundArtwork) ++state.found;
        if (onMatch && (foundArtwork || hasMetadata)) {
            onMatch({group.trackIds, foundArtwork ? imagePath : std::filesystem::path{},
                     metadata.artist, metadata.album, metadata.genre, metadata.albumYear});
        }

        const bool artworkComplete = !group.needsArtwork || foundArtwork;
        if (artworkComplete && metadataComplete(group, metadata)) {
            attempts.erase(attemptKey);
        } else if (transientFailure) {
            ++state.retryPending;
        } else {
            attempts[attemptKey] = nowUnixMs();
            ++state.unavailable;
        }
        saveAttempts(attemptsPath, attempts);
        ++state.processed;
        if (onProgress) onProgress(state);
    }
    state.current.clear();
    if (onProgress) onProgress(state);
}

}  // namespace neon
