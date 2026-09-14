#include "neon/RadioDirectory.hpp"
#include "neon/Library.hpp"
#include "neon/Storage.hpp"
#include "neon/Utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;
using namespace neon;
int failures{};
#define CHECK(value) do { if (!(value)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " << #value << '\n'; ++failures; } } while (false)

Json station(unsigned int number = 1, std::string name = "İstanbul Radyo",
             std::string url = "https://stream.example/live") {
    auto suffix = std::to_string(number);
    suffix.insert(0, 12 - suffix.size(), '0');
    return {{"stationuuid", "01234567-89ab-cdef-0123-" + suffix}, {"name", name},
            {"countrycode", "TR"}, {"lastcheckok", 1}, {"url_resolved", url},
            {"url", "https://fallback.example/live"}, {"tags", "pop, Turkish, pop"}};
}

RadioDirectoryResult parse(const Json& stations) {
    return RadioDirectory::parseStations(stations.dump());
}

void testStationMetadata() {
    auto source = station();
    source["stationuuid"] = "01234567-89AB-CDEF-0123-000000000001";
    source["changeuuid"] = "different-change-id";
    source["name"] = " \tİstanbul  Radyo\r\n ";
    const auto result = parse(Json::array({source}));
    CHECK(result.error.empty());
    CHECK(result.tracks.size() == 1);
    if (result.tracks.empty()) return;
    const auto& track = result.tracks.front();
    CHECK(track.id == "radio:01234567-89ab-cdef-0123-000000000001");
    CHECK(track.mediaKind == MediaKind::Radio);
    CHECK(track.title == "İstanbul Radyo");
    CHECK(track.artist == track.title);
    CHECK(track.album == "Turkey live radio");
    CHECK(track.genre == "pop, Turkish");
    CHECK(track.streamUrl == "https://stream.example/live");
    CHECK(track.path.empty());
    CHECK(track.durationMs == 0);
    CHECK(!track.hasEmbeddedArtwork);

    source["url_resolved"] = "";
    const auto fallback = parse(Json::array({source}));
    CHECK(fallback.tracks.size() == 1);
    if (!fallback.tracks.empty()) {
        CHECK(fallback.tracks.front().streamUrl == "https://fallback.example/live");
        CHECK(fallback.tracks.front().id == track.id);
    }
    source["url_resolved"] = "https://hls.example/live/playlist.m3u8?token=AbC";
    source["hls"] = 1;
    source["codec"] = "AAC";
    source["tags"] = nullptr;
    const auto hls = parse(Json::array({source}));
    CHECK(hls.tracks.size() == 1);
    if (!hls.tracks.empty()) {
        CHECK(hls.tracks.front().streamUrl == source["url_resolved"].get<std::string>());
        CHECK(hls.tracks.front().genre == "Live Radio");
    }
}

void testMalformedEntries() {
    Json entries = Json::array({nullptr, 42, "station", Json::array(), Json::object()});
    for (const auto& field : {"countrycode", "lastcheckok", "stationuuid", "name"}) {
        for (const auto& wrong : {Json(nullptr), Json::array(), Json::object(), Json(false)}) {
            auto bad = station();
            bad[field] = wrong;
            entries.push_back(bad);
        }
        auto missing = station();
        missing.erase(field);
        entries.push_back(missing);
    }
    for (const auto& country : {"DE", "", "tr", "Turkey", "TR "}) {
        auto bad = station();
        bad["countrycode"] = country;
        entries.push_back(bad);
    }
    for (const auto& status : {Json(0), Json(-1), Json(2), Json(1.0), Json("1"), Json(true)}) {
        auto bad = station();
        bad["lastcheckok"] = status;
        entries.push_back(bad);
    }
    for (const auto& uuid : {"", "123", "0123456789ab-cdef-0123-000000000001",
                             "01234567-89ab-cdef-0123-00000000000z"}) {
        auto bad = station();
        bad["stationuuid"] = uuid;
        entries.push_back(bad);
    }
    for (const auto& name : {std::string{}, std::string(" \r\n\t"), std::string(2049, 'x'),
                              std::string("bad\0name", 8)}) {
        auto bad = station();
        bad["name"] = name;
        entries.push_back(bad);
    }
    const auto invalid = parse(entries);
    CHECK(invalid.error.empty());
    CHECK(invalid.tracks.empty());
    entries.push_back(station(9, "Valid station"));
    const auto mixed = parse(entries);
    CHECK(mixed.error.empty());
    CHECK(mixed.tracks.size() == 1);

    for (const auto& text : {"", "{", "null", "{}", "123", "[{}", "[{},]"}) {
        const auto result = RadioDirectory::parseStations(text);
        CHECK(!result.error.empty());
        CHECK(result.tracks.empty());
    }
    CHECK(RadioDirectory::parseStations("[]").error.empty());
    const auto deep = RadioDirectory::parseStations(std::string(64, '[') + "0" + std::string(64, ']'));
    CHECK(!deep.error.empty());
    CHECK(deep.tracks.empty());
    const auto oversized = RadioDirectory::parseStations(std::string(8 * 1024 * 1024 + 1, ' '));
    CHECK(!oversized.error.empty());
    CHECK(!parse(Json(std::vector<Json>(10001, nullptr))).error.empty());
    std::string badUtf8 = "[{\"name\":\"";
    badUtf8 += static_cast<char>(0xff);
    badUtf8 += "\"}]";
    CHECK(!RadioDirectory::parseStations(badUtf8).error.empty());
}

