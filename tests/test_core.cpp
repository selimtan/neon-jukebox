#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "neon/Library.hpp"
#include "neon/OnlineArtwork.hpp"
#include "neon/Queue.hpp"
#include "neon/Security.hpp"
#include "neon/Storage.hpp"
#include "neon/Utils.hpp"
#include "neon/VideoSourceCache.hpp"

namespace {

int failures{};

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #expression "\n"; \
        ++failures; \
    } \
} while (false)

void writeSilentWav(const std::filesystem::path& path) {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint32_t samples = 800;
    constexpr std::uint32_t dataBytes = samples * channels * bits / 8;
    auto write16 = [](std::ofstream& stream, std::uint16_t value) {
        const std::array<char, 2> bytes{static_cast<char>(value), static_cast<char>(value >> 8)};
        stream.write(bytes.data(), bytes.size());
    };
    auto write32 = [](std::ofstream& stream, std::uint32_t value) {
        const std::array<char, 4> bytes{static_cast<char>(value), static_cast<char>(value >> 8),
                                        static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
        stream.write(bytes.data(), bytes.size());
    };
    std::ofstream stream(path, std::ios::binary);
    stream.write("RIFF", 4); write32(stream, 36 + dataBytes); stream.write("WAVEfmt ", 8);
    write32(stream, 16); write16(stream, 1); write16(stream, channels); write32(stream, sampleRate);
    write32(stream, sampleRate * channels * bits / 8); write16(stream, channels * bits / 8); write16(stream, bits);
    stream.write("data", 4); write32(stream, dataBytes);
    const std::array<char, dataBytes> silence{};
    stream.write(silence.data(), silence.size());
}

void testQueue() {
    neon::CoinCreditBank credits;
    CHECK(credits.available() == 0);
    CHECK(!credits.consume());
    credits.insert();
    credits.insert();
    CHECK(credits.available() == 2);
    CHECK(credits.consume());
    CHECK(credits.available() == 1);
    CHECK(credits.consume());
    CHECK(!credits.consume());

    neon::RequestQueue queue;
    const auto firstId = queue.enqueue("track-a").id;
    queue.enqueue("track-a");
    queue.enqueue("track-b");
    CHECK(queue.size() == 3);
    CHECK(queue.front()->trackId == "track-a");
    CHECK(queue.move(2, 0));
    CHECK(queue.front()->trackId == "track-b");
    CHECK(queue.remove(firstId));
    CHECK(queue.size() == 2);
    CHECK(queue.popFront()->trackId == "track-b");

    // Only the first request may interrupt an automatically selected track.
    CHECK(neon::shouldOfferPlayNow(true, true));
    CHECK(!neon::shouldOfferPlayNow(true, false));
    CHECK(!neon::shouldOfferPlayNow(false, true));
    CHECK(!neon::shouldOfferPlayNow(false, false));

    std::mt19937 random(42);
    neon::AmbientSelector ambient;
    CHECK(ambient.next(neon::AmbientMode::Sequential, false, 3, random) == 0);
    CHECK(ambient.next(neon::AmbientMode::Sequential, false, 3, random) == 1);
    CHECK(ambient.next(neon::AmbientMode::Sequential, false, 3, random) == 2);
    CHECK(!ambient.next(neon::AmbientMode::Sequential, false, 3, random));
    ambient.reset();
    std::unordered_set<std::size_t> shuffled;
    for (int i = 0; i < 3; ++i) shuffled.insert(*ambient.next(neon::AmbientMode::Shuffle, false, 3, random));
    CHECK(shuffled.size() == 3);
    CHECK(!ambient.next(neon::AmbientMode::Shuffle, false, 3, random));
    ambient.reset();
    CHECK(ambient.next(neon::AmbientMode::Sequential, true, 2, random) == 0);
    CHECK(ambient.next(neon::AmbientMode::Sequential, true, 2, random) == 1);
    CHECK(ambient.next(neon::AmbientMode::Sequential, true, 2, random) == 0);
    ambient.reset();
    for (int i = 0; i < 12; ++i) {
        const auto selected = ambient.next(neon::AmbientMode::Shuffle, true, 3, random);
        CHECK(selected.has_value());
        CHECK(*selected < 3);
    }
}

void testPin() {
    CHECK(!neon::PinGuard::validFormat("123"));
    CHECK(!neon::PinGuard::validFormat("1234x"));
    CHECK(neon::PinGuard::validFormat("123456"));
    const auto record = neon::PinGuard::create("123456");
    CHECK(record.configured());
    CHECK(neon::PinGuard::verify("123456", record));
    CHECK(!neon::PinGuard::verify("654321", record));
    neon::PinGuard guard;
    for (int i = 0; i < 5; ++i) CHECK(!guard.attempt("0000", record));
    CHECK(guard.locked());
    CHECK(guard.secondsRemaining() > 0);
}

void testSearchAndExtensions() {
    neon::LibraryIndex library;
    library.tracks = {
        {.id="1", .title="İstanbul Nights", .artist="Ada", .album="City", .genre="Rock",
         .albumYear=2024,
         .favorite=true},
        {.id="2", .title="Neon Sky", .artist="Nova", .album="Signals", .genre="Pop",
         .favorite=false,
         .mediaKind=neon::MediaKind::Video}
    };
    CHECK(neon::LibraryScanner::filter(library, "NEON", neon::LibraryFilter::All).size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "ada", neon::LibraryFilter::All).size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Favorites).size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Music).size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video).size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::All, "rock").size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Music, "ROCK").size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video, "Rock").empty());
    CHECK(neon::LibraryScanner::filter(library, "pop", neon::LibraryFilter::All).size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "2024", neon::LibraryFilter::All).size() == 1);
    const auto genres = neon::LibraryScanner::genres(library);
    CHECK(genres == std::vector<std::string>({"Pop", "Rock"}));
    CHECK(neon::isSupportedAudioFile(L"music.MP3"));
    CHECK(neon::isSupportedAudioFile(L"music.flac"));
    CHECK(neon::isSupportedVideoFile(L"movie.MP4"));
    CHECK(neon::isSupportedVideoFile(L"movie.mkv"));
    CHECK(!neon::isSupportedAudioFile(L"cover.png"));

    neon::LibraryIndex largeLibrary;
    largeLibrary.tracks.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        largeLibrary.tracks.push_back({.id=std::to_string(i), .title="Track " + std::to_string(i),
                                       .artist="Artist", .album="Album"});
    }
    const auto started = std::chrono::steady_clock::now();
    CHECK(neon::LibraryScanner::filter(largeLibrary, "track 9999", neon::LibraryFilter::All).size() == 1);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

