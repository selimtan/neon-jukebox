#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <numeric>
#include <nlohmann/json.hpp>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "neon/UI.hpp"
#include "neon/Library.hpp"
#include "neon/Utils.hpp"
#include "neon/VideoThumbnail.hpp"
#include "neon/ArtworkPreparation.hpp"

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; ++failures; } } while (false)

bool actionAt(const neon::UI& ui, float x, float y, neon::UiActionKind kind,
              std::size_t index = 0) {
    const auto hit = ui.hitTest(x, y);
    return hit && hit->kind == kind && hit->index == index;
}

void verifyAsyncArtwork() {
    SDL_Surface* surface = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    CHECK(renderer != nullptr);
    if (!renderer) { if (surface) SDL_DestroySurface(surface); return; }
    {
        std::promise<void> release;
        auto gate = release.get_future().share();
        std::promise<void> started;
        auto loading = started.get_future();
        std::atomic_int loads{};
        std::atomic_bool background{true};
        const auto renderThread = std::this_thread::get_id();
        neon::ArtworkCache cache(1, [&](const neon::Track&) -> SDL_Surface* {
            if (std::this_thread::get_id() == renderThread) background = false;
            const int load = ++loads;
            if (load == 1) {
                started.set_value();
                gate.wait_for(std::chrono::seconds(2));
            }
            if (load >= 3) return nullptr; // Missing/corrupt artwork uses the fallback.
            auto* decoded = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
            const auto* format = SDL_GetPixelFormatDetails(decoded->format);
            SDL_FillSurfaceRect(decoded, nullptr, SDL_MapRGBA(format, nullptr,
                                load == 1 ? 255 : 0, 0, load == 1 ? 0 : 255, 255));
            return decoded;
        });
        neon::Track track;
        track.id = "aa";
        track.hasEmbeddedArtwork = true;
        const auto before = std::chrono::steady_clock::now();
        CHECK(cache.get(renderer, track) != nullptr);
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(150));
        CHECK(loading.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(background);
        cache.update(renderer);
        CHECK(cache.revision() == 0); // UI can render while the decoder is blocked.
        auto other = track;
        other.id = "bb";
        CHECK(cache.get(renderer, other) != nullptr);
        CHECK(cache.get(renderer, track) != nullptr); // Same key, new cache generation.
        release.set_value();
        const auto deadline = SDL_GetTicks() + 2000;
        while (cache.revision() == 0 && SDL_GetTicks() < deadline) {
            cache.update(renderer);
            SDL_Delay(1);
        }
        CHECK(cache.revision() == 1); // The evicted in-flight result must be discarded.
        SDL_RenderTexture(renderer, cache.get(renderer, track), nullptr, nullptr);
        SDL_RenderPresent(renderer);
        Uint8 red{}, green{}, blue{}, alpha{};
        CHECK(SDL_ReadSurfacePixel(surface, 16, 16, &red, &green, &blue, &alpha));
        CHECK(blue == 255 && red == 0);
        CHECK(loads == 2); // The superseded queued request was cancelled.
        cache.clear();
        CHECK(cache.get(renderer, track) != nullptr); // Worker can restart after cleanup.
        for (int frame = 0; frame < 50; ++frame) {
            cache.update(renderer);
            CHECK(cache.get(renderer, track) != nullptr);
            SDL_Delay(1);
        }
        CHECK(loads == 3); // Decode failures do not get retried on every frame.
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
}

void verifyArtworkPrefetch() {
    auto* surface = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    CHECK(renderer);
    if (!renderer) { SDL_DestroySurface(surface); return; }
    {
        std::promise<void> release;
        const auto gate = release.get_future().share();
        std::atomic_int active{}, peak{}, loads{};
        std::mutex loadedMutex;
        std::vector<std::string> loaded;
        neon::ArtworkCache cache(8, [&](const neon::Track& track) -> SDL_Surface* {
            const auto count = ++active;
            auto observed = peak.load();
            while (observed < count && !peak.compare_exchange_weak(observed, count)) {}
            ++loads;
            { std::scoped_lock lock(loadedMutex); loaded.push_back(track.id); }
            if (track.id == "aa" || track.id == "bb") gate.wait_for(std::chrono::seconds(3));
            --active;
            if (track.id == "ff") return nullptr;
            return SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
        }, 2);
        std::array<neon::Track, 6> tracks;
        for (int i = 0; i < 6; ++i) {
            tracks[i].id = std::string(2, static_cast<char>('a' + i));
            tracks[i].hasEmbeddedArtwork = true;
        }
        std::array<const neon::Track*, 4> initial{&tracks[0], &tracks[1], &tracks[2], &tracks[3]};
        const auto before = SDL_GetTicks();
        cache.prepare(initial);
        CHECK(SDL_GetTicks() - before < 150);
        const auto deadline = SDL_GetTicks() + 2000;
        while (active < 2 && SDL_GetTicks() < deadline) SDL_Delay(1);
        CHECK(active == 2); // Both decoders run without blocking the render thread.
        CHECK(!cache.ready(tracks[0]) && !cache.ready(tracks[1]));
        // A new destination supersedes queued neighbours before they touch disk.
        std::array<const neon::Track*, 2> destination{&tracks[4], &tracks[5]};
        for (int frame = 0; frame < 20; ++frame) {
            cache.prepare(destination);
            cache.update(renderer);
        }
        CHECK(loads == 2);
        release.set_value();
        const auto finish = SDL_GetTicks() + 3000;
        while ((!cache.ready(tracks[4]) || !cache.ready(tracks[5])) && SDL_GetTicks() < finish) {
            cache.prepare(destination);
            cache.update(renderer);
            SDL_Delay(1);
        }
        CHECK(cache.ready(tracks[4]) && cache.ready(tracks[5]));
        CHECK(!cache.ready(tracks[0])); // Stale in-flight results were discarded.
        CHECK(loads == 4 && peak == 2);
        {
            std::scoped_lock lock(loadedMutex);
            CHECK(std::find(loaded.begin(), loaded.end(), "cc") == loaded.end());
            CHECK(std::find(loaded.begin(), loaded.end(), "dd") == loaded.end());
        }
        const auto revision = cache.revision();
        CHECK(cache.get(renderer, tracks[4])); // First access is already the final cover.
        CHECK(cache.get(renderer, tracks[5])); // Corrupt/missing cover finishes with fallback.
        for (int frame = 0; frame < 20; ++frame) {
            cache.prepare(destination);
            cache.update(renderer);
        }
        CHECK(cache.revision() == revision && loads == 4);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
}

void verifyPageTurns(neon::UI& ui, neon::UiModel model, SDL_Surface* surface,
                     const std::filesystem::path& output) {
    std::uint64_t frameTick = 1000;
    const auto renderAt = [&](std::uint64_t target) {
        while (frameTick + 34 < target) {
            frameTick += 34;
            ui.render(model, frameTick);
        }
        frameTick = target;
        ui.render(model, target);
    };
    neon::LibraryIndex library = *model.library;
    while (library.tracks.size() < 61) {
        neon::Track track = library.tracks[(library.tracks.size() + 7) % 21];
        track.id = neon::makeStableId("turn-" + std::to_string(library.tracks.size()));
        library.tracks.push_back(std::move(track));
    }
    std::vector<std::size_t> filtered(library.tracks.size());
    std::iota(filtered.begin(), filtered.end(), 0);
    model.theme = neon::Theme::Retro;
    model.library = &library;
    model.filtered = &filtered;
    model.currentTrack = &library.tracks.front();
    model.selectedTrack = &library.tracks[1];
    model.page = 0;
    model.credits = 2;
    const float scale = std::min(surface->w / 1920.0F, surface->h / 1080.0F);
    const float offsetY = (surface->h - 1080 * scale) / 2;
    const float offsetX = (surface->w - 1920 * scale) / 2;
    const auto digest = [&](float x, float y, float w, float h) {
        std::uint64_t hash = 1469598103934665603ULL;
        SDL_LockSurface(surface);
        for (int row = static_cast<int>(offsetY + y * scale);
             row < static_cast<int>(offsetY + (y + h) * scale); row += 3) {
            const auto* pixels = static_cast<const unsigned char*>(surface->pixels) + row * surface->pitch;
            for (int col = static_cast<int>(offsetX + x * scale) * 4;
                 col < static_cast<int>(offsetX + (x + w) * scale) * 4; col += 4) {
                hash = (hash ^ pixels[col]) * 1099511628211ULL;
                hash = (hash ^ pixels[col + 1]) * 1099511628211ULL;
                hash = (hash ^ pixels[col + 2]) * 1099511628211ULL;
            }
        }
        SDL_UnlockSurface(surface);
        return hash;
    };
    const auto save = [&](std::string_view name) {
        if (output.empty()) return;
        const auto file = output / (std::string(name) + "-" + std::to_string(surface->w) + ".png");
        CHECK(IMG_SavePNG(surface, neon::pathToUtf8(file).c_str()));
    };
    renderAt(1000);
    const auto oldLeft = digest(60, 190, 600, 730);
    const auto oldRight = digest(740, 190, 620, 730);
    const auto console = digest(1440, 126, 430, 175);
    const auto artistBar = digest(48, 112, 1324, 48);
    const auto aboveLeft = digest(48, 112, 642, 60);
    const auto aboveRight = digest(715, 112, 657, 60);
    const auto aboveSpine = digest(696, 110, 12, 58);
    const auto leftHinge = digest(692, 200, 8, 740);
    const auto rightHinge = digest(704, 200, 8, 740);
    const auto searchControls = digest(48, 40, 1324, 62);
    const auto cabinetGutter = digest(1405, 112, 12, 932);
    CHECK(ui.beginPageTurn(true));
    CHECK(!ui.takePageTurnSound());
    CHECK(!ui.beginPageTurn(true));
    CHECK(!ui.beginPageTurn(false));
    CHECK(!ui.hitTest(300, 226)); // Also blocked before the next render/event batch.
    model.page = 1;
    ui.render(model, 1100);
    frameTick = 1100;
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    renderAt(1270);
    CHECK(!ui.hitTest(300, 226));
    CHECK(!ui.hitTest(1000, 226));
    CHECK(!ui.hitTest(1250, 992));
    CHECK(actionAt(ui, 1540, 1017, neon::UiActionKind::InsertCoin));
    CHECK(digest(60, 190, 600, 730) == oldLeft);
    CHECK(digest(740, 190, 620, 730) != oldRight);
    CHECK(digest(1440, 126, 430, 175) == console);
    // The raised corner must remain visible above the resting page frame.
    CHECK(digest(48, 112, 642, 60) == aboveLeft);
    CHECK(digest(715, 112, 657, 60) != aboveRight);
    CHECK(digest(48, 40, 1324, 62) == searchControls);
    CHECK(digest(1405, 112, 12, 932) == cabinetGutter);
    // The transparent pivot gap must not become a grey strip over the hinge.
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    save("retro-turn-next-front");
    renderAt(1406);
    // Near the spine, the actual page frame must still occlude the hinge.
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) != rightHinge);
    save("retro-turn-next-near-spine");
    renderAt(1440);
    CHECK(!ui.takePageTurnSound());
    CHECK(digest(696, 110, 12, 58) != aboveSpine);
    save("retro-turn-edge");
    renderAt(1610);
    CHECK(ui.takePageTurnSound());
    CHECK(!ui.takePageTurnSound());
    CHECK(digest(60, 190, 600, 730) != oldLeft);
    CHECK(digest(48, 112, 642, 60) != aboveLeft);
    CHECK(digest(715, 112, 657, 60) == aboveRight);
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    const auto newRight = digest(740, 190, 620, 730);
    save("retro-turn-next-back");
    renderAt(1800);
    CHECK(!ui.takePageTurnSound());
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 20));
    CHECK(actionAt(ui, 1000, 226, neon::UiActionKind::SelectTrack, 30));
    CHECK(digest(740, 190, 620, 730) == newRight);
    CHECK(actionAt(ui, 1250, 992, neon::UiActionKind::PageNext));
    CHECK(digest(48, 112, 1324, 48) == artistBar);
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);

    CHECK(ui.beginPageTurn(false));
    model.page = 0;
    ui.render(model, 1900);
    frameTick = 1900;
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    renderAt(2070);
    CHECK(!ui.takePageTurnSound());
    CHECK(digest(740, 190, 620, 730) == newRight);
    CHECK(!ui.hitTest(300, 226));
    CHECK(digest(48, 112, 642, 60) != aboveLeft);
    CHECK(digest(715, 112, 657, 60) == aboveRight);
    CHECK(digest(48, 40, 1324, 62) == searchControls);
    CHECK(digest(1405, 112, 12, 932) == cabinetGutter);
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    save("retro-turn-previous-front");
    renderAt(2206);
    CHECK(digest(692, 200, 8, 740) != leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    save("retro-turn-previous-near-spine");
    renderAt(2240);
    CHECK(!ui.takePageTurnSound());
    CHECK(digest(696, 110, 12, 58) != aboveSpine);
    save("retro-turn-previous-edge");
    renderAt(2410);
    CHECK(ui.takePageTurnSound());
    CHECK(!ui.takePageTurnSound());
    CHECK(digest(48, 112, 642, 60) == aboveLeft);
    CHECK(digest(715, 112, 657, 60) != aboveRight);
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    save("retro-turn-previous-back");
    renderAt(2600);
    CHECK(digest(60, 190, 600, 730) == oldLeft);
    CHECK(digest(740, 190, 620, 730) == oldRight);
    CHECK(digest(48, 112, 1324, 48) == artistBar);
    CHECK(digest(692, 200, 8, 740) == leftHinge);
    CHECK(digest(704, 200, 8, 740) == rightHinge);
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 0));

    // Search/filter changes, modals and leaving the theme cancel old snapshots.
    CHECK(ui.beginPageTurn(true));
    model.page = 1;
    renderAt(2700);
    model.search = "different search";
    model.page = 0;
    renderAt(2800);
    CHECK(!ui.takePageTurnSound());
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 0));
    CHECK(ui.beginPageTurn(true));
    model.page = 1;
    renderAt(2900);
    model.mode = neon::UiMode::Admin;
    renderAt(3000);
    CHECK(!ui.takePageTurnSound());
    model.mode = neon::UiMode::Browse;
    renderAt(3100);
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 20));
    CHECK(ui.beginPageTurn(false));
    model.theme = neon::Theme::Neon;
    model.page = 0;
    renderAt(3200);
    CHECK(!ui.takePageTurnSound());
    CHECK(actionAt(ui, 600, 260, neon::UiActionKind::SelectTrack, 0));
    CHECK(ui.beginPageTurn(true));
    model.page = 1;
    renderAt(3300);
    CHECK(!ui.takePageTurnSound());
    CHECK(actionAt(ui, 600, 260, neon::UiActionKind::SelectTrack, 9));
    model.theme = neon::Theme::Retro;
    model.search.clear();
    model.page = 2;
    renderAt(3400);
    CHECK(ui.beginPageTurn(true));
    model.page = 3; // The final leaf has just one title.
    renderAt(3500);
    renderAt(4200);
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 60));
    CHECK(!ui.hitTest(1000, 226));
    CHECK(!ui.hitTest(1250, 992));
    CHECK(ui.beginPageTurn(false));
    model.page = 2;
    renderAt(4300);
    filtered.clear(); // A rescan can replace the collection during a turn.
    model.page = 0;
    renderAt(4400);
    CHECK(!ui.takePageTurnSound());
    CHECK(!ui.hitTest(300, 226));
    CHECK(!ui.hitTest(1250, 992));
    CHECK(actionAt(ui, 1540, 1017, neon::UiActionKind::InsertCoin));

    // A long frame must not skip the animation or replay a burst of old taps.
    filtered.resize(library.tracks.size());
    std::iota(filtered.begin(), filtered.end(), 0);
    model.page = 0;
    ui.render(model, 10000);
    CHECK(ui.beginPageTurn(true));
    model.page = 1;
    ui.render(model, 10020);
    for (int click = 0; click < 40; ++click) {
        CHECK(!ui.beginPageTurn(click % 2 == 0));
        CHECK(!ui.hitTest(1250, 992, (10030ULL + click) * 1000000));
    }
    filtered.push_back(0); // A background scan publishes another visible record.
    ui.render(model, 10040);
    CHECK(!ui.hitTest(300, 226));
    ui.render(model, 14020); // Reproduce the old multi-second artwork stall.
    CHECK(!ui.takePageTurnSound());
    CHECK(!ui.hitTest(300, 226));
    CHECK(!ui.beginPageTurn(true));
    int soundCount = 0;
    for (std::uint64_t tick = 14040; tick <= 14740; tick += 20) {
        ui.render(model, tick);
        soundCount += ui.takePageTurnSound() ? 1 : 0;
    }
    CHECK(soundCount == 1);
    CHECK(!ui.hitTest(1250, 992, 12000000000ULL));
    CHECK(!ui.hitTest(300, 226, 12000000000ULL));
    const auto coin = ui.hitTest(1540, 1017, 12000000000ULL);
    const auto next = ui.hitTest(1250, 992, 14741000000ULL);
    CHECK(coin && coin->kind == neon::UiActionKind::InsertCoin);
    CHECK(next && next->kind == neon::UiActionKind::PageNext);
    CHECK(!ui.takePageTurnSound());
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 20));
    // Input is usable immediately for the next deliberate turn, in reverse.
    CHECK(ui.beginPageTurn(false));
    model.page = 0;
    soundCount = 0;
    for (std::uint64_t tick = 14800; tick <= 15500; tick += 20) {
        ui.render(model, tick);
        soundCount += ui.takePageTurnSound() ? 1 : 0;
    }
    CHECK(soundCount == 1);
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 0));

    // Changing only the artist index cancels an old leaf and its pending sound.
    CHECK(ui.beginPageTurn(true));
    model.page = 1;
    ui.render(model, 15600);
    model.selectedArtistInitial = 'M';
    model.page = 0;
    ui.render(model, 15620);
    CHECK(!ui.takePageTurnSound());
    CHECK(actionAt(ui, 300, 226, neon::UiActionKind::SelectTrack, 0));
    model.selectedArtistInitial = '\0';
    ui.render(model, 15640);

    // Optional rendered evidence: 50 fps, with both directions and settled holds.
    if (!output.empty() && surface->w == 1280) {
        filtered.resize(library.tracks.size());
        std::iota(filtered.begin(), filtered.end(), 0);
        model.page = 0;
        for (int frame = 0; frame < 135; ++frame) {
            if (frame == 20) { CHECK(ui.beginPageTurn(true)); model.page = 1; }
            if (frame == 80) { CHECK(ui.beginPageTurn(false)); model.page = 0; }
            ui.render(model, 5000 + frame * 20);
            const auto name = "retro-flip-" + std::to_string(1000 + frame) + ".png";
            CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / name).c_str()));
        }
    }
}