void testUrls() {
    const std::vector<std::string> rejected{
        "file:///C:/station.mp3", "javascript:alert(1)", "ftp://stream.example/live",
        "mms://stream.example/live", "//stream.example/live", "C:\\station.mp3", "https://",
        "http:///live", "https://user:password@stream.example/live", "https://stream.example:0/live",
        "https://stream.example:65536/live", "https://stream.example:bad/live", "https://stream.example:/live",
        "https://stream.example\\other/live", "https://stream.example/live stream",
        "https://stream.example/live\r\nHeader: injected", "https://stream.example/%ZZ",
        "https://stream.example/%", "https://..example/live", "https://[::1/live",
        "https://[bad]/live", "https://[::1]oops/live", "https://[::1]:/live",
        std::string("https://stream.example/\0live", 27), "https://stream.example/" + std::string(4096, 'a')};
    for (const auto& url : rejected) {
        auto source = station();
        source["url_resolved"] = url;
        source["url"] = url;
        const auto result = parse(Json::array({source}));
        CHECK(result.error.empty());
        CHECK(result.tracks.empty());
        // An unusable resolved URL must not hide a usable original URL.
        source["url"] = "http://fallback.example/live";
        CHECK(parse(Json::array({source})).tracks.size() == 1);
    }
    for (const auto& url : {"http://stream.example:8000/Live?Token=Aa", "https://stream.example/live.aac",
                            "http://[::1]:8000/live", "https://stream.example/live%20radio"}) {
        const auto result = parse(Json::array({station(1, "Station", url)}));
        CHECK(result.tracks.size() == 1);
        if (!result.tracks.empty()) CHECK(result.tracks.front().streamUrl == url);
    }
    auto source = station();
    source["url_resolved"] = Json::object();
    source["url"] = nullptr;
    CHECK(parse(Json::array({source})).tracks.empty());
}

void testDeduplication() {
    Json entries = Json::array({
        station(8, "Different name", "HTTPS://STREAM.example:443/live#player"),
        station(4, "İSTANBUL   RADYO", "https://other.example/live"),
        station(3, "ıstanbul radyo", "https://third.example/live"),
        station(1),
        station(1, "Alias", "https://alias.example/live"),
        station(5, "Separate programme", "https://stream.example/live?programme=2"),
        station(6, "Case-sensitive path", "https://stream.example/Live"),
        station(7, "İstanbul Radyo Rock", "https://rock.example/live")});
    // Same UUID is represented twice; choose its first canonical URL deterministically.
    entries[4]["url_resolved"] = "https://zz-alias.example/live";
    const auto result = parse(entries);
    CHECK(result.error.empty());
    CHECK(result.tracks.size() == 4);
    CHECK(!result.tracks.empty() && result.tracks.front().id == "radio:01234567-89ab-cdef-0123-000000000001");
    std::reverse(entries.begin(), entries.end());
    CHECK(Json(parse(entries).tracks) == Json(result.tracks));

    const auto origins = parse(Json::array({
        station(2, "Mirror", "HTTP://stream.example:80#fragment"),
        station(1, "Origin", "http://stream.example/")}));
    CHECK(origins.tracks.size() == 1);
}