void testArtistBrowse() {
    neon::LibraryIndex library;
    library.tracks = {
        {.id="z", .title="A song", .artist="ZZ Top"},
        {.id="ten", .title="Song", .artist="10cc"},
        {.id="two", .title="Song", .artist="2Pac"},
        {.id="d10", .title="Song 10", .artist="Dua Lipa", .genre="Pop"},
        {.id="d2", .title="Song 2", .artist="dua lipa", .genre="Pop", .mediaKind=neon::MediaKind::Video},
        {.id="m", .title="Dua Lipa tribute", .artist="Madonna", .album="D album", .genre="Pop"},
        {.id="mt", .title="A song", .artist="Metallica", .genre="Rock"},
        {.id="a", .title="Z song", .artist="ABBA"}
    };
    const auto ids = [&](std::string_view query = {}, neon::LibraryFilter media = neon::LibraryFilter::All,
                         std::string_view genre = {}, char initial = '\0') {
        std::vector<std::string> result;
        for (const auto index : neon::LibraryScanner::filter(library, query, media, genre, initial))
            result.push_back(library.tracks[index].id);
        return result;
    };
    const std::vector<std::string> expected{"two", "ten", "a", "d2", "d10", "m", "mt", "z"};
    CHECK(ids() == expected);
    CHECK(ids({}, neon::LibraryFilter::All, {}, 'D') == std::vector<std::string>({"d2", "d10"}));
    CHECK(ids({}, neon::LibraryFilter::All, {}, 'M') == std::vector<std::string>({"m", "mt"}));
    CHECK(ids({}, neon::LibraryFilter::All, {}, '#') == std::vector<std::string>({"two", "ten"}));
    CHECK(ids("song", neon::LibraryFilter::Music, "POP", 'D') == std::vector<std::string>({"d10"}));
    CHECK(ids("song", neon::LibraryFilter::Video, "pop", 'D') == std::vector<std::string>({"d2"}));
    CHECK(ids({}, neon::LibraryFilter::All, "Rock", 'D').empty());
    CHECK(ids({}, neon::LibraryFilter::All, {}, 'Q').empty());
    std::ranges::reverse(library.tracks);
    CHECK(ids() == expected); // Old caches and scan order cannot alter browse order.

    for (const auto& [artist, initial] : std::vector<std::pair<std::string, char>>{
             {"Şebnem Ferah", 'S'}, {"Çelik", 'C'}, {"İbrahim Tatlıses", 'I'},
             {"ışık", 'I'}, {"Özlem Tekin", 'O'}, {"Ümit Besen", 'U'},
             {"Ğ test", 'G'}, {"Édith Piaf", 'E'}, {"  Madonna  ", 'M'},
             {"E\xCC\x81" "dith Piaf", 'E'}}) {
        library.tracks = {{.id="unicode", .title="A song", .artist=artist}};
        CHECK(ids({}, neon::LibraryFilter::All, {}, initial).size() == 1);
        CHECK(ids({}, neon::LibraryFilter::All, {}, 'Z').empty());
    }
}