void verifySize(int width, int height, const std::filesystem::path& output) {
    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    CHECK(renderer != nullptr);
    if (!renderer) { if (surface) SDL_DestroySurface(surface); return; }
    CHECK(SDL_SetRenderLogicalPresentation(renderer, 1920, 1080, SDL_LOGICAL_PRESENTATION_LETTERBOX));
    {
        neon::UI ui;
        std::string error;
        CHECK(ui.initialize(renderer, error));
        neon::LibraryIndex library;
        constexpr std::array titles{"Dream a Little Dream", "La Vie en Rose", "Fly Me to the Moon",
            "Feeling Good", "Blue Moon", "What a Wonderful World", "At Last", "Autumn Leaves",
            "My Baby Just Cares for Me", "The Look of Love", "Unforgettable", "Sway",
            "Beyond the Sea", "Summertime", "Misty", "L-O-V-E", "Moon River",
            "Cheek to Cheek", "Cry Me a River", "Ain't That a Kick in the Head", "Georgia on My Mind"};
        constexpr std::array artists{"Ella Fitzgerald", "Édith Piaf", "Frank Sinatra", "Nina Simone",
            "Billie Holiday", "Louis Armstrong", "Etta James", "Nat King Cole"};
        std::vector<std::size_t> filtered;
        for (std::size_t i = 0; i < titles.size(); ++i) {
            neon::Track track;
            track.id = neon::makeStableId(std::to_string(i));
            track.title = titles[i];
            track.artist = artists[i % artists.size()];
            track.album = "The Jazz Collection";
            track.albumYear = 1954 + static_cast<int>(i);
            track.durationMs = 193000 + static_cast<std::int64_t>(i) * 1000;
            track.genre = "Jazz";
            track.favorite = i == 1;
            track.mediaKind = i == 17 ? neon::MediaKind::Video : neon::MediaKind::Music;
            library.tracks.push_back(track);
            filtered.push_back(i);
        }
        std::vector<neon::QueueItem> queue;
        for (std::size_t i = 0; i < 5; ++i) queue.push_back({std::to_string(i), library.tracks[i + 3].id, 0});
        std::vector<std::string> genres{"Jazz", "Rock", "Türkçe Pop", "Blues", "Soul", "Classical", "Folk", "Disco", "R&B", "World"};
        neon::UiModel model;
        model.library = &library;
        model.filtered = &filtered;
        model.queue = &queue;
        model.genres = &genres;
        model.credits = 2;
        model.currentTrack = &library.tracks.front();
        model.selectedTrack = &library.tracks[1];
        model.playback.state = neon::PlaybackState::Playing;
        model.playback.durationMs = 193000;
        model.playback.positionMs = 67000;
        model.pinPrompt = "Enter administrator PIN";
        model.pinLength = 4;
        model.visualization.rmsLeft = 0.57F;
        model.visualization.rmsRight = 0.48F;
        model.visualization.bands.fill(0.45F);
        for (std::size_t i = 0; i < model.visualization.leftWaveform.size(); ++i) {
            const float t = static_cast<float>(i) / (model.visualization.leftWaveform.size() - 1);
            model.visualization.leftWaveform[i] = 0.6F * std::sin(t * 32) + 0.12F * std::sin(t * 130);
            model.visualization.rightWaveform[i] = 0.5F * std::sin(t * 32 + 0.3F) + 0.1F * std::sin(t * 170);
        }
        const auto save = [&](std::string_view name) {
            if (output.empty()) return;
            const auto file = output / (std::string(name) + "-" + std::to_string(width) + ".png");
            CHECK(IMG_SavePNG(surface, neon::pathToUtf8(file).c_str()));
        };

        for (const auto& definition : neon::themeDefinitions) {
            model.theme = definition.theme;
            model.mode = neon::UiMode::Browse;
            model.page = 0;
            model.credits = 2;
            const bool retro = model.theme == neon::Theme::Retro;
            const float cardX = retro ? 300.0F : 600.0F;
            const float cardY = retro ? 226.0F : 260.0F;
            const float coinX = retro ? 1540.0F : 140.0F;
            const float addX = retro ? 1770.0F : 330.0F;
            const float controlY = retro ? 1017.0F : 978.0F;
            const float nextX = retro ? 1250.0F : 1260.0F;
            ui.render(model);
            CHECK(actionAt(ui, cardX, cardY, neon::UiActionKind::SelectTrack, 0));
            CHECK(actionAt(ui, coinX, controlY, neon::UiActionKind::InsertCoin));
            CHECK(actionAt(ui, addX, controlY, neon::UiActionKind::AddSelected));
            CHECK(actionAt(ui, nextX, 992, neon::UiActionKind::PageNext));
            const float navigationY = retro ? 71.0F : 53.0F;
            CHECK(actionAt(ui, retro ? 318.0F : 430.0F, navigationY, neon::UiActionKind::OpenKeyboard));
            CHECK(actionAt(ui, retro ? 786.0F : 1082.0F, navigationY, neon::UiActionKind::ToggleGenreMenu));
            CHECK(actionAt(ui, retro ? 1077.0F : 1470.0F, navigationY, neon::UiActionKind::ShowMusic));
            CHECK(actionAt(ui, retro ? 1279.0F : 1754.0F, navigationY, neon::UiActionKind::ShowVideo));
            save(definition.id);
            model.visualizerMode = neon::VisualizerMode::NeonMosaic;
            ui.render(model);
            save(retro ? "retro-mosaic" : "neon-mosaic");
            model.visualizerMode = neon::VisualizerMode::AuroraSpectrum;
            model.scanning = true;
            model.scanProcessed = 1;
            model.scanTotal = 2;
            model.scanFile = "Next record.mp3";
            ui.render(model);
            CHECK(actionAt(ui, cardX, cardY, neon::UiActionKind::SelectTrack, 0));
            CHECK(actionAt(ui, addX, controlY, neon::UiActionKind::AddSelected));
            CHECK(actionAt(ui, nextX, 992, neon::UiActionKind::PageNext));
            save(retro ? "retro-scanning" : "neon-scanning");
            model.scanning = false;
            if (retro) {
                // Every left/right rail and title strip must identify its own record.
                for (std::size_t slot = 0; slot < 20; ++slot) {
                    const auto row = slot % 10;
                    const float y = 226 + static_cast<float>(row / 2) * 155.6F +
                                    static_cast<float>(row % 2) * 71.8F;
                    CHECK(actionAt(ui, slot < 10 ? 80.0F : 1330.0F, y,
                                   neon::UiActionKind::SelectTrack, slot));
                    CHECK(actionAt(ui, slot < 10 ? 150.0F : 756.0F, y,
                                   neon::UiActionKind::SelectTrack, slot));
                }
                CHECK(!ui.hitTest(300, 340)); // Metal gap between the two-record inserts.
                CHECK(!ui.hitTest(1000, 340));
            }
            const float artistY = retro ? 136.0F : 120.0F;
            const float artistStart = retro ? 172.0F : 182.0F;
            const float artistPitch = retro ? 1206.0F / 26 : 66.0F;
            const float artistWidth = artistPitch - (retro ? 6.0F : 8.0F);
            const float allArtistsX = retro ? 76.0F : 64.0F;
            for (int letter = 0; letter < 26; ++letter) {
                // Adjacent buttons must not steal each other's edge clicks.
                for (const float offset : {1.0F, artistWidth / 2, artistWidth - 1}) {
                    const auto hit = ui.hitTest(artistStart + letter * artistPitch + offset, artistY);
                    CHECK(hit && hit->kind == neon::UiActionKind::SelectArtistInitial &&
                          hit->character == 'A' + letter);
                }
            }
            const auto allHit = ui.hitTest(allArtistsX, artistY);
            const auto numberHit = ui.hitTest(retro ? 138.0F : 140.0F, artistY);
            CHECK(allHit && allHit->kind == neon::UiActionKind::SelectArtistInitial && !allHit->character);
            CHECK(numberHit && numberHit->kind == neon::UiActionKind::SelectArtistInitial && numberHit->character == '#');
            const auto unfiltered = filtered;
            filtered = neon::LibraryScanner::filter(library, {}, neon::LibraryFilter::All);
            model.nowPlayingArtworkMode = neon::NowPlayingArtworkMode::SpinningDisc;
            ui.render(model);
            save(retro ? "retro-artists-all" : "neon-artists-all");
            model.selectedArtistInitial = 'E';
            filtered = neon::LibraryScanner::filter(library, {}, neon::LibraryFilter::All, {}, 'E');
            ui.render(model);
            CHECK(!filtered.empty());
            CHECK(actionAt(ui, cardX, cardY, neon::UiActionKind::SelectTrack, filtered.front()));
            CHECK(!ui.hitTest(nextX, 992));
            save(retro ? "retro-artists-e" : "neon-artists-e");
            model.selectedArtistInitial = 'Z';
            filtered = neon::LibraryScanner::filter(library, {}, neon::LibraryFilter::All, {}, 'Z');
            ui.render(model);
            CHECK(filtered.empty() && !ui.hitTest(cardX, cardY));
            CHECK(actionAt(ui, allArtistsX, artistY, neon::UiActionKind::SelectArtistInitial));
            save(retro ? "retro-artists-empty" : "neon-artists-empty");
            model.selectedArtistInitial = '\0';
            model.nowPlayingArtworkMode = neon::NowPlayingArtworkMode::Artwork;
            filtered = unfiltered;
            ui.render(model);
            model.credits = 0;
            ui.render(model);
            CHECK(!ui.hitTest(cardX, cardY));
            CHECK(!ui.hitTest(addX, controlY));
            CHECK(!ui.hitTest(nextX, 992));
            CHECK(actionAt(ui, coinX, controlY, neon::UiActionKind::InsertCoin));
            CHECK(!ui.hitTest(allArtistsX, artistY));
            CHECK(!ui.hitTest(artistStart + 12 * artistPitch + artistWidth / 2, artistY));
            save(retro ? "retro-no-credit" : "neon-no-credit");
            model.credits = 2;
            model.page = (filtered.size() - 1) / definition.pageSize;
            ui.render(model);
            CHECK(actionAt(ui, cardX, cardY, neon::UiActionKind::SelectTrack,
                           model.page * definition.pageSize));
            CHECK(!ui.hitTest(nextX, 992));
            if (retro) {
                CHECK(!ui.hitTest(1000, 226)); // Empty B0 on the one-item last page.
                save("retro-last-page");
            }
            model.page = 0;
            model.videoPlaying = true;
            ui.render(model);
            const auto media = neon::UI::nowPlayingMediaRect(model.theme, true);
            CHECK(actionAt(ui, media.x + media.w / 2, media.y + media.h / 2,
                           neon::UiActionKind::ToggleVideoFullscreen));
            CHECK(media.x >= 0 && media.x + media.w <= 1920 && media.y + media.h <= 1080);
            if (retro) {
                CHECK(media.y > 298 && media.y + media.h < 666);
                CHECK(media.x - 10 == 1440 && media.x + media.w + 10 == 1876);
                CHECK(actionAt(ui, 1452, 482, neon::UiActionKind::ToggleVideoFullscreen));
                CHECK(actionAt(ui, 1864, 482, neon::UiActionKind::ToggleVideoFullscreen));
                CHECK(actionAt(ui, 1658, 733, neon::UiActionKind::OpenVisualizer));
                save("retro-video-wide");
            }
            model.videoPlaying = false;
            model.genreMenuOpen = true;
            ui.render(model);
            CHECK(actionAt(ui, cardX, cardY, neon::UiActionKind::CloseGenreMenu));
            CHECK(actionAt(ui, retro ? 786.0F : 1020.0F, retro ? 190.0F : 172.0F, neon::UiActionKind::SelectGenre, 0));
            save(retro ? "retro-genres" : "neon-genres");
            model.genreMenuOpen = false;
            model.keyboardOpen = true;
            ui.render(model);
            const auto keyboardHit = ui.hitTest(coinX, controlY);
            CHECK(keyboardHit && keyboardHit->kind != neon::UiActionKind::InsertCoin);
            save(retro ? "retro-keyboard" : "neon-keyboard");
            model.keyboardOpen = false;
            model.playNowPrompt = true;
            ui.render(model);
            CHECK(actionAt(ui, coinX, controlY, neon::UiActionKind::None));
            CHECK(actionAt(ui, 1140, 670, neon::UiActionKind::PlayRequestNow));
            model.playNowPrompt = false;
            model.visualizerOpen = true;
            model.visualizerMode = neon::VisualizerMode::WarmTwinVu;
            ui.render(model);
            CHECK(actionAt(ui, coinX, controlY, neon::UiActionKind::None));
            CHECK(actionAt(ui, 1680, 124, neon::UiActionKind::CloseVisualizer));
            CHECK(actionAt(ui, 200, 925, retro ? neon::UiActionKind::None : neon::UiActionKind::VisualizerPrevious));
            CHECK(actionAt(ui, 1660, 925, retro ? neon::UiActionKind::None : neon::UiActionKind::VisualizerNext));
            if (retro) save("retro-visualizer");
            model.visualizerOpen = false;
            model.visualizerMode = neon::VisualizerMode::AuroraSpectrum;
            model.mode = neon::UiMode::AdminPin;
            ui.render(model);
            CHECK(actionAt(ui, coinX, controlY, neon::UiActionKind::None));
            if (retro) save("retro-pin");
            model.mode = neon::UiMode::Admin;
            ui.render(model);
            CHECK(actionAt(ui, 600, 245, neon::UiActionKind::AdminSelectTheme, 0));
            CHECK(actionAt(ui, 1230, 245, neon::UiActionKind::AdminSelectTheme, 1));
            CHECK(actionAt(ui, 100, 1000, neon::UiActionKind::None));
            save(retro ? "retro-admin" : "neon-admin");
            model.mode = neon::UiMode::Browse;
            model.adminReveal = true;
            ui.render(model);
            CHECK(actionAt(ui, 100, 40, neon::UiActionKind::OpenAdmin));
            model.adminReveal = false;
        }

        verifyPageTurns(ui, model, surface, output);
        model.theme = neon::Theme::Retro;
        const auto pixel = [&](float x, float y) {
            const float scale = std::min(width / 1920.0F, height / 1080.0F);
            const float ox = (width - 1920 * scale) / 2;
            const float oy = (height - 1080 * scale) / 2;
            std::array<Uint8, 4> color{};
            CHECK(SDL_ReadSurfacePixel(surface, static_cast<int>(ox + x * scale),
                static_cast<int>(oy + y * scale), &color[0], &color[1], &color[2], &color[3]));
            return color;
        };
        model.credits = 2;
        ui.clearPointer();
        ui.render(model, 22000);
        const auto whiteInsert = pixel(132, 198);
        const auto metalGap = pixel(142, 340);
        int darkestDot = 255;
        for (float y = 338; y <= 342; y += 1)
            for (float x = 129; x <= 132; x += 1) {
                const auto dotPixel = pixel(x, y);
                darkestDot = std::min(darkestDot,
                    (static_cast<int>(dotPixel[0]) + dotPixel[1] + dotPixel[2]) / 3);
            }
        CHECK(whiteInsert[0] > 240 && whiteInsert[1] > 240 && whiteInsert[2] > 240);
        CHECK(metalGap[0] < 235 && metalGap[1] < 235 && metalGap[2] < 235);
        CHECK(darkestDot < 140); // Small dots remain visible after subpixel rasterization.
        const auto restingKey = pixel(1450, 996);
        ui.updatePointer(1540, 1017, false);
        ui.render(model, 22020);
        const auto litKey = pixel(1450, 996);
        CHECK(litKey != restingKey);
        save("retro-button-hover");
        ui.updatePointer(1540, 1017, true);
        ui.render(model, 22040);
        CHECK(pixel(1450, 996) != litKey);
        save("retro-button-pressed");
        ui.updatePointer(1540, 1017, false);
        ui.notifyAction({neon::UiActionKind::InsertCoin});
        ui.clearPointer(); // Leaving the window cancels visual feedback.
        ui.render(model, 22060);
        CHECK(pixel(1450, 996) == restingKey);
        model.credits = 0;
        model.currentTrack = nullptr;
        ui.render(model, 22300);
        for (auto tick = 22320ULL; tick <= 22500; tick += 20) ui.render(model, tick);
        save("retro-idle-message");
        ui.render(model, 23600);
        save("retro-idle-blink");
        model.credits = 2;
        model.currentTrack = &library.tracks.front();
        ui.render(model, 23620);
        ui.render(model, 23660);
        save("retro-display-transition");
        // A full 20-second information interval is followed by a 5-second reminder.
        model.theme = neon::Theme::Neon;
        ui.render(model, 30000);
        model.theme = neon::Theme::Retro;
        ui.render(model, 30000);
        const auto statusInk = [&] {
            std::size_t lit = 0;
            for (float y = 72; y < 163; y += 2)
                for (float x = 1456; x < 1860; x += 2) {
                    const auto value = pixel(x, y);
                    if (value[0] >= 60 && value[1] >= 120) ++lit;
                }
            return lit;
        };
        const auto informationInk = statusInk();
        CHECK(informationInk > 0);
        ui.render(model, 49999);
        CHECK(statusInk() == informationInk);
        ui.render(model, 50000);
        CHECK(statusInk() > 0);
        save("retro-coin-reminder-on");
        ui.render(model, 50500);
        CHECK(statusInk() == 0);
        save("retro-coin-reminder-off");
        ui.render(model, 51000);
        CHECK(statusInk() > 0);
        ui.render(model, 54999);
        CHECK(statusInk() == 0);
        ui.render(model, 55000);
        CHECK(statusInk() == informationInk);
        save("retro-coin-reminder-info");
        ui.render(model, 74999);
        CHECK(statusInk() == informationInk);
        ui.render(model, 75000);
        CHECK(statusInk() > 0);
        ui.render(model, 75500);
        CHECK(statusInk() == 0);
        // Returning to an earlier test clock starts a fresh information interval.
        ui.render(model, 23680);
        library.tracks[0].title = "İçimdeki Çok Uzun Şarkının İsmi — Çağrışımlar ve Düşler";
        library.tracks[0].artist = "Şebnem Ferah / Barış Manço / Müslüm Gürses";
        model.credits = 12345;
        model.nowPlayingArtworkMode = neon::NowPlayingArtworkMode::SpinningDisc;
        ui.render(model);
        save("retro-long-text-disc");
        // Album artwork belongs to the small centre label; the playing surface is black vinyl.
        for (const SDL_FPoint point : {SDL_FPoint{1764, 482}, SDL_FPoint{1556, 482},
                                      SDL_FPoint{1660, 378}, SDL_FPoint{1660, 586}}) {
            const auto surfaceColor = pixel(point.x, point.y);
            CHECK(surfaceColor[0] < 95 && surfaceColor[1] < 95 && surfaceColor[2] < 95);
        }
        // Four former frame corners must expose the same cabinet metal as their neighbours.
        for (const float y : {315.0F, 647.0F}) {
            CHECK(pixel(1493, y) == pixel(1480, y));
            CHECK(pixel(1827, y) == pixel(1840, y));
        }
        filtered.clear();
        queue.clear();
        model.selectedTrack = nullptr;
        model.currentTrack = nullptr;
        ui.render(model);
        CHECK(!ui.hitTest(300, 226));
        CHECK(!ui.hitTest(1770, 1017));
        save("retro-empty");
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
}

// Optional local diagnosis with real embedded covers; never changes library data.
void verifyVideoThumbnailCache() {
    const auto root = std::filesystem::temp_directory_path() / neon::pathFromUtf8("neon-video-cache-" + neon::randomId());
    std::filesystem::create_directories(root);
    neon::Track track;
    track.id = "cached-video";
    track.mediaKind = neon::MediaKind::Video;
    track.path = root / "unavailable-original.mp4";
    track.fileSize = 123;
    track.modifiedTicks = 456;
    const auto path = neon::videoThumbnailCachePath(track, root);
    auto* surface = SDL_CreateSurface(48, 27, SDL_PIXELFORMAT_RGBA32);
    CHECK(surface);
    if (surface) {
        SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGB(surface, 20, 150, 70));
        CHECK(IMG_SavePNG(surface, neon::pathToUtf8(path).c_str()));
        SDL_DestroySurface(surface);
    }
    const auto written = std::filesystem::last_write_time(path);
    for (int pass = 0; pass < 3; ++pass) {
        surface = neon::loadVideoThumbnail(track, root);
        CHECK(surface && surface->w == 48 && surface->h == 27);
        if (surface) SDL_DestroySurface(surface);
        CHECK(std::filesystem::last_write_time(path) == written);
    }
    ++track.modifiedTicks;
    const auto changed = neon::videoThumbnailCachePath(track, root);
    CHECK(changed != path);
    // A persisted failed extraction also avoids reopening the decoder.
    { std::ofstream marker(std::filesystem::path(changed.wstring() + L".missing")); marker << "no thumbnail"; }
    CHECK(!neon::loadVideoThumbnail(track, root));
    ++track.modifiedTicks;
    const auto cancelledPath = neon::videoThumbnailCachePath(track, root);
    std::stop_source stop;
    stop.request_stop();
    CHECK(!neon::loadVideoThumbnail(track, root, stop.get_token()));
    CHECK(!std::filesystem::exists(cancelledPath));
    CHECK(!std::filesystem::exists(cancelledPath.wstring() + L".missing"));
    std::filesystem::remove_all(root);
}

