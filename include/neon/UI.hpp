#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "neon/Artwork.hpp"
#include "neon/Models.hpp"
#include "neon/Visualizer.hpp"

namespace neon {

enum class UiMode { SetupPin, SetupFolder, Browse, AdminPin, Admin, ChangePin };

enum class UiActionKind {
    None, OpenAdmin, OpenVisualizer, VisualizerPrevious, VisualizerNext, CloseVisualizer,
    ToggleVideoFullscreen, InsertCoin, SelectTrack, AddSelected,
    ToggleGenreMenu, CloseGenreMenu, SelectGenre, GenrePagePrevious, GenrePageNext,
    ShowMusic, ShowVideo, SelectArtistInitial, OpenKeyboard,
    KeyCharacter, KeyBackspace, KeySpace, KeyClear, SubmitSearch, CloseKeyboard,
    PlayRequestNow, WaitForCurrentTrack,
    PinDigit, PinBackspace, PinSubmit, PinCancel,
    ChooseMusicFolders, ChooseVideoFolders, FinishFolderSetup,
    PagePrevious, PageNext, AdminClose, AdminPlayPause, AdminSkip,
    AdminSeekBackward, AdminSeekForward,
    AdminVolumeDown, AdminVolumeUp, AdminClearQueue, AdminRescan,
    AdminChooseMusicFolders, AdminChooseVideoFolders,
    AdminCycleAmbient, AdminToggleRepeat, AdminToggleFavorite,
    AdminUseArtwork, AdminUseSpinningDisc,
    AdminSelectTheme,
    AdminChangePin,
    AdminExit, AdminQueueSelect, AdminQueueUp, AdminQueueDown, AdminQueueRemove
};

struct UiAction {
    UiActionKind kind{UiActionKind::None};
    std::size_t index{};
    char character{};
};

struct UiModel {
    UiMode mode{UiMode::Browse};
    Theme theme{Theme::Neon};
    const LibraryIndex* library{};
    const std::vector<std::size_t>* filtered{};
    const std::vector<QueueItem>* queue{};
    const Track* currentTrack{};
    const Track* selectedTrack{};
    PlaybackSnapshot playback;
    AudioVisualizationFrame visualization;
    std::string search;
    std::string searchDraft;
    const std::vector<std::string>* genres{};
    std::string selectedGenre;
    char selectedArtistInitial{};
    bool genreMenuOpen{};
    std::size_t genreMenuPage{};
    std::string pinPrompt;
    std::size_t pinLength{};
    LibraryFilter libraryFilter{LibraryFilter::All};
    bool keyboardOpen{};
    bool adminReveal{};
    bool visualizerOpen{};
    bool videoFullscreen{};
    bool videoPlaying{};
    std::string videoLoadingStatus;
    VisualizerMode visualizerMode{VisualizerMode::AuroraSpectrum};
    NowPlayingArtworkMode nowPlayingArtworkMode{NowPlayingArtworkMode::Artwork};
    std::size_t credits{};
    bool playNowPrompt{};
    std::string requestedTrackTitle;
    bool scanning{};
    std::size_t scanProcessed{};
    std::size_t scanTotal{};
    std::string scanFile;
    bool artworkFetching{};
    std::size_t artworkProcessed{};
    std::size_t artworkTotal{};
    std::size_t artworkFound{};
    std::string artworkCurrent;
    std::string toast;
    std::size_t page{};
    std::optional<std::size_t> pendingPage;
    std::size_t adminQueueSelection{static_cast<std::size_t>(-1)};
    AmbientMode ambientMode{AmbientMode::Off};
    bool ambientRepeat{true};
    std::size_t musicSourceCount{};
    std::size_t videoSourceCount{};

    [[nodiscard]] bool buildingEmptyLibrary() const {
        return scanning && (!library || library->tracks.empty()) &&
            !selectedArtistInitial && search.empty() && selectedGenre.empty();
    }
};

class UI {
public:
    // Text composited over transparent pixels, with premultiplied alpha.
    struct VideoInfoImage { SDL_Surface* pixels{}; std::uint64_t revision{}; };
    UI() = default;
    ~UI();
    UI(const UI&) = delete;
    UI& operator=(const UI&) = delete;

    bool initialize(SDL_Renderer* renderer, std::string& error);
    void shutdown();
    void cancelBackgroundWork();
    void render(const UiModel& model, std::uint64_t ticks = SDL_GetTicks());
    [[nodiscard]] const VideoInfoImage& videoInfoImage(const Track& track);
    void refreshArtwork(const Track& track);
    void useLibraryArtworkCache(const std::filesystem::path& libraryRoot);
    // Returns false while a mechanical leaf is already moving.
    bool beginPageTurn(bool forward);
    [[nodiscard]] bool pageTurnActive() const { return retroTurnDirection_ != 0; }
    [[nodiscard]] bool pageArtworkReady(const LibraryIndex& library,
        const std::vector<std::size_t>& filtered, Theme theme, std::size_t page,
        LibraryFilter filter = LibraryFilter::All) const;
    [[nodiscard]] bool takePageTurnSound();
    void updatePointer(float x, float y, bool down);
    void clearPointer();
    void notifyAction(const UiAction& action);
    [[nodiscard]] std::optional<UiAction> hitTest(float x, float y, std::uint64_t timestampNs = 0) const;
    [[nodiscard]] static SDL_FRect nowPlayingMediaRect(Theme theme, bool video = false) {
        if (theme == Theme::Retro)
            return video ? SDL_FRect{1450, 322, 416, 320} : SDL_FRect{1500, 322, 320, 320};
        return {80, 222, 310, 310};
    }

private:
    struct HitTarget { SDL_FRect rect; UiAction action; };
    struct TextEntry { SDL_Texture* texture{}; float width{}; float height{}; std::uint64_t used{}; };
    struct VisualizerTarget { SDL_Texture* texture{}; int width{}; int height{}; };

