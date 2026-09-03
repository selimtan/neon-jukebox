#include "neon/App.hpp"

#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "neon/Utils.hpp"

namespace neon {
namespace {
constexpr std::size_t noSelection = static_cast<std::size_t>(-1);

std::vector<std::filesystem::path> uniqueRoots(
    const std::vector<std::filesystem::path>& roots) {
    std::vector<std::filesystem::path> result;
    std::unordered_set<std::string> seen;
    result.reserve(roots.size());
    for (const auto& requestedRoot : roots) {
        if (requestedRoot.empty()) continue;
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(requestedRoot, error);
        auto root = error ? requestedRoot.lexically_normal() : canonical;
        if (seen.insert(normalizeForSearch(pathToUtf8(root))).second) {
            result.push_back(std::move(root));
        }
    }
    return result;
}

bool isUnknown(std::string_view value, std::string_view fallback) {
    return value.empty() || normalizeForSearch(value) == normalizeForSearch(fallback);
}

void preserveEnrichedMetadata(Track& target, const Track& current) {
    if (isUnknown(target.artist, "Unknown Artist") &&
        !isUnknown(current.artist, "Unknown Artist")) target.artist = current.artist;
    if (isUnknown(target.album, "Unknown Album") &&
        !isUnknown(current.album, "Unknown Album")) target.album = current.album;
    if (isUnknown(target.genre, "Unknown Genre") &&
        !isUnknown(current.genre, "Unknown Genre")) target.genre = current.genre;
    if (target.albumYear <= 0 && current.albumYear > 0) target.albumYear = current.albumYear;
    if (current.onlineArtwork) target.onlineArtwork = current.onlineArtwork;
}
}

App::~App() { shutdown(); }

int App::run() {
    std::string error;
    if (!initialize(error)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Neon Jukebox", error.c_str(), window_);
        return 1;
    }
    running_ = true;
    while (running_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) handleEvent(event);
        update();
        render();
        SDL_Delay(8);
    }
    shutdown();
    return 0;
}

bool App::initialize(std::string& error) {
    SDL_SetAppMetadata("Neon Jukebox", "1.0.0", "com.neonjukebox.app");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) { error = SDL_GetError(); return false; }
    sdlInitialized_ = true;
    if (!TTF_Init()) { error = SDL_GetError(); return false; }
    ttfInitialized_ = true;
    window_ = SDL_CreateWindow("Neon Jukebox", 1920, 1080,
                               SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) { error = SDL_GetError(); return false; }
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) { error = SDL_GetError(); return false; }
    SDL_SetRenderVSync(renderer_, 1);
    if (!SDL_SetRenderLogicalPresentation(renderer_, 1920, 1080, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        error = SDL_GetError(); return false;
    }
    if (!ui_.initialize(renderer_, error)) return false;

    char* preference = SDL_GetPrefPath("NeonJukebox", "NeonJukebox");
    if (!preference) { error = SDL_GetError(); return false; }
    storage_ = std::make_unique<Storage>(pathFromUtf8(preference));
    SDL_free(preference);
    settings_ = storage_->loadSettings();
    settings_.musicRoots = uniqueRoots(settings_.musicRoots);
    settings_.videoRoots = uniqueRoots(settings_.videoRoots);
    if (settings_.ambientMode != AmbientMode::Shuffle || !settings_.ambientRepeat) {
        settings_.ambientMode = AmbientMode::Shuffle;
        settings_.ambientRepeat = true;
    }
    // Also commits older single/combined-root settings in the split library form.
    storage_->saveSettings(settings_);
    library_ = storage_->loadLibrary();
    rebuildGenres();
    queue_.restore(storage_->loadQueue());
    audio_.setVolume(settings_.volume);
    std::string audioError;
    if (!audio_.initialize(audioError)) {
        audioRecoveryPending_ = true;
        audioRecoveryAt_ = SDL_GetTicks() + 2000;
        setToast("Audio unavailable: " + audioError, 6000);
    }
    std::string videoError;
    if (!video_.initialize(window_, videoError)) setToast("Video unavailable: " + videoError, 6000);
    video_.setVolume(settings_.volume);

    if (!settings_.adminPin.configured()) mode_ = UiMode::SetupPin;
    else if (settings_.musicRoots.empty() && settings_.videoRoots.empty()) mode_ = UiMode::SetupFolder;
    else mode_ = UiMode::Browse;
    updateFilter();

    if (mode_ == UiMode::Browse) {
        if (settings_.playbackWasActive && !settings_.currentTrackId.empty()) {
            if (const Track* track = LibraryScanner::find(library_, settings_.currentTrackId)) {
                std::string playError;
                if (playTrack(*track, settings_.playbackPositionMs, playError)) {
                    currentTrackId_ = track->id;
                    currentIsManual_ = settings_.currentTrackManual;
                }
            }
        }
        startScan(settings_.musicRoots, settings_.videoRoots);
        // Existing cached titles/albums are sufficient to begin safe online
        // enrichment immediately; local file metadata continues scanning in
        // parallel and takes precedence when present.
        startArtworkFetch();
        if (currentTrackId_.empty()) startNextTrack();
    }
    lastPersist_ = SDL_GetTicks();
    initialized_ = true;
    return true;
}

