#include "neon/UI.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <sstream>

#include "neon/Drawing.hpp"
#include "neon/Library.hpp"
#include "neon/Utils.hpp"

namespace neon {
namespace {

constexpr SDL_Color background{5, 7, 18, 255};
constexpr SDL_Color panelFill{13, 17, 36, 242};
constexpr SDL_Color panelBorder{41, 64, 94, 255};
constexpr SDL_Color cyan{0, 239, 255, 255};
constexpr SDL_Color pink{255, 44, 190, 255};
constexpr SDL_Color white{238, 246, 255, 255};
constexpr SDL_Color muted{138, 153, 180, 255};
constexpr SDL_Color danger{255, 77, 100, 255};
constexpr std::size_t pageSize = 9;
constexpr float uiPi = 3.14159265358979323846F;

void fillCircle(SDL_Renderer* renderer, float centerX, float centerY, float radius,
                SDL_Color color) {
    drawing::disc(renderer, centerX, centerY, radius, color);
}

void antialiasedArc(SDL_Renderer* renderer, float centerX, float centerY, float radius,
                    float startAngle, float endAngle, SDL_Color color, int segments) {
    drawing::arc(renderer, centerX, centerY, radius, startAngle, endAngle, color, 0, segments);
}

void circle(SDL_Renderer* renderer, float centerX, float centerY, float radius,
            SDL_Color color) {
    antialiasedArc(renderer, centerX, centerY, radius, 0.0F, uiPi * 2.0F,
                   color, 192);
}

void arc(SDL_Renderer* renderer, float centerX, float centerY, float radius,
         float startAngle, float endAngle, SDL_Color color) {
    antialiasedArc(renderer, centerX, centerY, radius, startAngle, endAngle,
                   color, 72);
}

}  // namespace

UI::~UI() { shutdown(); }

void UI::cancelBackgroundWork() {
    artwork_.requestStop();
    videoArtwork_.requestStop();
}

void UI::shutdown() {
    cancelBackgroundWork();
    resetRetroCatalogue();
    artwork_.clear();
    videoArtwork_.clear();
    if (videoInfoImage_.pixels) SDL_DestroySurface(videoInfoImage_.pixels);
    videoInfoImage_.pixels = nullptr;
    videoInfoKey_.clear();
    for (auto& [_, entry] : textCache_) if (entry.texture) SDL_DestroyTexture(entry.texture);
    for (auto& [_, value] : regularFonts_) if (value) TTF_CloseFont(value);
    for (auto& [_, value] : boldFonts_) if (value) TTF_CloseFont(value);
    for (auto& [_, value] : meterFonts_) if (value) TTF_CloseFont(value);
    for (auto& [_, value] : retroFonts_) if (value) TTF_CloseFont(value);
    for (auto& [_, value] : retroRegularFonts_) if (value) TTF_CloseFont(value);
    for (auto& [_, value] : videoInfoFonts_) if (value) TTF_CloseFont(value);
    textCache_.clear();
    regularFonts_.clear();
    boldFonts_.clear();
    meterFonts_.clear();
    retroFonts_.clear();
    retroRegularFonts_.clear();
    videoInfoFonts_.clear();
    hits_.clear();
    renderer_ = nullptr;
}

bool UI::initialize(SDL_Renderer* renderer, std::string& error) {
    renderer_ = renderer;
    const std::filesystem::path fontDirectory = std::filesystem::path(SDL_GetBasePath()) /
                                                L"assets" / L"fonts";
    const std::filesystem::path regular = fontDirectory / L"NotoSans-Regular.ttf";
    const std::filesystem::path bold = fontDirectory / L"NotoSans-Bold.ttf";
    const std::filesystem::path meter = fontDirectory / L"NotoSansMono-Regular.ttf";
    regularFontPath_ = std::filesystem::exists(regular)
        ? pathToUtf8(regular) : "C:/Windows/Fonts/segoeui.ttf";
    boldFontPath_ = std::filesystem::exists(bold)
        ? pathToUtf8(bold) : "C:/Windows/Fonts/segoeuib.ttf";
    meterFontPath_ = std::filesystem::exists(meter)
        ? pathToUtf8(meter) : "C:/Windows/Fonts/consola.ttf";
    videoInfoFontPath_ = std::filesystem::exists("C:/Windows/Fonts/ariblk.ttf")
        ? "C:/Windows/Fonts/ariblk.ttf" : boldFontPath_;
    if (!std::filesystem::exists(pathFromUtf8(meterFontPath_))) meterFontPath_ = regularFontPath_;
    retroFontPath_ = std::filesystem::exists("C:/Windows/Fonts/arialnb.ttf")
        ? "C:/Windows/Fonts/arialnb.ttf" :
        std::filesystem::exists("C:/Windows/Fonts/bahnschrift.ttf")
        ? "C:/Windows/Fonts/bahnschrift.ttf" : boldFontPath_;
    retroRegularFontPath_ = std::filesystem::exists("C:/Windows/Fonts/arialn.ttf")
        ? "C:/Windows/Fonts/arialn.ttf" : regularFontPath_;

    refreshRenderMetrics();
    if (!font(24)) {
        error = SDL_GetError();
        return false;
    }
    visualizer_.setTextRenderer(
        [this](std::string_view value, float centerX, float top, float maxWidth,
               float maxHeight, SDL_Color color) {
            const int size = std::max(5, static_cast<int>(std::ceil(maxHeight * 1.25F)));
            text(value, centerX, top, size, color, maxWidth, true, maxHeight, true);
        });
    return true;
}

void UI::render(const UiModel& model, std::uint64_t ticks) {
    if (theme_ != model.theme) {
        retroStatusText_.clear();
        retroPreviousStatusText_.clear();
        retroStatusCycleStart_.reset();
        retroCoinPrompt_ = false;
        feedbackAction_.reset();
    }
    theme_ = model.theme;
    const auto feedbackDelta = ticks >= feedbackLastTick_ ? ticks - feedbackLastTick_ : 0;
    feedbackElapsed_ = std::min<std::uint64_t>(120, feedbackElapsed_ + std::min<std::uint64_t>(feedbackDelta, 50));
    feedbackLastTick_ = ticks;
    if (feedbackElapsed_ >= 120) feedbackAction_.reset();
    refreshRenderMetrics();
    hits_.clear();
    pageArtworkPending_ = model.pendingPage.has_value();
    prepareArtwork(model);
    artwork_.update(renderer_);
    videoArtwork_.update(renderer_);
    if (model.theme != Theme::Retro || model.mode != UiMode::Browse ||
        model.keyboardOpen || model.genreMenuOpen || model.playNowPrompt ||
        model.visualizerOpen || model.videoFullscreen) resetRetroCatalogue();
    visualizer_.update(model.visualization, ticks);
    if (lastDiscTicks_ == 0) lastDiscTicks_ = ticks;
    const auto discElapsed = std::min<std::uint64_t>(ticks - lastDiscTicks_, 100);
    if (model.nowPlayingArtworkMode == NowPlayingArtworkMode::SpinningDisc &&
        model.currentTrack && !model.videoPlaying &&
        model.playback.state == PlaybackState::Playing) {
        discRotationDegrees_ = std::fmod(
            discRotationDegrees_ + static_cast<float>(discElapsed) * 0.012F, 360.0F);
    }
    lastDiscTicks_ = ticks;
    SDL_SetRenderDrawColor(renderer_, background.r, background.g, background.b, background.a);
    SDL_RenderClear(renderer_);

    // Subtle retro grid.
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 14, 30, 49, 90);
    for (int x = 0; x <= 1920; x += 80) drawing::line(renderer_, static_cast<float>(x), 0, static_cast<float>(x), 1080, {14, 30, 49, 90});
    for (int y = 0; y <= 1080; y += 80) drawing::line(renderer_, 0, static_cast<float>(y), 1920, static_cast<float>(y), {14, 30, 49, 90});

    if (model.videoFullscreen) {
        drawVideoFullscreen();
        pruneTextCache();
        SDL_RenderPresent(renderer_);
        return;
    }

    if (model.mode == UiMode::SetupPin) drawSetupPin(model);
    else if (model.mode == UiMode::SetupFolder) drawSetupFolder(model);
    else {
        if (model.theme == Theme::Retro) drawRetroBrowse(model, ticks);
        else drawBrowse(model);
        if (model.mode == UiMode::Browse && model.genreMenuOpen) drawGenreMenu(model);
        if (model.mode == UiMode::AdminPin || model.mode == UiMode::ChangePin) drawPinPad(model, true);
        if (model.mode == UiMode::Admin) drawAdmin(model);
        if (model.visualizerOpen) drawVisualizerOverlay(model);
        if (model.keyboardOpen) drawKeyboard(model);
        if (model.playNowPrompt) drawPlayNowPrompt(model);
    }