void verifyLibraryArtworkPreparation() {
    const auto root = std::filesystem::temp_directory_path() / neon::pathFromUtf8("neon-library-covers-" + neon::randomId());
    std::filesystem::create_directories(root);
    const auto pathFor = [](const neon::Track& track, const std::filesystem::path& directory) {
        return track.mediaKind == neon::MediaKind::Video ? neon::videoThumbnailCachePath(track, directory / "video-thumbnails") :
            neon::ArtworkCache::coverCachePath(track, directory / "music-covers");
    };
    const auto waitFor = [](const auto& predicate) {
        const auto end = SDL_GetTicks() + 4000;
        while (!predicate() && SDL_GetTicks() < end) SDL_Delay(2);
        return predicate();
    };
    const auto waitIdle = [&](const auto& preparer) {
        return waitFor([&] { const auto progress = preparer.progress(); return progress.pending == 0 && progress.active == 0; });
    };
    const auto writePng = [](const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
        auto* pixels = SDL_CreateSurface(40, 24, SDL_PIXELFORMAT_RGBA32);
        SDL_FillSurfaceRect(pixels, nullptr, SDL_MapSurfaceRGBA(pixels, 37, 180, 91, 255));
        const auto saved = IMG_SavePNG(pixels, neon::pathToUtf8(path).c_str());
        SDL_DestroySurface(pixels);
        return saved;
    };
    std::atomic_int calls{};
    const auto prepare = [&](const neon::Track& track, const auto& directory, std::stop_token stop) {
        if (stop.stop_requested()) return;
        ++calls;
        if (!writePng(pathFor(track, directory))) throw std::runtime_error("Fixture PNG could not be saved");
    };
    std::vector<neon::Track> tracks(40);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        auto& track = tracks[i];
        track.id = neon::makeStableId(std::to_string(i));
        track.path = root / (std::to_string(i) + ".media");
        track.modifiedTicks = 10;
        track.fileSize = 100;
        track.mediaKind = i % 2 ? neon::MediaKind::Video : neon::MediaKind::Music;
    }
    std::vector<std::filesystem::file_time_type> timestamps;
    {
        neon::LibraryArtworkPreparer preparer(prepare);
        preparer.start(root);
        preparer.enqueue(tracks); // No renderer, navigation or Video section exists here.
        CHECK(waitIdle(preparer));
        CHECK(calls == 40 && preparer.progress().prepared == 40);
        CHECK(preparer.takeCompleted().size() == 40);
        for (const auto& track : tracks) timestamps.push_back(std::filesystem::last_write_time(pathFor(track, root)));
        preparer.enqueue(tracks);
        CHECK(waitIdle(preparer) && calls == 40 && preparer.takeCompleted().empty());
    }
    {
        neon::LibraryArtworkPreparer restarted(prepare);
        restarted.start(root);
        restarted.enqueue(tracks);
        CHECK(waitIdle(restarted));
        CHECK(calls == 40 && restarted.progress().reused == 40 && restarted.progress().prepared == 0);
        for (std::size_t i = 0; i < tracks.size(); ++i)
            CHECK(std::filesystem::last_write_time(pathFor(tracks[i], root)) == timestamps[i]);
        ++tracks[0].modifiedTicks;
        auto addedMusic = tracks[2]; addedMusic.id = "added-music"; addedMusic.path = root / "new-song.mp3";
        auto addedVideo = tracks[3]; addedVideo.id = "added-video"; addedVideo.path = root / "new-clip.mp4";
        tracks.push_back(addedMusic); tracks.push_back(addedVideo);
        restarted.enqueue(tracks);
        CHECK(waitIdle(restarted));
        CHECK(calls == 43 && restarted.progress().prepared == 3);
        CHECK(restarted.takeCompleted().size() == 3);
    }
    // A blocked video cannot hold music or the shutdown path. Cancellation must
    // leave no completed marker, so the next launch can finish that exact item.
    struct Gate { std::atomic_bool entered{}, release{}, exited{}, sawStop{}; };
    const auto gate = std::make_shared<Gate>();
    auto slowTracks = std::vector<neon::Track>{tracks[0], tracks[1]};
    slowTracks[0].id = "fresh-music";
    slowTracks[1].path = root / "slow-video.mp4";
    {
        neon::LibraryArtworkPreparer slow([&, gate](const neon::Track& track, const auto& directory, std::stop_token stop) {
            if (track.mediaKind == neon::MediaKind::Music) { prepare(track, directory, stop); return; }
            gate->entered.store(true);
            while (!gate->release.load()) {
                if (stop.stop_requested()) gate->sawStop.store(true);
                SDL_Delay(2);
            }
            gate->exited.store(true);
        });
        slow.start(root);
        slow.enqueue(slowTracks);
        CHECK(waitFor([&] { return gate->entered.load() && slow.progress().prepared == 1; }));
        const auto before = SDL_GetTicks();
        slow.stop();
        CHECK(SDL_GetTicks() - before < 100);
        CHECK(waitFor([&] { return gate->sawStop.load(); }));
        CHECK(!std::filesystem::exists(pathFor(slowTracks[1], root)));
        CHECK(!std::filesystem::exists(pathFor(slowTracks[1], root).wstring() + L".missing"));
        gate->release.store(true);
        CHECK(waitFor([&] { return gate->exited.load(); }));
    }
    {
        neon::LibraryArtworkPreparer resumed(prepare);
        resumed.start(root);
        resumed.enqueue(slowTracks);
        CHECK(waitIdle(resumed));
        CHECK(resumed.progress().prepared == 1 && resumed.progress().reused == 1);
    }
    // Exercise real music extraction and persisted reuse with a sidecar cover.
    auto music = tracks[0];
    music.sidecarArtwork = root / "source-cover.png";
    CHECK(writePng(*music.sidecarArtwork));
    auto* first = neon::ArtworkCache::loadCachedCover(music, root / "real-music");
    CHECK(first);
    SDL_DestroySurface(first);
    const auto cached = neon::ArtworkCache::coverCachePath(music, root / "real-music");
    CHECK(std::filesystem::is_regular_file(cached));
    const auto savedAt = std::filesystem::last_write_time(cached);
    std::filesystem::rename(*music.sidecarArtwork, root / "source-offline.png");
    auto* reused = neon::ArtworkCache::loadCachedCover(music, root / "real-music");
    CHECK(reused && std::filesystem::last_write_time(cached) == savedAt);
    SDL_DestroySurface(reused);
    std::stop_source cancelled;
    cancelled.request_stop();
    ++music.modifiedTicks;
    CHECK(!neon::ArtworkCache::loadCachedCover(music, root / "real-music", cancelled.get_token()));
    CHECK(!std::filesystem::exists(neon::ArtworkCache::coverCachePath(music, root / "real-music")));
    // Browsing only reads prepared files. It never starts extraction itself,
    // even when the source cover is available; completed work refreshes the card.
    auto visible = tracks[2];
    visible.id = "visible-music";
    visible.sidecarArtwork = root / "source-offline.png";
    auto* target = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = SDL_CreateSoftwareRenderer(target);
    {
        neon::ArtworkCache view;
        view.useDiskCache(root);
        CHECK(view.get(renderer, visible));
        CHECK(waitFor([&] { view.update(renderer); return view.ready(visible); }));
        const auto visiblePath = pathFor(visible, root);
        CHECK(!std::filesystem::exists(visiblePath));
        neon::LibraryArtworkPreparer background;
        background.start(root);
        background.enqueue(std::span<const neon::Track>(&visible, 1));
        CHECK(waitIdle(background));
        CHECK(std::filesystem::exists(visiblePath));
        const auto revision = view.revision();
        for (const auto& track : background.takeCompleted()) view.invalidate(track);
        CHECK(view.revision() > revision);
        CHECK(view.get(renderer, visible));
        CHECK(waitFor([&] { view.update(renderer); return view.ready(visible); }));
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(target);
    // All paths belong to the unique temporary fixture created at the top.
    std::filesystem::remove_all(root);
    std::cout << "Whole library: 40 prepared, restart reused 40, delta prepared 3; cancellation and music persistence passed\n";
}