void App::shutdown() {
    if (!initialized_ && !sdlInitialized_ && !ttfInitialized_ && !window_ && !renderer_) return;
    scanCancel_.store(true, std::memory_order_release);
    if (scanFuture_.valid()) scanFuture_.wait();
    processScanTrackUpdates();
    artworkCancel_.store(true, std::memory_order_release);
    if (artworkFuture_.valid()) artworkFuture_.wait();
    artworkRestartPending_ = false;
    processArtworkFetch();
    if (storage_) {
        persistPlayback();
        storage_->saveQueue(queue_.snapshot());
        storage_->saveLibrary(library_);
    }
    // Renderer-owned textures and audio callbacks must be released while the
    // SDL subsystems they depend on are still alive.
    video_.shutdown();
    audio_.shutdown();
    ui_.shutdown();
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    if (ttfInitialized_) { TTF_Quit(); ttfInitialized_ = false; }
    if (sdlInitialized_) { SDL_Quit(); sdlInitialized_ = false; }
    initialized_ = false;
}

void App::handleEvent(SDL_Event& event) {
    // Mixers opened on SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK migrate with the
    // system default device. SDL_mixer also consumes FORMAT_CHANGED events
    // and updates its conversion streams, so rebuilding the mixer here would
    // turn its own notifications into a reconnect loop.
    if (event.type == SDL_EVENT_AUDIO_DEVICE_ADDED && !event.adevice.recording && audioRecoveryPending_) {
        audioRecoveryAt_ = SDL_GetTicks();
        return;
    }
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        setToast("Administrator access is required to exit");
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        adminReveal_ = false;
        visualizerPointerDown_ = false;
        return;
    }
    SDL_ConvertEventToRenderCoordinates(renderer_, &event);
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const bool revealArea = event.motion.x <= 230.0F && event.motion.y <= 100.0F;
        const bool hotCorner = event.motion.x <= 12.0F && event.motion.y <= 12.0F;
        adminReveal_ = mode_ == UiMode::Browse && !visualizerOpen_ && !videoFullscreen_ && revealArea &&
                       (adminReveal_ || hotCorner);
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        if (visualizerOpen_) {
            visualizerPointerDown_ = true;
            visualizerPointerStartX_ = event.button.x;
        } else if (mode_ == UiMode::Browse && event.button.x <= 12.0F && event.button.y <= 12.0F) {
            adminReveal_ = true;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
        if (visualizerOpen_ && visualizerPointerDown_) {
            visualizerPointerDown_ = false;
            const float travel = event.button.x - visualizerPointerStartX_;
            if (std::abs(travel) >= 120.0F) {
                dispatch({travel < 0.0F ? UiActionKind::VisualizerNext
                                       : UiActionKind::VisualizerPrevious});
                return;
            }
        }
        if (const auto action = ui_.hitTest(event.button.x, event.button.y)) dispatch(*action);
    }
    if (event.type == SDL_EVENT_KEY_DOWN && keyboardOpen_ &&
        (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
        dispatch({UiActionKind::SubmitSearch});
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        if (videoFullscreen_) {
            videoFullscreen_ = false;
        } else if (visualizerOpen_) {
            visualizerOpen_ = false;
            visualizerPointerDown_ = false;
        } else if (playNowPrompt_) {
            playNowPrompt_ = false; requestedTrackTitle_.clear();
            setToast("Request will play after the current track");
        } else if (genreMenuOpen_) {
            genreMenuOpen_ = false;
        } else if (keyboardOpen_) { keyboardOpen_ = false; searchDraft_ = search_; }
        else if (mode_ == UiMode::AdminPin) { mode_ = UiMode::Browse; pinInput_.clear(); }
        else if (mode_ == UiMode::ChangePin) { mode_ = UiMode::Admin; pinInput_.clear(); firstPin_.clear(); }
        else if (mode_ == UiMode::Admin) mode_ = UiMode::Browse;
        else setToast("Move the pointer to the top-left corner for administrator access");
    }
}

