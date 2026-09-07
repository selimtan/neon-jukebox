#include <algorithm>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <unordered_set>

#include "neon/App.hpp"
#include "neon/Utils.hpp"

namespace {
int failures{};
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; ++failures; } } while (false)

neon::Track track(std::string id, std::string title) {
    neon::Track result;
    result.id = std::move(id);
    result.title = std::move(title);
    result.path = std::filesystem::path(NEON_MIXER_FIXTURES) / "music.mp3";
    result.artist = "Fixture Artist";
    result.album = "Fixture Album";
    result.genre = "Rock";
    result.albumYear = 2024;
    // Keep the completion test offline; artwork lookup has nothing to fetch.
    result.hasEmbeddedArtwork = true;
    return result;
}
}

namespace neon {
struct AppScanTest {
    static void verifyVideoFullscreen(Theme theme, const std::filesystem::path& stateRoot) {
        CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS));
        App app;
        app.sdlInitialized_ = true;
        app.window_ = SDL_CreateWindow("Video fullscreen transition test", 640, 360, SDL_WINDOW_HIDDEN);
        CHECK(app.window_);
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = theme;
        app.settings_.volume = 0;
        app.settings_.ambientMediaKind = MediaKind::Video;
        app.mode_ = UiMode::Browse;
        // Use the offline audio fixture through Media Foundation: its real end
        // event exercises the same video transition without a codec dependency.
        auto first = track("first", "First clip");
        first.mediaKind = MediaKind::Video;
        auto second = first;
        second.id = "second";
        app.library_.tracks = {first, second, track("music", "Music")};
        app.updateFilter();
        std::string error;
        CHECK(app.video_.initialize(app.window_, error));
        CHECK(app.playTrack(first, 0, error));
        app.currentTrackId_ = first.id;
        const auto waitFor = [&](auto ready) {
            const auto deadline = SDL_GetTicks() + 6000;
            while (!ready() && SDL_GetTicks() < deadline) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {}
                app.update();
                SDL_Delay(5);
            }
            CHECK(ready());
        };
        waitFor([&] { return !app.video_.loading() && app.video_.positionMs() > 0; });
        app.dispatch({UiActionKind::ToggleVideoFullscreen});
        CHECK(app.videoFullscreen_);
        CHECK(app.fullscreenVideoInfoVisible(1000));
        CHECK(app.fullscreenVideoInfoVisible(10'999));
        CHECK(!app.fullscreenVideoInfoVisible(11'000));
        app.queue_.enqueue(second.id);
        CHECK(app.video_.seek(std::max<std::int64_t>(0, app.video_.durationMs() - 200)));
        waitFor([&] { return app.currentTrackId_ == second.id && !app.video_.loading(); });
        CHECK(app.videoFullscreen_); // Natural end, including the next source load.
        CHECK(app.fullscreenVideoInfoVisible(20'000));
        CHECK(!app.fullscreenVideoInfoVisible(30'000));
        app.skipCurrent();
        CHECK(app.currentIsVideo() && app.videoFullscreen_); // Shuffle/admin skip.
        CHECK(!app.fullscreenVideoInfoVisible(40'000)); // Loading does not spend the ten seconds.
        CHECK(!app.videoInfoStartedAt_);
        app.dispatch({UiActionKind::ToggleVideoFullscreen});
        CHECK(!app.videoFullscreen_);
        CHECK(!app.fullscreenVideoInfoVisible(50'000));
        app.skipCurrent();
        CHECK(app.currentIsVideo() && !app.videoFullscreen_);
        app.dispatch({UiActionKind::ToggleVideoFullscreen});
        app.queue_.enqueue("music");
        app.skipCurrent();
        CHECK(app.currentTrackId_ == "music" && !app.videoFullscreen_);
        app.stopPlayback();
        app.currentTrackId_.clear();
        app.library_.tracks.clear();
        app.videoFullscreen_ = true;
        app.startNextTrack();
        CHECK(!app.videoFullscreen_); // An empty library must remain navigable.
        std::cout << "Video fullscreen: end, shuffle, skip, resize and music transition checked\n";
    }

    static void verifySourcesDuringScan(Theme theme, const std::filesystem::path& root) {
        const auto oldMusic = root / "original";
        const auto newMusic = root / "added-music";
        const auto moreMusic = root / "more-music";
        const auto newVideo = root / "added-video";
        for (const auto& directory : {oldMusic, newMusic, moreMusic, newVideo})
            std::filesystem::create_directories(directory);
        for (const auto& directory : {oldMusic, newMusic, moreMusic})
            std::filesystem::copy_file(std::filesystem::path(NEON_MIXER_FIXTURES) / "music.mp3", directory / "song.mp3");
        { std::ofstream file(newVideo / "Artist - Clip.mp4", std::ios::binary); file << "Video catalogue fixture"; }

        CHECK(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS));
        App app;
        app.sdlInitialized_ = true;
        app.settings_.theme = theme;
        app.settings_.musicRoots = {oldMusic};
        app.mode_ = UiMode::Admin;
        app.storage_ = std::make_unique<Storage>(root / "state");
        app.library_ = LibraryScanner{}.scan(app.settings_.musicRoots, {}, {}, {});
        CHECK(app.library_.tracks.size() == 1);
        app.updateFilter();
        const auto playingId = app.library_.tracks.front().id;
        app.library_.tracks.front().favorite = true;
        app.selectedIndex_ = 0;
        std::string error;
        CHECK(app.playTrack(app.library_.tracks.front(), 500, error));
        app.currentTrackId_ = playingId;
        CHECK(app.audio_.pause());
        const auto position = app.audio_.positionMs();
        app.queue_.enqueue(playingId);

        // Keep an old scan pending while multiple native-dialog results arrive.
        // Gate enrichment as well so this test never contacts online providers.
        std::promise<LibraryIndex> originalScan;
        app.scanFuture_ = originalScan.get_future();
        app.scanning_ = true;
        std::promise<void> enrichment;
        app.artworkFuture_ = enrichment.get_future();
        app.artworkFetching_ = true;
        const auto choose = [&](MediaKind kind, std::initializer_list<std::filesystem::path> directories) {
            std::vector<std::string> names;
            for (const auto& directory : directories) names.push_back(pathToUtf8(directory));
            std::vector<const char*> pointers;
            for (const auto& name : names) pointers.push_back(name.c_str());
            pointers.push_back(nullptr);
            app.folderDialogOpen_ = true;
            app.folderMediaKind_ = kind;
            App::folderCallback(&app, pointers.data(), 0);
            const auto start = std::chrono::steady_clock::now();
            app.processFolderResult();
            CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150));
            CHECK(!app.folderDialogOpen_);
        };
        choose(MediaKind::Music, {newMusic, newMusic / "."});
        CHECK(app.settings_.musicRoots.size() == 2 && app.pendingScan_);
        CHECK(app.scanCancel_ && app.artworkCancel_);
        choose(MediaKind::Video, {newVideo});
        choose(MediaKind::Music, {moreMusic});
        choose(MediaKind::Music, {moreMusic}); // Duplicate selection adds no source.
        CHECK(app.settings_.musicRoots.size() == 3 && app.settings_.videoRoots.size() == 1);
        CHECK(app.pendingScan_->musicRoots == app.settings_.musicRoots);
        CHECK(app.pendingScan_->videoRoots == app.settings_.videoRoots);
        CHECK(app.scanFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        const auto savedSettings = app.storage_->loadSettings();
        CHECK(savedSettings.musicRoots == app.settings_.musicRoots);
        CHECK(savedSettings.videoRoots == app.settings_.videoRoots);
        app.folderDialogOpen_ = true;
        App::folderCallback(&app, nullptr, 0); // Cancel a subsequent picker.
        app.processFolderResult();
        CHECK(!app.folderDialogOpen_ && app.pendingScan_->musicRoots.size() == 3);

        originalScan.set_value({}); // Partial/empty cancelled scan cannot wipe the cache.
        app.processScan();
        CHECK(app.scanning_ && !app.pendingScan_ && !app.scanCancel_);
        CHECK(LibraryScanner::find(app.library_, playingId));
        CHECK(app.selectedTrack() && app.selectedTrack()->id == playingId);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (app.scanning_ && std::chrono::steady_clock::now() < deadline) {
            app.processScan();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(!app.scanning_ && !app.pendingScan_);
        CHECK(app.library_.tracks.size() == 4);
        CHECK(LibraryScanner::filter(app.library_, "", LibraryFilter::Music).size() == 3);
        CHECK(LibraryScanner::filter(app.library_, "", LibraryFilter::Video).size() == 1);
        CHECK(app.library_.musicRoots.size() == 3 && app.library_.videoRoots.size() == 1);
        CHECK(app.currentTrackId_ == playingId && app.audio_.paused() && app.audio_.positionMs() == position);
        CHECK(app.selectedTrack() && app.selectedTrack()->id == playingId && app.selectedTrack()->favorite);
        CHECK(app.queue_.size() == 1);
        CHECK(app.storage_->loadLibrary().tracks.size() == 4);
        enrichment.set_value();
        std::cout << "Source edits during scan: latest folders indexed; playback, queue and cache preserved\n";
    }

    static void verifyShutdown(const std::filesystem::path& root) {
        CHECK(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS));
        App app;
        app.sdlInitialized_ = true;
        app.storage_ = std::make_unique<Storage>(root);
        app.library_.tracks = {track("playing", "Playing"), track("unvisited", "Cached record")};
        std::string error;
        CHECK(app.audio_.initialize(error));
        CHECK(app.playTrack(app.library_.tracks.front(), 1000, error));
        app.currentTrackId_ = "playing";
        app.queue_.enqueue("unvisited");
        app.running_ = true;
        app.mode_ = UiMode::Admin;
        app.scanning_ = app.artworkFetching_ = app.artworkRestartPending_ = true;
        app.pendingScan_ = App::ScanRequest{{root / "pending"}, {}};
        app.scanTrackUpdates_.push_back(track("arrived", "New record"));

        const auto probe = CreateWindowExW(0, L"STATIC", L"Shutdown responsiveness probe",
            0, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(probe != nullptr);
        std::atomic_bool cancelledTogether{}, windowResponded{};
        app.scanFuture_ = std::async(std::launch::async, [&] {
            while (!app.scanCancel_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            cancelledTogether = app.artworkCancel_.load();
            DWORD_PTR result{};
            windowResponded = SendMessageTimeoutW(probe, WM_NULL, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) != 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            return LibraryIndex{}; // A cancelled partial scan must not erase the cache.
        });
        app.artworkFuture_ = std::async(std::launch::async, [&] {
            while (!app.artworkCancel_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });
        app.dispatch({UiActionKind::AdminExit});
        CHECK(!app.running_);
        const auto started = std::chrono::steady_clock::now();
        app.shutdown();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        CHECK(elapsed < 2000);
        CHECK(cancelledTogether && windowResponded);
        CHECK(!app.artworkRestartPending_ && !app.artworkFetching_);
        CHECK(!app.pendingScan_);
        const auto saved = app.storage_->loadSettings();
        CHECK(saved.currentTrackId == "playing" && saved.playbackWasActive);
        CHECK(saved.playbackPositionMs >= 1000);
        CHECK(app.storage_->loadQueue().size() == 1);
        CHECK(app.storage_->loadLibrary().tracks.size() == 3);
        app.shutdown(); // Also called by App's destructor.
        DestroyWindow(probe);
        std::cout << "Exit with pending workers: " << elapsed << " ms, native messages serviced\n";
    }

    static void verifyMediaShuffle(Theme theme, const std::filesystem::path& stateRoot) {
        CHECK(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS));
        App app;
        app.sdlInitialized_ = true;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = theme;
        app.mode_ = UiMode::Browse;
        app.random_.seed(42);
        for (int i = 0; i < 8; ++i) {
            auto item = track(std::to_string(i), "Record " + std::to_string(i));
            if (i % 2) {
                item.mediaKind = MediaKind::Video;
                item.path = stateRoot / "missing-video.mp4";
            }
            app.library_.tracks.push_back(std::move(item));
        }
        app.updateFilter();
        app.rebuildGenres();
        app.credits_.insert();
        app.dispatch({UiActionKind::ShowVideo});
        CHECK(app.credits_.consume());
        CHECK(app.storage_->loadSettings().ambientMediaKind == MediaKind::Video);

        // Spending the last coin and clearing/hiding catalogue results must
        // not admit music into a video shuffle, including across repeat cycles.
        app.search_ = "No matching record";
        app.selectedArtistInitial_ = 'Z';
        app.selectedGenre_ = "Jazz";
        app.updateFilter();
        CHECK(app.filtered_.empty());
        app.dispatch({UiActionKind::ShowMusic}); // No credit: ignored.
        for (int cycle = 0; cycle < 3; ++cycle) {
            std::unordered_set<std::string> seen;
            for (int pick = 0; pick < 4; ++pick) {
                const auto* next = app.nextAmbientTrack();
                CHECK(next && next->mediaKind == MediaKind::Video);
                if (next) seen.insert(next->id);
            }
            CHECK(seen.size() == 4);
        }
        app.focusLibraryTrack(1);
        CHECK(app.libraryFilter_ == LibraryFilter::Video && app.filtered_.size() == 4);
        CHECK(app.search_.empty() && !app.selectedArtistInitial_ && app.selectedGenre_.empty());

        // An earlier music request still plays first and is revealed, but does
        // not replace the visitor's last explicit Video choice.
        app.nextAmbientVideo_ = app.library_.tracks[3];
        app.queue_.enqueue("0");
        app.startNextTrack();
        CHECK(!app.nextAmbientVideo_);
        CHECK(app.currentTrackId_ == "0" && app.currentIsManual_ && app.audio_.playing());
        CHECK(app.libraryFilter_ == LibraryFilter::Music);
        CHECK(app.settings_.ambientMediaKind == MediaKind::Video);
        app.skipCurrent();
        CHECK(app.currentTrackId_.empty() && !app.audio_.playing()); // Unavailable videos: no music fallback.
        CHECK(app.storage_->loadSettings().ambientMediaKind == MediaKind::Video);

        app.credits_.insert();
        app.dispatch({UiActionKind::ShowMusic});
        app.dispatch({UiActionKind::SelectGenre, 0});
        CHECK(app.libraryFilter_ == LibraryFilter::Music && app.filtered_.size() == 4);
        CHECK(app.credits_.consume());
        CHECK(app.storage_->loadSettings().ambientMediaKind == MediaKind::Music);
        // Missing queued videos are skipped; valid requests still precede shuffle.
        app.queue_.enqueue("1");
        app.queue_.enqueue("2");
        app.startNextTrack();
        CHECK(app.currentTrackId_ == "2" && app.currentIsManual_ && app.queue_.empty());
        for (int cycle = 0; cycle < 3; ++cycle) {
            std::unordered_set<std::string> seen;
            for (int pick = 0; pick < 4; ++pick) {
                app.skipCurrent();
                CHECK(app.currentTrack() && app.currentTrack()->mediaKind == MediaKind::Music);
                CHECK(!app.currentIsManual_ && app.audio_.playing());
                CHECK(app.credits_.available() == 0);
                seen.insert(app.currentTrackId_);
            }
            CHECK(seen.size() == 4);
        }

        // A section may become empty during a rescan, then gain new items.
        app.credits_.insert();
        app.dispatch({UiActionKind::ShowVideo});
        std::erase_if(app.library_.tracks, [](const Track& item) { return item.mediaKind == MediaKind::Video; });
        CHECK(!app.nextAmbientTrack());
        auto arrival = track("new-video", "New video");
        arrival.mediaKind = MediaKind::Video;
        app.library_.tracks.push_back(arrival);
        const auto* next = app.nextAmbientTrack();
        CHECK(next && next->id == "new-video");

        // After an empty/unavailable section stops playback, choosing a usable
        // section resumes shuffle. A choice during playback does not interrupt it.
        app.stopPlayback();
        app.currentTrackId_.clear();
        app.initialized_ = true;
        app.dispatch({UiActionKind::ShowMusic});
        CHECK(app.currentTrack() && app.currentTrack()->mediaKind == MediaKind::Music);
        CHECK(app.audio_.playing() && !app.currentIsManual_);
        const auto playing = app.currentTrackId_;
        app.dispatch({UiActionKind::ShowVideo});
        CHECK(app.currentTrackId_ == playing && app.audio_.playing());
        CHECK(app.settings_.ambientMediaKind == MediaKind::Video);
    }

    static void verifyVideoCatalogue(const std::filesystem::path& stateRoot) {
        App app;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = Theme::Retro;
        app.mode_ = UiMode::Browse;
        app.credits_.insert();
        for (int i = 0; i < 24; ++i) {
            auto item = track(std::to_string(i), "Video " + std::to_string(i));
            item.mediaKind = i < 20 ? MediaKind::Video : MediaKind::Music;
            app.library_.tracks.push_back(item);
        }
        app.updateFilter();
        app.rebuildGenres();
        app.dispatch({UiActionKind::ShowVideo});
        CHECK(app.libraryFilter_ == LibraryFilter::Video && app.filtered_.size() == 20);
        // A slow/missing video thumbnail never holds the catalogue navigation.
        app.dispatch({UiActionKind::PageNext});
        CHECK(app.page_ == 1 && !app.pendingPage_);
        app.dispatch({UiActionKind::PagePrevious});
        CHECK(app.page_ == 0 && !app.pendingPage_);
        const auto ninth = app.filtered_[9];
        app.focusLibraryTrack(ninth);
        CHECK(app.page_ == 4 && app.selectedIndex_ == ninth);
        app.mode_ = UiMode::Admin;
        app.dispatch({UiActionKind::AdminSelectTheme, 0});
        CHECK(app.page_ == 1 && app.selectedIndex_ == ninth);
        app.dispatch({UiActionKind::AdminSelectTheme, 1});
        CHECK(app.page_ == 4 && app.selectedIndex_ == ninth);
        app.mode_ = UiMode::Browse;
        app.genreMenuOpen_ = true;
        app.dispatch({UiActionKind::SelectGenre, 0});
        CHECK(app.libraryFilter_ == LibraryFilter::Video && app.filtered_.size() == 20 && app.page_ == 0);
        app.dispatch({UiActionKind::ShowMusic});
        CHECK(app.filtered_.size() == 4 && app.page_ == 0);
        CHECK(themePageSize(Theme::Retro, true) == 2);
        CHECK(themePageSize(Theme::Retro, false) == 20);
        CHECK(themePageSize(Theme::Neon, true) == 9);
    }

    static void verifyArtworkPaging(Theme theme) {
        App app;
        app.mode_ = UiMode::Browse;
        app.settings_.theme = theme;
        app.credits_.insert();
        const auto pageSize = themeDefinition(theme).pageSize;
        for (std::size_t i = 0; i < pageSize * 4; ++i)
            app.library_.tracks.push_back(track(std::to_string(i), std::to_string(i)));
        app.updateFilter();
        app.dispatch({UiActionKind::PageNext});
        CHECK(app.page_ == 0 && app.pendingPage_ == 1);
        for (int click = 0; click < 30; ++click) {
            app.dispatch({UiActionKind::PageNext});
            app.dispatch({UiActionKind::PagePrevious});
        }
        CHECK(app.page_ == 0 && app.pendingPage_ == 1);
        // Every destination cover must finish; one missing cover keeps the old page.
        for (std::size_t slot = pageSize; slot < pageSize * 2 - 1; ++slot)
            app.library_.tracks[app.filtered_[slot]].hasEmbeddedArtwork = false;
        app.finishPendingPage();
        CHECK(app.page_ == 0 && app.pendingPage_ == 1);
        app.library_.tracks[app.filtered_[pageSize * 2 - 1]].hasEmbeddedArtwork = false;
        app.finishPendingPage();
        CHECK(app.page_ == 1 && !app.pendingPage_);
        app.dispatch({UiActionKind::PagePrevious});
        CHECK(app.page_ == 1 && app.pendingPage_ == 0);
        app.updateFilter(); // Changing filters cancels a destination from the old list.
        CHECK(app.page_ == 0 && !app.pendingPage_);
        app.dispatch({UiActionKind::PageNext}); // Cached/coverless page is immediate.
        CHECK(app.page_ == 1 && !app.pendingPage_);
        app.dispatch({UiActionKind::PageNext});
        CHECK(app.pendingPage_ == 2);
        (void)app.credits_.consume();
        app.finishPendingPage();
        CHECK(!app.pendingPage_ && app.page_ == 1);
        app.credits_.insert();
        app.dispatch({UiActionKind::PageNext});
        app.keyboardOpen_ = true;
        app.finishPendingPage();
        CHECK(!app.pendingPage_ && app.page_ == 1);
    }

    static void verifyThemeMeters(const std::filesystem::path& stateRoot) {
        App app;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.mode_ = UiMode::Admin;
        app.settings_.visualizerMode = VisualizerMode::WarmTwinVu;
        app.dispatch({UiActionKind::AdminSelectTheme, 1});
        CHECK(app.settings_.theme == Theme::Retro);
        app.dispatch({UiActionKind::VisualizerNext});
        app.dispatch({UiActionKind::VisualizerPrevious});
        CHECK(app.settings_.retroVisualizerMode == VisualizerMode::RetroPhosphorScope);
        CHECK(app.settings_.visualizerMode == VisualizerMode::WarmTwinVu);
        app.dispatch({UiActionKind::AdminSelectTheme, 0});
        CHECK(app.settings_.theme == Theme::Neon);
        CHECK(app.settings_.visualizerMode == VisualizerMode::WarmTwinVu);
        app.dispatch({UiActionKind::VisualizerNext});
        CHECK(app.settings_.visualizerMode == VisualizerMode::AuroraSpectrum);
        app.dispatch({UiActionKind::VisualizerPrevious});
        CHECK(app.settings_.visualizerMode == VisualizerMode::WarmTwinVu);
        const auto restored = app.storage_->loadSettings();
        CHECK(restored.visualizerMode == VisualizerMode::WarmTwinVu);
        CHECK(restored.retroVisualizerMode == VisualizerMode::RetroPhosphorScope);
    }

    static void verifyArtistBrowse(Theme theme, const std::filesystem::path& stateRoot) {
        App app;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = theme;
        app.mode_ = UiMode::Browse;
        for (int i = 0; i < 26; ++i) {
            auto item = track(std::to_string(i), "Song " + std::to_string(i));
            item.artist = "Madonna";
            app.library_.tracks.push_back(std::move(item));
        }
        auto dua = track("dua", "Song 1");
        dua.artist = "Dua Lipa";
        app.library_.tracks.push_back(dua);
        app.updateFilter();
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'M'});
        CHECK(app.selectedArtistInitial_ == '\0'); // Same credit rules as search.
        app.credits_.insert();
        app.selectedIndex_ = 26;
        app.page_ = 1;
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'M'});
        CHECK(app.selectedArtistInitial_ == 'M' && app.page_ == 0);
        CHECK(app.filtered_.size() == 26);
        CHECK(!app.selectedTrack()); // A hidden previous artist cannot be requested.
        CHECK(app.credits_.available() == 1);
        app.page_ = 1;
        app.focusLibraryTrack(26); // A new background song leaves the artist index intact.
        CHECK(app.selectedArtistInitial_ == 'M' && app.page_ == 1);
        CHECK(app.filtered_.size() == 26);
        auto newArrival = track("new", "Song 0");
        newArrival.artist = "Madonna";
        app.scanTrackUpdates_.push_back(newArrival);
        CHECK(app.processScanTrackUpdates());
        CHECK(app.filtered_.size() == 27 && app.page_ == 1);
        app.keyboardOpen_ = true;
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'D'});
        CHECK(app.selectedArtistInitial_ == 'M');
        app.keyboardOpen_ = false;
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'D'});
        CHECK(app.filtered_.size() == 1 && app.page_ == 0);
        CHECK(app.library_.tracks[app.filtered_.front()].id == "dua");
        app.selectedGenre_ = "Jazz";
        app.updateFilter();
        CHECK(app.filtered_.empty());
        app.dispatch({UiActionKind::SelectArtistInitial});
        CHECK(app.filtered_.empty() && app.selectedArtistInitial_ == '\0');
        app.selectedGenre_.clear();
        app.updateFilter();
        CHECK(app.filtered_.size() == 28);
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'M'});
        app.mode_ = UiMode::Admin;
        const auto otherTheme = theme == Theme::Retro ? Theme::Neon : Theme::Retro;
        app.dispatch({UiActionKind::AdminSelectTheme, theme == Theme::Retro ? 0U : 1U});
        CHECK(app.settings_.theme == otherTheme && app.selectedArtistInitial_ == 'M');
        CHECK(app.filtered_.size() == 27);
    }

    static void verifyVideoArtistBrowse(Theme theme, const std::filesystem::path& stateRoot) {
        App app;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = theme;
        app.mode_ = UiMode::Browse;
        app.credits_.insert();
        for (const auto title : {"Madonna - Frozen", "Michael Jackson - Thriller", "Mariah Carey - Fantasy", "Aqua - Barbie Girl"}) {
            auto item = track(title, title);
            item.artist = "Unknown Artist";
            item.mediaKind = MediaKind::Video;
            app.library_.tracks.push_back(std::move(item));
        }
        app.dispatch({UiActionKind::ShowVideo});
        app.scanning_ = true; // Background rescan must not hide cached M artists.
        app.page_ = 1;
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'M'});
        CHECK(app.page_ == 0 && app.filtered_.size() == 3);
        CHECK(app.selectedArtistInitial_ == 'M' && app.libraryFilter_ == LibraryFilter::Video);
        CHECK(trackLabel(app.library_.tracks[app.filtered_[0]]).artist == "Madonna");
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'A'});
        CHECK(app.filtered_.size() == 1);
        app.dispatch({UiActionKind::SelectArtistInitial, 0, 'Z'});
        CHECK(app.filtered_.empty());
        UiModel model;
        model.scanning = true;
        model.library = &app.library_;
        model.selectedArtistInitial = 'Z';
        CHECK(!model.buildingEmptyLibrary());
        app.dispatch({UiActionKind::SelectArtistInitial});
        CHECK(app.filtered_.size() == 4);
        model.selectedArtistInitial = 0;
        CHECK(!model.buildingEmptyLibrary());
        model.library = nullptr;
        CHECK(model.buildingEmptyLibrary());
        model.search = "absent";
        CHECK(!model.buildingEmptyLibrary());
    }

    static void verifyQueuedTrackFocus(Theme theme, const std::filesystem::path& stateRoot) {
        CHECK(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS));
        App app;
        app.sdlInitialized_ = true;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = theme;
        app.mode_ = UiMode::Browse;
        // Library positions deliberately differ from the sorted catalogue order.
        for (int i = 44; i >= 0; --i)
            app.library_.tracks.push_back(track(std::to_string(i), "Song " + std::to_string(i)));
        const auto pageSize = themeDefinition(theme).pageSize;
        app.updateFilter();
        app.selectedIndex_ = 44;
        app.queue_.enqueue("44");
        app.startNextTrack();
        CHECK(app.currentTrackId_ == "44" && app.currentIsManual_ && app.audio_.playing());
        CHECK(app.selectedTrack() && app.selectedTrack()->id == "44");
        CHECK(app.page_ == 44 / pageSize && app.queue_.empty());
        CHECK(app.credits_.available() == 0);

        // A queued track must not displace a visitor's selection while credit remains.
        app.credits_.insert();
        app.selectedIndex_ = 44;
        app.page_ = 0;
        app.queue_.enqueue("43");
        app.skipCurrent();
        CHECK(app.currentTrackId_ == "43" && app.currentIsManual_);
        CHECK(app.selectedTrack() && app.selectedTrack()->id == "0");
        CHECK(app.page_ == 0 && app.credits_.available() == 1);

        // Matching filters survive following a queued song after the last credit.
        CHECK(app.credits_.consume());
        app.selectedArtistInitial_ = 'F';
        app.search_ = "Song";
        app.selectedGenre_ = "Rock";
        app.libraryFilter_ = LibraryFilter::Music;
        app.updateFilter();
        app.queue_.enqueue("42");
        app.skipCurrent();
        CHECK(app.currentTrackId_ == "42" && app.currentIsManual_);
        CHECK(app.selectedTrack() && app.selectedTrack()->id == "42");
        CHECK(app.page_ == 42 / pageSize);
        CHECK(app.selectedArtistInitial_ == 'F' && app.search_ == "Song");
        CHECK(app.selectedGenre_ == "Rock" && app.libraryFilter_ == LibraryFilter::Music);

        // Hidden queued songs become visible; missing queue entries are skipped.
        app.selectedArtistInitial_ = 'M';
        app.search_ = app.searchDraft_ = "Hidden";
        app.selectedGenre_ = "Jazz";
        app.libraryFilter_ = LibraryFilter::Video;
        app.updateFilter();
        CHECK(app.filtered_.empty());
        app.queue_.enqueue("missing");
        app.queue_.enqueue("41");
        app.skipCurrent();
        CHECK(app.currentTrackId_ == "41" && app.currentIsManual_ && app.audio_.playing());
        CHECK(app.selectedTrack() && app.selectedTrack()->id == "41");
        CHECK(app.page_ == 41 / pageSize && app.filtered_.size() == 45);
        CHECK(!app.selectedArtistInitial_ && app.search_.empty() && app.searchDraft_.empty());
        CHECK(app.selectedGenre_.empty() && app.libraryFilter_ == LibraryFilter::Music);
        CHECK(app.queue_.empty() && app.credits_.available() == 0);

        app.skipCurrent();
        CHECK(!app.currentIsManual_ && app.audio_.playing());
        CHECK(app.selectedTrack() && app.selectedTrack()->id == app.currentTrackId_);
    }

    static void verifyCreditSound(Theme theme, bool full, const std::filesystem::path& stateRoot) {
        CHECK(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS));
        App app;
        app.sdlInitialized_ = true;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = theme;
        app.mode_ = UiMode::Browse;
        std::string error;
        CHECK(app.audio_.initialize(error, NEON_EFFECT_FIXTURES));
        CHECK(app.audio_.takeEffectError().empty());
        if (full) for (int i = 0; i < 99; ++i) app.credits_.insert();
        app.dispatch({UiActionKind::InsertCoin});
        CHECK(app.credits_.available() == (full ? 99 : 1));
        float peak = 0;
        for (int i = 0; i < 35; ++i) {
            SDL_Delay(10);
            peak = std::max(peak, app.audio_.visualization().peakLeft);
        }
        CHECK((peak > 0.01F) == (theme == Theme::Retro && !full));
        CHECK(!app.audio_.playing() && !app.audio_.takeFinished());
    }

    static void run(Theme theme, const std::filesystem::path& stateRoot) {
        CHECK(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS));
        App app;
        app.sdlInitialized_ = true;
        app.storage_ = std::make_unique<Storage>(stateRoot);
        app.settings_.theme = theme;
        app.mode_ = UiMode::Browse;

        // Hold the worker's final result pending while delivering its mailbox
        // updates. No timing or large real-world collection is needed.
        std::promise<LibraryIndex> completion;
        app.scanFuture_ = completion.get_future();
        app.scanning_ = true;
        app.scanTrackUpdates_.push_back(track("first", "Zulu"));
        app.processScan();
        CHECK(app.scanning_);
        CHECK(app.scanFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        CHECK(app.library_.tracks.size() == 1);
        CHECK(app.filtered_ == std::vector<std::size_t>{0});
        CHECK(app.scanCacheDirty_);
        CHECK(app.currentTrackId_ == "first");
        CHECK(app.audio_.playing());

        CHECK(app.audio_.pause());
        CHECK(app.audio_.seek(1000));
        const auto position = app.audio_.positionMs();
        app.scanTrackUpdates_.push_back(track("second", "Alpha"));
        app.scanTrackUpdates_.push_back(track("second", "Alpha"));
        app.processScan();
        CHECK(app.library_.tracks.size() == 2);
        CHECK(app.filtered_.size() == 2);
        CHECK(app.currentTrackId_ == "first");
        CHECK(app.audio_.paused());
        CHECK(app.audio_.positionMs() == position);
        CHECK(app.selectedTrack() && app.selectedTrack()->id == "first");

        // The newly arrived record can be requested and played before the
        // final result is ready, with the usual coin and queue rules.
        app.dispatch({UiActionKind::InsertCoin});
        app.dispatch({UiActionKind::SelectTrack, 1});
        app.dispatch({UiActionKind::AddSelected});
        CHECK(app.queue_.size() == 1);
        CHECK(app.playNowPrompt_);
        app.dispatch({UiActionKind::PlayRequestNow});
        CHECK(app.currentTrackId_ == "second");
        CHECK(app.currentIsManual_);
        CHECK(app.audio_.playing());
        CHECK(app.scanning_);
        CHECK(app.queue_.empty());

        for (int i = 2; i < 25; ++i) {
            app.scanTrackUpdates_.push_back(track(std::to_string(i), "Record " + std::to_string(i)));
        }
        app.processScan();
        app.page_ = 1;
        app.selectedIndex_ = 20;
        app.scanTrackUpdates_.push_back(track("last", "Last arrival"));
        app.processScan();
        CHECK(app.page_ == 1);
        CHECK(app.selectedTrack() && app.selectedTrack()->id == "20");
        CHECK(app.currentTrackId_ == "second");

        // New arrivals also join active search, genre and media filters.
        app.search_ = "Needle";
        app.selectedGenre_ = "Rock";
        app.libraryFilter_ = LibraryFilter::Music;
        app.updateFilter();
        CHECK(app.filtered_.empty());
        auto matching = track("matching", "Needle in the collection");
        auto video = track("video", "Needle video");
        video.mediaKind = MediaKind::Video;
        app.scanTrackUpdates_ = {matching, video};
        app.processScan();
        CHECK(app.filtered_.size() == 1);
        CHECK(app.library_.tracks[app.filtered_.front()].id == "matching");
        app.search_.clear();
        app.selectedGenre_.clear();
        app.libraryFilter_ = LibraryFilter::All;
        app.updateFilter();
        app.page_ = 1;
        app.selectedIndex_ = 20;

        // Final sorting must preserve live selection, playback and edits made
        // while the scanner was using its older metadata snapshot.
        auto finalLibrary = app.library_;
        std::ranges::reverse(finalLibrary.tracks);
        app.library_.tracks[1].favorite = true;
        app.library_.tracks[1].genre = "Enriched Genre";
        for (auto& item : finalLibrary.tracks) {
            if (item.id == "second") item.genre = "Unknown Genre";
        }
        CHECK(app.audio_.pause());
        CHECK(app.audio_.seek(1000));
        const auto finalPosition = app.audio_.positionMs();
        auto staleUpdate = track("second", "Alpha");
        staleUpdate.genre = "Unknown Genre";
        app.scanTrackUpdates_.push_back(std::move(staleUpdate));
        completion.set_value(std::move(finalLibrary));
        app.processScan();
        CHECK(!app.scanning_);
        CHECK(app.scanTrackUpdates_.empty());
        CHECK(app.library_.tracks.size() == 28);
        CHECK(app.page_ == 1);
        CHECK(app.selectedTrack() && app.selectedTrack()->id == "20");
        CHECK(app.currentTrackId_ == "second");
        CHECK(app.currentTrack() && app.currentTrack()->favorite);
        CHECK(app.currentTrack() && app.currentTrack()->genre == "Enriched Genre");
        CHECK(app.audio_.paused());
        CHECK(app.audio_.positionMs() == finalPosition);
        CHECK(app.storage_->loadLibrary().tracks.size() == app.library_.tracks.size());
    }
};
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        neon::pathFromUtf8("neon-streaming-test-" + neon::randomId());
    try {
        neon::AppScanTest::verifyVideoFullscreen(neon::Theme::Neon, root / "fullscreen-modern");
        neon::AppScanTest::verifyVideoFullscreen(neon::Theme::Retro, root / "fullscreen-retro");
        neon::AppScanTest::verifyShutdown(root / "shutdown");
        neon::AppScanTest::verifySourcesDuringScan(neon::Theme::Retro, root / "sources-retro");
        neon::AppScanTest::verifySourcesDuringScan(neon::Theme::Neon, root / "sources-modern");
        neon::AppScanTest::verifyMediaShuffle(neon::Theme::Neon, root / "shuffle-neon");
        neon::AppScanTest::verifyMediaShuffle(neon::Theme::Retro, root / "shuffle-retro");
        neon::AppScanTest::verifyVideoCatalogue(root / "vhs-catalogue");
        neon::AppScanTest::verifyArtworkPaging(neon::Theme::Neon);
        neon::AppScanTest::verifyArtworkPaging(neon::Theme::Retro);
        neon::AppScanTest::verifyThemeMeters(root / "theme-meters");
        neon::AppScanTest::verifyArtistBrowse(neon::Theme::Retro, root / "artists-retro");
        neon::AppScanTest::verifyArtistBrowse(neon::Theme::Neon, root / "artists-neon");
        neon::AppScanTest::verifyVideoArtistBrowse(neon::Theme::Retro, root / "video-artists-retro");
        neon::AppScanTest::verifyVideoArtistBrowse(neon::Theme::Neon, root / "video-artists-neon");
        neon::AppScanTest::verifyQueuedTrackFocus(neon::Theme::Neon, root / "queue-focus-neon");
        neon::AppScanTest::verifyQueuedTrackFocus(neon::Theme::Retro, root / "queue-focus-retro");
        neon::AppScanTest::verifyCreditSound(neon::Theme::Retro, false, root / "credit-retro");
        neon::AppScanTest::verifyCreditSound(neon::Theme::Retro, true, root / "credit-full");
        neon::AppScanTest::verifyCreditSound(neon::Theme::Neon, false, root / "credit-neon");
        neon::AppScanTest::run(neon::Theme::Neon, root / "neon");
        neon::AppScanTest::run(neon::Theme::Retro, root / "retro");
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        ++failures;
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::cout << "Incremental scan checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