    pruneTextCache();
    SDL_RenderPresent(renderer_);
}

std::optional<UiAction> UI::hitTest(float x, float y, std::uint64_t timestampNs) const {
    for (auto found = hits_.rbegin(); found != hits_.rend(); ++found) {
        if (contains(found->rect, x, y)) {
            if ((pageArtworkPending_ || retroTurnDirection_ || (timestampNs && timestampNs <= retroInputBlockedUntilNs_)) &&
                (found->action.kind == UiActionKind::SelectTrack ||
                found->action.kind == UiActionKind::PagePrevious ||
                found->action.kind == UiActionKind::PageNext)) return std::nullopt;
            return found->action;
        }
    }
    return std::nullopt;
}

bool UI::pageArtworkReady(const LibraryIndex& library, const std::vector<std::size_t>& filtered,
                          Theme theme, std::size_t page, LibraryFilter filter) const {
    const auto size = themePageSize(theme, filter == LibraryFilter::Video);
    const auto& cache = theme == Theme::Retro && filter == LibraryFilter::Video ? videoArtwork_ : artwork_;
    if (filtered.empty() || page > (filtered.size() - 1) / size) return false;
    for (auto slot = page * size; slot < std::min((page + 1) * size, filtered.size()); ++slot) {
        if (filtered[slot] >= library.tracks.size() || !cache.ready(library.tracks[filtered[slot]])) return false;
    }
    return true;
}

void UI::prepareArtwork(const UiModel& model) {
    std::vector<const Track*> ordered;
    ordered.reserve(122);
    ordered.push_back(model.currentTrack);
    const auto artworkPageSize = themePageSize(model.theme, model.libraryFilter == LibraryFilter::Video);
    const auto appendPage = [&](std::size_t page) {
        if (!model.library || !model.filtered || model.filtered->empty() ||
            page > (model.filtered->size() - 1) / artworkPageSize) return;
        for (auto slot = page * artworkPageSize; slot < std::min((page + 1) * artworkPageSize, model.filtered->size()); ++slot) {
            const auto index = (*model.filtered)[slot];
            if (index < model.library->tracks.size()) ordered.push_back(&model.library->tracks[index]);
        }
    };
    if (model.pendingPage) appendPage(*model.pendingPage);
    appendPage(model.page);
    ordered.push_back(model.selectedTrack);
    // Keep two pages on either side plus an extra page in the travel direction.
    // This also gives slow network/disk sources time to finish before a click.
    for (std::size_t distance = 1; distance <= 2; ++distance) {
        appendPage(model.page + distance);
        if (model.page >= distance) appendPage(model.page - distance);
    }
    const bool backwards = model.page < artworkLastPage_;
    if (model.page != artworkLastPage_) artworkBrowseBackwards_ = backwards;
    artworkLastPage_ = model.page;
    if (artworkBrowseBackwards_) {
        if (model.page >= 3) appendPage(model.page - 3);
    } else appendPage(model.page + 3);
    if (model.theme == Theme::Retro && model.libraryFilter == LibraryFilter::Video) {
        std::erase_if(ordered, [](const Track* track) { return !track || track->mediaKind != MediaKind::Video; });
        videoArtwork_.prepare(ordered);
        const std::array<const Track*, 2> console{model.currentTrack, model.selectedTrack};
        artwork_.prepare(console);
    } else {
        artwork_.prepare(ordered);
        videoArtwork_.prepare({});
    }
}

void UI::refreshArtwork(const Track& track) {
    artwork_.invalidate(track);
    videoArtwork_.invalidate(track);
}

void UI::useLibraryArtworkCache(const std::filesystem::path& libraryRoot) {
    artwork_.useDiskCache(libraryRoot);
    videoArtwork_.useDiskCache(libraryRoot);
}

void UI::updatePointer(float x, float y, bool down) {
    pointerX_ = x;
    pointerY_ = y;
    if (down && !pointerDown_) pressedAction_ = hitTest(x, y);
    if (!down) pressedAction_.reset();
    pointerDown_ = down;
}

void UI::clearPointer() {
    pointerX_ = pointerY_ = -1;
    pointerDown_ = false;
    pressedAction_.reset();
    feedbackAction_.reset();
}

void UI::notifyAction(const UiAction& action) {
    if (theme_ != Theme::Retro || action.kind == UiActionKind::None) return;
    feedbackAction_ = action;
    feedbackElapsed_ = 0;
}

std::string UI::radioSubtitle(const Track& track) {
    std::string subtitle;
    const auto title = normalizeForSearch(track.title);
    std::vector<std::string> seen{title};
    for (const auto* value : {&track.artist, &track.genre, &track.album}) {
        const auto normalized = normalizeForSearch(*value);
        if (normalized.empty() || normalized == "unknown artist" ||
            normalized == "unknown genre" || normalized == "unknown album" ||
            std::find(seen.begin(), seen.end(), normalized) != seen.end()) continue;
        if (!subtitle.empty()) subtitle += " · ";
        subtitle += *value;
        seen.push_back(normalized);
    }
    return subtitle.empty() ? "RADIO STATION" : subtitle;
}

void UI::drawRadioNotice(const UiModel& model, const SDL_FRect& rect) {
    const bool retro = model.theme == Theme::Retro;
    const bool filtered = !model.search.empty() || !model.selectedGenre.empty() || model.selectedArtistInitial;
    const std::string heading = model.radioFetching ? "LOADING RADIO STATIONS" :
        filtered ? "NO MATCHING STATIONS" : "NO RADIO STATIONS AVAILABLE";
    const std::string detail = !model.radioStatus.empty() ? model.radioStatus :
        model.radioFetching ? "Finding live stations. Please wait..." :
        filtered ? "Try ALL, a different search or another genre." :
        "Check your connection and select RADYO to try again.";
    panel(rect, retro ? SDL_Color{247, 245, 229, 255} : panelFill,
          retro ? SDL_Color{157, 49, 41, 255} : cyan);
    text(heading, rect.x + rect.w / 2, rect.y + 25, retro ? 34 : 28,
         retro ? SDL_Color{32, 38, 31, 255} : cyan, rect.w - 48, true, 42);
    text(detail, rect.x + rect.w / 2, rect.y + 84, retro ? 23 : 20,
         retro ? SDL_Color{92, 98, 87, 255} : muted, rect.w - 48, true, 36);
}