void App::dispatch(const UiAction& action) {
    switch (action.kind) {
        case UiActionKind::OpenAdmin:
            adminReveal_ = false;
            genreMenuOpen_ = false;
            visualizerOpen_ = false;
            playNowPrompt_ = false;
            requestedTrackTitle_.clear();
            pinInput_.clear();
            pinPrompt_ = "Enter administrator PIN";
            mode_ = UiMode::AdminPin;
            break;
        case UiActionKind::OpenVisualizer:
            if (mode_ == UiMode::Browse && !keyboardOpen_ && !playNowPrompt_) {
                adminReveal_ = false;
                genreMenuOpen_ = false;
                visualizerOpen_ = true;
            }
            break;
        case UiActionKind::VisualizerPrevious: {
            const auto current = static_cast<std::size_t>(settings_.visualizerMode);
            settings_.visualizerMode = static_cast<VisualizerMode>(
                (current + visualizerModeCount - 1) % visualizerModeCount);
            storage_->saveSettings(settings_);
            break;
        }
        case UiActionKind::VisualizerNext: {
            const auto current = static_cast<std::size_t>(settings_.visualizerMode);
            settings_.visualizerMode = static_cast<VisualizerMode>((current + 1) % visualizerModeCount);
            storage_->saveSettings(settings_);
            break;
        }
        case UiActionKind::CloseVisualizer:
            visualizerOpen_ = false;
            visualizerPointerDown_ = false;
            break;
        case UiActionKind::ToggleVideoFullscreen:
            if (currentIsVideo()) {
                visualizerOpen_ = false;
                videoFullscreen_ = !videoFullscreen_;
            }
            break;
        case UiActionKind::InsertCoin:
            credits_.insert();
            setToast(std::to_string(credits_.available()) +
                     (credits_.available() == 1 ? " credit available" : " credits available"));
            break;
        case UiActionKind::SelectTrack:
            if (credits_.available() > 0 && action.index < library_.tracks.size()) selectedIndex_ = action.index;
            break;
        case UiActionKind::AddSelected:
            if (credits_.available() == 0) {
                setToast("Insert a coin to request a track");
            } else if (const auto* track = selectedTrack()) {
                const bool backgroundPlaying = !currentTrackId_.empty() && !currentIsManual_;
                const bool requestQueueWasEmpty = queue_.empty();
                queue_.enqueue(track->id);
                (void)credits_.consume();
                storage_->saveQueue(queue_.snapshot());
                if (currentTrackId_.empty()) {
                    setToast(track->title + " is playing now");
                    startNextTrack();
                } else if (shouldOfferPlayNow(backgroundPlaying, requestQueueWasEmpty)) {
                    const auto* firstRequest = queue_.front();
                    const auto* firstTrack = firstRequest ? LibraryScanner::find(library_, firstRequest->trackId) : nullptr;
                    requestedTrackTitle_ = firstTrack ? firstTrack->title : track->title;
                    playNowPrompt_ = true;
                } else {
                    setToast(track->title + " added to queue");
                }
            }
            break;
        case UiActionKind::ToggleGenreMenu:
            if (credits_.available() > 0 && mode_ == UiMode::Browse) {
                genreMenuOpen_ = !genreMenuOpen_;
                if (genreMenuOpen_) genreMenuPage_ = 0;
            }
            break;
        case UiActionKind::CloseGenreMenu:
            genreMenuOpen_ = false;
            break;
        case UiActionKind::SelectGenre:
            if (credits_.available() == 0) break;
            if (action.index == 0) {
                selectedGenre_.clear();
            } else if (action.index <= genres_.size()) {
                selectedGenre_ = genres_[action.index - 1];
            } else {
                break;
            }
            libraryFilter_ = LibraryFilter::All;
            genreMenuOpen_ = false;
            updateFilter();
            setToast(selectedGenre_.empty() ? "Showing all genres"
                                            : "Genre: " + selectedGenre_);
            break;
        case UiActionKind::GenrePagePrevious:
            if (genreMenuPage_ > 0) --genreMenuPage_;
            break;
        case UiActionKind::GenrePageNext:
            if ((genreMenuPage_ + 1) * 9 < genres_.size() + 1) ++genreMenuPage_;
            break;
        case UiActionKind::ShowMusic:
            if (credits_.available() > 0) {
                genreMenuOpen_ = false; libraryFilter_ = LibraryFilter::Music; updateFilter();
            }
            break;
        case UiActionKind::ShowVideo:
            if (credits_.available() > 0) {
                genreMenuOpen_ = false; libraryFilter_ = LibraryFilter::Video; updateFilter();
            }
            break;
        case UiActionKind::OpenKeyboard:
            if (credits_.available() > 0) {
                genreMenuOpen_ = false; searchDraft_ = search_; keyboardOpen_ = true;
            }
            break;
        case UiActionKind::KeyCharacter:
            if (searchDraft_.size() < 80) searchDraft_.push_back(action.character);
            break;
        case UiActionKind::KeySpace:
            if (!searchDraft_.empty() && searchDraft_.size() < 80) searchDraft_.push_back(' ');
            break;
        case UiActionKind::KeyBackspace:
            if (!searchDraft_.empty()) searchDraft_.pop_back();
            break;
        case UiActionKind::KeyClear: searchDraft_.clear(); break;
        case UiActionKind::SubmitSearch:
            search_ = searchDraft_; keyboardOpen_ = false; updateFilter(); break;
        case UiActionKind::CloseKeyboard:
            searchDraft_ = search_; keyboardOpen_ = false; break;
        case UiActionKind::PlayRequestNow:
            playNowPrompt_ = false;
            requestedTrackTitle_.clear();
            if (!currentTrackId_.empty() && !currentIsManual_ && !queue_.empty()) {
                stopPlayback();
                currentTrackId_.clear();
                startNextTrack();
            } else if (currentTrackId_.empty()) {
                startNextTrack();
            }
            break;
        case UiActionKind::WaitForCurrentTrack:
            playNowPrompt_ = false;
            requestedTrackTitle_.clear();
            setToast("Request will play after the current track");
            break;
        case UiActionKind::PinDigit:
            if (pinInput_.size() < 8) pinInput_.push_back(action.character); break;
        case UiActionKind::PinBackspace:
            if (!pinInput_.empty()) pinInput_.pop_back(); break;
        case UiActionKind::PinCancel:
            pinInput_.clear(); firstPin_.clear();
            mode_ = mode_ == UiMode::ChangePin ? UiMode::Admin : UiMode::Browse;
            break;
        case UiActionKind::PinSubmit:
            if (mode_ == UiMode::SetupPin || mode_ == UiMode::ChangePin) {
                if (!PinGuard::validFormat(pinInput_)) { setToast("PIN must contain 4-8 digits"); break; }
                if (firstPin_.empty()) {
                    firstPin_ = pinInput_; pinInput_.clear(); pinPrompt_ = "Enter the same PIN again";
                } else if (firstPin_ == pinInput_) {
                    settings_.adminPin = PinGuard::create(pinInput_);
                    storage_->saveSettings(settings_);
                    const bool initialSetup = mode_ == UiMode::SetupPin;
                    firstPin_.clear(); pinInput_.clear();
                    mode_ = initialSetup ? UiMode::SetupFolder : UiMode::Admin;
                    pinPrompt_ = "Enter 4-8 digits";
                    if (!initialSetup) setToast("Administrator PIN changed");
                } else {
                    firstPin_.clear(); pinInput_.clear(); pinPrompt_ = "PINs did not match. Try again";
                }
            } else if (mode_ == UiMode::AdminPin) {
                if (pinGuard_.locked()) setToast("Try again in " + std::to_string(pinGuard_.secondsRemaining()) + " seconds");
                else if (pinGuard_.attempt(pinInput_, settings_.adminPin)) { pinInput_.clear(); mode_ = UiMode::Admin; }
                else { pinInput_.clear(); setToast("Incorrect PIN"); }
            }
            break;
        case UiActionKind::ChooseMusicFolders: showFolderDialog(MediaKind::Music, true); break;
        case UiActionKind::ChooseVideoFolders: showFolderDialog(MediaKind::Video, true); break;
        case UiActionKind::FinishFolderSetup:
            if (!settings_.musicRoots.empty() || !settings_.videoRoots.empty()) {
                library_ = {};
                library_.musicRoots = settings_.musicRoots;
                library_.videoRoots = settings_.videoRoots;
                ambientSelector_.reset();
                selectedIndex_.reset();
                mode_ = UiMode::Browse;
                rebuildGenres();
                updateFilter();
                startScan(settings_.musicRoots, settings_.videoRoots);
            }
            break;
        case UiActionKind::PagePrevious:
            if (credits_.available() > 0 && page_ > 0) --page_; break;
        case UiActionKind::PageNext:
            if (credits_.available() > 0 && (page_ + 1) * 9 < filtered_.size()) ++page_; break;
        case UiActionKind::AdminClose: videoFullscreen_ = false; mode_ = UiMode::Browse; break;
        case UiActionKind::AdminPlayPause:
            if (currentIsVideo()) {
                if (video_.paused()) video_.resume(); else video_.pause();
            } else {
                if (audio_.paused()) audio_.resume(); else audio_.pause();
            }
            break;
        case UiActionKind::AdminSkip: skipCurrent(); break;
        case UiActionKind::AdminSeekBackward: {
            const auto snapshot = playbackSnapshot();
            const auto position = std::max<std::int64_t>(0, snapshot.positionMs - 15000);
            if (currentIsVideo()) video_.seek(position); else audio_.seek(position);
            break;
        }
        case UiActionKind::AdminSeekForward: {
            const auto snapshot = playbackSnapshot();
            const auto position = std::min(snapshot.durationMs, snapshot.positionMs + 15000);
            if (currentIsVideo()) video_.seek(position); else audio_.seek(position);
            break;
        }
        case UiActionKind::AdminVolumeDown:
            settings_.volume = std::max(0.0F, settings_.volume - 0.1F);
            audio_.setVolume(settings_.volume); video_.setVolume(settings_.volume); persistPlayback(); break;
        case UiActionKind::AdminVolumeUp:
            settings_.volume = std::min(1.0F, settings_.volume + 0.1F);
            audio_.setVolume(settings_.volume); video_.setVolume(settings_.volume); persistPlayback(); break;
        case UiActionKind::AdminClearQueue:
            queue_.clear(); adminQueueSelection_ = noSelection; storage_->saveQueue(queue_.snapshot()); setToast("Queue cleared"); break;
        case UiActionKind::AdminRescan:
            if (!settings_.musicRoots.empty() || !settings_.videoRoots.empty())
                startScan(settings_.musicRoots, settings_.videoRoots);
            break;
        case UiActionKind::AdminChooseMusicFolders:
            showFolderDialog(MediaKind::Music, false); break;
        case UiActionKind::AdminChooseVideoFolders:
            showFolderDialog(MediaKind::Video, false); break;
        case UiActionKind::AdminCycleAmbient:
            settings_.ambientMode = AmbientMode::Shuffle;
            settings_.ambientRepeat = true;
            persistPlayback(); break;
        case UiActionKind::AdminToggleRepeat:
            settings_.ambientMode = AmbientMode::Shuffle;
            settings_.ambientRepeat = true;
            persistPlayback(); break;
        case UiActionKind::AdminToggleFavorite:
            if (selectedIndex_ && *selectedIndex_ < library_.tracks.size()) {
                auto& track = library_.tracks[*selectedIndex_]; track.favorite = !track.favorite;
                storage_->saveLibrary(library_); updateFilter();
            }
            break;
        case UiActionKind::AdminUseArtwork:
            settings_.nowPlayingArtworkMode = NowPlayingArtworkMode::Artwork;
            storage_->saveSettings(settings_);
            setToast("Now Playing display: artwork");
            break;
        case UiActionKind::AdminUseSpinningDisc:
            settings_.nowPlayingArtworkMode = NowPlayingArtworkMode::SpinningDisc;
            storage_->saveSettings(settings_);
            setToast("Now Playing display: spinning CD");
            break;
        case UiActionKind::AdminChangePin:
            firstPin_.clear(); pinInput_.clear(); pinPrompt_ = "Enter a new 4-8 digit PIN";
            mode_ = UiMode::ChangePin; break;
        case UiActionKind::AdminExit: running_ = false; break;
        case UiActionKind::AdminQueueSelect:
            if (action.index < queue_.size()) adminQueueSelection_ = action.index; break;
        case UiActionKind::AdminQueueUp:
            if (adminQueueSelection_ != noSelection && adminQueueSelection_ > 0 &&
                queue_.move(adminQueueSelection_, adminQueueSelection_ - 1)) {
                --adminQueueSelection_; storage_->saveQueue(queue_.snapshot());
            }
            break;
        case UiActionKind::AdminQueueDown:
            if (adminQueueSelection_ != noSelection && adminQueueSelection_ + 1 < queue_.size() &&
                queue_.move(adminQueueSelection_, adminQueueSelection_ + 1)) {
                ++adminQueueSelection_; storage_->saveQueue(queue_.snapshot());
            }
            break;
        case UiActionKind::AdminQueueRemove:
            if (adminQueueSelection_ != noSelection) {
                const auto snapshot = queue_.snapshot();
                if (adminQueueSelection_ < snapshot.size()) queue_.remove(snapshot[adminQueueSelection_].id);
                adminQueueSelection_ = noSelection; storage_->saveQueue(queue_.snapshot());
            }
            break;
        default: break;
    }
}