void previewAdminSources(const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    neon::LibraryIndex library;
    std::vector<neon::QueueItem> queue;
    for (int i = 0; i < 4; ++i) {
        neon::Track track;
        track.id = std::to_string(i);
        track.title = "Queued track " + std::to_string(i + 1);
        library.tracks.push_back(track);
        queue.push_back({track.id, track.id, 0});
    }
    const std::vector<std::size_t> filtered;
    auto* surface = SDL_CreateSurface(1280, 720, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = SDL_CreateSoftwareRenderer(surface);
    CHECK(renderer && SDL_SetRenderLogicalPresentation(renderer, 1920, 1080, SDL_LOGICAL_PRESENTATION_LETTERBOX));
    for (const auto theme : {neon::Theme::Retro, neon::Theme::Neon}) {
        neon::UI ui;
        std::string error;
        CHECK(ui.initialize(renderer, error));
        neon::UiModel model;
        model.theme = theme;
        model.mode = neon::UiMode::Admin;
        model.library = &library;
        model.filtered = &filtered;
        model.musicSourceCount = 36;
        model.videoSourceCount = 5;
        model.queue = &queue;
        model.scanProcessed = 4090;
        model.scanTotal = 4091;
        model.scanFile = "E:\\Müzik Arşivi\\80's music hits\\Scorpions - Send Me An Angel.mp4";
        for (const bool scanning : {true, false}) {
            model.scanning = scanning;
            ui.render(model, 1000);
            CHECK(actionAt(ui, 580, 536, neon::UiActionKind::AdminChooseMusicFolders));
            CHECK(actionAt(ui, 850, 536, neon::UiActionKind::AdminChooseVideoFolders));
            CHECK(actionAt(ui, 540, 900, neon::UiActionKind::AdminQueueSelect, 3));
            CHECK(actionAt(ui, 1470, 907, neon::UiActionKind::AdminExit));
            if (scanning) {
                CHECK(actionAt(ui, 340, 536, neon::UiActionKind::None)); // Modal panel consumes disabled controls.
                const auto name = theme == neon::Theme::Retro ? "retro-scanning.png" : "modern-scanning.png";
                CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / name).c_str()));
            } else {
                CHECK(actionAt(ui, 340, 536, neon::UiActionKind::AdminRescan));
                const auto name = theme == neon::Theme::Retro ? "retro-idle.png" : "modern-idle.png";
                CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / name).c_str()));
            }
        }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
}

