#include "neon/RadioDirectory.hpp"

#include "neon/HttpRequest.hpp"
#include "neon/Utils.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace neon {
namespace {

using Json = nlohmann::json;
constexpr std::size_t maxJsonBytes = 8 * 1024 * 1024;
constexpr std::size_t maxStations = 10000;
constexpr std::size_t maxUrlBytes = 4096;
constexpr std::size_t maxTextBytes = 2048;
constexpr std::size_t maxServers = 16;

bool cancelled(const std::atomic_bool* cancel) {
    return cancel && cancel->load(std::memory_order_acquire);
}

struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
};

Json boundedJson(std::string_view text, const std::atomic_bool* cancel = nullptr) {
    if (text.empty() || text.size() > maxJsonBytes)
        throw std::runtime_error("Radio directory JSON is empty or exceeds the size limit");
    std::size_t events{};
    return Json::parse(text.begin(), text.end(), [&](int depth, Json::parse_event_t, Json&) {
        if (cancelled(cancel)) throw std::runtime_error("Radio directory request cancelled");
        if (depth > 16 || ++events > 1000000)
            throw std::runtime_error("Radio directory JSON exceeds the complexity limit");
        return true;
    });
}

std::string_view stringField(const Json& object, std::string_view key, std::size_t maximum) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return {};
    const auto& value = found->get_ref<const std::string&>();
    return value.size() <= maximum ? std::string_view(value) : std::string_view{};
}

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string asciiLower(std::string_view value) {
    std::string output(value);
    for (auto& c : output) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return output;
}

std::string displayText(std::string_view value) {
    std::string output;
    bool space{};
    for (const unsigned char c : trim(value)) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = true; continue; }
        if (c < 32 || c == 127) return {};
        if (space && !output.empty()) output += ' ';
        output += static_cast<char>(c);
        space = false;
    }
    return output;
}

bool hexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string stationUuid(std::string_view value) {
    if (value.size() != 36) return {};
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return {};
        } else if (!hexDigit(value[i])) return {};
    }
    return asciiLower(value);
}

// Validate the authority as well as the scheme: WinHttpCrackUrl alone accepts
// some unusable URLs. Preserve path/query case and query parameters for streams.
std::string streamUrl(std::string_view value) {
    value = trim(value);
    if (value.empty() || value.size() > maxUrlBytes) return {};
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c <= 32 || c == 127 || c == '\\') return {};
        if (c == '%' && (i + 2 >= value.size() || !hexDigit(value[i + 1]) || !hexDigit(value[i + 2])))
            return {};
    }
    const auto schemeEnd = value.find("://");
    if (schemeEnd == std::string_view::npos) return {};
    const auto scheme = asciiLower(value.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") return {};
    const auto authorityEnd = value.find_first_of("/?#", schemeEnd + 3);
    auto authority = value.substr(schemeEnd + 3, authorityEnd == std::string_view::npos
        ? std::string_view::npos : authorityEnd - schemeEnd - 3);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return {};
    std::string_view host, port;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string_view::npos || end <= 1) return {};
        host = authority.substr(0, end + 1);
        const auto address = host.substr(1, host.size() - 2);
        if (address.find(':') == std::string_view::npos ||
            !std::ranges::all_of(address, [](char c) { return hexDigit(c) || c == ':' || c == '.'; }))
            return {};
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') return {};
            port = authority.substr(end + 2);
            if (port.empty()) return {};
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (host.empty() || !std::ranges::all_of(host, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        })) return {};
        if (host.front() == '.' || host.front() == '-' || host.back() == '-' ||
            host.find("..") != std::string_view::npos) return {};
        if (colon != std::string_view::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) return {};
        }
    }
    unsigned int portNumber = scheme == "https" ? 443 : 80;
    if (!port.empty()) {
        const auto result = std::from_chars(port.data(), port.data() + port.size(), portNumber);
        if (result.ec != std::errc{} || result.ptr != port.data() + port.size() ||
            portNumber == 0 || portNumber > 65535) return {};
    }
    std::string url = scheme + "://" + asciiLower(host);
    if ((scheme == "https" && portNumber != 443) || (scheme == "http" && portNumber != 80))
        url += ':' + std::to_string(portNumber);
    auto resource = authorityEnd == std::string_view::npos ? std::string_view{} : value.substr(authorityEnd);
    resource = resource.substr(0, resource.find('#'));
    if (resource.empty() || resource.front() == '?') url += '/';
    url += resource;
    const auto wide = fromUtf8(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) ||
        !parts.dwHostNameLength) return {};
    return url;
}