void App::update() {
    processFolderResult();
    processScan();
    processArtworkFetch();
    if (video_.takeSurfaceTouch() && mode_ == UiMode::Browse && currentIsVideo() &&
        !keyboardOpen_ && !playNowPrompt_ && !visualizerOpen_) {
        dispatch({UiActionKind::ToggleVideoFullscreen});
    }
    if (audioRecoveryPending_ && SDL_GetTicks() >= audioRecoveryAt_) recoverAudio();
    if (audio_.takeFinished() || video_.takeFinished()) {
        playNowPrompt_ = false;
        requestedTrackTitle_.clear();
        stopPlayback();
        currentTrackId_.clear();
        currentIsManual_ = false;
        videoFullscreen_ = false;
        startNextTrack();
    }
    if (toastUntil_ && SDL_GetTicks() >= toastUntil_) { toast_.clear(); toastUntil_ = 0; }
    if (SDL_GetTicks() - lastPersist_ >= 5000) {
        persistPlayback();
        if (scanCacheDirty_ && storage_) {
            storage_->saveLibrary(library_);
            scanCacheDirty_ = false;
        }
        lastPersist_ = SDL_GetTicks();
    }
}

void App::render() {
    queueView_ = queue_.snapshot();
    ScanProgress progress;
    { std::scoped_lock lock(scanProgressMutex_); progress = scanProgress_; }
    OnlineArtworkProgress artworkProgress;
    { std::scoped_lock lock(artworkMutex_); artworkProgress = artworkProgress_; }
    UiModel model;
    model.mode = mode_;
    model.library = &library_;
    model.filtered = &filtered_;
    model.queue = &queueView_;
    model.currentTrack = currentTrack();
    model.selectedTrack = selectedTrack();
    model.playback = playbackSnapshot();
    if (SDL_GetTicks() - lastSpectrumUpdate_ >= 33) {
        visualization_ = currentIsVideo() ? video_.visualization() : audio_.visualization();
        lastSpectrumUpdate_ = SDL_GetTicks();
    }
    model.visualization = visualization_;
    model.search = search_;
    model.searchDraft = searchDraft_;
    model.genres = &genres_;
    model.selectedGenre = selectedGenre_;
    model.genreMenuOpen = genreMenuOpen_;
    model.genreMenuPage = genreMenuPage_;
    model.pinPrompt = pinPrompt_;
    model.pinLength = pinInput_.size();
    model.libraryFilter = libraryFilter_;
    model.keyboardOpen = keyboardOpen_;
    model.adminReveal = adminReveal_;
    model.visualizerOpen = visualizerOpen_;
    model.visualizerMode = settings_.visualizerMode;
    model.nowPlayingArtworkMode = settings_.nowPlayingArtworkMode;
    model.videoFullscreen = videoFullscreen_;
    model.videoPlaying = currentIsVideo();
    model.credits = credits_.available();
    model.playNowPrompt = playNowPrompt_;
    model.requestedTrackTitle = requestedTrackTitle_;
    model.scanning = scanning_;
    model.scanProcessed = progress.processed;
    model.scanTotal = progress.discovered;
    model.scanFile = progress.currentFile;
    model.artworkFetching = artworkFetching_;
    model.artworkProcessed = artworkProgress.processed;
    model.artworkTotal = artworkProgress.discovered;
    model.artworkFound = artworkProgress.found;
    model.artworkCurrent = artworkProgress.current;
    model.toast = toast_;
    model.page = page_;
    model.adminQueueSelection = adminQueueSelection_;
    model.ambientMode = settings_.ambientMode;
    model.ambientRepeat = settings_.ambientRepeat;
    model.musicSourceCount = settings_.musicRoots.size();
    model.videoSourceCount = settings_.videoRoots.size();
    ui_.render(model);
    if (mode_ == UiMode::Browse && currentIsVideo() && !visualizerOpen_) {
        video_.render(videoFullscreen_ ? SDL_FRect{0, 0, 1920, 1080}
                                      : SDL_FRect{65, 178, 340, 340});
    } else {
        video_.hide();
    }
}