void UI::drawBrowse(const UiModel& model) {
    const bool radio = model.libraryFilter == LibraryFilter::Radio;
    const bool currentRadio = model.currentTrack && model.currentTrack->mediaKind == MediaKind::Radio;
    const bool selectedRadio = model.selectedTrack && model.selectedTrack->mediaKind == MediaKind::Radio;
    const bool radioConnecting = !model.radioLoadingStatus.empty();
    const bool browseMode = model.mode == UiMode::Browse;
    const bool genreButtonEnabled = browseMode && model.credits > 0 &&
                                !model.keyboardOpen && !model.playNowPrompt && !model.visualizerOpen;
    const bool visitorEnabled = genreButtonEnabled && !model.genreMenuOpen;
    const bool coinEnabled = browseMode && !model.keyboardOpen && !model.playNowPrompt &&
                             !model.visualizerOpen && !model.genreMenuOpen;
    // Two navigation rows span the cabinet, as in Retro, with Neon styling.
    const SDL_FRect search{30, 22, 690, 62};
    button(search, model.search.empty() ? (radio ? "SEARCH RADIO STATIONS" : "SEARCH TITLE / ARTIST / ALBUM") : ellipsize(model.search, 48),
           {UiActionKind::OpenKeyboard}, visitorEnabled, cyan);
    const std::string genreCaption = model.selectedGenre.empty()
        ? "ALL GENRES  ▼" : ellipsize(model.selectedGenre, 34) + "  ▼";
    button({732, 22, 450, 62}, genreCaption, {UiActionKind::ToggleGenreMenu},
           genreButtonEnabled,
           model.genreMenuOpen ? pink
                               : model.libraryFilter == LibraryFilter::All ? cyan : muted);
    button({1194, 22, 224, 62}, "MUSIC", {UiActionKind::ShowMusic}, visitorEnabled,
           model.libraryFilter == LibraryFilter::Music ? cyan : muted);
    button({1430, 22, 224, 62}, "VIDEO", {UiActionKind::ShowVideo}, visitorEnabled,
           model.libraryFilter == LibraryFilter::Video ? pink : muted);
    button({1666, 22, 224, 62}, "RADYO", {UiActionKind::ShowRadio}, visitorEnabled,
           radio ? cyan : muted);
    button({30, 96, 68, 48}, "ALL", {UiActionKind::SelectArtistInitial}, visitorEnabled,
           model.selectedArtistInitial == '\0' ? cyan : panelBorder);
    button({106, 96, 68, 48}, "0–9", {UiActionKind::SelectArtistInitial, 0, '#'}, visitorEnabled,
           model.selectedArtistInitial == '#' ? cyan : panelBorder);
    for (int i = 0; i < 26; ++i) {
        const char initial = static_cast<char>('A' + i);
        button({182 + i * 66.0F, 96, 58, 48}, std::string(1, initial),
               {UiActionKind::SelectArtistInitial, 0, initial}, visitorEnabled,
               model.selectedArtistInitial == initial ? cyan : panelBorder);
    }

    panel({30, 160, 410, 870}, panelFill, {38, 50, 82, 255});
    text("NOW PLAYING", 58, 182, 19, cyan);
    const SDL_FRect nowPlayingMedia = nowPlayingMediaRect(model.theme, model.videoPlaying);
    if (model.videoPlaying) {
        panel(nowPlayingMedia, {0, 0, 0, 255}, panelBorder);
        text("VIDEO · TAP TO EXPAND", 300, 184, 14, pink, 200, true);
        if (browseMode && !model.keyboardOpen && !model.playNowPrompt && !model.visualizerOpen)
            addHit(nowPlayingMedia, {UiActionKind::ToggleVideoFullscreen});
    } else {
        if (model.nowPlayingArtworkMode == NowPlayingArtworkMode::SpinningDisc) {
            drawSpinningDisc(model.currentTrack, nowPlayingMedia);
        } else {
            drawCover(model.currentTrack, nowPlayingMedia);
        }
    }
    text(model.currentTrack ? ellipsize(model.currentTrack->title, 30) : "Waiting for a request",
         235, 552, 30, white, 350, true);
    text(model.currentTrack ? ellipsize(currentRadio ? radioSubtitle(*model.currentTrack) : model.currentTrack->artist, 34) : "Queue a track to begin",
         235, 594, 21, muted, 350, true);
    std::string currentMetadata;
    if (model.currentTrack && model.currentTrack->genre != "Unknown Genre") {
        currentMetadata = model.currentTrack->genre;
    }
    if (model.currentTrack && model.currentTrack->albumYear > 0) {
        if (!currentMetadata.empty()) currentMetadata += " · ";
        currentMetadata += std::to_string(model.currentTrack->albumYear);
    }
    if (currentRadio) currentMetadata.clear();
    if (!model.videoLoadingStatus.empty()) currentMetadata = model.videoLoadingStatus;
    if (radioConnecting) currentMetadata = model.radioLoadingStatus;
    text(ellipsize(currentMetadata, 36), 235, 620, 15, muted, 350, true);
    const float progress = !currentRadio && !radioConnecting && model.playback.durationMs > 0
        ? std::clamp(static_cast<float>(model.playback.positionMs) / static_cast<float>(model.playback.durationMs), 0.0F, 1.0F) : 0.0F;
    // The progress and visualizer block starts one former credit-row lower so
    // artist/genre/year text always has clear space above the progress bar.
    SDL_FRect progressBack{65, 658, 340, 8};
    SDL_FRect progressFront{65, 658, 340 * progress, 8};
    SDL_SetRenderDrawColor(renderer_, 35, 43, 68, 255); SDL_RenderFillRect(renderer_, &progressBack);
    SDL_SetRenderDrawColor(renderer_, pink.r, pink.g, pink.b, 255); SDL_RenderFillRect(renderer_, &progressFront);
    if (currentRadio || radioConnecting) {
        fillCircle(renderer_, 77, 687, 5, radioConnecting ? pink : cyan);
        text(radioConnecting ? "CONNECTING" : "CANLI · LIVE", 94, 675, 17, cyan, 310);
    } else {
        text(formatDuration(model.playback.positionMs), 65, 675, 17, muted);
        text(formatDuration(model.playback.durationMs), 405, 675, 17, muted, 0, true);
    }
    const SDL_FRect compactVisualizer{65, 716, 340, 120};
    drawVisualizer(compactVisualizer, model.visualizerMode);
    if (browseMode && !model.keyboardOpen && !model.playNowPrompt && !model.visualizerOpen) {
        addHit(compactVisualizer, {UiActionKind::OpenVisualizer});
    }

    text("SELECTED", 58, 842, 17, pink);
    text(model.selectedTrack ? ellipsize(model.selectedTrack->title, 30) : "Tap a track",
         58, 870, 23, white, 345);
    text(model.selectedTrack ? ellipsize(selectedRadio ? radioSubtitle(*model.selectedTrack) : model.selectedTrack->artist, 34) : "",
         58, 900, 17, muted, 345);
    std::string selectedMetadata;
    if (model.selectedTrack && model.selectedTrack->genre != "Unknown Genre") {
        selectedMetadata = model.selectedTrack->genre;
    }
    if (model.selectedTrack && model.selectedTrack->albumYear > 0) {
        if (!selectedMetadata.empty()) selectedMetadata += " · ";
        selectedMetadata += std::to_string(model.selectedTrack->albumYear);
    }
    text(selectedRadio ? "CANLI · LIVE" : ellipsize(selectedMetadata, 38), 58, 924, 14, selectedRadio ? cyan : muted, 345);
    const SDL_FRect coinButton{58, 946, 170, 64};
    panel(coinButton,
          coinEnabled ? SDL_Color{22, 29, 58, 255} : SDL_Color{13, 17, 31, 230},
          coinEnabled ? cyan : panelBorder);
    text("INSERT COIN", coinButton.x + coinButton.w * 0.5F, coinButton.y + 7.0F,
         17, coinEnabled ? white : muted, coinButton.w - 20.0F, true);
    text("CREDITS: " + std::to_string(model.credits),
         coinButton.x + coinButton.w * 0.5F, coinButton.y + 33.0F, 16,
         coinEnabled && model.credits > 0 ? cyan : muted,
         coinButton.w - 20.0F, true);
    if (coinEnabled) addHit(coinButton, {UiActionKind::InsertCoin});
    button({240, 946, 172, 64}, selectedRadio ? "PLAY RADIO" : "ADD TO QUEUE", {UiActionKind::AddSelected},
           visitorEnabled && model.selectedTrack != nullptr, pink);

    panel({460, 160, 940, 870}, {9, 13, 29, 238}, panelBorder);
    std::string libraryTitle = model.libraryFilter == LibraryFilter::Music ? "MUSIC LIBRARY" :
                               model.libraryFilter == LibraryFilter::Video ? "VIDEO LIBRARY" :
                               radio ? "RADYO · LIVE STATIONS" :
                               model.libraryFilter == LibraryFilter::Favorites ? "FAVORITES" : "ALL MEDIA";
    if (!model.selectedGenre.empty()) {
        libraryTitle = ellipsize(model.selectedGenre, 18) + " / " + libraryTitle;
    }
    text(!visitorEnabled && browseMode ? "INSERT COIN TO CHOOSE MEDIA" : libraryTitle, 490, 186, 24,
         !visitorEnabled && browseMode ? pink : white);
    const auto filteredCount = model.filtered ? model.filtered->size() : 0;
    text(std::to_string(filteredCount) + " RESULTS", 1280, 191, 17, muted, 90, true);
    if (!model.filtered || model.filtered->empty()) {
        if (radio) drawRadioNotice(model, {500, 450, 860, 146});
        else text(model.buildingEmptyLibrary() ? "Building your library..." : "No matching media found",
                  930, 485, 30, muted, 700, true);
    } else {
        const std::size_t start = model.page * pageSize;
        for (std::size_t visible = 0; visible < pageSize && start + visible < model.filtered->size(); ++visible) {
            const std::size_t trackIndex = (*model.filtered)[start + visible];
            const auto& track = model.library->tracks[trackIndex];
            const int column = static_cast<int>(visible % 3);
            const int row = static_cast<int>(visible / 3);
            const SDL_FRect card{490.0F + column * 296.0F, 230.0F + row * 242.0F, 276, 222};
            const bool selected = model.selectedTrack && model.selectedTrack->id == track.id;
            panel(card, selected ? SDL_Color{25, 25, 57, 255} : SDL_Color{14, 19, 40, 255}, selected ? pink : panelBorder);
            const SDL_FRect cardArtwork{card.x + 16, card.y + 14, 112, 112};
            if (model.nowPlayingArtworkMode == NowPlayingArtworkMode::SpinningDisc &&
                track.mediaKind == MediaKind::Music) {
                // Library discs use a fixed orientation; only NOW PLAYING spins.
                drawDiscCover(&track, cardArtwork, 0.0F, false);
            } else {
                drawCover(&track, cardArtwork);
            }
            text(track.favorite ? "★" : "", card.x + 236, card.y + 14, 24, pink);
            if (track.mediaKind == MediaKind::Video)
                text("VIDEO", card.x + 182, card.y + 105, 13, pink, 70, true);
            const auto label = trackLabel(track);
            const bool station = track.mediaKind == MediaKind::Radio;
            text(ellipsize(station ? track.title : label.title, 25), card.x + 16, card.y + 132, 21, white, card.w - 32);
            text(ellipsize(station ? radioSubtitle(track) : label.artist, 28), card.x + 16, card.y + 162, 17, cyan, card.w - 32);
            std::string cardMetadata = station ? "CANLI · LIVE" : formatDuration(track.durationMs);
            if (!station && track.genre != "Unknown Genre") cardMetadata += " · " + track.genre;
            if (!station && track.albumYear > 0) cardMetadata += " · " + std::to_string(track.albumYear);
            text(ellipsize(cardMetadata, 32), card.x + 16, card.y + 191, 16, station ? pink : muted,
                 card.w - 32);
            if (visitorEnabled) addHit(card, {UiActionKind::SelectTrack, trackIndex});
        }
        const std::size_t pages = (filteredCount + pageSize - 1) / pageSize;
        if (radio && (model.radioFetching || !model.radioStatus.empty()))
            text(model.radioFetching ? "Refreshing radio stations..." : model.radioStatus,
                 930, pages > 1 ? 999.0F : 975.0F, 14, cyan, pages > 1 ? 450.0F : 840.0F, true, 20);
        if (pages > 1) {
            button({490, 950, 190, 68}, "PREVIOUS", {UiActionKind::PagePrevious},
                   visitorEnabled && model.page > 0, cyan);
            text("PAGE " + std::to_string(model.page + 1) + " / " + std::to_string(pages),
                 930, 972, 18, muted, 250, true);
            button({1168, 950, 190, 68}, "NEXT", {UiActionKind::PageNext},
                   visitorEnabled && model.page + 1 < pages, cyan);
        }
    }

    panel({1420, 160, 470, 870}, panelFill, {38, 50, 82, 255});
    text("UP NEXT", 1450, 188, 24, white);
    const std::size_t queued = model.queue ? model.queue->size() : 0;
    text(std::to_string(queued) + " REQUESTS", 1800, 191, 17, pink, 70, true);
    if (!model.queue || model.queue->empty()) {
        text(currentRadio ? "Live radio is playing" : radio ? "Select a station to listen live" : "Automatic shuffle is active",
             1655, 480, 25, muted, 360, true);
    } else {
        const std::size_t shown = std::min<std::size_t>(10, model.queue->size());
        for (std::size_t i = 0; i < shown; ++i) {
            const auto* queuedTrack = LibraryScanner::find(*model.library, (*model.queue)[i].trackId);
            const SDL_FRect row{1445, 230.0F + i * 72.0F, 420, 62};
            panel(row, {16, 22, 44, 255}, i == 0 ? cyan : panelBorder);
            text(std::to_string(i + 1), row.x + 18, row.y + 18, 21, i == 0 ? cyan : muted);
            text(queuedTrack ? ellipsize(queuedTrack->title, 31) : "Missing track",
                 row.x + 58, row.y + 10, 19, white, 320);
            text(queuedTrack ? ellipsize(queuedTrack->artist, 34) : "",
                 row.x + 58, row.y + 36, 15, muted, 320);
        }
        if (queued > shown) text("+ " + std::to_string(queued - shown) + " MORE", 1655, 960, 18, muted, 300, true);
    }

    if (browseMode && model.adminReveal) {
        button({8, 8, 210, 76}, "ADMIN", {UiActionKind::OpenAdmin}, true, pink);
    }

}