void testSerialization() {
    static_assert(static_cast<int>(MediaKind::Music) == 0);
    static_assert(static_cast<int>(MediaKind::Video) == 1);
    static_assert(static_cast<int>(MediaKind::Radio) == 2);
    static_assert(static_cast<int>(LibraryFilter::Favorites) == 3);
    static_assert(static_cast<int>(LibraryFilter::Radio) == 4);
    const auto parsed = parse(Json::array({station()}));
    if (parsed.tracks.empty()) { CHECK(false); return; }
    auto track = parsed.tracks.front();
    track.favorite = true;
    const Json encoded = track;
    CHECK(encoded["mediaKind"] == "radio");
    CHECK(encoded["streamUrl"] == track.streamUrl);
    CHECK(Json(encoded.get<Track>()) == encoded);
    auto legacy = encoded;
    legacy.erase("streamUrl");
    legacy.erase("mediaKind");
    const auto restoredLegacy = legacy.get<Track>();
    CHECK(restoredLegacy.mediaKind == MediaKind::Music);
    CHECK(restoredLegacy.streamUrl.empty());
    from_json(legacy, track);
    CHECK(track.streamUrl.empty());

    for (auto kind : {MediaKind::Music, MediaKind::Video, MediaKind::Radio}) {
        Settings settings;
        settings.ambientMediaKind = kind;
        const Json value = settings;
        CHECK(value.get<Settings>().ambientMediaKind == kind);
        if (kind == MediaKind::Radio) CHECK(value["ambientMediaKind"] == "radio");
    }
    CHECK(!Json(Settings{}).get<Settings>().ambientMediaKind);
    CHECK((!Json{{"ambientMediaKind", "future-kind"}}.get<Settings>().ambientMediaKind));

    LibraryIndex library;
    library.tracks = parsed.tracks;
    library.tracks.front().favorite = true;
    const auto roundTrip = Json(library).get<LibraryIndex>();
    CHECK(Json(roundTrip) == Json(library));
    // Exercise the App's actual persistence path, including its background writer.
    const auto folder = std::filesystem::temp_directory_path() / pathFromUtf8("neon-radio-" + randomId());
    {
        Storage storage(folder);
        storage.saveLibraryAsync(library);
        CHECK(storage.flush());
        CHECK(Json(storage.loadLibrary()) == Json(library));
    }
    std::error_code error;
    std::filesystem::remove(folder / "library.json", error);
    std::filesystem::remove(folder, error);
}

