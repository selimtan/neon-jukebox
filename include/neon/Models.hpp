#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "neon/Theme.hpp"

namespace neon {

enum class ArtworkSource { Embedded, Sidecar, Online, Generated };
enum class PlaybackState { Stopped, Playing, Paused, Error };
enum class AmbientMode { Off, Sequential, Shuffle };
enum class MediaKind { Music, Video };
enum class LibraryFilter { All, Music, Video, Favorites };
enum class NowPlayingArtworkMode { Artwork, SpinningDisc };
enum class VisualizerMode {
    AuroraSpectrum,
    ReferenceVu,
    NeonArcVu,
    MirrorStage,
    ChromaticWaterfall,
    OrbitVinyl,
    StereoVector,
    SignalRibbon,
    StudioLed,
    PrecisionLevels,
    CavaMonstercat,
    PrismReflect,
    PhosphorScope,
    LissajousPro,
    RadialInferno,
    CircularWave,
    SpectrogramMagma,
    MilkdropMesh,
    ParticleGalaxy,
    MasteringDashboard,
    VintageFlatVu,
    OwLevelMeter,
    RackmountSpectrum,
    GreenDbMeter,
    SpectrumSkyline,
    NeonMosaic,
    TripleSoundMeter,
    WarmTwinVu,
    RetroPhosphorScope
};
inline constexpr std::size_t visualizerModeCount = 29;
inline constexpr auto neonVisualizerModes = [] {
    std::array<VisualizerMode, 28> modes{};
    for (std::size_t i = 0; i < modes.size(); ++i) modes[i] = static_cast<VisualizerMode>(i);
    return modes;
}();
inline constexpr std::array retroVisualizerModes{VisualizerMode::RetroPhosphorScope};

constexpr std::span<const VisualizerMode> visualizersForTheme(Theme theme) {
    if (theme == Theme::Retro) return retroVisualizerModes;
    return neonVisualizerModes;
}

constexpr std::size_t themeVisualizerIndex(Theme theme, VisualizerMode mode) {
    const auto modes = visualizersForTheme(theme);
    for (std::size_t i = 0; i < modes.size(); ++i) if (modes[i] == mode) return i;
    return 0;
}

constexpr VisualizerMode themeVisualizer(Theme theme, VisualizerMode mode) {
    return visualizersForTheme(theme)[themeVisualizerIndex(theme, mode)];
}

constexpr VisualizerMode adjacentVisualizer(Theme theme, VisualizerMode mode, bool forward) {
    const auto modes = visualizersForTheme(theme);
    const auto current = themeVisualizerIndex(theme, mode);
    return modes[(current + (forward ? 1 : modes.size() - 1)) % modes.size()];
}

struct Track {
    std::string id;
    std::filesystem::path path;
    std::string title;
    std::string artist{"Unknown Artist"};
    std::string album{"Unknown Album"};
    std::string genre{"Unknown Genre"};
    int albumYear{};
    std::int64_t durationMs{};
    int trackNumber{};
    std::uintmax_t fileSize{};
    std::int64_t modifiedTicks{};
    bool favorite{};
    bool hasEmbeddedArtwork{};
    std::optional<std::filesystem::path> sidecarArtwork;
    std::optional<std::filesystem::path> onlineArtwork;
    MediaKind mediaKind{MediaKind::Music};
};

struct QueueItem {
    std::string id;
    std::string trackId;
    std::int64_t requestedAtMs{};
};

struct PinRecord {
    std::string saltHex;
    std::string hashHex;
    std::uint32_t iterations{200000};

    [[nodiscard]] bool configured() const { return !saltHex.empty() && !hashHex.empty(); }
};

struct Settings {
    int schemaVersion{5};
    Theme theme{Theme::Neon};
    std::vector<std::filesystem::path> musicRoots;
    std::vector<std::filesystem::path> videoRoots;
    PinRecord adminPin;
    float volume{0.8F};
    AmbientMode ambientMode{AmbientMode::Shuffle};
    bool ambientRepeat{true};
    std::optional<MediaKind> ambientMediaKind;
    std::int64_t playbackPositionMs{};
    std::string currentTrackId;
    bool playbackWasActive{};
    bool currentTrackManual{};
    VisualizerMode visualizerMode{VisualizerMode::AuroraSpectrum};
    VisualizerMode retroVisualizerMode{VisualizerMode::RetroPhosphorScope};
    NowPlayingArtworkMode nowPlayingArtworkMode{NowPlayingArtworkMode::Artwork};
};

struct PlaybackSnapshot {
    PlaybackState state{PlaybackState::Stopped};
    std::string trackId;
    std::int64_t positionMs{};
    std::int64_t durationMs{};
    float volume{0.8F};
    std::string error;
};

struct LibraryIndex {
    int schemaVersion{5};
    std::vector<std::filesystem::path> musicRoots;
    std::vector<std::filesystem::path> videoRoots;
    std::vector<Track> tracks;
    std::int64_t scannedAtMs{};
};

void to_json(nlohmann::json& json, const Track& track);
void from_json(const nlohmann::json& json, Track& track);
void to_json(nlohmann::json& json, const QueueItem& item);
void from_json(const nlohmann::json& json, QueueItem& item);
void to_json(nlohmann::json& json, const PinRecord& pin);
void from_json(const nlohmann::json& json, PinRecord& pin);
void to_json(nlohmann::json& json, const Settings& settings);
void from_json(const nlohmann::json& json, Settings& settings);
void to_json(nlohmann::json& json, const LibraryIndex& index);
void from_json(const nlohmann::json& json, LibraryIndex& index);

}  // namespace neon