void UI::drawGenreMenu(const UiModel& model) {
    // Insert a dismissal layer after the normal page hits and before the menu
    // rows. Reverse hit testing then gives every genre row priority while an
    // outside tap closes the popup.
    addHit({0, 0, 1920, 1080}, {UiActionKind::CloseGenreMenu});
    constexpr std::size_t genrePageSize = 9;
    const std::size_t genreCount = model.genres ? model.genres->size() : 0;
    const std::size_t optionCount = genreCount + 1;  // ALL GENRES is option zero.
    const std::size_t pageCount = std::max<std::size_t>(1,
        (optionCount + genrePageSize - 1) / genrePageSize);
    const std::size_t menuPage = std::min(model.genreMenuPage, pageCount - 1);
    const std::size_t firstOption = menuPage * genrePageSize;
    const std::size_t shown = std::min(genrePageSize, optionCount - firstOption);
    const bool paged = pageCount > 1;
    const float menuHeight = 58.0F + static_cast<float>(shown) * 72.0F +
                             (paged ? 82.0F : 14.0F);
    const bool retro = model.theme == Theme::Retro;
    const SDL_FRect menu = retro ? SDL_FRect{504, 112, 384, menuHeight}
                                : SDL_FRect{732, 96, 450, menuHeight};

    SDL_SetRenderDrawColor(renderer_, 2, 4, 12, 150);
    const SDL_FRect shade = retro ? SDL_FRect{0, 104, 1920, 976}
                                 : SDL_FRect{0, 90, 1920, 990};
    SDL_RenderFillRect(renderer_, &shade);
    panel(menu, {8, 12, 27, 252}, cyan);
    text("SELECT GENRE", menu.x + 18, menu.y + 16, 19, cyan);
    text(std::to_string(genreCount) + " GENRES", menu.x + menu.w - 82,
         menu.y + 18, 15, muted, 130, true);

    for (std::size_t row = 0; row < shown; ++row) {
        const std::size_t option = firstOption + row;
        const bool allGenres = option == 0;
        const std::string label = allGenres
            ? "ALL GENRES"
            : ellipsize((*model.genres)[option - 1], 32);
        const bool selected = allGenres ? model.selectedGenre.empty()
            : normalizeForSearch((*model.genres)[option - 1]) ==
              normalizeForSearch(model.selectedGenre);
        button({menu.x + 12, menu.y + 52 + static_cast<float>(row) * 72.0F,
                menu.w - 24, 62}, label,
               {UiActionKind::SelectGenre, option}, true,
               selected ? pink : panelBorder);
    }

    if (paged) {
        const float navigationY = menu.y + 58.0F + static_cast<float>(shown) * 72.0F;
        button({menu.x + 12, navigationY, 126, 62}, "PREVIOUS",
               {UiActionKind::GenrePagePrevious}, menuPage > 0, cyan);
        text(std::to_string(menuPage + 1) + " / " + std::to_string(pageCount),
             menu.x + menu.w * 0.5F, navigationY + 19, 17, muted, 95, true);
        button({menu.x + menu.w - 138, navigationY, 126, 62}, "NEXT",
               {UiActionKind::GenrePageNext}, menuPage + 1 < pageCount, cyan);
    }
}

void UI::drawSetupPin(const UiModel& model) {
    text("NEON JUKEBOX", 960, 100, 58, cyan, 800, true);
    text("FIRST-RUN SETUP", 960, 182, 22, pink, 500, true);
    drawPinPad(model, false);
}

void UI::drawSetupFolder(const UiModel& model) {
    panel({360, 150, 1200, 790}, panelFill, pink);
    text("CONNECT YOUR MEDIA", 960, 205, 44, white, 900, true);
    text("Music and video folders stay separate but share one request queue.",
         960, 270, 22, muted, 950, true);

    panel({430, 340, 500, 330}, {12, 18, 38, 255}, cyan);
    text("MUSIC LIBRARY", 680, 385, 30, cyan, 430, true);
    text("MP3 · OGG · FLAC · WAV", 680, 440, 18, muted, 430, true);
    text(std::to_string(model.musicSourceCount) + " SOURCE FOLDERS", 680, 500, 22, white, 430, true);
    button({490, 560, 380, 78}, "SELECT MUSIC FOLDERS",
           {UiActionKind::ChooseMusicFolders}, true, cyan);

    panel({990, 340, 500, 330}, {12, 18, 38, 255}, pink);
    text("VIDEO LIBRARY", 1240, 385, 30, pink, 430, true);
    text("MP4 · MOV · AVI · WMV · MKV", 1240, 440, 18, muted, 430, true);
    text(std::to_string(model.videoSourceCount) + " SOURCE FOLDERS", 1240, 500, 22, white, 430, true);
    button({1050, 560, 380, 78}, "SELECT VIDEO FOLDERS",
           {UiActionKind::ChooseVideoFolders}, true, pink);

    text("Folders are optional. Select RADYO in the jukebox to listen online; radio needs internet.",
         960, 704, 21, cyan, 1100, true, 34);
    button({700, 760, 520, 86}, "CONTINUE TO JUKEBOX", {UiActionKind::FinishFolderSetup}, true, pink);
    text(model.toast, 960, 875, 20, muted, 1060, true, 36);
}