void testStorageAndScan() {
    const auto root = std::filesystem::temp_directory_path() / neon::pathFromUtf8("neon-jukebox-test-" + neon::randomId());
    const auto musicA = root / L"music-a";
    const auto musicB = root / L"music-b";
    const auto videos = root / L"videos";
    std::filesystem::create_directories(musicA);
    std::filesystem::create_directories(musicB);
    std::filesystem::create_directories(videos);
    writeSilentWav(musicA / L"Demo Track.wav");
    writeSilentWav(musicB / L"Second Track.wav");
    { std::ofstream cover(musicA / L"cover.jpg", std::ios::binary); cover << "fixture"; }
    { std::ofstream broken(musicA / L"Broken.mp3", std::ios::binary); broken << "not audio"; }
    { std::ofstream video(videos / L"Demo Video.mp4", std::ios::binary); video << "video fixture"; }

    neon::LibraryScanner scanner;
    const std::vector<std::filesystem::path> roots{musicA, musicB, musicA};
    const std::vector<std::filesystem::path> videoRoots{videos};
    auto library = scanner.scan(roots, videoRoots, {}, {});
    CHECK(library.musicRoots.size() == 2);
    CHECK(library.videoRoots.size() == 1);
    CHECK(library.tracks.size() == 3);
    CHECK(library.tracks.front().title == "Demo Track");
    CHECK(library.tracks.front().genre == "Unknown Genre");
    CHECK(library.tracks.front().sidecarArtwork.has_value());
    CHECK(library.tracks.front().durationMs >= 90);

    neon::OnlineArtworkProgress artworkProgress;
    neon::OnlineArtworkFetcher artworkFetcher;
    auto completeMetadata = library.tracks;
    for (auto& track : completeMetadata) {
        if (track.mediaKind != neon::MediaKind::Music) continue;
        track.artist = "Fixture Artist";
        track.album = "Fixture Album";
        track.genre = "Fixture Genre";
        track.albumYear = 2024;
        track.hasEmbeddedArtwork = true;
    }
    artworkFetcher.run(completeMetadata, root / L"artwork",
                       [&artworkProgress](const auto& progress) { artworkProgress = progress; }, {});
    CHECK(artworkProgress.discovered == 0);

    std::filesystem::remove(musicA / L"cover.jpg");
    const auto refreshedLibrary = scanner.scan(roots, videoRoots, library, {});
    CHECK(refreshedLibrary.tracks.size() == 3);
    CHECK(!refreshedLibrary.tracks.front().sidecarArtwork.has_value());

    // A track from the first root must arrive before even discovering later
    // roots. Cancelling there must leave the rest of the tree untouched.
    for (const auto* cached : std::array<const neon::LibraryIndex*, 2>{nullptr, &library}) {
        std::atomic_bool cancel{};
        neon::ScanProgress progress;
        std::vector<neon::Track> published;
        const std::vector<std::filesystem::path> streamingRoots{musicB, musicA};
        const auto partial = scanner.scan(streamingRoots, videoRoots,
            cached ? *cached : neon::LibraryIndex{}, {},
            [&](const auto& update) { progress = update; }, &cancel, {},
            [&](const auto& track) {
                CHECK(progress.discovered == 1);
                CHECK(neon::pathFromUtf8(progress.currentFile) == track.path);
                CHECK(track.title == "Second Track");
                published.push_back(track);
                cancel.store(true);
            });
        CHECK(published.size() == 1);
        CHECK(partial.tracks.size() == 1);
        CHECK(partial.musicRoots.size() == 1);
        CHECK(partial.videoRoots.empty());
        CHECK(progress.discovered == 1);
        CHECK(progress.processed == 1);
        CHECK(progress.currentFile.empty());
    }

    library.tracks.front().onlineArtwork = root / L"online-cover.jpg";
    library.tracks.front().genre = "Alternative Rock";
    library.tracks.front().albumYear = 2004;

    neon::Storage storage(root / L"state");
    neon::Settings settings;
    settings.musicRoots = {musicA, musicB};
    settings.videoRoots = {videos};
    settings.adminPin = neon::PinGuard::create("2468");
    settings.volume = 0.6F;
    settings.ambientMode = neon::AmbientMode::Shuffle;
    settings.ambientMediaKind = neon::MediaKind::Video;
    settings.currentTrackManual = true;
    settings.visualizerMode = neon::VisualizerMode::WarmTwinVu;
    settings.nowPlayingArtworkMode = neon::NowPlayingArtworkMode::SpinningDisc;
    settings.theme = neon::Theme::Retro;
    CHECK(storage.saveSettings(settings));
    CHECK(storage.saveLibrary(library));
    CHECK(storage.loadSettings().musicRoots == settings.musicRoots);
    CHECK(storage.loadSettings().videoRoots == settings.videoRoots);
    CHECK(storage.loadSettings().ambientMode == neon::AmbientMode::Shuffle);
    CHECK(storage.loadSettings().ambientMediaKind == neon::MediaKind::Video);
    settings.ambientMediaKind = neon::MediaKind::Music;
    CHECK(storage.saveSettings(settings));
    CHECK(storage.loadSettings().ambientMediaKind == neon::MediaKind::Music);
    CHECK(!nlohmann::json::object().get<neon::Settings>().ambientMediaKind);
    for (const auto invalid : {nlohmann::json("all"), nlohmann::json("unknown"),
                               nlohmann::json(42), nlohmann::json(nullptr)}) {
        CHECK(!nlohmann::json({{"ambientMediaKind", invalid}}).get<neon::Settings>().ambientMediaKind);
    }
    CHECK(storage.loadSettings().currentTrackManual);
    CHECK(storage.loadSettings().theme == neon::Theme::Retro);
    CHECK(storage.loadSettings().visualizerMode == neon::VisualizerMode::WarmTwinVu);
    CHECK(storage.loadSettings().retroVisualizerMode == neon::VisualizerMode::RetroPhosphorScope);
    CHECK(neon::visualizersForTheme(neon::Theme::Neon).size() == 28);
    CHECK(neon::visualizersForTheme(neon::Theme::Retro).size() == 1);
    for (auto mode : neon::visualizersForTheme(neon::Theme::Neon)) {
        CHECK(mode != neon::VisualizerMode::RetroPhosphorScope);
        CHECK(neon::adjacentVisualizer(neon::Theme::Neon,
            neon::adjacentVisualizer(neon::Theme::Neon, mode, true), false) == mode);
    }
    CHECK(neon::adjacentVisualizer(neon::Theme::Neon, neon::VisualizerMode::WarmTwinVu, true) ==
          neon::VisualizerMode::AuroraSpectrum);
    for (bool forward : {false, true}) {
        CHECK(neon::adjacentVisualizer(neon::Theme::Retro,
              neon::VisualizerMode::RetroPhosphorScope, forward) == neon::VisualizerMode::RetroPhosphorScope);
    }
    const nlohmann::json legacyMeter{{"theme", "retro"}, {"visualizerMode", "neon-mosaic"}};
    const auto migratedMeter = legacyMeter.get<neon::Settings>();
    CHECK(migratedMeter.visualizerMode == neon::VisualizerMode::NeonMosaic);
    CHECK(migratedMeter.retroVisualizerMode == neon::VisualizerMode::RetroPhosphorScope);
    for (const auto invalid : {nlohmann::json("future-meter"), nlohmann::json(42), nlohmann::json(nullptr)}) {
        const nlohmann::json malformed{{"visualizerMode", invalid}, {"retroVisualizerMode", invalid}};
        const auto restored = malformed.get<neon::Settings>();
        CHECK(restored.visualizerMode == neon::VisualizerMode::AuroraSpectrum);
        CHECK(restored.retroVisualizerMode == neon::VisualizerMode::RetroPhosphorScope);
    }
    CHECK(storage.loadSettings().nowPlayingArtworkMode ==
          neon::NowPlayingArtworkMode::SpinningDisc);
    const auto restoredLibrary = storage.loadLibrary();
    CHECK(restoredLibrary.musicRoots.size() == 2);
    CHECK(restoredLibrary.videoRoots.size() == 1);
    CHECK(restoredLibrary.tracks.size() == 3);
    CHECK(restoredLibrary.tracks.front().onlineArtwork == library.tracks.front().onlineArtwork);
    CHECK(restoredLibrary.tracks.front().genre == "Alternative Rock");
    CHECK(restoredLibrary.tracks.front().albumYear == 2004);

    // Even with a saved title, an eagerly evaluated ANSI fallback used to make
    // one Unicode filename invalidate the entire cached library on Windows.
    auto unicodeLibrary = library;
    unicodeLibrary.tracks.front().path = musicA / L"\u65e5\u672c\U0001F3B5.wav";
    CHECK(storage.saveLibrary(unicodeLibrary));
    const auto unicodeRestored = storage.loadLibrary();
    CHECK(unicodeRestored.tracks.size() == library.tracks.size());
    if (!unicodeRestored.tracks.empty()) {
        CHECK(unicodeRestored.tracks.front().path == unicodeLibrary.tracks.front().path);
        CHECK(unicodeRestored.tracks.front().title == library.tracks.front().title);
    }
    auto untitled = nlohmann::json(unicodeLibrary.tracks.front());
    untitled.erase("title");
    CHECK(untitled.get<neon::Track>().title == neon::pathToUtf8(unicodeLibrary.tracks.front().path.stem()));
    CHECK(storage.saveLibrary(library));

    const nlohmann::json legacySettings{
        {"schemaVersion", 1}, {"libraryRoot", neon::pathToUtf8(musicA)}};
    const auto migratedSettings = legacySettings.get<neon::Settings>();
    CHECK(migratedSettings.schemaVersion == 5);
    CHECK(migratedSettings.theme == neon::Theme::Neon);
    for (const nlohmann::json theme : {nlohmann::json("future-theme"), nlohmann::json(42),
                                     nlohmann::json(nullptr), nlohmann::json::object()}) {
        const nlohmann::json malformed{{"theme", theme}, {"volume", 0.35F}};
        const auto restored = malformed.get<neon::Settings>();
        CHECK(restored.theme == neon::Theme::Neon);
        CHECK(restored.volume == 0.35F);
    }
    for (const auto& definition : neon::themeDefinitions) {
        settings.theme = definition.theme;
        CHECK(storage.saveSettings(settings));
        CHECK(storage.loadSettings().theme == definition.theme);
    }
    CHECK(migratedSettings.musicRoots == std::vector<std::filesystem::path>{musicA});
    CHECK(migratedSettings.videoRoots.empty());
    CHECK(migratedSettings.visualizerMode == neon::VisualizerMode::AuroraSpectrum);
    CHECK(migratedSettings.nowPlayingArtworkMode == neon::NowPlayingArtworkMode::Artwork);
    const nlohmann::json upgradedSettings = settings;
    CHECK(upgradedSettings.contains("musicRoots"));
    CHECK(upgradedSettings.contains("videoRoots"));
    CHECK(upgradedSettings.contains("nowPlayingArtworkMode"));
    CHECK(upgradedSettings.contains("theme"));
    CHECK(!upgradedSettings.contains("libraryRoot"));

    const nlohmann::json legacyLibrary{
        {"schemaVersion", 1}, {"root", neon::pathToUtf8(musicB)},
        {"tracks", nlohmann::json::array()}, {"scannedAtMs", 0}};
    const auto migratedLibrary = legacyLibrary.get<neon::LibraryIndex>();
    CHECK(migratedLibrary.schemaVersion == 5);
    CHECK(migratedLibrary.musicRoots == std::vector<std::filesystem::path>{musicB});
    CHECK(migratedLibrary.videoRoots.empty());

    const auto resolvedRoot = std::filesystem::weakly_canonical(root);
    // Coalesced background writes preserve the latest state; a later explicit
    // save cannot be overwritten by an older pending snapshot.
    for (int revision = 0; revision < 30; ++revision) {
        settings.playbackPositionMs = revision;
        library.scannedAtMs = revision;
        storage.saveSettingsAsync(settings);
        storage.saveLibraryAsync(library);
    }
    settings.playbackPositionMs = 100;
    CHECK(storage.saveSettings(settings));
    CHECK(storage.flush());
    CHECK(storage.loadSettings().playbackPositionMs == 100);
    CHECK(storage.loadLibrary().scannedAtMs == 29);
    {
        const auto portableRoot = root / "beside-exe" / "library";
        neon::Storage portable(root / L"state", portableRoot);
        CHECK(portable.loadLibrary().scannedAtMs == 29); // Legacy metadata migration.
        library.scannedAtMs = 101;
        portable.saveLibraryAsync(library);
        CHECK(portable.flush());
        CHECK(std::filesystem::is_regular_file(portableRoot / "library.json"));
        CHECK(portable.loadLibrary().scannedAtMs == 101);
        CHECK(storage.loadLibrary().scannedAtMs == 29);
    }

    // Reuse enrichment even if local tags are incomplete or a previous app
    // session ended before applying the result to library.json. No network needed.
    auto incomplete = library.tracks.front();
    incomplete.artist = "Unknown Artist";
    incomplete.album = "Unknown Album";
    incomplete.genre = "Unknown Genre";
    incomplete.albumYear = 0;
    incomplete.hasEmbeddedArtwork = false;
    incomplete.sidecarArtwork.reset();
    incomplete.onlineArtwork.reset();
    const auto onlineRoot = root / "cached-enrichment";
    std::filesystem::create_directories(onlineRoot);
    const auto key = neon::onlineArtworkCacheKey(incomplete);
    {
        std::ofstream cached(onlineRoot / "metadata.json");
        cached << nlohmann::json{{key, {{"artist", "Cached Artist"}, {"album", "Cached Album"},
            {"genre", "Rock"}, {"albumYear", 1998}, {"image", ""}}}};
    }
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<neon::OnlineArtworkMatch> matches;
        artworkFetcher.run(std::span<const neon::Track>(&incomplete, 1), onlineRoot,
            [&](const auto& progress) { artworkProgress = progress; },
            [&](auto match) { matches.push_back(std::move(match)); });
        CHECK(artworkProgress.discovered == 0);
        CHECK(matches.size() == 1);
        CHECK(!matches.empty() && matches.front().artist == "Cached Artist" && matches.front().albumYear == 1998);
        incomplete.artist = "Different display metadata";
    }
    ++incomplete.modifiedTicks;
    CHECK(neon::onlineArtworkCacheKey(incomplete) != key);
    const auto tempRoot = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    if (resolvedRoot.native().starts_with(tempRoot.native())) std::filesystem::remove_all(resolvedRoot);
}