void App::updateFilter(bool resetPage) {
    std::string selectedId = selectedTrack() ? selectedTrack()->id : std::string{};
    const std::size_t previousPage = page_;
    filtered_ = LibraryScanner::filter(library_, search_, libraryFilter_, selectedGenre_);
    if (resetPage || filtered_.empty()) {
        page_ = 0;
    } else {
        constexpr std::size_t pageSize = 9;
        const std::size_t lastPage = (filtered_.size() - 1) / pageSize;
        page_ = std::min(previousPage, lastPage);
    }
    selectedIndex_.reset();
    if (!selectedId.empty()) {
        for (std::size_t i = 0; i < library_.tracks.size(); ++i) {
            if (library_.tracks[i].id == selectedId) { selectedIndex_ = i; break; }
        }
    }
}

void App::rebuildGenres() {
    genres_ = LibraryScanner::genres(library_);
    if (!selectedGenre_.empty()) {
        const auto selectedKey = normalizeForSearch(selectedGenre_);
        const auto found = std::ranges::find_if(genres_, [&](const std::string& genre) {
            return normalizeForSearch(genre) == selectedKey;
        });
        if (found == genres_.end()) selectedGenre_.clear();
        else selectedGenre_ = *found;
    }
    genreMenuPage_ = 0;
}