std::string nameKey(std::string_view name) {
    auto wide = fromUtf8(name);
    for (auto& c : wide) if (c == L'ı' || c == L'İ') c = L'I';
    return normalizeForSearch(toUtf8(wide));
}

std::string stationTags(std::string_view value) {
    std::string output;
    std::unordered_set<std::string> seen;
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto tag = displayText(value.substr(0, comma));
        if (!tag.empty() && seen.insert(normalizeForSearch(tag)).second) {
            if (!output.empty()) output += ", ";
            output += tag;
        }
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return output.empty() ? "Live Radio" : output;
}

RadioDirectoryResult parseStationsImpl(std::string_view text, const std::atomic_bool* cancel) {
    try {
        const auto stations = boundedJson(text, cancel);
        if (!stations.is_array()) return {{}, "Radio directory response must be a station array"};
        if (stations.size() > maxStations) return {{}, "Radio directory contains too many stations"};
        struct Candidate { Track track; std::string name; };
        std::vector<Candidate> candidates;
        candidates.reserve(stations.size());
        for (const auto& station : stations) {
            if (cancelled(cancel)) return {{}, "Radio directory request cancelled"};
            if (!station.is_object() || stringField(station, "countrycode", 2) != "TR") continue;
            const auto ok = station.find("lastcheckok");
            if (ok == station.end() || !ok->is_number_integer() || *ok != 1) continue;
            auto uuid = stationUuid(stringField(station, "stationuuid", 36));
            auto name = displayText(stringField(station, "name", maxTextBytes));
            if (uuid.empty() || name.empty()) continue;
            auto url = streamUrl(stringField(station, "url_resolved", maxUrlBytes));
            if (url.empty()) url = streamUrl(stringField(station, "url", maxUrlBytes));
            if (url.empty()) continue;
            Track track;
            track.id = "radio:" + uuid;
            track.mediaKind = MediaKind::Radio;
            track.title = name;
            track.artist = name; // Existing artist-first UI and A-Z index show the station.
            track.album = "Turkey live radio";
            track.genre = stationTags(stringField(station, "tags", maxTextBytes));
            track.streamUrl = std::move(url);
            candidates.push_back({std::move(track), nameKey(name)});
        }
        // A stable UUID wins aliases, irrespective of server ordering, popularity
        // or bitrate. Do not strip programme names/genres or stream query strings.
        std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right) {
            if (left.track.id != right.track.id) return left.track.id < right.track.id;
            if (left.track.streamUrl != right.track.streamUrl) return left.track.streamUrl < right.track.streamUrl;
            if (left.track.title != right.track.title) return left.track.title < right.track.title;
            return left.track.genre < right.track.genre;
        });
        std::unordered_set<std::string> ids, streams, names;
        RadioDirectoryResult result;
        result.tracks.reserve(candidates.size());
        for (auto& candidate : candidates) {
            if (cancelled(cancel)) return {{}, "Radio directory request cancelled"};
            const auto& track = candidate.track;
            if (ids.contains(track.id) || streams.contains(track.streamUrl) || names.contains(candidate.name)) continue;
            ids.insert(track.id);
            streams.insert(track.streamUrl);
            names.insert(candidate.name);
            result.tracks.push_back(std::move(candidate.track));
        }
        return result;
    } catch (const std::exception&) {
        return {{}, cancelled(cancel) ? "Radio directory request cancelled" : "Invalid or oversized radio directory JSON"};
    }
}