void UI::drawPinPad(const UiModel& model, bool cancelAllowed) {
    addHit({0, 0, 1920, 1080}, {UiActionKind::None});
    const SDL_FRect overlay{560, cancelAllowed ? 120.0F : 240.0F, 800, 800};
    panel(overlay, {8, 12, 29, 252}, pink);
    const std::string title = model.mode == UiMode::ChangePin ? "CHANGE ADMIN PIN" :
                              cancelAllowed ? "ADMIN ACCESS" : "CREATE ADMIN PIN";
    text(title, 960, overlay.y + 55, 36, white, 700, true);
    text(model.pinPrompt, 960, overlay.y + 112, 20, muted, 680, true);
    std::string dots;
    for (std::size_t i = 0; i < model.pinLength; ++i) dots += theme_ == Theme::Retro ? "• " : "● ";
    text(dots.empty() ? "_ _ _ _" : dots, 960, overlay.y + 158, 32, cyan, 500, true);
    for (int digit = 1; digit <= 9; ++digit) {
        const int index = digit - 1;
        const SDL_FRect key{745.0F + (index % 3) * 150.0F, overlay.y + 230.0F + (index / 3) * 112.0F, 130, 88};
        button(key, std::to_string(digit), {UiActionKind::PinDigit, 0, static_cast<char>('0' + digit)}, true, cyan);
    }
    button({745, overlay.y + 566, 130, 88}, "BACK", {UiActionKind::PinBackspace}, true, muted);
    button({895, overlay.y + 566, 130, 88}, "0", {UiActionKind::PinDigit, 0, '0'}, true, cyan);
    button({1045, overlay.y + 566, 130, 88}, "ENTER", {UiActionKind::PinSubmit}, true, pink);
    if (cancelAllowed) button({745, overlay.y + 680, 430, 64}, "CANCEL", {UiActionKind::PinCancel}, true, muted);
}

void UI::drawKeyboard(const UiModel& model) {
    const bool retro = model.theme == Theme::Retro;
    const SDL_FRect shade{0, 0, 1920, 1080};
    SDL_SetRenderDrawColor(renderer_, 2, 4, 12, 170); SDL_RenderFillRect(renderer_, &shade);
    addHit(shade, {UiActionKind::None});
    if (retro) chrome({80, 330, 1760, 750}, true);
    else panel({80, 330, 1760, 750}, {9, 13, 29, 254}, cyan);
    const SDL_Color keyboardInk = retro ? SDL_Color{38, 39, 32, 255} : cyan;
    text("SEARCH MEDIA", 140, 365, 28, keyboardInk);
    text("Type a title, artist, or album, then press ENTER", 1410, 370, 18,
         retro ? SDL_Color{99, 99, 86, 255} : muted, 720, true);
    if (retro) phosphor({140, 410, 1640, 82});
    else panel({140, 410, 1640, 82}, {17, 24, 48, 255}, panelBorder);
    text(model.searchDraft.empty() ? "Type a title, artist, or album" : model.searchDraft,
         175, 434, 28, retro ? SDL_Color{170, 230, 113, 255} : model.searchDraft.empty() ? muted : white,
         1550, false, 36, retro);
    static constexpr std::array<std::string_view, 3> rows{"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const float startX = row == 0 ? 210.0F : row == 1 ? 285.0F : 435.0F;
        for (std::size_t column = 0; column < rows[row].size(); ++column) {
            const char value = rows[row][column];
            button({startX + column * 150.0F, 520.0F + row * 110.0F, 130, 90}, std::string(1, value),
                   {UiActionKind::KeyCharacter, 0, value}, true, cyan);
        }
    }
    button({210, 860, 220, 92}, "CLEAR", {UiActionKind::KeyClear}, true, muted);
    button({450, 860, 620, 92}, "SPACE", {UiActionKind::KeySpace}, true, cyan);
    button({1090, 860, 220, 92}, "BACK", {UiActionKind::KeyBackspace}, true, muted);
    button({1330, 860, 180, 92}, "CANCEL", {UiActionKind::CloseKeyboard}, true, muted);
    button({1530, 860, 180, 92}, "ENTER", {UiActionKind::SubmitSearch}, true, pink);
}

void UI::drawPlayNowPrompt(const UiModel& model) {
    const SDL_FRect shade{0, 0, 1920, 1080};
    SDL_SetRenderDrawColor(renderer_, 2, 4, 12, 205);
    SDL_RenderFillRect(renderer_, &shade);
    addHit(shade, {UiActionKind::None});

    const SDL_FRect dialog{510, 275, 900, 500};
    panel(dialog, {9, 13, 29, 255}, pink);
    text("START THE REQUEST QUEUE NOW?", 960, 335, 38, white, 780, true);
    text("A background track is currently playing.", 960, 405, 23, muted, 760, true);
    text("First request: " + ellipsize(model.requestedTrackTitle, 44), 960, 468, 30, cyan, 760, true);
    text("Start the request immediately or let the current track finish?",
         960, 530, 21, muted, 780, true);
    button({590, 625, 340, 92}, "WAIT FOR CURRENT", {UiActionKind::WaitForCurrentTrack}, true, cyan);
    button({990, 625, 340, 92}, "PLAY NOW", {UiActionKind::PlayRequestNow}, true, pink);
}

void UI::drawVisualizerOverlay(const UiModel& model) {
    const SDL_FRect shade{0, 0, 1920, 1080};
    SDL_SetRenderDrawColor(renderer_, 2, 4, 12, 235);
    SDL_RenderFillRect(renderer_, &shade);
    addHit(shade, {UiActionKind::None});

    panel({60, 55, 1800, 970}, {6, 9, 21, 255}, cyan);
    const auto choices = visualizersForTheme(model.theme);
    const auto selectedMode = themeVisualizer(model.theme, model.visualizerMode);
    const auto number = themeVisualizerIndex(model.theme, selectedMode) + 1;
    text("VISUALIZER " + std::string(number < 10 ? "0" : "") + std::to_string(number) +
             " / " + std::to_string(choices.size()),
         125, 102, 20, pink);
    text(VisualizerRenderer::name(selectedMode), 125, 137, 42, white);
    text(VisualizerRenderer::subtitle(selectedMode), 125, 193, 20, muted, 1250);
    button({1600, 92, 190, 68}, "CLOSE", {UiActionKind::CloseVisualizer}, true, muted);

    const SDL_FRect display{125, 245, 1670, 610};
    panel({display.x - 2, display.y - 2, display.w + 4, display.h + 4},
          {5, 7, 18, 255}, panelBorder);
    drawVisualizer(display, selectedMode);

    if (choices.size() > 1) {
        button({125, 895, 260, 76}, "PREVIOUS", {UiActionKind::VisualizerPrevious}, true, cyan);
        text("SWIPE LEFT OR RIGHT TO CHANGE STYLE", 960, 916, 18, muted, 720, true);
        button({1535, 895, 260, 76}, "NEXT", {UiActionKind::VisualizerNext}, true, pink);
    }
}

const UI::VideoInfoImage& UI::videoInfoImage(const Track& track) {
    refreshRenderMetrics();
    const auto label = trackLabel(track);
    std::string details = track.album;
    if (normalizeForSearch(details) == "unknown album") details.clear();
    if (track.albumYear > 0) {
        if (!details.empty()) details += " · ";
        details += std::to_string(track.albumYear);
    }
    const auto artist = normalizeForSearch(label.artist) == "unknown artist" ? std::string{} : label.artist;
    const auto key = label.title + '\n' + artist + '\n' + details + '\n' + std::to_string(textRasterScale_);
    if (key == videoInfoKey_ && videoInfoImage_.pixels) return videoInfoImage_;
    videoInfoKey_ = key;
    if (videoInfoImage_.pixels) SDL_DestroySurface(videoInfoImage_.pixels);
    videoInfoImage_.pixels = nullptr;
    ++videoInfoImage_.revision;
    const int width = std::max(1, static_cast<int>(std::lround(1440 * textRasterScale_)));
    const int inset = std::max(2, static_cast<int>(std::lround(8 * textRasterScale_)));
    const int gap = std::max(1, static_cast<int>(std::lround(2 * textRasterScale_)));
    struct Line { SDL_Surface* pixels; int y; };
    std::vector<Line> lines;
    int height = inset;
    const auto append = [&](const std::string& value, int size, SDL_Color color) {
        if (value.empty()) return;
        const auto rasterSize = std::max(1, static_cast<int>(std::lround(size * textRasterScale_)));
        auto& selected = videoInfoFonts_[rasterSize];
        if (!selected) selected = TTF_OpenFont(videoInfoFontPath_.c_str(), static_cast<float>(rasterSize));
        if (selected) {
            TTF_SetFontWrapAlignment(selected, TTF_HORIZONTAL_ALIGN_LEFT);
            if (auto* pixels = TTF_RenderText_Blended_Wrapped(selected, value.c_str(), value.size(), color, width - inset * 2)) {
                lines.push_back({pixels, height});
                height += pixels->h + gap;
            }
        }
    };
    append(artist, 72, {174, 242, 112, 255});
    append("\"" + label.title + "\"", 66, {174, 242, 112, 255});
    append(details, 60, {163, 231, 102, 255});
    if (!lines.empty()) {
        height += inset;
        videoInfoImage_.pixels = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
        if (auto* image = videoInfoImage_.pixels) {
            SDL_FillSurfaceRect(image, nullptr, 0);
            for (const auto& line : lines) {
                SDL_Rect destination{inset + inset / 2, line.y + inset / 2, 0, 0};
                // Strong broadcast lettering stays readable over bright video.
                SDL_SetSurfaceColorMod(line.pixels, 0, 0, 0);
                SDL_SetSurfaceAlphaMod(line.pixels, 225);
                SDL_BlitSurface(line.pixels, nullptr, image, &destination);
                const int edge = std::max(1, inset / 4);
                for (int dy : {-edge, 0, edge}) for (int dx : {-edge, 0, edge}) {
                    if (!dx && !dy) continue;
                    destination.x = inset + dx;
                    destination.y = line.y + dy;
                    SDL_BlitSurface(line.pixels, nullptr, image, &destination);
                }
                SDL_SetSurfaceColorMod(line.pixels, 255, 255, 255);
                SDL_SetSurfaceAlphaMod(line.pixels, 255);
                destination.x = inset;
                destination.y = line.y;
                SDL_BlitSurface(line.pixels, nullptr, image, &destination);
            }
        }
    }
    for (const auto& line : lines) SDL_DestroySurface(line.pixels);
    return videoInfoImage_;
}