void App::processScanTrackUpdates() {
    std::vector<Track> updates;
    {
        std::scoped_lock lock(scanTrackMutex_);
        updates.swap(scanTrackUpdates_);
    }
    if (updates.empty()) return;

    bool metadataChanged = false;
    std::unordered_map<std::string, std::size_t> indexById;
    indexById.reserve(library_.tracks.size());
    for (std::size_t index = 0; index < library_.tracks.size(); ++index) {
        indexById.emplace(library_.tracks[index].id, index);
    }
    for (auto& update : updates) {
        const auto position = indexById.find(update.id);
        if (position == indexById.end()) continue;
        auto& found = library_.tracks[position->second];

        preserveEnrichedMetadata(update, found);
        metadataChanged = metadataChanged || found.title != update.title ||
            found.artist != update.artist || found.album != update.album ||
            found.genre != update.genre || found.albumYear != update.albumYear;
        found = std::move(update);
        scanCacheDirty_ = true;
    }
    if (!metadataChanged) return;

    const auto oldPage = genreMenuPage_;
    rebuildGenres();
    constexpr std::size_t genrePageSize = 9;
    const auto genrePages = std::max<std::size_t>(1, (genres_.size() + 1 + genrePageSize - 1) /
                                                     genrePageSize);
    genreMenuPage_ = std::min(oldPage, genrePages - 1);
    if (!selectedGenre_.empty() || !search_.empty()) updateFilter(false);
}

void App::startScan(std::vector<std::filesystem::path> musicRoots,
                    std::vector<std::filesystem::path> videoRoots) {
    musicRoots = uniqueRoots(musicRoots);
    videoRoots = uniqueRoots(videoRoots);
    if ((musicRoots.empty() && videoRoots.empty()) || scanning_) return;
    if (artworkFetching_) {
        artworkCancel_.store(true, std::memory_order_release);
        artworkRestartPending_ = true;
    }
    scanCancel_.store(false, std::memory_order_release);
    scanning_ = true;
    { std::scoped_lock lock(scanProgressMutex_); scanProgress_ = {}; }
    { std::scoped_lock lock(scanTrackMutex_); scanTrackUpdates_.clear(); }
    std::vector<std::string> favorites;
    for (const auto& track : library_.tracks) if (track.favorite) favorites.push_back(track.id);
    const LibraryIndex cached = library_;
    scanFuture_ = std::async(std::launch::async,
                             [this, musicRoots = std::move(musicRoots),
                              videoRoots = std::move(videoRoots), cached,
                              favorites = std::move(favorites)] {
        LibraryScanner scanner;
        return scanner.scan(musicRoots, videoRoots, cached, favorites,
                            [this](const ScanProgress& progress) {
            std::scoped_lock lock(scanProgressMutex_); scanProgress_ = progress;
        }, &scanCancel_, [this](const std::filesystem::path& path, std::string_view message) {
            if (!storage_) return;
            std::ofstream log(storage_->root() / L"jukebox.log", std::ios::binary | std::ios::app);
            log << nowUnixMs() << "  skipped  " << pathToUtf8(path) << "  " << message << '\n';
        }, [this](const Track& track) {
            std::scoped_lock lock(scanTrackMutex_);
            scanTrackUpdates_.push_back(track);
        });
    });
}

void App::processScan() {
    processScanTrackUpdates();
    if (!scanning_ || !scanFuture_.valid() ||
        scanFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        const std::string selectedId = selectedTrack() ? selectedTrack()->id : std::string{};
        auto scannedLibrary = scanFuture_.get();
        std::unordered_map<std::string, const Track*> currentById;
        currentById.reserve(library_.tracks.size());
        for (const auto& track : library_.tracks) currentById.emplace(track.id, &track);
        for (auto& track : scannedLibrary.tracks) {
            const auto current = currentById.find(track.id);
            if (current != currentById.end()) preserveEnrichedMetadata(track, *current->second);
        }
        library_ = std::move(scannedLibrary);
        ambientSelector_.reset();
        rebuildGenres();
        selectedIndex_.reset();
        if (!selectedId.empty()) {
            for (std::size_t i = 0; i < library_.tracks.size(); ++i)
                if (library_.tracks[i].id == selectedId) { selectedIndex_ = i; break; }
        }
        storage_->saveLibrary(library_);
        scanCacheDirty_ = false;
        updateFilter(false);
        setToast("Library ready: " + std::to_string(library_.tracks.size()) + " tracks");
        startArtworkFetch();
        if (currentTrackId_.empty()) startNextTrack();
    } catch (const std::exception& exception) {
        setToast(std::string("Library scan failed: ") + exception.what(), 6000);
    }
    scanning_ = false;
}