void previewVideoPreparation(const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    neon::LibraryIndex library;
    neon::Track clip;
    clip.id = "preparing-video";
    clip.artist = "Scorpions";
    clip.title = "Send Me An Angel";
    clip.mediaKind = neon::MediaKind::Video;
    clip.durationMs = 268934;
    library.tracks.push_back(clip);
    const std::vector<std::size_t> filtered{0};
    auto* surface = SDL_CreateSurface(1280, 720, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = SDL_CreateSoftwareRenderer(surface);
    CHECK(renderer && SDL_SetRenderLogicalPresentation(renderer, 1920, 1080, SDL_LOGICAL_PRESENTATION_LETTERBOX));
    for (const auto theme : {neon::Theme::Retro, neon::Theme::Neon}) {
        neon::UI ui;
        std::string error;
        CHECK(ui.initialize(renderer, error));
        neon::UiModel model;
        model.theme = theme;
        model.library = &library;
        model.filtered = &filtered;
        model.currentTrack = &library.tracks.front();
        model.videoPlaying = true;
        model.videoLoadingStatus = "PREPARING VIDEO  42%";
        model.libraryFilter = neon::LibraryFilter::Video;
        model.playback.durationMs = clip.durationMs;
        model.credits = 1;
        ui.render(model, 1000);
        ui.render(model, 23000); // Preparing text remains visible during the coin-prompt interval.
        CHECK(actionAt(ui, theme == neon::Theme::Retro ? 1540.0F : 135.0F,
                       theme == neon::Theme::Retro ? 1017.0F : 977.0F, neon::UiActionKind::InsertCoin));
        CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / (theme == neon::Theme::Retro ? "retro.png" : "modern.png")).c_str()));
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
}