void UI::drawVideoFullscreen() {
    const SDL_FRect screen{0, 0, 1920, 1080};
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer_, &screen);
    addHit(screen, {UiActionKind::ToggleVideoFullscreen});
}

void UI::drawAdmin(const UiModel& model) {
    const SDL_FRect shade{0, 0, 1920, 1080};
    SDL_SetRenderDrawColor(renderer_, 2, 4, 12, 220); SDL_RenderFillRect(renderer_, &shade);
    addHit(shade, {UiActionKind::None});
    panel({180, 90, 1560, model.scanning ? 950.0F : 900.0F}, {9, 13, 29, 252}, pink);
    text("ADMIN CONTROL", 240, 135, 36, pink);
    button({1490, 120, 190, 64}, "CLOSE", {UiActionKind::AdminClose}, true, muted);

    text("TEMA", 240, 208, 20, cyan);
    for (std::size_t i = 0; i < themeDefinitions.size(); ++i) {
        const auto& definition = themeDefinitions[i];
        const bool selected = model.theme == definition.theme;
        const SDL_FRect choice{340.0F + static_cast<float>(i) * 640, 198, 620, 100};
        panel(choice, {16, 22, 44, 255}, selected ? pink : panelBorder);
        const SDL_FRect swatch{choice.x + 14, choice.y + 14, 86, 72};
        if (definition.theme == Theme::Retro) {
            chrome(swatch);
            phosphor({swatch.x + 55, swatch.y + 9, 23, 54});
            SDL_FRect rail{swatch.x + 7, swatch.y + 9, 12, 54};
            SDL_SetRenderDrawColor(renderer_, 172, 34, 25, 255);
            SDL_RenderFillRect(renderer_, &rail);
        } else {
            // Filled, inset borders keep every edge visible after scaling.
            // Use the preview's own colors even when the active theme is Retro.
            const auto previewBox = [&](const SDL_FRect& rect, SDL_Color border) {
                SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
                SDL_RenderFillRect(renderer_, &rect);
                const SDL_FRect inside{rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4};
                SDL_SetRenderDrawColor(renderer_, 8, 12, 29, 255);
                SDL_RenderFillRect(renderer_, &inside);
            };
            previewBox(swatch, cyan);
            for (int cell = 0; cell < 6; ++cell) {
                const SDL_FRect tile{swatch.x + 10 + (cell % 3) * 23.0F,
                                     swatch.y + 12 + (cell / 3) * 28.0F, 17, 21};
                previewBox(tile, pink);
            }
        }
        text(definition.label, choice.x + 118, choice.y + 15, 26, white, 350);
        text(definition.theme == Theme::Retro ? "Chrome / title cards / green displays"
                                             : "Original / neon / album grid",
             choice.x + 118, choice.y + 58, 18, muted, 420);
        if (selected) text("AKTİF", choice.x + choice.w - 54, choice.y + 22, 18, pink, 72, true);
        addHit(choice, {UiActionKind::AdminSelectTheme, i});
    }

    text("PLAYBACK", 240, 327, 20, cyan);
    button({240, 360, 220, 70}, model.playback.state == PlaybackState::Paused ? "RESUME" : "PAUSE",
           {UiActionKind::AdminPlayPause}, model.currentTrack != nullptr, cyan);
    button({480, 360, 160, 70}, "SKIP", {UiActionKind::AdminSkip}, model.currentTrack != nullptr, pink);
    button({660, 360, 160, 70}, "-15 SEC", {UiActionKind::AdminSeekBackward}, model.currentTrack != nullptr, cyan);
    button({840, 360, 160, 70}, "+15 SEC", {UiActionKind::AdminSeekForward}, model.currentTrack != nullptr, cyan);
    button({1020, 360, 140, 70}, "VOL -", {UiActionKind::AdminVolumeDown}, true, cyan);
    button({1180, 360, 140, 70}, "VOL +", {UiActionKind::AdminVolumeUp}, true, cyan);
    text("VOLUME " + std::to_string(static_cast<int>(model.playback.volume * 100.0F)) + "%", 1370, 382, 22, white);

    text("LIBRARY & DISPLAY", 240, 467, 20, cyan);
    button({240, 502, 200, 68}, "RESCAN", {UiActionKind::AdminRescan}, !model.scanning, cyan);
    button({460, 502, 250, 68}, "MUSIC SOURCES (" + std::to_string(model.musicSourceCount) + ")",
           {UiActionKind::AdminChooseMusicFolders}, true, cyan);
    button({730, 502, 250, 68}, "VIDEO SOURCES (" + std::to_string(model.videoSourceCount) + ")",
           {UiActionKind::AdminChooseVideoFolders}, true, pink);
    button({1000, 502, 190, 68}, "ARTWORK", {UiActionKind::AdminUseArtwork}, true,
           model.nowPlayingArtworkMode == NowPlayingArtworkMode::Artwork ? pink : panelBorder);
    button({1210, 502, 190, 68}, model.theme == Theme::Retro ? "SPINNING LP" : "SPINNING CD", {UiActionKind::AdminUseSpinningDisc}, true,
           model.nowPlayingArtworkMode == NowPlayingArtworkMode::SpinningDisc ? pink : panelBorder);
    button({1420, 502, 200, 68}, model.selectedTrack && model.selectedTrack->favorite ? "UNFAVORITE" : "FAVORITE",
           {UiActionKind::AdminToggleFavorite}, model.selectedTrack != nullptr, pink);

    text("REQUEST QUEUE", 240, 590, 20, cyan);
    const std::size_t shown = model.queue ? std::min<std::size_t>(4, model.queue->size()) : 0;
    for (std::size_t i = 0; i < shown; ++i) {
        const auto* queuedTrack = LibraryScanner::find(*model.library, (*model.queue)[i].trackId);
        const SDL_FRect row{240, 630.0F + i * 80.0F, 840, 68};
        panel(row, model.adminQueueSelection == i ? SDL_Color{33, 29, 62, 255} : SDL_Color{16, 22, 44, 255},
              model.adminQueueSelection == i ? pink : panelBorder);
        text(std::to_string(i + 1) + ".  " + (queuedTrack ? ellipsize(queuedTrack->title, 48) : "Missing track"),
             row.x + 15, row.y + 10, 17, white, 800);
        addHit(row, {UiActionKind::AdminQueueSelect, i});
    }
    button({1120, 630, 220, 62}, "MOVE UP", {UiActionKind::AdminQueueUp}, shown > 1, cyan);
    button({1360, 630, 220, 62}, "MOVE DOWN", {UiActionKind::AdminQueueDown}, shown > 1, cyan);
    button({1120, 712, 220, 62}, "REMOVE", {UiActionKind::AdminQueueRemove}, shown > 0, pink);
    button({1360, 712, 220, 62}, "CLEAR QUEUE", {UiActionKind::AdminClearQueue}, shown > 0, danger);
    // Administration feedback stays in the free space between controls.
    text(model.toast, 1350, 800, 20, muted, 460, true, 40);
    button({1120, 870, 220, 74}, "CHANGE PIN", {UiActionKind::AdminChangePin}, true, cyan);
    button({1360, 870, 220, 74}, "EXIT JUKEBOX", {UiActionKind::AdminExit}, true, danger);

    if (model.scanning) {
        // The footer has its own space below all four queue rows and exit controls.
        const bool retro = model.theme == Theme::Retro;
        const SDL_FRect status{240, 956, 1440, 76};
        if (retro) phosphor(status);
        else panel(status, {12, 18, 38, 255}, panelBorder);
        const SDL_Color statusInk = retro ? SDL_Color{170, 230, 113, 255} : cyan;
        const SDL_Color pathInk = retro ? SDL_Color{170, 230, 113, 255} : white;
        text("SCANNING  ·  " + std::to_string(model.scanProcessed) + " FILES CHECKED",
             status.x + 16, status.y + 8, 20, statusInk, status.w - 32, false, 24, retro);
        text(model.scanFile.empty() ? "Preparing folder scan..." : model.scanFile,
             status.x + 16, status.y + 40, 18, pathInk, status.w - 32, false, 24, retro);
    }
}

