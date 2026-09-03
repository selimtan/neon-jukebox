#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "neon/Audio.hpp"
#include "neon/Library.hpp"
#include "neon/OnlineArtwork.hpp"
#include "neon/Queue.hpp"
#include "neon/Security.hpp"
#include "neon/Storage.hpp"
#include "neon/UI.hpp"
#include "neon/Video.hpp"

namespace neon {

class App {
public:
    App() = default;
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

private:
    bool initialize(std::string& error);
    void shutdown();
    void handleEvent(SDL_Event& event);
    void dispatch(const UiAction& action);
    void update();
    void render();
    void updateFilter(bool resetPage = true);
    void rebuildGenres();
    void processScanTrackUpdates();
    void startScan(std::vector<std::filesystem::path> musicRoots,
                   std::vector<std::filesystem::path> videoRoots);
    void processScan();
    void startArtworkFetch();
    void processArtworkFetch();
    void showFolderDialog(MediaKind mediaKind, bool setup);
    void processFolderResult();
    void recoverAudio();
    bool playTrack(const Track& track, std::int64_t startMs, std::string& error);
    void stopPlayback();
    [[nodiscard]] bool currentIsVideo() const;
    void startNextTrack();
    void skipCurrent();
    void persistPlayback();
    void setToast(std::string message, std::uint64_t durationMs = 2800);
    const Track* selectedTrack() const;
    const Track* currentTrack() const;
    PlaybackSnapshot playbackSnapshot() const;

    static void SDLCALL folderCallback(void* userdata, const char* const* filelist, int filter);

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    bool running_{};
    bool initialized_{};
    bool sdlInitialized_{};
    bool ttfInitialized_{};
    UI ui_;
    AudioEngine audio_;
    VideoEngine video_;
    std::unique_ptr<Storage> storage_;
    Settings settings_;
    LibraryIndex library_;
    RequestQueue queue_;
    CoinCreditBank credits_;
    std::vector<QueueItem> queueView_;
    std::vector<std::size_t> filtered_;
    std::optional<std::size_t> selectedIndex_;
    std::string currentTrackId_;
    bool currentIsManual_{};
    std::string search_;
    std::string searchDraft_;
    std::vector<std::string> genres_;
    std::string selectedGenre_;
    bool genreMenuOpen_{};
    std::size_t genreMenuPage_{};
    LibraryFilter libraryFilter_{LibraryFilter::All};
    bool keyboardOpen_{};
    bool visualizerOpen_{};
    bool videoFullscreen_{};
    bool visualizerPointerDown_{};
    float visualizerPointerStartX_{};
    bool playNowPrompt_{};
    std::string requestedTrackTitle_;
    std::size_t page_{};
    UiMode mode_{UiMode::SetupPin};
    std::string pinInput_;
    std::string firstPin_;
    std::string pinPrompt_{"Enter 4-8 digits"};
    PinGuard pinGuard_;
    std::size_t adminQueueSelection_{static_cast<std::size_t>(-1)};

    std::future<LibraryIndex> scanFuture_;
    std::atomic_bool scanCancel_{};
    std::mutex scanProgressMutex_;
    ScanProgress scanProgress_;
    std::mutex scanTrackMutex_;
    std::vector<Track> scanTrackUpdates_;
    bool scanCacheDirty_{};
    bool scanning_{};

    std::future<void> artworkFuture_;
    std::atomic_bool artworkCancel_{};
    std::mutex artworkMutex_;
    OnlineArtworkProgress artworkProgress_;
    std::vector<OnlineArtworkMatch> artworkMatches_;
    bool artworkFetching_{};
    bool artworkRestartPending_{};

    std::mutex folderMutex_;
    std::vector<std::filesystem::path> pendingFolders_;
    bool folderResultReady_{};
    bool folderForSetup_{};
    MediaKind folderMediaKind_{MediaKind::Music};

    bool adminReveal_{};
    std::string toast_;
    std::uint64_t toastUntil_{};
    std::uint64_t lastPersist_{};
    bool audioRecoveryPending_{};
    std::uint64_t audioRecoveryAt_{};
    std::int64_t recoveryPositionMs_{};
    bool recoveryPaused_{};
    AudioVisualizationFrame visualization_{};
    std::uint64_t lastSpectrumUpdate_{};
    AmbientSelector ambientSelector_;
    std::mt19937 random_{std::random_device{}()};
};

}  // namespace neon