void previewVideoArtistFilter(const std::filesystem::path& libraryRoot, const std::filesystem::path& output) {
    std::ifstream input(libraryRoot / "library.json", std::ios::binary);
    auto library = nlohmann::json::parse(input).get<neon::LibraryIndex>();
    auto filtered = neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video, "", 'M');
    CHECK(filtered.size() >= 3);
    std::cout << "Saved video catalogue, M: " << filtered.size() << " results\n";
    for (const auto index : filtered) {
        const auto label = neon::trackLabel(library.tracks[index]);
        CHECK(neon::uppercaseForDisplay(label.artist).starts_with("M"));
    }
    // This preview checks labels and empty states without reopening source
    // videos or generating/changing the user's saved thumbnail cache.
    for (auto& track : library.tracks) {
        track.path.clear();
        track.hasEmbeddedArtwork = false;
        track.sidecarArtwork.reset();
        track.onlineArtwork.reset();
    }
    std::filesystem::create_directories(output);
    for (const auto theme : {neon::Theme::Retro, neon::Theme::Neon}) {
        auto* surface = SDL_CreateSurface(1280, 720, SDL_PIXELFORMAT_RGBA32);
        auto* renderer = SDL_CreateSoftwareRenderer(surface);
        CHECK(renderer && SDL_SetRenderLogicalPresentation(renderer, 1920, 1080, SDL_LOGICAL_PRESENTATION_LETTERBOX));
        {
            neon::UI ui;
            std::string error;
            CHECK(ui.initialize(renderer, error));
            neon::UiModel model;
            model.theme = theme;
            model.library = &library;
            model.filtered = &filtered;
            model.libraryFilter = neon::LibraryFilter::Video;
            model.selectedArtistInitial = 'M';
            model.credits = 2;
            model.scanning = true;
            const auto name = theme == neon::Theme::Retro ? "retro" : "modern";
            ui.render(model, 1000);
            CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / (std::string(name) + "-M.png")).c_str()));
            const std::vector<std::size_t> empty;
            model.filtered = &empty;
            model.search = "No matching clip";
            CHECK(!model.buildingEmptyLibrary());
            ui.render(model, 1200);
            CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / (std::string(name) + "-empty.png")).c_str()));
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
}