void App::startArtworkFetch() {
    if (!storage_ || library_.tracks.empty()) return;
    if (artworkFetching_) {
        artworkRestartPending_ = true;
        return;
    }

    artworkCancel_.store(false, std::memory_order_release);
    artworkFetching_ = true;
    artworkRestartPending_ = false;
    {
        std::scoped_lock lock(artworkMutex_);
        artworkProgress_ = {};
        artworkMatches_.clear();
    }

    const auto tracks = library_.tracks;
    const auto cacheRoot = storage_->root() / L"artwork";
    artworkFuture_ = std::async(std::launch::async, [this, tracks, cacheRoot] {
        OnlineArtworkFetcher fetcher;
        fetcher.run(tracks, cacheRoot,
                    [this](const OnlineArtworkProgress& progress) {
                        std::scoped_lock lock(artworkMutex_);
                        artworkProgress_ = progress;
                    },
                    [this](OnlineArtworkMatch match) {
                        std::scoped_lock lock(artworkMutex_);
                        artworkMatches_.push_back(std::move(match));
                    },
                    &artworkCancel_);
    });
}

void App::processArtworkFetch() {
    std::vector<OnlineArtworkMatch> matches;
    {
        std::scoped_lock lock(artworkMutex_);
        matches.swap(artworkMatches_);
    }

    bool libraryChanged = false;
    bool metadataChanged = false;
    for (const auto& match : matches) {
        for (const auto& trackId : match.trackIds) {
            auto found = std::find_if(library_.tracks.begin(), library_.tracks.end(),
                                      [&trackId](const Track& track) { return track.id == trackId; });
            if (found == library_.tracks.end()) continue;

            if (!match.imagePath.empty() && !found->hasEmbeddedArtwork && !found->sidecarArtwork &&
                found->onlineArtwork != match.imagePath) {
                found->onlineArtwork = match.imagePath;
                libraryChanged = true;
            }
            if ((found->artist.empty() || found->artist == "Unknown Artist") && !match.artist.empty()) {
                found->artist = match.artist;
                libraryChanged = metadataChanged = true;
            }
            if ((found->album.empty() || found->album == "Unknown Album") && !match.album.empty()) {
                found->album = match.album;
                libraryChanged = metadataChanged = true;
            }
            if ((found->genre.empty() || found->genre == "Unknown Genre") && !match.genre.empty()) {
                found->genre = match.genre;
                libraryChanged = metadataChanged = true;
            }
            if (found->albumYear <= 0 && match.albumYear > 0) {
                found->albumYear = match.albumYear;
                libraryChanged = metadataChanged = true;
            }
        }
    }
    if (metadataChanged) {
        const auto oldPage = genreMenuPage_;
        rebuildGenres();
        constexpr std::size_t genrePageSize = 9;
        const auto genrePages = std::max<std::size_t>(1, (genres_.size() + 1 + genrePageSize - 1) /
                                                         genrePageSize);
        genreMenuPage_ = std::min(oldPage, genrePages - 1);
        if (!selectedGenre_.empty() || !search_.empty()) updateFilter(false);
    }
    if (libraryChanged && storage_) storage_->saveLibrary(library_);

    if (!artworkFetching_) {
        if (artworkRestartPending_ && !scanning_) startArtworkFetch();
        return;
    }
    if (!artworkFuture_.valid() ||
        artworkFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        artworkFuture_.get();
    } catch (const std::exception& exception) {
        setToast(std::string("Artwork update failed: ") + exception.what(), 6000);
    }
    artworkFetching_ = false;

    OnlineArtworkProgress progress;
    { std::scoped_lock lock(artworkMutex_); progress = artworkProgress_; }
    if (!artworkCancel_.load(std::memory_order_acquire) && progress.discovered > 0) {
        setToast("Artwork complete: " + std::to_string(progress.found) + " found", 5000);
    }
    if (artworkRestartPending_ && !scanning_) {
        startArtworkFetch();
    }
}

void App::showFolderDialog(MediaKind mediaKind, bool setup) {
    folderForSetup_ = setup;
    folderMediaKind_ = mediaKind;
    const auto& roots = mediaKind == MediaKind::Music ? settings_.musicRoots : settings_.videoRoots;
    const std::optional<std::string> defaultPath = roots.empty()
        ? std::nullopt : std::optional<std::string>(pathToUtf8(roots.front()));
    SDL_ShowOpenFolderDialog(&App::folderCallback, this, window_,
                             defaultPath ? defaultPath->c_str() : nullptr, true);
}

void App::processFolderResult() {
    std::vector<std::filesystem::path> selectedFolders;
    {
        std::scoped_lock lock(folderMutex_);
        if (!folderResultReady_) return;
        selectedFolders = std::move(pendingFolders_);
        pendingFolders_.clear();
        folderResultReady_ = false;
    }
    selectedFolders = uniqueRoots(selectedFolders);
    if (selectedFolders.empty()) return;

    auto& targetRoots = folderMediaKind_ == MediaKind::Music
        ? settings_.musicRoots : settings_.videoRoots;
    const auto oldRoots = uniqueRoots(targetRoots);
    auto merged = oldRoots;
    merged.insert(merged.end(), selectedFolders.begin(), selectedFolders.end());
    merged = uniqueRoots(merged);
    const auto added = merged.size() - oldRoots.size();
    targetRoots = std::move(merged);
    library_.musicRoots = settings_.musicRoots;
    library_.videoRoots = settings_.videoRoots;
    storage_->saveSettings(settings_);
    if (added == 0) {
        setToast("Those media sources are already in the library");
        return;
    }
    const std::string type = folderMediaKind_ == MediaKind::Music ? "music" : "video";
    if (folderForSetup_) {
        setToast(std::to_string(added) + " " + type + " source(s) selected");
        return;
    }
    setToast(std::to_string(added) + " " + type + " source(s) added. Scanning...");
    startScan(settings_.musicRoots, settings_.videoRoots);
}