    void drawBrowse(const UiModel& model);
    void prepareArtwork(const UiModel& model);
    void drawRetroBrowse(const UiModel& model, std::uint64_t ticks);
    void drawRetroCards(const UiModel& model, bool enabled);
    void drawRetroVideoCards(const UiModel& model, bool enabled);
    void drawRetroCatalogue(const UiModel& model, bool enabled, std::uint64_t ticks);
    void resetRetroCatalogue();
    void drawGenreMenu(const UiModel& model);
    void chrome(const SDL_FRect& rect, bool screws = false);
    void phosphor(const SDL_FRect& rect);
    void drawRetroStatus(const UiModel& model, std::uint64_t ticks);
    void retroButton(const SDL_FRect& rect, std::string_view label, UiAction action,
                     bool enabled, bool selected = false);
    void drawSetupPin(const UiModel& model);
    void drawSetupFolder(const UiModel& model);
    void drawPinPad(const UiModel& model, bool cancelAllowed);
    void drawKeyboard(const UiModel& model);
    void drawPlayNowPrompt(const UiModel& model);
    void drawVisualizerOverlay(const UiModel& model);
    void drawVideoFullscreen();
    void drawAdmin(const UiModel& model);
    void drawVisualizer(const SDL_FRect& rect, VisualizerMode mode);
    void drawCover(const Track* track, const SDL_FRect& rect);
    void drawDiscCover(const Track* track, const SDL_FRect& rect, float rotationDegrees,
                       bool framed);
    void drawSpinningDisc(const Track* track, const SDL_FRect& rect, bool framed = true);

    void panel(const SDL_FRect& rect, SDL_Color fill, SDL_Color border);
    void button(const SDL_FRect& rect, std::string text, UiAction action,
                bool active = false, SDL_Color accent = {0, 239, 255, 255});
    void text(std::string_view value, float x, float y, int size, SDL_Color color,
              float maxWidth = 0.0F, bool centered = false, float maxHeight = 0.0F,
              bool meterFont = false, bool regularRetro = false);
    void addHit(const SDL_FRect& rect, UiAction action);
    static bool contains(const SDL_FRect& rect, float x, float y);
    static std::string ellipsize(std::string_view value, std::size_t maximum);
    TTF_Font* font(int size, bool meterFont = false, bool regularRetro = false);
    TextEntry& cachedText(std::string_view value, int size, SDL_Color color,
                          bool meterFont = false, bool regularRetro = false);
    void refreshRenderMetrics();
    void pruneTextCache();

    SDL_Renderer* renderer_{};
    Theme theme_{Theme::Neon};
    std::string retroFontPath_;
    std::unordered_map<int, TTF_Font*> retroFonts_;
    std::string retroRegularFontPath_;
    std::unordered_map<int, TTF_Font*> retroRegularFonts_;
    std::string regularFontPath_;
    std::string boldFontPath_;
    std::string meterFontPath_;
    std::unordered_map<int, TTF_Font*> regularFonts_;
    std::unordered_map<int, TTF_Font*> boldFonts_;
    std::unordered_map<int, TTF_Font*> meterFonts_;
    std::unordered_map<std::string, TextEntry> textCache_;
    VideoInfoImage videoInfoImage_;
    std::string videoInfoKey_;
    std::string videoInfoFontPath_;
    std::unordered_map<int, TTF_Font*> videoInfoFonts_;
    std::uint64_t textClock_{};
    float outputScale_{1.0F};
    float textRasterScale_{1.0F};
    float discRotationDegrees_{};
    std::uint64_t lastDiscTicks_{};
    std::array<VisualizerTarget, 2> retroCatalogueTargets_{};
    const LibraryIndex* retroCatalogueLibrary_{};
    std::string retroCatalogueFilter_;
    std::size_t retroCatalogueCount_{};
    std::size_t retroCatalogueTrackCount_{};
    std::size_t retroCataloguePage_{};
    std::size_t retroCatalogueSignature_{};
    bool retroCatalogueReady_{};
    bool retroTurnPending_{};
    bool retroTurnSoundPlayed_{};
    bool retroTurnSoundPending_{};
    int retroTurnDirection_{};
    std::uint64_t retroTurnElapsed_{};
    std::uint64_t retroTurnLastTick_{};
    std::uint64_t retroInputBlockedUntilNs_{};
    std::string retroStatusText_;
    std::string retroPreviousStatusText_;
    std::uint64_t retroStatusElapsed_{180};
    std::uint64_t retroStatusLastTick_{};
    std::optional<std::uint64_t> retroStatusCycleStart_;
    std::string retroStatusCycleTrack_;
    bool retroCoinPrompt_{};
    float pointerX_{-1};
    float pointerY_{-1};
    bool pointerDown_{};
    std::optional<UiAction> pressedAction_;
    std::optional<UiAction> feedbackAction_;
    std::uint64_t feedbackElapsed_{120};
    std::uint64_t feedbackLastTick_{};
    std::vector<HitTarget> hits_;
    bool pageArtworkPending_{};
    std::size_t artworkLastPage_{};
    bool artworkBrowseBackwards_{};
    ArtworkCache artwork_;
    ArtworkCache videoArtwork_{24, {}, 2, ArtworkCache::Kind::VideoFrame};
    VisualizerRenderer visualizer_;
};

}  // namespace neon