void verifyVideoCards(const std::filesystem::path& libraryRoot, const std::filesystem::path& output) {
    neon::LibraryIndex library;
    std::ifstream input(libraryRoot / "library.json", std::ios::binary);
    library = nlohmann::json::parse(input).get<neon::LibraryIndex>();
    auto filtered = neon::LibraryScanner::filter(library, "", neon::LibraryFilter::Video);
    CHECK(filtered.size() >= 3);
    if (filtered.size() < 3) return;
    std::filesystem::create_directories(output);
    // This verifies an actual video frame independently of the generated fallback.
    auto* thumbnail = neon::loadVideoThumbnail(library.tracks[filtered[0]]);
    CHECK(thumbnail);
    if (thumbnail) {
        CHECK(thumbnail->w > 64 && thumbnail->h > 36);
        CHECK(IMG_SavePNG(thumbnail, neon::pathToUtf8(output / "source-video-frame.png").c_str()));
        SDL_DestroySurface(thumbnail);
        const auto path = neon::videoThumbnailCachePath(library.tracks[filtered[0]]);
        CHECK(std::filesystem::is_regular_file(path));
        const auto saved = std::filesystem::last_write_time(path);
        const auto started = SDL_GetTicksNS();
        thumbnail = neon::loadVideoThumbnail(library.tracks[filtered[0]]);
        std::cout << "Saved video thumbnail read: " << (SDL_GetTicksNS() - started) / 1000000.0 << " ms\n";
        CHECK(thumbnail && std::filesystem::last_write_time(path) == saved);
        if (thumbnail) SDL_DestroySurface(thumbnail);
    }
    filtered.resize(3); // Odd final page: the right slot must not select a ghost item.
    for (const auto size : {std::pair{1920,1080}, std::pair{1280,720}, std::pair{1024,768}}) {
        auto* surface = SDL_CreateSurface(size.first, size.second, SDL_PIXELFORMAT_RGBA32);
        auto* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        CHECK(renderer);
        if (!renderer) { SDL_DestroySurface(surface); continue; }
        SDL_SetRenderLogicalPresentation(renderer, 1920, 1080, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        {
            neon::UI ui;
            std::string error;
            CHECK(ui.initialize(renderer, error));
            neon::UiModel model;
            model.theme = neon::Theme::Retro;
            model.libraryFilter = neon::LibraryFilter::Video;
            model.library = &library;
            model.filtered = &filtered;
            model.selectedTrack = &library.tracks[filtered[0]];
            model.currentTrack = model.selectedTrack;
            model.playback.durationMs = model.currentTrack->durationMs;
            model.credits = 2;
            model.visualizerMode = neon::VisualizerMode::RetroPhosphorScope;
            std::uint64_t tick = 1000;
            ui.render(model, tick);
            const auto deadline = SDL_GetTicks() + 15000;
            while (!ui.pageArtworkReady(library, filtered, model.theme, 0, model.libraryFilter) && SDL_GetTicks() < deadline) {
                ui.render(model, tick += 20);
                SDL_Delay(1);
            }
            CHECK(ui.pageArtworkReady(library, filtered, model.theme, 0, model.libraryFilter));
            ui.render(model, tick += 20);
            CHECK(actionAt(ui, 350, 400, neon::UiActionKind::SelectTrack, filtered[0]));
            CHECK(actionAt(ui, 1040, 400, neon::UiActionKind::SelectTrack, filtered[1]));
            CHECK(!ui.hitTest(700, 400));
            CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / ("retro-vhs-" + std::to_string(size.first) + ".png")).c_str()));
            model.pendingPage = 1;
            ui.render(model, tick += 20);
            CHECK(!ui.hitTest(350, 400));
            CHECK(actionAt(ui, 1540, 1017, neon::UiActionKind::InsertCoin));
            const auto nextDeadline = SDL_GetTicks() + 15000;
            while (!ui.pageArtworkReady(library, filtered, model.theme, 1, model.libraryFilter) && SDL_GetTicks() < nextDeadline)
                ui.render(model, tick += 20);
            CHECK(ui.pageArtworkReady(library, filtered, model.theme, 1, model.libraryFilter));
            CHECK(ui.beginPageTurn(true));
            model.pendingPage.reset();
            model.page = 1;
            for (int frame = 0; frame < 37; ++frame) {
                ui.render(model, tick += 20);
                if (frame == 15) CHECK(IMG_SavePNG(surface, neon::pathToUtf8(output / ("retro-vhs-turn-" + std::to_string(size.first) + ".png")).c_str()));
            }
            CHECK(actionAt(ui, 350, 400, neon::UiActionKind::SelectTrack, filtered[2]));
            CHECK(!ui.hitTest(1040, 400));
            CHECK(!ui.hitTest(1250, 1020)); // No next page after the last video.
            model.credits = 0;
            ui.render(model, tick += 20);
            CHECK(!ui.hitTest(350, 400));
            CHECK(actionAt(ui, 1540, 1017, neon::UiActionKind::InsertCoin));
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
}