void testOnlineArtworkNetworkSmoke() {
    if (!std::getenv("NEON_ONLINE_ARTWORK_SMOKE")) return;
    const auto root = std::filesystem::temp_directory_path() /
                      neon::pathFromUtf8("neon-artwork-smoke-" + neon::randomId());
    neon::Track track{
        .id = "beyonce-four-deluxe",
        .title = "Love On Top",
        .artist = "Beyonce",
        .album = "4 (Deluxe Edition)"
    };
    neon::OnlineArtworkProgress progress;
    std::vector<neon::OnlineArtworkMatch> matches;
    neon::OnlineArtworkFetcher fetcher;
    fetcher.run(std::span<const neon::Track>(&track, 1), root,
                [&progress](const auto& update) { progress = update; },
                [&matches](auto match) { matches.push_back(std::move(match)); });
    CHECK(progress.discovered == 1);
    CHECK(progress.processed == 1);
    CHECK(progress.found == 1);
    CHECK(progress.unavailable == 0);
    CHECK(progress.retryPending == 0);
    CHECK(matches.size() == 1);
    CHECK(!matches.empty() && std::filesystem::is_regular_file(matches.front().imagePath));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

}  // namespace

void testVideoArtistLabels() {
    neon::LibraryIndex library;
    const auto add = [&](std::string id, std::string artist, std::string title, neon::MediaKind kind = neon::MediaKind::Video) {
        neon::Track track;
        track.id = std::move(id);
        track.artist = std::move(artist);
        track.title = std::move(title);
        track.mediaKind = kind;
        track.genre = "Pop";
        library.tracks.push_back(std::move(track));
    };
    add("michael", "Unknown Artist", "Michael Jackson - Thriller (Official Video)");
    add("madonna", "Unknown Artist", "Madonna - Like A Virgin (Official Video) [HD]");
    add("mariah", "", "Mariah Carey – Fantasy");
    add("men", "Men At Work", "Down Under");
    add("tagged", "Madonna", "Madonna - Frozen (Live)");
    add("conflict", "Björk", "Madonna - A cover version");
    add("numeric", "Unknown Artist", "2 Unlimited — No Limit");
    add("turkish", "Unknown Artist", "İşın Karaca - Şarkı");
    add("accent", "Unknown Artist", "Álvaro Soler - Sofia");
    add("unsplit", "Unknown Artist", "Mystery clip");
    add("music", "Unknown Artist", "Madonna - A music title", neon::MediaKind::Music);
    const auto saved = nlohmann::json(library);
    // Exercise an old persisted catalogue without rescanning or editing its tags.
    library = saved.get<neon::LibraryIndex>();
    const auto filtered = neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video, "", 'M');
    CHECK(filtered.size() == 5);
    std::vector<std::string> ids;
    for (const auto index : filtered) ids.push_back(library.tracks[index].id);
    CHECK(ids == std::vector<std::string>({"tagged", "madonna", "mariah", "men", "michael"}));
    CHECK(neon::trackLabel(library.tracks[1]).artist == "Madonna");
    CHECK(neon::trackLabel(library.tracks[1]).title == "Like A Virgin");
    CHECK(neon::trackLabel(library.tracks[4]).title == "Frozen (Live)");
    CHECK(neon::trackLabel(library.tracks[5]).artist == "Björk");
    CHECK(neon::trackLabel(library.tracks[5]).title == "Madonna - A cover version");
    auto underscoredVideo = library.tracks[1];
    underscoredVideo.artist = "Unknown Artist";
    underscoredVideo.title = "Metallica_ ManUNkind";
    CHECK(neon::trackLabel(underscoredVideo).artist == "Metallica");
    CHECK(neon::trackLabel(underscoredVideo).title == "ManUNkind");
    CHECK(neon::LibraryScanner::filter(library, "thriller", neon::LibraryFilter::Video, "Pop", 'M').size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video, "Rock", 'M').empty());
    for (char initial : {'#', 'I', 'A', 'B'})
        CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video, "", initial).size() == 1);
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Music, "", 'M').empty());
    CHECK(neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video).size() == 10);
    CHECK(nlohmann::json(library) == saved);
    neon::Track filename;
    filename.mediaKind = neon::MediaKind::Video;
    filename.path = neon::pathFromUtf8("Melis Sökmen - Ara Sıra.mp4");
    CHECK(neon::trackLabel(filename).artist == "Melis Sökmen");
    CHECK(neon::trackLabel(filename).title == "Ara Sıra");
}

