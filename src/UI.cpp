#include "neon/UI.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <sstream>

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
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int extent = static_cast<int>(std::ceil(radius));
    for (int y = -extent; y <= extent; ++y) {
        const float halfWidth = std::sqrt(std::max(
            0.0F, radius * radius - static_cast<float>(y * y)));
        SDL_RenderLine(renderer, centerX - halfWidth, centerY + static_cast<float>(y),
                       centerX + halfWidth, centerY + static_cast<float>(y));
    }
}

void circle(SDL_Renderer* renderer, float centerX, float centerY, float radius,
            SDL_Color color) {
    constexpr int segments = 128;
    std::array<SDL_FPoint, segments + 1> points{};
    for (int segment = 0; segment <= segments; ++segment) {
        const float angle = static_cast<float>(segment) / static_cast<float>(segments) *
                            uiPi * 2.0F;
        points[static_cast<std::size_t>(segment)] = {
            centerX + std::cos(angle) * radius,
            centerY + std::sin(angle) * radius
        };
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderLines(renderer, points.data(), static_cast<int>(points.size()));
}

void arc(SDL_Renderer* renderer, float centerX, float centerY, float radius,
         float startAngle, float endAngle, SDL_Color color) {
    constexpr int segments = 42;
    std::array<SDL_FPoint, segments + 1> points{};
    for (int segment = 0; segment <= segments; ++segment) {
        const float amount = static_cast<float>(segment) / static_cast<float>(segments);
        const float angle = startAngle + (endAngle - startAngle) * amount;
        points[static_cast<std::size_t>(segment)] = {
            centerX + std::cos(angle) * radius,
            centerY + std::sin(angle) * radius
        };
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderLines(renderer, points.data(), static_cast<int>(points.size()));
}

}  // namespace

UI::~UI() { shutdown(); }

void UI::shutdown() {
    destroyVisualizerTargets();
    artwork_.clear();
    for (auto& [_, entry] : textCache_) if (entry.texture) SDL_DestroyTexture(entry.texture);
    for (auto& [_, value] : regularFonts_) if (value) TTF_CloseFont(value);
    for (auto& [_, value] : boldFonts_) if (value) TTF_CloseFont(value);
    for (auto& [_, value] : meterFonts_) if (value) TTF_CloseFont(value);
    textCache_.clear();
    regularFonts_.clear();
    boldFonts_.clear();
    meterFonts_.clear();
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
    if (!std::filesystem::exists(pathFromUtf8(meterFontPath_))) meterFontPath_ = regularFontPath_;

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

void UI::render(const UiModel& model) {
    refreshRenderMetrics();
    hits_.clear();
    const std::uint64_t ticks = SDL_GetTicks();
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
    for (int x = 0; x <= 1920; x += 80) SDL_RenderLine(renderer_, static_cast<float>(x), 0, static_cast<float>(x), 1080);
    for (int y = 0; y <= 1080; y += 80) SDL_RenderLine(renderer_, 0, static_cast<float>(y), 1920, static_cast<float>(y));

    if (model.videoFullscreen) {
        drawVideoFullscreen();
        pruneTextCache();
        SDL_RenderPresent(renderer_);
        return;
    }

    if (model.mode == UiMode::SetupPin) drawSetupPin(model);
    else if (model.mode == UiMode::SetupFolder) drawSetupFolder(model);
    else {
        drawBrowse(model);
        if (model.mode == UiMode::AdminPin || model.mode == UiMode::ChangePin) drawPinPad(model, true);
        if (model.mode == UiMode::Admin) drawAdmin(model);
        if (model.visualizerOpen) drawVisualizerOverlay(model);
        if (model.keyboardOpen) drawKeyboard(model);
        if (model.playNowPrompt) drawPlayNowPrompt(model);
    }

    if (!model.toast.empty()) {
        const SDL_FRect toast{610, 970, 700, 72};
        panel(toast, {22, 28, 52, 250}, pink);
        text(model.toast, toast.x + toast.w / 2, toast.y + 20, 24, white, toast.w - 36, true);
    }
    if (model.scanning || model.artworkFetching) {
        // Keep background work progress in the bottom margin. The library panel
        // and its paging controls end above this strip, so neither border is
        // covered while a scan or artwork lookup is active.
        const SDL_FRect status{460, 1044, 940, 32};
        SDL_SetRenderDrawColor(renderer_, 10, 20, 38, 245);
        SDL_RenderFillRect(renderer_, &status);
        const std::size_t processed = model.scanning ? model.scanProcessed : model.artworkProcessed;
        const std::size_t total = model.scanning ? model.scanTotal : model.artworkTotal;
        const float ratio = total ? static_cast<float>(processed) / static_cast<float>(total) : 0.0F;
        SDL_FRect bar{status.x, status.y + status.h - 4, status.w * ratio, 4};
        const SDL_Color accent = model.scanning ? cyan : pink;
        SDL_SetRenderDrawColor(renderer_, accent.r, accent.g, accent.b, 255);
        SDL_RenderFillRect(renderer_, &bar);
        const std::string label = model.scanning
            ? "SCANNING " + std::to_string(processed) + "/" + std::to_string(total) +
                  "  " + ellipsize(model.scanFile, 42)
            : "ARTWORK " + std::to_string(processed) + "/" + std::to_string(total) +
                  "  FOUND " + std::to_string(model.artworkFound) +
                  "  " + ellipsize(model.artworkCurrent, 42);
        text(label, status.x + 12, status.y + 5, 15, muted, status.w - 24);
    }
    pruneTextCache();
    SDL_RenderPresent(renderer_);
}

std::optional<UiAction> UI::hitTest(float x, float y) const {
    for (auto found = hits_.rbegin(); found != hits_.rend(); ++found) {
        if (contains(found->rect, x, y)) return found->action;
    }
    return std::nullopt;
}

void UI::drawBrowse(const UiModel& model) {
    const bool browseMode = model.mode == UiMode::Browse;
    const bool genreButtonEnabled = browseMode && model.credits > 0 &&
                                !model.keyboardOpen && !model.playNowPrompt && !model.visualizerOpen;
    const bool visitorEnabled = genreButtonEnabled && !model.genreMenuOpen;
    const bool coinEnabled = browseMode && !model.keyboardOpen && !model.playNowPrompt &&
                             !model.visualizerOpen && !model.genreMenuOpen;
    text("NEON", 42, 27, 42, pink);
    text("JUKEBOX", 171, 27, 42, cyan);
    // Align the complete media-navigation group with the 940 px library panel.
    // Four controls plus three 12 px gaps fill x=460..1400 exactly.
    const SDL_FRect search{460, 22, 400, 64};
    button(search, model.search.empty() ? "SEARCH MEDIA" : ellipsize(model.search, 28),
           {UiActionKind::OpenKeyboard}, visitorEnabled, cyan);
    const std::string genreCaption = model.selectedGenre.empty()
        ? "ALL  ▼" : ellipsize(model.selectedGenre, 12) + "  ▼";
    button({872, 22, 168, 64}, genreCaption, {UiActionKind::ToggleGenreMenu},
           genreButtonEnabled,
           model.genreMenuOpen ? pink
                               : model.libraryFilter == LibraryFilter::All ? cyan : muted);
    button({1052, 22, 168, 64}, "MUSIC", {UiActionKind::ShowMusic}, visitorEnabled,
           model.libraryFilter == LibraryFilter::Music ? cyan : muted);
    button({1232, 22, 168, 64}, "VIDEO", {UiActionKind::ShowVideo}, visitorEnabled,
           model.libraryFilter == LibraryFilter::Video ? pink : muted);
    const auto count = model.library ? model.library->tracks.size() : 0;
    text(std::to_string(count) + " MEDIA", 1740, 44, 20, muted, 150, true);

    panel({30, 110, 410, 920}, panelFill, {38, 50, 82, 255});
    text("NOW PLAYING", 58, 138, 19, cyan);
    const SDL_FRect nowPlayingMedia{65, 178, 340, 340};
    if (model.videoPlaying) {
        panel(nowPlayingMedia, {0, 0, 0, 255}, panelBorder);
        text("VIDEO · TAP TO EXPAND", 300, 140, 14, pink, 200, true);
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
         235, 545, 30, white, 350, true);
    text(model.currentTrack ? ellipsize(model.currentTrack->artist, 34) : "Queue a track to begin",
         235, 588, 21, muted, 350, true);
    std::string currentMetadata;
    if (model.currentTrack && model.currentTrack->genre != "Unknown Genre") {
        currentMetadata = model.currentTrack->genre;
    }
    if (model.currentTrack && model.currentTrack->albumYear > 0) {
        if (!currentMetadata.empty()) currentMetadata += " · ";
        currentMetadata += std::to_string(model.currentTrack->albumYear);
    }
    text(ellipsize(currentMetadata, 36), 235, 612, 15, muted, 350, true);
    const float progress = model.playback.durationMs > 0
        ? std::clamp(static_cast<float>(model.playback.positionMs) / static_cast<float>(model.playback.durationMs), 0.0F, 1.0F) : 0.0F;
    // The progress and visualizer block starts one former credit-row lower so
    // artist/genre/year text always has clear space above the progress bar.
    SDL_FRect progressBack{65, 658, 340, 8};
    SDL_FRect progressFront{65, 658, 340 * progress, 8};
    SDL_SetRenderDrawColor(renderer_, 35, 43, 68, 255); SDL_RenderFillRect(renderer_, &progressBack);
    SDL_SetRenderDrawColor(renderer_, pink.r, pink.g, pink.b, 255); SDL_RenderFillRect(renderer_, &progressFront);
    text(formatDuration(model.playback.positionMs), 65, 675, 17, muted);
    text(formatDuration(model.playback.durationMs), 405, 675, 17, muted, 0, true);
    const SDL_FRect compactVisualizer{65, 716, 340, 120};
    drawVisualizer(compactVisualizer, model.visualizerMode);
    if (browseMode && !model.keyboardOpen && !model.playNowPrompt && !model.visualizerOpen) {
        addHit(compactVisualizer, {UiActionKind::OpenVisualizer});
    }

    text("SELECTED", 58, 842, 17, pink);
    text(model.selectedTrack ? ellipsize(model.selectedTrack->title, 30) : "Tap a track",
         58, 870, 23, white, 345);
    text(model.selectedTrack ? ellipsize(model.selectedTrack->artist, 34) : "",
         58, 900, 17, muted, 345);
    std::string selectedMetadata;
    if (model.selectedTrack && model.selectedTrack->genre != "Unknown Genre") {
        selectedMetadata = model.selectedTrack->genre;
    }
    if (model.selectedTrack && model.selectedTrack->albumYear > 0) {
        if (!selectedMetadata.empty()) selectedMetadata += " · ";
        selectedMetadata += std::to_string(model.selectedTrack->albumYear);
    }
    text(ellipsize(selectedMetadata, 38), 58, 924, 14, muted, 345);
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
    button({240, 946, 172, 64}, "ADD TO QUEUE", {UiActionKind::AddSelected},
           visitorEnabled && model.selectedTrack != nullptr, pink);

    panel({460, 110, 940, 920}, {9, 13, 29, 238}, panelBorder);
    std::string libraryTitle = model.libraryFilter == LibraryFilter::Music ? "MUSIC LIBRARY" :
                               model.libraryFilter == LibraryFilter::Video ? "VIDEO LIBRARY" :
                               model.libraryFilter == LibraryFilter::Favorites ? "FAVORITES" : "ALL MEDIA";
    if (!model.selectedGenre.empty()) {
        libraryTitle = ellipsize(model.selectedGenre, 18) + " / " + libraryTitle;
    }
    text(!visitorEnabled && browseMode ? "INSERT COIN TO CHOOSE MEDIA" : libraryTitle, 490, 136, 24,
         !visitorEnabled && browseMode ? pink : white);
    const auto filteredCount = model.filtered ? model.filtered->size() : 0;
    text(std::to_string(filteredCount) + " RESULTS", 1280, 141, 17, muted, 90, true);
    if (!model.filtered || model.filtered->empty()) {
        text(model.scanning ? "Building your library..." : "No matching media found",
             930, 485, 30, muted, 700, true);
    } else {
        const std::size_t start = model.page * pageSize;
        for (std::size_t visible = 0; visible < pageSize && start + visible < model.filtered->size(); ++visible) {
            const std::size_t trackIndex = (*model.filtered)[start + visible];
            const auto& track = model.library->tracks[trackIndex];
            const int column = static_cast<int>(visible % 3);
            const int row = static_cast<int>(visible / 3);
            const SDL_FRect card{490.0F + column * 296.0F, 180.0F + row * 260.0F, 276, 238};
            const bool selected = model.selectedTrack && model.selectedTrack->id == track.id;
            panel(card, selected ? SDL_Color{25, 25, 57, 255} : SDL_Color{14, 19, 40, 255}, selected ? pink : panelBorder);
            drawCover(&track, {card.x + 16, card.y + 14, 112, 112});
            text(track.favorite ? "★" : "", card.x + 236, card.y + 14, 24, pink);
            if (track.mediaKind == MediaKind::Video)
                text("VIDEO", card.x + 182, card.y + 105, 13, pink, 70, true);
            text(ellipsize(track.title, 25), card.x + 16, card.y + 140, 21, white, card.w - 32);
            text(ellipsize(track.artist, 28), card.x + 16, card.y + 172, 17, cyan, card.w - 32);
            std::string cardMetadata = formatDuration(track.durationMs);
            if (track.genre != "Unknown Genre") cardMetadata += " · " + track.genre;
            if (track.albumYear > 0) cardMetadata += " · " + std::to_string(track.albumYear);
            text(ellipsize(cardMetadata, 32), card.x + 16, card.y + 204, 16, muted,
                 card.w - 32);
            if (visitorEnabled) addHit(card, {UiActionKind::SelectTrack, trackIndex});
        }
        const std::size_t pages = (filteredCount + pageSize - 1) / pageSize;
        if (pages > 1) {
            button({490, 950, 190, 68}, "PREVIOUS", {UiActionKind::PagePrevious},
                   visitorEnabled && model.page > 0, cyan);
            text("PAGE " + std::to_string(model.page + 1) + " / " + std::to_string(pages),
                 930, 972, 18, muted, 250, true);
            button({1168, 950, 190, 68}, "NEXT", {UiActionKind::PageNext},
                   visitorEnabled && model.page + 1 < pages, cyan);
        }
    }

    panel({1420, 110, 470, 920}, panelFill, {38, 50, 82, 255});
    text("UP NEXT", 1450, 138, 24, white);
    const std::size_t queued = model.queue ? model.queue->size() : 0;
    text(std::to_string(queued) + " REQUESTS", 1800, 141, 17, pink, 70, true);
    if (!model.queue || model.queue->empty()) {
        text("Automatic shuffle is active", 1655, 480, 25, muted, 360, true);
    } else {
        const std::size_t shown = std::min<std::size_t>(10, model.queue->size());
        for (std::size_t i = 0; i < shown; ++i) {
            const auto* queuedTrack = LibraryScanner::find(*model.library, (*model.queue)[i].trackId);
            const SDL_FRect row{1445, 184.0F + i * 76.0F, 420, 64};
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

    if (browseMode && model.genreMenuOpen) {
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
        const SDL_FRect menu{872, 96, 400, menuHeight};

        SDL_SetRenderDrawColor(renderer_, 2, 4, 12, 150);
        const SDL_FRect shade{0, 90, 1920, 990};
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

    button({700, 760, 520, 86}, "CONTINUE TO JUKEBOX", {UiActionKind::FinishFolderSetup},
           model.musicSourceCount + model.videoSourceCount > 0, pink);
}

void UI::drawPinPad(const UiModel& model, bool cancelAllowed) {
    const SDL_FRect overlay{560, cancelAllowed ? 120.0F : 240.0F, 800, 800};
    panel(overlay, {8, 12, 29, 252}, pink);
    const std::string title = model.mode == UiMode::ChangePin ? "CHANGE ADMIN PIN" :
                              cancelAllowed ? "ADMIN ACCESS" : "CREATE ADMIN PIN";
    text(title, 960, overlay.y + 55, 36, white, 700, true);
    text(model.pinPrompt, 960, overlay.y + 112, 20, muted, 680, true);
    std::string dots;
    for (std::size_t i = 0; i < model.pinLength; ++i) dots += "● ";
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
    const SDL_FRect shade{0, 0, 1920, 1080};
    SDL_SetRenderDrawColor(renderer_, 2, 4, 12, 170); SDL_RenderFillRect(renderer_, &shade);
    addHit(shade, {UiActionKind::None});
    panel({80, 330, 1760, 750}, {9, 13, 29, 254}, cyan);
    text("SEARCH MEDIA", 140, 365, 28, cyan);
    text("Type a title, artist, or album, then press ENTER", 1770, 370, 18, muted, 720, true);
    panel({140, 410, 1640, 82}, {17, 24, 48, 255}, panelBorder);
    text(model.searchDraft.empty() ? "Type a title, artist, or album" : model.searchDraft,
         175, 434, 28, model.searchDraft.empty() ? muted : white, 1550);
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
    const auto number = static_cast<std::size_t>(model.visualizerMode) + 1;
    text("VISUALIZER " + std::string(number < 10 ? "0" : "") + std::to_string(number) +
             " / " + std::to_string(visualizerModeCount),
         125, 102, 20, pink);
    text(VisualizerRenderer::name(model.visualizerMode), 125, 137, 42, white);
    text(VisualizerRenderer::subtitle(model.visualizerMode), 125, 193, 20, muted, 1250);
    button({1600, 92, 190, 68}, "CLOSE", {UiActionKind::CloseVisualizer}, true, muted);

    const SDL_FRect display{125, 245, 1670, 610};
    panel({display.x - 2, display.y - 2, display.w + 4, display.h + 4},
          {5, 7, 18, 255}, panelBorder);
    drawVisualizer(display, model.visualizerMode);

    button({125, 895, 260, 76}, "PREVIOUS", {UiActionKind::VisualizerPrevious}, true, cyan);
    text("SWIPE LEFT OR RIGHT TO CHANGE STYLE", 960, 916, 18, muted, 720, true);
    button({1535, 895, 260, 76}, "NEXT", {UiActionKind::VisualizerNext}, true, pink);
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
    panel({180, 90, 1560, 900}, {9, 13, 29, 252}, pink);
    text("ADMIN CONTROL", 240, 135, 36, pink);
    button({1490, 120, 190, 64}, "CLOSE", {UiActionKind::AdminClose}, true, muted);

    text("PLAYBACK", 240, 230, 20, cyan);
    button({240, 270, 220, 78}, model.playback.state == PlaybackState::Paused ? "RESUME" : "PAUSE",
           {UiActionKind::AdminPlayPause}, model.currentTrack != nullptr, cyan);
    button({480, 270, 160, 78}, "SKIP", {UiActionKind::AdminSkip}, model.currentTrack != nullptr, pink);
    button({660, 270, 160, 78}, "-15 SEC", {UiActionKind::AdminSeekBackward}, model.currentTrack != nullptr, cyan);
    button({840, 270, 160, 78}, "+15 SEC", {UiActionKind::AdminSeekForward}, model.currentTrack != nullptr, cyan);
    button({1020, 270, 140, 78}, "VOL -", {UiActionKind::AdminVolumeDown}, true, cyan);
    button({1180, 270, 140, 78}, "VOL +", {UiActionKind::AdminVolumeUp}, true, cyan);
    text("VOLUME " + std::to_string(static_cast<int>(model.playback.volume * 100.0F)) + "%", 1370, 295, 22, white);

    text("LIBRARY & DISPLAY", 240, 410, 20, cyan);
    button({240, 450, 200, 76}, "RESCAN", {UiActionKind::AdminRescan}, !model.scanning, cyan);
    button({460, 450, 250, 76}, "MUSIC SOURCES (" + std::to_string(model.musicSourceCount) + ")",
           {UiActionKind::AdminChooseMusicFolders}, !model.scanning, cyan);
    button({730, 450, 250, 76}, "VIDEO SOURCES (" + std::to_string(model.videoSourceCount) + ")",
           {UiActionKind::AdminChooseVideoFolders}, !model.scanning, pink);
    button({1000, 450, 190, 76}, "ARTWORK", {UiActionKind::AdminUseArtwork}, true,
           model.nowPlayingArtworkMode == NowPlayingArtworkMode::Artwork ? pink : panelBorder);
    button({1210, 450, 190, 76}, "SPINNING CD", {UiActionKind::AdminUseSpinningDisc}, true,
           model.nowPlayingArtworkMode == NowPlayingArtworkMode::SpinningDisc ? pink : panelBorder);
    button({1420, 450, 200, 76}, model.selectedTrack && model.selectedTrack->favorite ? "UNFAVORITE" : "FAVORITE",
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
    button({1120, 870, 220, 74}, "CHANGE PIN", {UiActionKind::AdminChangePin}, true, cyan);
    button({1360, 870, 220, 74}, "EXIT JUKEBOX", {UiActionKind::AdminExit}, true, danger);
}

void UI::drawVisualizer(const SDL_FRect& rect, VisualizerMode mode) {
    const float targetScale = std::clamp(std::max(2.0F, outputScale_), 1.0F, 4.0F);
    const int targetWidth = std::max(1, static_cast<int>(std::ceil(rect.w * targetScale)));
    const int targetHeight = std::max(1, static_cast<int>(std::ceil(rect.h * targetScale)));
    auto& target = visualizerTargets_[rect.w >= 700.0F ? 1U : 0U];

    if (target.texture && (target.width != targetWidth || target.height != targetHeight)) {
        SDL_DestroyTexture(target.texture);
        target = {};
    }
    if (!target.texture) {
        target.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET,
                                           targetWidth, targetHeight);
        if (target.texture) {
            target.width = targetWidth;
            target.height = targetHeight;
            SDL_SetTextureScaleMode(target.texture, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(target.texture, SDL_BLENDMODE_NONE);
        }
    }

    SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer_);
    float previousScaleX = 1.0F;
    float previousScaleY = 1.0F;
    SDL_GetRenderScale(renderer_, &previousScaleX, &previousScaleY);
    if (!target.texture || !SDL_SetRenderTarget(renderer_, target.texture)) {
        textRasterScale_ = outputScale_;
        visualizer_.draw(renderer_, rect, mode);
        return;
    }

    SDL_SetRenderScale(renderer_, targetScale, targetScale);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    textRasterScale_ = targetScale;
    visualizer_.draw(renderer_, {0.0F, 0.0F, rect.w, rect.h}, mode);

    SDL_SetRenderTarget(renderer_, previousTarget);
    SDL_SetRenderScale(renderer_, previousScaleX, previousScaleY);
    textRasterScale_ = outputScale_;
    SDL_RenderTexture(renderer_, target.texture, nullptr, &rect);
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

void UI::drawSpinningDisc(const Track* track, const SDL_FRect& rect) {
    panel(rect, {4, 6, 15, 255}, panelBorder);
    if (!track) {
        text("♪", rect.x + rect.w / 2.0F, rect.y + rect.h / 2.0F - 35.0F,
             static_cast<int>(std::min(rect.w, rect.h) * 0.32F), cyan, 0, true);
        return;
    }
    SDL_Texture* artwork = artwork_.get(renderer_, *track);
    if (!artwork) return;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    const float centerX = rect.x + rect.w * 0.5F;
    const float centerY = rect.y + rect.h * 0.5F;
    const float radius = std::min(rect.w, rect.h) * 0.465F;

    // Soft shadow and machined outer rim remain stationary while the printed
    // album artwork rotates underneath the transparent CD highlights.
    fillCircle(renderer_, centerX + 4.0F, centerY + 7.0F, radius,
               {0, 0, 0, 145});
    fillCircle(renderer_, centerX, centerY, radius, {180, 192, 205, 255});
    fillCircle(renderer_, centerX, centerY, radius * 0.972F, {24, 29, 39, 255});

    constexpr int segments = 128;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(segments + 2);
    indices.reserve(segments * 3);
    const SDL_FColor opaqueWhite{1.0F, 1.0F, 1.0F, 1.0F};
    vertices.push_back({{centerX, centerY}, opaqueWhite, {0.5F, 0.5F}});
    const float artworkRadius = radius * 0.945F;
    const float rotation = discRotationDegrees_ * uiPi / 180.0F;
    for (int segment = 0; segment <= segments; ++segment) {
        const float angle = static_cast<float>(segment) / static_cast<float>(segments) *
                            uiPi * 2.0F;
        const float textureAngle = angle - rotation;
        vertices.push_back({
            {centerX + std::cos(angle) * artworkRadius,
             centerY + std::sin(angle) * artworkRadius},
            opaqueWhite,
            {0.5F + std::cos(textureAngle) * 0.5F,
             0.5F + std::sin(textureAngle) * 0.5F}
        });
        if (segment < segments) {
            indices.push_back(0);
            indices.push_back(segment + 1);
            indices.push_back(segment + 2);
        }
    }
    SDL_RenderGeometry(renderer_, artwork, vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));

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
    panel(rect, active ? SDL_Color{22, 29, 58, 255} : SDL_Color{13, 17, 31, 230}, active ? accent : panelBorder);
    text(label, rect.x + rect.w / 2, rect.y + rect.h / 2 - 13, rect.h >= 76 ? 23 : 18,
         active ? white : muted, rect.w - 24, true);
    if (active) addHit(rect, action);
}

void UI::text(std::string_view value, float x, float y, int size, SDL_Color color,
              float maxWidth, bool centered, float maxHeight, bool meterFont) {
    if (value.empty()) return;
    auto& entry = cachedText(value, size, color, meterFont);
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
    if (touch.w < 72.0F) { touch.x -= (72.0F - touch.w) * 0.5F; touch.w = 72.0F; }
    if (touch.h < 72.0F) { touch.y -= (72.0F - touch.h) * 0.5F; touch.h = 72.0F; }
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

TTF_Font* UI::font(int size, bool meterFont) {
    const int rasterSize = std::max(1, static_cast<int>(std::lround(
        static_cast<float>(size) * textRasterScale_)));
    auto& collection = meterFont ? meterFonts_ : size >= 26 ? boldFonts_ : regularFonts_;
    if (const auto found = collection.find(rasterSize); found != collection.end()) return found->second;
    const auto& path = meterFont ? meterFontPath_ : size >= 26 ? boldFontPath_ : regularFontPath_;
    TTF_Font* loaded = TTF_OpenFont(path.c_str(), static_cast<float>(rasterSize));
    collection.emplace(rasterSize, loaded);
    return loaded;
}

UI::TextEntry& UI::cachedText(std::string_view value, int size, SDL_Color color,
                              bool meterFont) {
    const int scaleKey = static_cast<int>(std::lround(textRasterScale_ * 1000.0F));
    std::string key = std::string(meterFont ? "M:" : "U:") + std::to_string(size) + ':' +
                      std::to_string(scaleKey) + ':' + std::to_string(color.r) + ':' +
                      std::to_string(color.g) + ':' + std::to_string(color.b) + ':' + std::string(value);
    if (auto found = textCache_.find(key); found != textCache_.end()) {
        found->second.used = ++textClock_;
        return found->second;
    }
    TextEntry entry;
    if (auto* selectedFont = font(size, meterFont)) {
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

void UI::destroyVisualizerTargets() {
    for (auto& target : visualizerTargets_) {
        if (target.texture) SDL_DestroyTexture(target.texture);
        target = {};
    }
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