void profilePaging(const std::filesystem::path& libraryRoot) {
    neon::LibraryIndex library;
    try {
        std::ifstream stream(libraryRoot / "library.json", std::ios::binary);
        library = nlohmann::json::parse(stream).get<neon::LibraryIndex>();
    } catch (const std::exception& error) {
        std::cerr << "Cannot profile library: " << error.what() << '\n';
        CHECK(false);
        return;
    }
    CHECK(!library.tracks.empty());
    std::vector<std::size_t> filtered(library.tracks.size());
    std::iota(filtered.begin(), filtered.end(), 0);
    SDL_Window* window = SDL_CreateWindow("Paging profile", 1920, 1080, SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    CHECK(renderer != nullptr);
    if (!renderer) { if (window) SDL_DestroyWindow(window); return; }
    SDL_SetRenderVSync(renderer, 0);
    SDL_SetRenderLogicalPresentation(renderer, 1920, 1080, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    for (const auto theme : {neon::Theme::Retro, neon::Theme::Neon}) {
        neon::UI ui;
        std::string error;
        CHECK(ui.initialize(renderer, error));
        neon::UiModel model;
        model.theme = theme;
        model.library = &library;
        model.filtered = &filtered;
        model.credits = 2;
        model.nowPlayingArtworkMode = neon::NowPlayingArtworkMode::SpinningDisc;
        std::cout << "Paging profile: renderer=" << SDL_GetRendererName(renderer)
                  << ", theme=" << neon::themeDefinition(theme).label
                  << ", tracks=" << library.tracks.size() << std::endl;
        std::uint64_t tick = 1000;
        const auto renderFrame = [&] { tick += 20; ui.render(model, tick); };
        const auto waitForPage = [&](std::size_t page) {
            const auto started = SDL_GetTicks();
            while (!ui.pageArtworkReady(library, filtered, theme, page) && SDL_GetTicks() - started < 10000) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {}
                renderFrame();
                SDL_Delay(1);
            }
            CHECK(ui.pageArtworkReady(library, filtered, theme, page));
            return SDL_GetTicks() - started;
        };
        renderFrame();
        waitForPage(0);
        // Let nearby pages preload during the normal time spent reading a page.
        const auto warmUntil = SDL_GetTicks() + 500;
        while (SDL_GetTicks() < warmUntil) renderFrame();
        const auto pageSize = neon::themeDefinition(theme).pageSize;
        const auto pages = std::min<std::size_t>(8, (filtered.size() + pageSize - 1) / pageSize);
        for (std::size_t step = 1; step < pages * 2 - 1; ++step) {
            const auto page = step < pages ? step : pages * 2 - 2 - step;
            const bool prefetched = ui.pageArtworkReady(library, filtered, theme, page);
            model.pendingPage = page;
            const auto preparation = waitForPage(page);
            CHECK(ui.beginPageTurn(page > model.page));
            if (theme == neon::Theme::Retro)
                for (int click = 0; click < 30; ++click) CHECK(!ui.beginPageTurn(true));
            model.pendingPage.reset();
            model.page = page;
            double cold = 0, total = 0, maximum = 0;
            for (int frame = 0; frame <= 35; ++frame) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {}
                const auto start = std::chrono::steady_clock::now();
                tick += 20;
                ui.render(model, tick);
                CHECK(ui.pageArtworkReady(library, filtered, theme, page));
                const double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                if (frame == 0) cold = elapsed;
                else { total += elapsed; maximum = std::max(maximum, elapsed); }
            }
            std::cout << "Page " << page + 1 << ": prefetched=" << prefetched
                      << ", preparation=" << preparation << "ms, first=" << cold
                      << "ms, moving mean=" << total / 35 << "ms, max=" << maximum << "ms" << std::endl;
            CHECK(actionAt(ui, theme == neon::Theme::Retro ? 300.0F : 600.0F,
                           theme == neon::Theme::Retro ? 226.0F : 300.0F,
                           neon::UiActionKind::SelectTrack, page * pageSize));
        }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}
} // namespace

void verifyVideoInfo(const std::filesystem::path& output) {
    if (!output.empty()) std::filesystem::create_directories(output);
    auto* canvas = SDL_CreateSurface(1280, 720, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = SDL_CreateSoftwareRenderer(canvas);
    CHECK(renderer);
    if (!renderer) { SDL_DestroySurface(canvas); return; }
    {
        neon::UI ui;
        std::string error;
        CHECK(ui.initialize(renderer, error));
        neon::Track track;
        track.title = "Send Me an Angel";
        track.artist = "Scorpions";
        track.album = "Crazy World";
        track.albumYear = 1990;
        const auto image = ui.videoInfoImage(track);
        CHECK(image.pixels && image.revision);
        if (image.pixels) {
            std::size_t transparent{}, green{}, antialiased{};
            for (int y = 0; y < image.pixels->h; ++y) for (int x = 0; x < image.pixels->w; ++x) {
                Uint8 r{}, g{}, b{}, a{};
                SDL_ReadSurfacePixel(image.pixels, x, y, &r, &g, &b, &a);
                transparent += a == 0;
                green += a > 200 && g > r && g > b;
                antialiased += a > 0 && a < 255;
            }
            CHECK(transparent > static_cast<std::size_t>(image.pixels->w) * image.pixels->h * 8 / 10);
            CHECK(green > 100 && antialiased > 100);
            CHECK(ui.videoInfoImage(track).revision == image.revision);
            // Composite over a varied frame: only glyphs/shadows may obscure it.
            for (int y = 0; y < 720; y += 40) for (int x = 0; x < 1280; x += 40) {
                SDL_SetRenderDrawColor(renderer, static_cast<Uint8>(25 + x / 10),
                    static_cast<Uint8>(30 + y / 8), static_cast<Uint8>(65 + (x + y) / 16), 255);
                const SDL_FRect tile{static_cast<float>(x), static_cast<float>(y), 40, 40};
                SDL_RenderFillRect(renderer, &tile);
            }
            auto* texture = SDL_CreateTextureFromSurface(renderer, image.pixels);
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            const SDL_FRect destination{96, 720.0F - 48 - image.pixels->h,
                static_cast<float>(image.pixels->w), static_cast<float>(image.pixels->h)};
            SDL_RenderTexture(renderer, texture, nullptr, &destination);
            SDL_RenderPresent(renderer);
            if (!output.empty()) CHECK(IMG_SavePNG(canvas, neon::pathToUtf8(output / "video-info.png").c_str()));
            SDL_DestroyTexture(texture);
            track.title = "İstanbul — Gökyüzünde Bir Yalnızlık / Live at the Royal Albert Hall — Extended Performance";
            const auto wrapped = ui.videoInfoImage(track);
            CHECK(wrapped.revision != image.revision && wrapped.pixels);
            CHECK(wrapped.pixels && wrapped.pixels->w <= 1280);
        }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(canvas);
}

int main(int argc, char** argv) {
    using neon::Theme;
    CHECK(neon::pageAfterThemeChange(Theme::Neon, Theme::Retro, 4, 100) == 1);
    CHECK(neon::pageAfterThemeChange(Theme::Neon, Theme::Retro, 4, 100, 42) == 2);
    CHECK(neon::pageAfterThemeChange(Theme::Retro, Theme::Neon, 2, 100, 59) == 6);
    CHECK(neon::pageAfterThemeChange(Theme::Retro, Theme::Neon, 2, 100, 0) == 4);
    CHECK(neon::pageAfterThemeChange(Theme::Retro, Theme::Neon, 1, 21, 20) == 2);
    CHECK(neon::pageAfterThemeChange(Theme::Retro, Theme::Neon, 99, 0) == 0);
    CHECK(neon::pageAfterThemeChange(Theme::Neon, Theme::Retro, 99, 21) == 0);
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) { std::cerr << SDL_GetError(); return 1; }
    if (argc == 3 && std::string_view(argv[1]) == "--preview-video-info") {
        verifyVideoInfo(neon::pathFromUtf8(argv[2]));
        TTF_Quit();
        SDL_Quit();
        std::cout << "Fullscreen video information: " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--library-artwork-only") {
        verifyLibraryArtworkPreparation();
        verifyVideoThumbnailCache();
        TTF_Quit();
        SDL_Quit();
        std::cout << "Library artwork preparation: " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--video-cache-only") {
        verifyVideoThumbnailCache();
        TTF_Quit();
        SDL_Quit();
        std::cout << "Video thumbnail cache checks: " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--preview-vhs") {
        verifyVideoCards(neon::pathFromUtf8(argv[2]), neon::pathFromUtf8(argv[3]));
        TTF_Quit();
        SDL_Quit();
        std::cout << "VHS catalogue checks: " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--preview-video-filter") {
        previewVideoArtistFilter(neon::pathFromUtf8(argv[2]), neon::pathFromUtf8(argv[3]));
        TTF_Quit();
        SDL_Quit();
        std::cout << "Video artist preview checks: " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--profile-paging") {
        profilePaging(neon::pathFromUtf8(argv[2]));
        TTF_Quit();
        SDL_Quit();
        return failures ? 1 : 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--preview-admin-sources") {
        previewAdminSources(neon::pathFromUtf8(argv[2]));
        TTF_Quit();
        SDL_Quit();
        std::cout << "Admin source controls: " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--preview-video-preparation") {
        previewVideoPreparation(neon::pathFromUtf8(argv[2]));
        TTF_Quit();
        SDL_Quit();
        std::cout << "Video preparation display: " << failures << " failures\n";
        return failures ? 1 : 0;
    }
    std::filesystem::path output;
    const bool artworkOnly = argc == 2 && std::string_view(argv[1]) == "--artwork-only";
    if (argc > 1 && !artworkOnly) {
        output = neon::pathFromUtf8(argv[1]);
        std::filesystem::create_directories(output);
    }
    verifyAsyncArtwork();
    verifyArtworkPrefetch();
    if (!artworkOnly) {
        verifyVideoInfo({});
        verifySize(1920, 1080, output);
        verifySize(1280, 720, output);
        verifySize(1024, 768, output);
    }
    TTF_Quit();
    SDL_Quit();
    std::cout << "Theme UI checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