void testVideoPlaybackCache() {
    const auto root = std::filesystem::temp_directory_path() / neon::pathFromUtf8("neon-video-cache-test-" + neon::randomId());
    const auto cache = root / "cache";
    std::filesystem::create_directories(root);
    const auto write = [](const auto& path, std::size_t bytes, char value) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        const std::string block(bytes, value);
        stream.write(block.data(), static_cast<std::streamsize>(block.size()));
        CHECK(stream.good());
    };
    const auto indexed = [](const auto& path) {
        neon::Track track;
        track.path = path;
        track.fileSize = std::filesystem::file_size(path);
        track.modifiedTicks = std::filesystem::last_write_time(path).time_since_epoch().count();
        track.mediaKind = neon::MediaKind::Video;
        return track;
    };
    const auto running = [] { return false; };
    constexpr auto megabyte = 1024 * 1024;
    write(root / "clip.mp4", 3 * megabyte, 'v');
    auto track = indexed(root / "clip.mp4");
    auto prepared = neon::cacheVideoSource(track, cache, [] { return true; });
    CHECK(prepared.path.empty() && prepared.error.empty());
    CHECK(!std::filesystem::exists(cache));
    std::atomic_bool cancel{};
    prepared = neon::cacheVideoSource(track, cache, [&] { return cancel.load(); }, [&](int percent) { cancel.store(percent >= 25); });
    CHECK(cancel && prepared.path.empty() && prepared.error.empty());
    CHECK(std::filesystem::is_empty(cache)); // No partial MP4 may become playable.
    std::vector<int> progress;
    prepared = neon::cacheVideoSource(track, cache, running, [&](int percent) { progress.push_back(percent); });
    CHECK(prepared.error.empty() && !prepared.path.empty());
    CHECK(prepared.path == neon::videoSourceCachePath(track, cache));
    CHECK(std::filesystem::file_size(prepared.path) == track.fileSize);
    CHECK(std::is_sorted(progress.begin(), progress.end()) && progress.back() == 100);
    std::ifstream copied(prepared.path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(copied)), {});
    CHECK(content == std::string(3 * megabyte, 'v'));
    copied.close();
    // Replaying the indexed version must not even require the source drive.
    std::filesystem::rename(track.path, root / "disconnected.mp4");
    const auto again = neon::cacheVideoSource(track, cache, running);
    CHECK(again.path == prepared.path && again.error.empty());
    std::filesystem::rename(root / "disconnected.mp4", track.path);
    // A truncated cache file must be rebuilt, not passed to the decoder.
    write(prepared.path, 12, 'x');
    const auto repaired = neon::cacheVideoSource(track, cache, running);
    CHECK(repaired.error.empty() && std::filesystem::file_size(repaired.path) == track.fileSize);
    const auto oldPath = repaired.path;
    write(track.path, 2 * megabyte, 'n');
    track = indexed(track.path);
    const auto newer = neon::cacheVideoSource(track, cache, running);
    CHECK(newer.error.empty() && newer.path != oldPath);
    CHECK(std::filesystem::file_size(newer.path) == 2 * megabyte);
    // Enforce the budget on owned cache files only, keeping the incoming clip.
    write(cache / "keep-my-file.txt", 20, 'k');
    write(root / "second.mp4", 3 * megabyte, 's');
    const auto second = neon::cacheVideoSource(indexed(root / "second.mp4"), cache, running, {}, 3 * megabyte);
    CHECK(!second.path.empty() && second.error.empty());
    CHECK(!std::filesystem::exists(oldPath) && !std::filesystem::exists(newer.path));
    CHECK(std::filesystem::exists(cache / "keep-my-file.txt"));
    CHECK(std::filesystem::exists(second.path));
    auto missing = track;
    missing.path = root / "missing.mp4";
    const auto failed = neon::cacheVideoSource(missing, cache, running);
    CHECK(failed.path.empty() && !failed.error.empty());
    CHECK(neon::videoSourceNeedsCache(L"\\\\NAS\\videos\\clip.mp4"));
    // This is the unique temporary fixture root created above.
    std::filesystem::remove_all(root);
}

int main() {
    try {
        testQueue();
        testPin();
        testSearchAndExtensions();
        testArtistBrowse();
        testVideoArtistLabels();
        testVideoPlaybackCache();
        testStorageAndScan();
        testOnlineArtworkNetworkSmoke();
    } catch (const std::exception& exception) {
        std::cerr << "Unhandled test exception: " << exception.what() << '\n';
        ++failures;
    }
    if (failures == 0) std::cout << "All Neon Jukebox core tests passed.\n";
    return failures == 0 ? 0 : 1;
}