void App::recoverAudio() {
    std::string error;
    if (!audio_.initialize(error)) {
        audioRecoveryAt_ = SDL_GetTicks() + 2000;
        return;
    }
    audio_.setVolume(settings_.volume);
    audioRecoveryPending_ = false;
    if (const auto* track = currentTrack()) {
        if (track->mediaKind == MediaKind::Video) {
            setToast("Audio device ready");
            return;
        }
        if (!playTrack(*track, recoveryPositionMs_, error)) {
            currentTrackId_.clear();
            startNextTrack();
        } else if (recoveryPaused_) {
            audio_.pause();
        }
    } else {
        currentTrackId_.clear();
        startNextTrack();
    }
    setToast("Audio device ready");
}

bool App::playTrack(const Track& track, std::int64_t startMs, std::string& error) {
    videoFullscreen_ = false;
    if (track.mediaKind == MediaKind::Video) {
        audio_.stop();
        video_.setVolume(settings_.volume);
        return video_.play(track, startMs, error);
    }
    video_.stop();
    if (!audio_.initialized() && !audio_.initialize(error)) return false;
    audio_.setVolume(settings_.volume);
    return audio_.play(track, startMs, error);
}

void App::stopPlayback() {
    audio_.stop();
    video_.stop();
    videoFullscreen_ = false;
}

bool App::currentIsVideo() const {
    const auto* track = currentTrack();
    return track && track->mediaKind == MediaKind::Video;
}

void App::startNextTrack() {
    if (!currentTrackId_.empty()) return;
    std::size_t attempts = queue_.size() + library_.tracks.size() + 1;
    while (attempts-- > 0) {
        const Track* next{};
        currentIsManual_ = false;
        if (const auto queued = queue_.popFront()) {
            next = LibraryScanner::find(library_, queued->trackId);
            currentIsManual_ = true;
            storage_->saveQueue(queue_.snapshot());
        } else if (const auto index = ambientSelector_.next(
                       AmbientMode::Shuffle, true, library_.tracks.size(), random_)) {
            next = &library_.tracks[*index];
        } else break;
        if (!next || !std::filesystem::exists(next->path)) continue;
        std::string error;
        if (playTrack(*next, 0, error)) {
            currentTrackId_ = next->id;
            persistPlayback();
            return;
        }
        setToast("Skipped unreadable media: " + next->title, 4500);
    }
    persistPlayback();
}

void App::skipCurrent() {
    stopPlayback();
    currentTrackId_.clear();
    currentIsManual_ = false;
    startNextTrack();
}

void App::persistPlayback() {
    if (!storage_) return;
    const bool video = currentIsVideo();
    settings_.volume = video ? video_.volume() : audio_.volume();
    settings_.currentTrackId = currentTrackId_;
    settings_.playbackPositionMs = video ? video_.positionMs() :
        audioRecoveryPending_ ? recoveryPositionMs_ : audio_.positionMs();
    settings_.playbackWasActive = !currentTrackId_.empty() &&
        (video ? video_.playing() && !video_.paused() :
         audioRecoveryPending_ ? !recoveryPaused_ : audio_.playing() && !audio_.paused());
    settings_.currentTrackManual = currentIsManual_;
    storage_->saveSettings(settings_);
}

void App::setToast(std::string message, std::uint64_t durationMs) {
    toast_ = std::move(message);
    toastUntil_ = SDL_GetTicks() + durationMs;
}

const Track* App::selectedTrack() const {
    return selectedIndex_ && *selectedIndex_ < library_.tracks.size() ? &library_.tracks[*selectedIndex_] : nullptr;
}

const Track* App::currentTrack() const { return LibraryScanner::find(library_, currentTrackId_); }

PlaybackSnapshot App::playbackSnapshot() const {
    PlaybackSnapshot snapshot;
    snapshot.trackId = currentTrackId_;
    const bool video = currentIsVideo();
    snapshot.positionMs = video ? video_.positionMs() : audio_.positionMs();
    snapshot.durationMs = video ? video_.durationMs() : audio_.durationMs();
    snapshot.volume = video ? video_.volume() : audio_.volume();
    snapshot.state = currentTrackId_.empty() ? PlaybackState::Stopped :
                     (video ? video_.paused() : audio_.paused())
                         ? PlaybackState::Paused : PlaybackState::Playing;
    return snapshot;
}

void SDLCALL App::folderCallback(void* userdata, const char* const* filelist, int) {
    auto* app = static_cast<App*>(userdata);
    std::scoped_lock lock(app->folderMutex_);
    app->pendingFolders_.clear();
    if (filelist) {
        for (std::size_t i = 0; filelist[i]; ++i) {
            app->pendingFolders_.push_back(pathFromUtf8(filelist[i]));
        }
    }
    app->folderResultReady_ = true;
}

}  // namespace neon
