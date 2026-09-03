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
    ShowMusic, ShowVideo, OpenKeyboard,
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
    std::size_t adminQueueSelection{static_cast<std::size_t>(-1)};
    AmbientMode ambientMode{AmbientMode::Off};
    bool ambientRepeat{true};
    std::size_t musicSourceCount{};
    std::size_t videoSourceCount{};
};

class UI {
public:
    UI() = default;
    ~UI();
    UI(const UI&) = delete;
    UI& operator=(const UI&) = delete;

    bool initialize(SDL_Renderer* renderer, std::string& error);
    void shutdown();
    void render(const UiModel& model);
    [[nodiscard]] std::optional<UiAction> hitTest(float x, float y) const;

private:
    struct HitTarget { SDL_FRect rect; UiAction action; };
    struct TextEntry { SDL_Texture* texture{}; float width{}; float height{}; std::uint64_t used{}; };
    struct VisualizerTarget { SDL_Texture* texture{}; int width{}; int height{}; };

    void drawBrowse(const UiModel& model);
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
    void drawSpinningDisc(const Track* track, const SDL_FRect& rect);

    void panel(const SDL_FRect& rect, SDL_Color fill, SDL_Color border);
    void button(const SDL_FRect& rect, std::string text, UiAction action,
                bool active = false, SDL_Color accent = {0, 239, 255, 255});
    void text(std::string_view value, float x, float y, int size, SDL_Color color,
              float maxWidth = 0.0F, bool centered = false, float maxHeight = 0.0F,
              bool meterFont = false);
    void addHit(const SDL_FRect& rect, UiAction action);
    static bool contains(const SDL_FRect& rect, float x, float y);
    static std::string ellipsize(std::string_view value, std::size_t maximum);
    TTF_Font* font(int size, bool meterFont = false);
    TextEntry& cachedText(std::string_view value, int size, SDL_Color color,
                          bool meterFont = false);
    void refreshRenderMetrics();
    void destroyVisualizerTargets();
    void pruneTextCache();

    SDL_Renderer* renderer_{};
    std::string regularFontPath_;
    std::string boldFontPath_;
    std::string meterFontPath_;
    std::unordered_map<int, TTF_Font*> regularFonts_;
    std::unordered_map<int, TTF_Font*> boldFonts_;
    std::unordered_map<int, TTF_Font*> meterFonts_;
    std::unordered_map<std::string, TextEntry> textCache_;
    std::uint64_t textClock_{};
    float outputScale_{1.0F};
    float textRasterScale_{1.0F};
    float discRotationDegrees_{};
    std::uint64_t lastDiscTicks_{};
    std::array<VisualizerTarget, 2> visualizerTargets_{};
    std::vector<HitTarget> hits_;
    ArtworkCache artwork_;
    VisualizerRenderer visualizer_;
};

}  // namespace neon