void UI::drawVisualizer(const SDL_FRect& rect, VisualizerMode mode) {
    // Lay out primitives at their final on-screen size, including display density.
    textRasterScale_ = outputScale_;
    visualizer_.draw(renderer_, rect, themeVisualizer(theme_, mode), outputScale_);
}

void UI::drawCover(const Track* track, const SDL_FRect& rect) {
    if (!track) {
        panel(rect, {10, 14, 31, 255}, panelBorder);
        text("♪", rect.x + rect.w / 2, rect.y + rect.h / 2 - 35, static_cast<int>(std::min(rect.w, rect.h) * 0.32F), cyan, 0, true);
        return;
    }
    if (auto* texture = artwork_.get(renderer_, *track)) SDL_RenderTexture(renderer_, texture, nullptr, &rect);
    else panel(rect, {10, 14, 31, 255}, pink);
}

void UI::drawSpinningDisc(const Track* track, const SDL_FRect& rect, bool framed) {
    drawDiscCover(track, rect, discRotationDegrees_, framed);
}

void UI::drawDiscCover(const Track* track, const SDL_FRect& rect, float rotationDegrees,
                       bool framed) {
    const bool vinyl = theme_ == Theme::Retro;
    if (framed && !vinyl) panel(rect, {4, 6, 15, 255}, panelBorder);
    if (!track && !vinyl) {
        text("♪", rect.x + rect.w / 2.0F, rect.y + rect.h / 2.0F - 35.0F,
             static_cast<int>(std::min(rect.w, rect.h) * 0.32F), cyan, 0, true);
        return;
    }
    SDL_Texture* artwork = track ? artwork_.get(renderer_, *track) : nullptr;
    if (!artwork && !vinyl) return;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    const float centerX = rect.x + rect.w * 0.5F;
    const float centerY = rect.y + rect.h * 0.5F;
    const float radius = std::min(rect.w, rect.h) * 0.465F;

    if (vinyl) {
        fillCircle(renderer_, centerX + 3, centerY + 7, radius + 1, {0, 0, 0, 55});
        fillCircle(renderer_, centerX + 2, centerY + 4, radius, {0, 0, 0, 110});
        fillCircle(renderer_, centerX, centerY, radius, {42, 43, 40, 255});
        fillCircle(renderer_, centerX, centerY, radius * 0.983F, {10, 11, 12, 255});

        // Fewer grooves on catalogue thumbnails keep the black vinyl readable.
        const int grooves = std::clamp(static_cast<int>(radius * outputScale_ * 0.54F / 3.5F), 4, 28);
        const int grooveSegments = std::clamp(static_cast<int>(radius * outputScale_ * 2), 24, 160);
        for (int groove = 0; groove < grooves; ++groove) {
            const float ratio = 0.42F + 0.53F * static_cast<float>(groove) / (grooves - 1);
            antialiasedArc(renderer_, centerX, centerY, radius * ratio, 0, uiPi * 2,
                           {55, 57, 56, 115}, grooveSegments);
        }
        // Broad, stationary reflections skim the grooves while the paper label turns.
        constexpr int lightSegments = 96;
        std::array<SDL_Vertex, lightSegments + 2> lightVertices{};
        std::array<int, lightSegments * 3> lightIndices{};
        lightVertices[0] = {{centerX, centerY}, {1, 1, 1, 0}, {}};
        for (int segment = 0; segment <= lightSegments; ++segment) {
            const float angle = static_cast<float>(segment) / lightSegments * uiPi * 2;
            const float shine = std::pow(std::abs(std::cos(angle + 0.95F)), 20.0F) * 0.16F;
            lightVertices[segment + 1] = {
                {centerX + std::cos(angle) * radius * 0.973F,
                 centerY + std::sin(angle) * radius * 0.973F}, {0.92F, 0.95F, 1, shine}, {}};
            if (segment < lightSegments) {
                lightIndices[segment * 3] = 0;
                lightIndices[segment * 3 + 1] = segment + 1;
                lightIndices[segment * 3 + 2] = segment + 2;
            }
        }
        std::array<int, lightSegments> lightBoundary{};
        for (int i = 0; i < lightSegments; ++i) lightBoundary[i] = i + 1;
        lightIndices[(lightSegments - 1) * 3 + 2] = 1;
        drawing::mesh(renderer_, nullptr, lightVertices, lightIndices, lightBoundary);
        circle(renderer_, centerX, centerY, radius * 0.397F, {38, 39, 37, 210});
        fillCircle(renderer_, centerX, centerY, radius * 0.365F, {133, 119, 88, 255});
        fillCircle(renderer_, centerX, centerY, radius * 0.35F, {169, 69, 45, 255});
    } else {
        // The original Neon CD retains its metallic rim and full-face artwork.
        fillCircle(renderer_, centerX + 4.0F, centerY + 7.0F, radius, {0, 0, 0, 145});
        fillCircle(renderer_, centerX, centerY, radius, {180, 192, 205, 255});
        fillCircle(renderer_, centerX, centerY, radius * 0.972F, {24, 29, 39, 255});
    }

    if (artwork) {
        constexpr int segments = 192;
        std::vector<SDL_Vertex> vertices;
        std::vector<int> indices;
        vertices.reserve(1 + (segments + 1) * 2);
        indices.reserve(segments * 9);
        const SDL_FColor opaqueWhite{1.0F, 1.0F, 1.0F, 1.0F};
        const SDL_FColor transparentWhite{1.0F, 1.0F, 1.0F, 0.0F};
        vertices.push_back({{centerX, centerY}, opaqueWhite, {0.5F, 0.5F}});
        const float artworkRadius = radius * (vinyl ? 0.35F : 0.945F);
        const float solidArtworkRadius = std::max(0.0F, artworkRadius - drawing::pixelSize(renderer_));
        const float solidTextureRadius = 0.5F * solidArtworkRadius / artworkRadius;
        const float rotation = rotationDegrees * uiPi / 180.0F;
        for (int segment = 0; segment <= segments; ++segment) {
            const float angle = static_cast<float>(segment) / static_cast<float>(segments) *
                                uiPi * 2.0F;
            const float textureAngle = angle - rotation;
            vertices.push_back({
                {centerX + std::cos(angle) * solidArtworkRadius,
                 centerY + std::sin(angle) * solidArtworkRadius},
                opaqueWhite,
                {0.5F + std::cos(textureAngle) * solidTextureRadius,
                 0.5F + std::sin(textureAngle) * solidTextureRadius}
            });
            vertices.push_back({
                {centerX + std::cos(angle) * artworkRadius,
                 centerY + std::sin(angle) * artworkRadius},
                transparentWhite,
                {0.5F + std::cos(textureAngle) * 0.5F,
                 0.5F + std::sin(textureAngle) * 0.5F}
            });
            if (segment < segments) {
                const int inner = 1 + segment * 2;
                const int outer = inner + 1;
                const int nextInner = inner + 2;
                const int nextOuter = inner + 3;
                indices.insert(indices.end(), {
                    0, inner, nextInner,
                    inner, outer, nextInner,
                    nextInner, outer, nextOuter
                });
            }
        }
        SDL_BlendMode previousBlendMode = SDL_BLENDMODE_NONE;
        SDL_GetTextureBlendMode(artwork, &previousBlendMode);
        SDL_SetTextureBlendMode(artwork, SDL_BLENDMODE_BLEND);
        SDL_RenderGeometry(renderer_, artwork, vertices.data(),
                           static_cast<int>(vertices.size()), indices.data(),
                           static_cast<int>(indices.size()));
        SDL_SetTextureBlendMode(artwork, previousBlendMode);
    }

    if (vinyl) {
        circle(renderer_, centerX, centerY, radius * 0.354F, {214, 192, 139, 150});
        circle(renderer_, centerX, centerY, radius * 0.092F, {16, 17, 16, 85});
        const float holeRadius = std::max(1.2F, radius * 0.024F);
        fillCircle(renderer_, centerX, centerY, holeRadius + 0.9F, {4, 5, 5, 255});
        fillCircle(renderer_, centerX, centerY, holeRadius, {165, 174, 162, 255});
        return;
    }

    // Subtle optical grooves, a fixed light reflection, and the centre hub make
    // the rotating cover read as a physical disc instead of a circular picture.
    for (const float groove : {0.35F, 0.56F, 0.76F, 0.93F}) {
        circle(renderer_, centerX, centerY, radius * groove,
               {220, 240, 255, static_cast<Uint8>(groove > 0.9F ? 78 : 30)});
    }
    arc(renderer_, centerX, centerY, radius * 0.88F, 3.62F, 5.02F,
        {255, 255, 255, 105});
    arc(renderer_, centerX, centerY, radius * 0.83F, 3.66F, 4.96F,
        {95, 235, 255, 55});
    circle(renderer_, centerX, centerY, radius, {237, 247, 255, 145});
    fillCircle(renderer_, centerX, centerY, radius * 0.155F,
               {177, 188, 198, 218});
    circle(renderer_, centerX, centerY, radius * 0.155F, {245, 250, 255, 190});
    fillCircle(renderer_, centerX, centerY, radius * 0.070F, {3, 5, 12, 255});
    circle(renderer_, centerX, centerY, radius * 0.070F, {76, 91, 112, 255});
}

