#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "neon/Library.hpp"
#include "neon/OnlineArtwork.hpp"
#include "neon/Queue.hpp"
#include "neon/Security.hpp"
#include "neon/Storage.hpp"
#include "neon/Utils.hpp"

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
    settings.currentTrackManual = true;
    settings.visualizerMode = neon::VisualizerMode::WarmTwinVu;
    settings.nowPlayingArtworkMode = neon::NowPlayingArtworkMode::SpinningDisc;
    CHECK(storage.saveSettings(settings));
    CHECK(storage.saveLibrary(library));
    CHECK(storage.loadSettings().musicRoots == settings.musicRoots);
    CHECK(storage.loadSettings().videoRoots == settings.videoRoots);
    CHECK(storage.loadSettings().ambientMode == neon::AmbientMode::Shuffle);
    CHECK(storage.loadSettings().currentTrackManual);
    CHECK(storage.loadSettings().visualizerMode == neon::VisualizerMode::WarmTwinVu);
    CHECK(storage.loadSettings().nowPlayingArtworkMode ==
          neon::NowPlayingArtworkMode::SpinningDisc);
    const auto restoredLibrary = storage.loadLibrary();
    CHECK(restoredLibrary.musicRoots.size() == 2);
    CHECK(restoredLibrary.videoRoots.size() == 1);
    CHECK(restoredLibrary.tracks.size() == 3);
    CHECK(restoredLibrary.tracks.front().onlineArtwork == library.tracks.front().onlineArtwork);
    CHECK(restoredLibrary.tracks.front().genre == "Alternative Rock");
    CHECK(restoredLibrary.tracks.front().albumYear == 2004);

    const nlohmann::json legacySettings{
        {"schemaVersion", 1}, {"libraryRoot", neon::pathToUtf8(musicA)}};
    const auto migratedSettings = legacySettings.get<neon::Settings>();
    CHECK(migratedSettings.schemaVersion == 5);
    CHECK(migratedSettings.musicRoots == std::vector<std::filesystem::path>{musicA});
    CHECK(migratedSettings.videoRoots.empty());
    CHECK(migratedSettings.visualizerMode == neon::VisualizerMode::AuroraSpectrum);
    CHECK(migratedSettings.nowPlayingArtworkMode == neon::NowPlayingArtworkMode::Artwork);
    const nlohmann::json upgradedSettings = settings;
    CHECK(upgradedSettings.contains("musicRoots"));
    CHECK(upgradedSettings.contains("videoRoots"));
    CHECK(upgradedSettings.contains("nowPlayingArtworkMode"));
    CHECK(!upgradedSettings.contains("libraryRoot"));

    const nlohmann::json legacyLibrary{
        {"schemaVersion", 1}, {"root", neon::pathToUtf8(musicB)},
        {"tracks", nlohmann::json::array()}, {"scannedAtMs", 0}};
    const auto migratedLibrary = legacyLibrary.get<neon::LibraryIndex>();
    CHECK(migratedLibrary.schemaVersion == 5);
    CHECK(migratedLibrary.musicRoots == std::vector<std::filesystem::path>{musicB});
    CHECK(migratedLibrary.videoRoots.empty());

    const auto resolvedRoot = std::filesystem::weakly_canonical(root);
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

int main() {
    try {
        testQueue();
        testPin();
        testSearchAndExtensions();
        testStorageAndScan();
        testOnlineArtworkNetworkSmoke();
    } catch (const std::exception& exception) {
        std::cerr << "Unhandled test exception: " << exception.what() << '\n';
        ++failures;
    }
    if (failures == 0) std::cout << "All Neon Jukebox core tests passed.\n";
    return failures == 0 ? 0 : 1;
}