std::string_view responseText(const detail::HttpResponse& response) {
    if (response.body.empty()) return {};
    return {reinterpret_cast<const char*>(response.body.data()), response.body.size()};
}

std::vector<std::string> serverNames(std::string_view text) {
    std::vector<std::string> names;
    try {
        const auto servers = boundedJson(text);
        if (!servers.is_array() || servers.size() > 256) return {};
        for (const auto& server : servers) {
            if (!server.is_object()) continue;
            auto name = asciiLower(stringField(server, "name", 253));
            constexpr std::string_view suffix = ".api.radio-browser.info";
            if (!name.ends_with(suffix) || name.size() <= suffix.size()) continue;
            const auto label = std::string_view(name).substr(0, name.size() - suffix.size());
            if (label.front() == '-' || label.back() == '-' || !std::ranges::all_of(label, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            })) continue;
            if (std::ranges::find(names, name) == names.end()) names.push_back(std::move(name));
            if (names.size() == maxServers) break;
        }
    } catch (const std::exception&) { return {}; }
    return names;
}

}  // namespace

RadioDirectoryResult RadioDirectory::parseStations(std::string_view json) {
    return parseStationsImpl(json, nullptr);
}

bool RadioDirectory::validStreamUrl(std::string_view url) {
    return trim(url) == url && !streamUrl(url).empty();
}

RadioDirectoryResult RadioDirectory::fetchTurkey(const std::atomic_bool* cancel) const {
    if (cancelled(cancel)) return {{}, "Radio directory request cancelled"};
    try {
        InternetHandle session(WinHttpOpen(L"NeonJukebox/1.0.2 (Turkey radio directory)",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        if (!session.value) return {{}, "Unable to open the radio directory HTTP session"};
        if (!WinHttpSetTimeouts(session.value, 4000, 4000, 7000, 10000))
            return {{}, "Unable to set radio directory HTTP timeouts"};

        // https://api.radio-browser.info/ documents randomizing mirrors/retrying.
        // Use the documented HTTP DNS-discovery bridge so discovery can use the
        // same cancellable I/O path as station requests, without blocking DNS APIs.
        // https://docs.radio-browser.info/#server-mirrors
        std::array<std::string, 3> fallback{
            "de1.api.radio-browser.info", "nl1.api.radio-browser.info", "de2.api.radio-browser.info"};
        std::mt19937 random(std::random_device{}());
        std::shuffle(fallback.begin(), fallback.end(), random);
        std::vector<std::string> servers;
        for (const auto& host : fallback) {
            if (cancelled(cancel)) return {{}, "Radio directory request cancelled"};
            const auto response = detail::httpGet(session.value, "https://" + host + "/json/servers",
                                                  L"application/json", 64 * 1024, cancel);
            if (response.received && response.status == 200) servers = serverNames(responseText(response));
            if (!servers.empty()) break;
        }
        std::shuffle(servers.begin(), servers.end(), random);
        for (const auto& host : fallback)
            if (std::ranges::find(servers, host) == servers.end()) servers.push_back(host);

        std::string error = "Unable to reach the Radio Browser directory";
        for (const auto& host : servers) {
            if (cancelled(cancel)) return {{}, "Radio directory request cancelled"};
            const auto response = detail::httpGet(session.value, "https://" + host +
                "/json/stations/bycountrycodeexact/TR?hidebroken=true&order=name&limit=10000",
                L"application/json", maxJsonBytes, cancel);
            if (!response.received || response.status != 200) continue;
            auto result = parseStationsImpl(responseText(response), cancel);
            if (cancelled(cancel)) return {{}, "Radio directory request cancelled"};
            if (result.error.empty() && !result.tracks.empty()) return result;
            error = result.error.empty() ? "Radio Browser returned no playable Turkey stations" : result.error;
        }
        return {{}, cancelled(cancel) ? "Radio directory request cancelled" : std::move(error)};
    } catch (const std::exception&) {
        return {{}, cancelled(cancel) ? "Radio directory request cancelled" : "Unable to fetch the Radio Browser directory"};
    }
}

}  // namespace neon