void UI::panel(const SDL_FRect& rect, SDL_Color fill, SDL_Color border) {
    if (theme_ == Theme::Retro) {
        // Shared dialogs use warm enamel instead of the original blue-black fill.
        if (fill.b > fill.r && fill.b >= fill.g && fill.r < 40 && fill.g < 45)
            fill = {232, 228, 213, 255};
        if (border.r == pink.r && border.g == pink.g) border = {155, 33, 28, 255};
        else if (border.r == cyan.r && border.g == cyan.g) border = {136, 63, 38, 255};
        else if (border.b > border.r) border = {126, 127, 119, 255};
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(renderer_, &rect);

    // SDL_RenderRect places part of a one-pixel stroke on the rectangle boundary.
    // That edge can disappear after logical-canvas scaling or parent clipping.
    // Four inward-filled strips keep every side complete at all resolutions.
    const float thickness = std::min(2.0F, std::min(rect.w, rect.h) * 0.5F);
    if (thickness <= 0.0F) return;
    SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
    const std::array<SDL_FRect, 4> edges{{
        {rect.x, rect.y, rect.w, thickness},
        {rect.x, rect.y + rect.h - thickness, rect.w, thickness},
        {rect.x, rect.y + thickness, thickness, std::max(0.0F, rect.h - thickness * 2.0F)},
        {rect.x + rect.w - thickness, rect.y + thickness, thickness,
         std::max(0.0F, rect.h - thickness * 2.0F)}
    }};
    SDL_RenderFillRects(renderer_, edges.data(), static_cast<int>(edges.size()));
}

void UI::button(const SDL_FRect& rect, std::string label, UiAction action, bool active, SDL_Color accent) {
    if (theme_ == Theme::Retro) {
        retroButton(rect, label, action, active, accent.r == pink.r && accent.g == pink.g);
        return;
    }
    panel(rect, active ? SDL_Color{22, 29, 58, 255} : SDL_Color{13, 17, 31, 230}, active ? accent : panelBorder);
    text(label, rect.x + rect.w / 2, rect.y + rect.h / 2 - 13, rect.h >= 76 ? 23 : 18,
         active ? white : muted, rect.w - 24, true);
    if (active) addHit(rect, action);
}

void UI::text(std::string_view value, float x, float y, int size, SDL_Color color,
              float maxWidth, bool centered, float maxHeight, bool meterFont, bool regularRetro) {
    if (value.empty()) return;
    if (theme_ == Theme::Retro && !meterFont) {
        if (color.r == white.r && color.g == white.g && color.b == white.b) color = {38, 39, 32, 255};
        else if ((color.r == cyan.r && color.g == cyan.g && color.b == cyan.b) ||
                 (color.r == pink.r && color.g == pink.g && color.b == pink.b)) color = {151, 35, 27, 255};
        else if (color.r == muted.r && color.g == muted.g && color.b == muted.b) color = {91, 92, 79, 255};
    }
    auto& entry = cachedText(value, size, color, meterFont, regularRetro);
    if (!entry.texture) return;
    float width = entry.width;
    float height = entry.height;
    float fit = 1.0F;
    if (maxWidth > 0.0F && width > maxWidth) fit = std::min(fit, maxWidth / width);
    if (maxHeight > 0.0F && height > maxHeight) fit = std::min(fit, maxHeight / height);
    width *= fit;
    height *= fit;
    SDL_FRect destination{centered ? x - width / 2.0F : x, y, width, height};
    SDL_RenderTexture(renderer_, entry.texture, nullptr, &destination);
}

void UI::addHit(const SDL_FRect& rect, UiAction action) {
    SDL_FRect touch = rect;
    const bool compactCatalogue = action.kind == UiActionKind::SelectArtistInitial ||
        (theme_ == Theme::Retro && action.kind == UiActionKind::SelectTrack);
    if (!compactCatalogue) {
        if (touch.w < 72.0F) { touch.x -= (72.0F - touch.w) * 0.5F; touch.w = 72.0F; }
        if (touch.h < 72.0F) { touch.y -= (72.0F - touch.h) * 0.5F; touch.h = 72.0F; }
    }
    hits_.push_back({touch, action});
}

bool UI::contains(const SDL_FRect& rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x <= rect.x + rect.w && y <= rect.y + rect.h;
}

std::string UI::ellipsize(std::string_view value, std::size_t maximum) {
    if (value.size() <= maximum) return std::string(value);
    std::size_t end = maximum > 3 ? maximum - 3 : maximum;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) --end;
    return std::string(value.substr(0, end)) + "...";
}

TTF_Font* UI::font(int size, bool meterFont, bool regularRetro) {
    const int rasterSize = std::max(1, static_cast<int>(std::lround(
        static_cast<float>(size) * textRasterScale_)));
    auto& collection = meterFont ? meterFonts_ : theme_ == Theme::Retro
        ? (regularRetro ? retroRegularFonts_ : retroFonts_) : size >= 26 ? boldFonts_ : regularFonts_;
    if (const auto found = collection.find(rasterSize); found != collection.end()) return found->second;
    const auto& path = meterFont ? meterFontPath_ : theme_ == Theme::Retro
        ? (regularRetro ? retroRegularFontPath_ : retroFontPath_) : size >= 26 ? boldFontPath_ : regularFontPath_;
    TTF_Font* loaded = TTF_OpenFont(path.c_str(), static_cast<float>(rasterSize));
    collection.emplace(rasterSize, loaded);
    return loaded;
}

UI::TextEntry& UI::cachedText(std::string_view value, int size, SDL_Color color,
                              bool meterFont, bool regularRetro) {
    const int scaleKey = static_cast<int>(std::lround(textRasterScale_ * 1000.0F));
    std::string key = std::string(meterFont ? "M:" : theme_ == Theme::Retro
        ? (regularRetro ? "Rr:" : "R:") : "U:") + std::to_string(size) + ':' +
                      std::to_string(scaleKey) + ':' + std::to_string(color.r) + ':' +
                      std::to_string(color.g) + ':' + std::to_string(color.b) + ':' + std::string(value);
    if (auto found = textCache_.find(key); found != textCache_.end()) {
        found->second.used = ++textClock_;
        return found->second;
    }
    TextEntry entry;
    if (auto* selectedFont = font(size, meterFont, regularRetro)) {
        if (SDL_Surface* surface = TTF_RenderText_Blended(selectedFont, value.data(), value.size(), color)) {
            entry.width = static_cast<float>(surface->w) / textRasterScale_;
            entry.height = static_cast<float>(surface->h) / textRasterScale_;
            entry.texture = SDL_CreateTextureFromSurface(renderer_, surface);
            if (entry.texture) SDL_SetTextureScaleMode(entry.texture, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(surface);
        }
    }
    entry.used = ++textClock_;
    return textCache_.emplace(std::move(key), entry).first->second;
}

void UI::refreshRenderMetrics() {
    int width = 1920;
    int height = 1080;
    if (renderer_ && SDL_GetRenderOutputSize(renderer_, &width, &height)) {
        outputScale_ = std::max(0.25F, std::min(static_cast<float>(width) / 1920.0F,
                                                static_cast<float>(height) / 1080.0F));
    } else {
        outputScale_ = 1.0F;
    }
    textRasterScale_ = outputScale_;
}

void UI::pruneTextCache() {
    while (textCache_.size() > 700) {
        const auto oldest = std::ranges::min_element(textCache_, {}, [](const auto& item) { return item.second.used; });
        if (oldest == textCache_.end()) break;
        SDL_DestroyTexture(oldest->second.texture);
        textCache_.erase(oldest);
    }
}

}  // namespace neon