void testFiltersAndInitials() {
    auto parsed = parse(Json::array({
        station(1, "İstanbul Radyo", "https://one.example/live"),
        station(2, "ışık FM", "https://two.example/live"),
        station(3, "Çınar FM", "https://three.example/live"),
        station(4, "Şehir FM", "https://four.example/live"),
        station(5, "90 Radyo", "https://five.example/live"),
        station(6, "Özgür FM", "https://six.example/live"),
        station(7, "Üsküdar FM", "https://seven.example/live"),
        station(8, "Ğüneş FM", "https://eight.example/live")}));
    CHECK(parsed.tracks.size() == 8);
    LibraryIndex library;
    library.tracks = parsed.tracks;
    // Radio ordering/filtering must be by title even for cached nonstation artists.
    for (auto& track : library.tracks) track.artist = "ZZZ Broadcaster";
    library.tracks.front().favorite = true;
    Track music;
    music.id = "music";
    music.artist = "İstanbul Artist";
    music.title = "Local music";
    library.tracks.push_back(music);
    auto video = music;
    video.id = "video";
    video.mediaKind = MediaKind::Video;
    library.tracks.push_back(video);
    CHECK(LibraryScanner::filter(library, {}, LibraryFilter::All).size() == 10);
    CHECK(LibraryScanner::filter(library, {}, LibraryFilter::Radio).size() == 8);
    CHECK(LibraryScanner::filter(library, {}, LibraryFilter::Music) == std::vector<std::size_t>{8});
    CHECK(LibraryScanner::filter(library, {}, LibraryFilter::Video) == std::vector<std::size_t>{9});
    CHECK(LibraryScanner::filter(library, {}, LibraryFilter::Favorites) == std::vector<std::size_t>{0});
    CHECK(LibraryScanner::filter(library, "İSTANBUL", LibraryFilter::Radio) == std::vector<std::size_t>{0});
    CHECK(LibraryScanner::filter(library, "radyo", LibraryFilter::Radio).size() == 2);
    CHECK(LibraryScanner::filter(library, {}, LibraryFilter::Radio, {}, 'I').size() == 2);
    for (char initial : {'C', 'S', 'O', 'U', 'G', '#'})
        CHECK(LibraryScanner::filter(library, {}, LibraryFilter::Radio, {}, initial).size() == 1);
    const auto order = LibraryScanner::filter(library, {}, LibraryFilter::Radio);
    CHECK(!order.empty() && library.tracks[order.front()].title == "90 Radyo");
    CHECK(LibraryScanner::filter(library, "Turkey", LibraryFilter::Radio).size() == 8);
    CHECK(LibraryScanner::filter(library, "Turkish", LibraryFilter::Radio).size() == 8);
}

void testScanIsolationAndCancellation() {
    const auto folder = std::filesystem::temp_directory_path() / pathFromUtf8("neon-radio-scan-" + randomId());
    std::filesystem::create_directory(folder);
    const auto file = folder / "local.mp4";
    { std::ofstream stream(file, std::ios::binary); stream << "video fixture"; }
    const auto parsed = parse(Json::array({station()}));
    if (parsed.tracks.empty()) { CHECK(false); return; }
    LibraryIndex cached;
    auto radio = parsed.tracks.front();
    // Even a bad old cache with a local path must not supply a radio UUID/URL
    // as the metadata of a scanned file.
    radio.path = file;
    radio.fileSize = std::filesystem::file_size(file);
    radio.modifiedTicks = std::filesystem::last_write_time(file).time_since_epoch().count();
    cached.tracks.push_back(radio);
    const std::array roots{folder};
    const auto scanned = LibraryScanner{}.scan({}, roots, cached, {});
    CHECK(scanned.tracks.size() == 1);
    if (!scanned.tracks.empty()) {
        CHECK(scanned.tracks.front().mediaKind == MediaKind::Video);
        CHECK(scanned.tracks.front().id != radio.id);
        CHECK(scanned.tracks.front().streamUrl.empty());
    }
    CHECK(LibraryScanner{}.scan({}, {}, cached, {}).tracks.empty());
    std::error_code error;
    std::filesystem::remove(file, error);
    std::filesystem::remove(folder, error);

    // Deterministic cancellation: no DNS or public network request is started.
    std::atomic_bool cancel{true};
    const auto start = std::chrono::steady_clock::now();
    const auto result = RadioDirectory{}.fetchTurkey(&cancel);
    CHECK(!result.error.empty());
    CHECK(result.tracks.empty());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--live") {
        const auto result = neon::RadioDirectory{}.fetchTurkey();
        if (!result.error.empty()) { std::cerr << result.error << '\n'; return 1; }
        std::cout << "Live Turkey directory: " << result.tracks.size() << " unique stations\n";
        neon::LibraryIndex library;
        library.tracks = result.tracks;
        const auto sorted = neon::LibraryScanner::filter(library, {}, neon::LibraryFilter::Radio);
        for (std::size_t i = 0; i < std::min<std::size_t>(10, sorted.size()); ++i)
            std::cout << library.tracks[sorted[i]].title << '\n';
        return result.tracks.empty() ? 1 : 0;
    }
    testStationMetadata();
    testMalformedEntries();
    testUrls();
    testDeduplication();
    testSerialization();
    testFiltersAndInitials();
    testScanIsolationAndCancellation();
    if (failures) std::cerr << failures << " radio test(s) failed\n";
    else std::cout << "All radio tests passed\n";
    return failures ? 1 : 0;
}
