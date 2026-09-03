#include "neon/Models.hpp"

#include <nlohmann/json.hpp>

#include "neon/Utils.hpp"

namespace neon {

namespace {

std::string ambientToString(AmbientMode mode) {
    switch (mode) {
        case AmbientMode::Sequential: return "sequential";
        case AmbientMode::Shuffle: return "shuffle";
        default: return "off";
    }
}

AmbientMode ambientFromString(std::string_view value) {
    if (value == "sequential") return AmbientMode::Sequential;
    if (value == "shuffle") return AmbientMode::Shuffle;
    return AmbientMode::Off;
}

std::string visualizerToString(VisualizerMode mode) {
    switch (mode) {
        case VisualizerMode::ReferenceVu: return "reference-vu";
        case VisualizerMode::NeonArcVu: return "neon-arc-vu";
        case VisualizerMode::MirrorStage: return "mirror-stage";
        case VisualizerMode::ChromaticWaterfall: return "chromatic-waterfall";
        case VisualizerMode::OrbitVinyl: return "orbit-vinyl";
        case VisualizerMode::StereoVector: return "stereo-vector";
        case VisualizerMode::SignalRibbon: return "signal-ribbon";
        case VisualizerMode::StudioLed: return "studio-led";
        case VisualizerMode::PrecisionLevels: return "precision-levels";
        case VisualizerMode::CavaMonstercat: return "cava-monstercat";
        case VisualizerMode::PrismReflect: return "prism-reflect";
        case VisualizerMode::PhosphorScope: return "phosphor-scope";
        case VisualizerMode::LissajousPro: return "lissajous-pro";
        case VisualizerMode::RadialInferno: return "radial-inferno";
        case VisualizerMode::CircularWave: return "circular-wave";
        case VisualizerMode::SpectrogramMagma: return "spectrogram-magma";
        case VisualizerMode::MilkdropMesh: return "milkdrop-mesh";
        case VisualizerMode::ParticleGalaxy: return "particle-galaxy";
        case VisualizerMode::MasteringDashboard: return "mastering-dashboard";
        case VisualizerMode::VintageFlatVu: return "vintage-flat-vu";
        case VisualizerMode::OwLevelMeter: return "ow-level-meter";
        case VisualizerMode::RackmountSpectrum: return "rackmount-spectrum";
        case VisualizerMode::GreenDbMeter: return "green-db-meter";
        case VisualizerMode::SpectrumSkyline: return "spectrum-skyline";
        case VisualizerMode::NeonMosaic: return "neon-mosaic";
        case VisualizerMode::TripleSoundMeter: return "triple-sound-meter";
        case VisualizerMode::WarmTwinVu: return "warm-twin-vu";
        default: return "aurora-spectrum";
    }
}

VisualizerMode visualizerFromString(std::string_view value) {
    if (value == "reference-vu") return VisualizerMode::ReferenceVu;
    if (value == "neon-arc-vu") return VisualizerMode::NeonArcVu;
    if (value == "mirror-stage") return VisualizerMode::MirrorStage;
    if (value == "chromatic-waterfall") return VisualizerMode::ChromaticWaterfall;
    if (value == "orbit-vinyl") return VisualizerMode::OrbitVinyl;
    if (value == "stereo-vector") return VisualizerMode::StereoVector;
    if (value == "signal-ribbon") return VisualizerMode::SignalRibbon;
    if (value == "studio-led") return VisualizerMode::StudioLed;
    if (value == "precision-levels") return VisualizerMode::PrecisionLevels;
    if (value == "cava-monstercat") return VisualizerMode::CavaMonstercat;
    if (value == "prism-reflect") return VisualizerMode::PrismReflect;
    if (value == "phosphor-scope") return VisualizerMode::PhosphorScope;
    if (value == "lissajous-pro") return VisualizerMode::LissajousPro;
    if (value == "radial-inferno") return VisualizerMode::RadialInferno;
    if (value == "circular-wave") return VisualizerMode::CircularWave;
    if (value == "spectrogram-magma") return VisualizerMode::SpectrogramMagma;
    if (value == "milkdrop-mesh") return VisualizerMode::MilkdropMesh;
    if (value == "particle-galaxy") return VisualizerMode::ParticleGalaxy;
    if (value == "mastering-dashboard") return VisualizerMode::MasteringDashboard;
    if (value == "vintage-flat-vu") return VisualizerMode::VintageFlatVu;
    if (value == "ow-level-meter") return VisualizerMode::OwLevelMeter;
    if (value == "rackmount-spectrum") return VisualizerMode::RackmountSpectrum;
    if (value == "green-db-meter") return VisualizerMode::GreenDbMeter;
    if (value == "spectrum-skyline") return VisualizerMode::SpectrumSkyline;
    if (value == "neon-mosaic") return VisualizerMode::NeonMosaic;
    if (value == "triple-sound-meter") return VisualizerMode::TripleSoundMeter;
    if (value == "warm-twin-vu") return VisualizerMode::WarmTwinVu;
    return VisualizerMode::AuroraSpectrum;
}

std::string mediaKindToString(MediaKind kind) {
    return kind == MediaKind::Video ? "video" : "music";
}

MediaKind mediaKindFromString(std::string_view value) {
    return value == "video" ? MediaKind::Video : MediaKind::Music;
}

std::string nowPlayingArtworkModeToString(NowPlayingArtworkMode mode) {
    return mode == NowPlayingArtworkMode::SpinningDisc ? "spinning-disc" : "artwork";
}

NowPlayingArtworkMode nowPlayingArtworkModeFromString(std::string_view value) {
    return value == "spinning-disc" ? NowPlayingArtworkMode::SpinningDisc
                                    : NowPlayingArtworkMode::Artwork;
}

nlohmann::json pathsToJson(const std::vector<std::filesystem::path>& paths) {
    auto values = nlohmann::json::array();
    for (const auto& path : paths) values.push_back(pathToUtf8(path));
    return values;
}

std::vector<std::filesystem::path> pathsFromJson(const nlohmann::json& json,
                                                 std::string_view arrayName,
                                                 std::string_view legacyName) {
    std::vector<std::filesystem::path> paths;
    const auto array = json.find(arrayName);
    if (array != json.end() && array->is_array()) {
        for (const auto& value : *array) {
            if (value.is_string() && !value.get_ref<const std::string&>().empty()) {
                paths.push_back(pathFromUtf8(value.get_ref<const std::string&>()));
            }
        }
    } else {
        const auto legacy = json.value(std::string(legacyName), std::string{});
        if (!legacy.empty()) paths.push_back(pathFromUtf8(legacy));
    }
    return paths;
}

}  // namespace

void to_json(nlohmann::json& json, const Track& track) {
    json = {
        {"id", track.id}, {"path", pathToUtf8(track.path)}, {"title", track.title},
        {"artist", track.artist}, {"album", track.album}, {"genre", track.genre},
        {"albumYear", track.albumYear}, {"durationMs", track.durationMs},
        {"trackNumber", track.trackNumber}, {"fileSize", track.fileSize},
        {"modifiedTicks", track.modifiedTicks}, {"favorite", track.favorite},
        {"hasEmbeddedArtwork", track.hasEmbeddedArtwork},
        {"mediaKind", mediaKindToString(track.mediaKind)}
    };
    if (track.sidecarArtwork) json["sidecarArtwork"] = pathToUtf8(*track.sidecarArtwork);
    if (track.onlineArtwork) json["onlineArtwork"] = pathToUtf8(*track.onlineArtwork);
}

void from_json(const nlohmann::json& json, Track& track) {
    track.id = json.value("id", "");
    track.path = pathFromUtf8(json.value("path", ""));
    track.title = json.value("title", track.path.stem().string());
    track.artist = json.value("artist", "Unknown Artist");
    track.album = json.value("album", "Unknown Album");
    track.genre = json.value("genre", "Unknown Genre");
    track.albumYear = json.value("albumYear", 0);
    track.durationMs = json.value("durationMs", std::int64_t{});
    track.trackNumber = json.value("trackNumber", 0);
    track.fileSize = json.value("fileSize", std::uintmax_t{});
    track.modifiedTicks = json.value("modifiedTicks", std::int64_t{});
    track.favorite = json.value("favorite", false);
    track.hasEmbeddedArtwork = json.value("hasEmbeddedArtwork", false);
    track.mediaKind = mediaKindFromString(json.value("mediaKind", "music"));
    if (json.contains("sidecarArtwork")) track.sidecarArtwork = pathFromUtf8(json.at("sidecarArtwork").get<std::string>());
    if (json.contains("onlineArtwork")) track.onlineArtwork = pathFromUtf8(json.at("onlineArtwork").get<std::string>());
}

void to_json(nlohmann::json& json, const QueueItem& item) {
    json = {{"id", item.id}, {"trackId", item.trackId}, {"requestedAtMs", item.requestedAtMs}};
}

void from_json(const nlohmann::json& json, QueueItem& item) {
    item.id = json.value("id", "");
    item.trackId = json.value("trackId", "");
    item.requestedAtMs = json.value("requestedAtMs", std::int64_t{});
}

void to_json(nlohmann::json& json, const PinRecord& pin) {
    json = {{"salt", pin.saltHex}, {"hash", pin.hashHex}, {"iterations", pin.iterations}};
}

void from_json(const nlohmann::json& json, PinRecord& pin) {
    pin.saltHex = json.value("salt", "");
    pin.hashHex = json.value("hash", "");
    pin.iterations = json.value("iterations", 200000U);
}

void to_json(nlohmann::json& json, const Settings& settings) {
    json = {
        {"schemaVersion", 5}, {"musicRoots", pathsToJson(settings.musicRoots)},
        {"videoRoots", pathsToJson(settings.videoRoots)},
        {"adminPin", settings.adminPin}, {"volume", settings.volume},
        {"ambientMode", ambientToString(settings.ambientMode)}, {"ambientRepeat", settings.ambientRepeat},
        {"playbackPositionMs", settings.playbackPositionMs}, {"currentTrackId", settings.currentTrackId},
        {"playbackWasActive", settings.playbackWasActive},
        {"currentTrackManual", settings.currentTrackManual},
        {"visualizerMode", visualizerToString(settings.visualizerMode)},
        {"nowPlayingArtworkMode",
         nowPlayingArtworkModeToString(settings.nowPlayingArtworkMode)}
    };
}

void from_json(const nlohmann::json& json, Settings& settings) {
    settings.schemaVersion = 5;
    if (json.contains("musicRoots")) {
        settings.musicRoots = pathsFromJson(json, "musicRoots", "libraryRoot");
    } else {
        settings.musicRoots = pathsFromJson(json, "libraryRoots", "libraryRoot");
    }
    settings.videoRoots = pathsFromJson(json, "videoRoots", "videoRoot");
    if (json.contains("adminPin")) settings.adminPin = json.at("adminPin").get<PinRecord>();
    settings.volume = json.value("volume", 0.8F);
    settings.ambientMode = ambientFromString(json.value("ambientMode", "shuffle"));
    settings.ambientRepeat = json.value("ambientRepeat", true);
    settings.playbackPositionMs = json.value("playbackPositionMs", std::int64_t{});
    settings.currentTrackId = json.value("currentTrackId", "");
    settings.playbackWasActive = json.value("playbackWasActive", false);
    settings.currentTrackManual = json.value("currentTrackManual", false);
    settings.visualizerMode = visualizerFromString(json.value("visualizerMode", "aurora-spectrum"));
    settings.nowPlayingArtworkMode = nowPlayingArtworkModeFromString(
        json.value("nowPlayingArtworkMode", "artwork"));
}

void to_json(nlohmann::json& json, const LibraryIndex& index) {
    json = {{"schemaVersion", 5}, {"musicRoots", pathsToJson(index.musicRoots)},
            {"videoRoots", pathsToJson(index.videoRoots)},
            {"tracks", index.tracks}, {"scannedAtMs", index.scannedAtMs}};
}

void from_json(const nlohmann::json& json, LibraryIndex& index) {
    const int sourceSchema = json.value("schemaVersion", 1);
    index.schemaVersion = 5;
    if (json.contains("musicRoots")) {
        index.musicRoots = pathsFromJson(json, "musicRoots", "root");
    } else {
        index.musicRoots = pathsFromJson(json, "roots", "root");
    }
    index.videoRoots = pathsFromJson(json, "videoRoots", "videoRoot");
    index.tracks = json.value("tracks", std::vector<Track>{});
    // Versions 4 and 5 add genre and album-year metadata. Invalidate only the
    // cheap file cache keys so older libraries are re-read once without
    // discarding paths or artwork state.
    if (sourceSchema < 5) {
        for (auto& track : index.tracks) track.fileSize = 0;
    }
    index.scannedAtMs = json.value("scannedAtMs", std::int64_t{});
}

}  // namespace neon
