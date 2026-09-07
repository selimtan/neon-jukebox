#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "neon/Video.hpp"

namespace {
int failures{};
double maximumUiCallMs{};
#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #expression "\n"; \
        ++failures; \
    } \
} while (false)

template <typename Function> auto timedUiCall(Function&& function) {
    const auto start = std::chrono::steady_clock::now();
    auto result = function();
    maximumUiCallMs = std::max(maximumUiCallMs, std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count());
    return result;
}

void pumpFor(int milliseconds) {
    const auto until = SDL_GetTicks() + milliseconds;
    do {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {}
        SDL_Delay(5);
    } while (SDL_GetTicks() < until);
}

template <typename Predicate> bool waitUntil(Predicate&& ready, int timeoutMs = 10000) {
    const auto until = SDL_GetTicks() + timeoutMs;
    while (!ready() && SDL_GetTicks() < until) pumpFor(5);
    return ready();
}

void appendFourCC(std::vector<std::uint8_t>& bytes, const char* value) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value[i]));
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void patchU32(std::vector<std::uint8_t>& bytes, std::size_t position, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes[position + static_cast<std::size_t>(shift / 8)] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::size_t beginChunk(std::vector<std::uint8_t>& bytes, const char* id) {
    appendFourCC(bytes, id);
    const auto sizePosition = bytes.size();
    appendU32(bytes, 0);
    return sizePosition;
}

void endChunk(std::vector<std::uint8_t>& bytes, std::size_t sizePosition) {
    const auto payloadStart = sizePosition + 4;
    const auto payloadSize = bytes.size() - payloadStart;
    patchU32(bytes, sizePosition, static_cast<std::uint32_t>(payloadSize));
    if ((payloadSize & 1U) != 0U) bytes.push_back(0);
}

// Produces a tiny codec-free RGB AVI. The DirectShow test therefore needs no
// internet access, installed third-party codec, or copyrighted media fixture.
bool writeTestAvi(const std::filesystem::path& path) {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 48;
    constexpr std::uint32_t fps = 10;
    constexpr std::uint32_t frameCount = 20;
    constexpr std::uint32_t stride = ((width * 3U + 3U) / 4U) * 4U;
    constexpr std::uint32_t frameSize = stride * height;

    std::vector<std::uint8_t> bytes;
    appendFourCC(bytes, "RIFF");
    const auto riffSize = bytes.size();
    appendU32(bytes, 0);
    appendFourCC(bytes, "AVI ");

    const auto hdrlSize = beginChunk(bytes, "LIST");
    appendFourCC(bytes, "hdrl");
    const auto avihSize = beginChunk(bytes, "avih");
    appendU32(bytes, 1'000'000U / fps);
    appendU32(bytes, frameSize * fps);
    appendU32(bytes, 0);
    appendU32(bytes, 0);
    appendU32(bytes, frameCount);
    appendU32(bytes, 0);
    appendU32(bytes, 1);
    appendU32(bytes, frameSize);
    appendU32(bytes, width);
    appendU32(bytes, height);
    for (int i = 0; i < 4; ++i) appendU32(bytes, 0);
    endChunk(bytes, avihSize);

    const auto strlSize = beginChunk(bytes, "LIST");
    appendFourCC(bytes, "strl");
    const auto strhSize = beginChunk(bytes, "strh");
    appendFourCC(bytes, "vids");
    appendFourCC(bytes, "DIB ");
    appendU32(bytes, 0);
    appendU16(bytes, 0);
    appendU16(bytes, 0);
    appendU32(bytes, 0);
    appendU32(bytes, 1);
    appendU32(bytes, fps);
    appendU32(bytes, 0);
    appendU32(bytes, frameCount);
    appendU32(bytes, frameSize);
    appendU32(bytes, 0xFFFFFFFFU);
    appendU32(bytes, 0);
    appendU16(bytes, 0);
    appendU16(bytes, 0);
    appendU16(bytes, static_cast<std::uint16_t>(width));
    appendU16(bytes, static_cast<std::uint16_t>(height));
    endChunk(bytes, strhSize);

    const auto strfSize = beginChunk(bytes, "strf");
    appendU32(bytes, 40);
    appendU32(bytes, width);
    appendU32(bytes, height);
    appendU16(bytes, 1);
    appendU16(bytes, 24);
    appendU32(bytes, 0);
    appendU32(bytes, frameSize);
    appendU32(bytes, 0);
    appendU32(bytes, 0);
    appendU32(bytes, 0);
    appendU32(bytes, 0);
    endChunk(bytes, strfSize);
    endChunk(bytes, strlSize);
    endChunk(bytes, hdrlSize);

    const auto moviSize = beginChunk(bytes, "LIST");
    appendFourCC(bytes, "movi");
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        const auto frameChunk = beginChunk(bytes, "00db");
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                bytes.push_back(static_cast<std::uint8_t>((x * 4U + frame * 9U) & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>((y * 5U + frame * 13U) & 0xFFU));
                bytes.push_back(static_cast<std::uint8_t>(((x + y) * 2U + frame * 17U) & 0xFFU));
            }
        }
        endChunk(bytes, frameChunk);
    }
    endChunk(bytes, moviSize);
    patchU32(bytes, riffSize, static_cast<std::uint32_t>(bytes.size() - 8));

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}
}

void verifyUnresponsiveSource(SDL_Window* window, const std::filesystem::path& playable) {
    struct Gate {
        std::atomic_bool entered{}, release{}, exited{}, sawCancellation{};
    };
    for (const bool exitDuringCopy : {false, true}) {
        const auto gate = std::make_shared<Gate>();
        neon::VideoEngine video([gate, playable](const auto&, const auto&, const auto& cancelled, const auto& progress) {
            progress(0);
            gate->entered.store(true);
            // A drive may acknowledge cancellation much later. Own the state
            // even after the player has shut down; do not touch its window.
            while (!gate->release.load()) {
                if (cancelled()) gate->sawCancellation.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            gate->exited.store(true);
            return neon::VideoSourceResult{playable, {}};
        });
        std::string error;
        CHECK(video.initialize(window, error));
        neon::Track slow;
        slow.path = L"\\\\test-video-source\\unresponsive\\clip.mp4";
        slow.fileSize = 1;
        slow.modifiedTicks = 1;
        slow.mediaKind = neon::MediaKind::Video;
        CHECK(timedUiCall([&] { return video.play(slow, 0, error); }));
        CHECK(waitUntil([&] { return gate->entered.load(); }, 2000));
        CHECK(waitUntil([&] { return video.loadingStatus().starts_with("PREPARING VIDEO"); }, 1000));
        const auto started = SDL_GetTicks();
        if (exitDuringCopy) {
            video.shutdown();
            std::cout << "Shutdown with an unresponsive source: " << SDL_GetTicks() - started << " ms\n";
            CHECK(SDL_GetTicks() - started < 1000);
        } else {
            neon::Track next;
            next.path = playable;
            next.mediaKind = neon::MediaKind::Video;
            CHECK(timedUiCall([&] { return video.play(next, 0, error); }));
            CHECK(waitUntil([&] { return !video.loading() && video.positionMs() > 0; }, 2500));
            CHECK(video.takeError().empty());
            CHECK(video.playing() && !video.takeFinished());
            std::cout << "Skip away from an unresponsive source: " << SDL_GetTicks() - started << " ms\n";
            video.shutdown();
        }
        CHECK(waitUntil([&] { return gate->sawCancellation.load(); }, 1000));
        CHECK(!gate->exited.load()); // Both controls returned before the stalled job did.
        gate->release.store(true);
        CHECK(waitUntil([&] { return gate->exited.load(); }, 1000));
    }
}

void verifyPreparedTransitions(SDL_Window* window, const std::filesystem::path& playable) {
    struct Preparation {
        std::atomic_int calls{};
        std::atomic_bool release{true}, cancelled{};
        std::atomic_int stalledEntered{}, stalledExited{};
        std::atomic_bool releaseStalled{};
    };
    const auto preparation = std::make_shared<Preparation>();
    neon::VideoEngine video([preparation, playable](const auto& track, const auto&, const auto& cancelled, const auto& progress) {
        ++preparation->calls;
        progress(0);
        if (track.id.starts_with("stalled")) {
            ++preparation->stalledEntered;
            while (!preparation->releaseStalled.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++preparation->stalledExited;
            return neon::VideoSourceResult{playable, {}};
        }
        if (track.id == "unreadable") return neon::VideoSourceResult{{}, "Unreadable next-video fixture"};
        while (!preparation->release.load()) {
            if (cancelled()) { preparation->cancelled.store(true); return neon::VideoSourceResult{}; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        progress(100);
        return neon::VideoSourceResult{playable, {}};
    });
    std::string error;
    CHECK(video.initialize(window, error));
    neon::Track current;
    current.path = playable;
    current.mediaKind = neon::MediaKind::Video;
    CHECK(video.play(current, 0, error));
    CHECK(waitUntil([&] { return !video.loading() && video.positionMs() > 0; }));
    for (int transition = 0; transition < 4; ++transition) {
        neon::Track next;
        next.id = "prepared-" + std::to_string(transition);
        next.path = L"\\\\next-video-fixture\\clips\\" + std::to_wstring(transition) + L".avi";
        next.fileSize = 1;
        next.modifiedTicks = 1;
        next.mediaKind = neon::MediaKind::Video;
        CHECK(timedUiCall([&] { video.preload(next); return true; }));
        CHECK(waitUntil([&] { return video.preloadState() == neon::VideoEngine::PreloadState::Ready; }, 1500));
        CHECK(video.playing() && !video.loading() && !video.takeFinished());
        CHECK(video.takeError().empty());
        CHECK(preparation->calls == transition + 1);
        // Advance to the real decoder end event, then follow App's stop/play path.
        CHECK(video.seek(std::max<std::int64_t>(0, video.durationMs() - 220)));
        bool ended = false;
        CHECK(waitUntil([&] { ended = ended || video.takeFinished(); return ended; }, 3000));
        CHECK(!video.takeFinished());
        const auto started = SDL_GetTicks();
        video.stop();
        CHECK(video.play(next, 0, error));
        CHECK(waitUntil([&] { return !video.loading() && video.positionMs() > 0; }, 2500));
        CHECK(video.playing() && !video.takeFinished() && video.takeError().empty());
        CHECK(preparation->calls == transition + 1); // The prepared copy is reused.
        CHECK(video.preloadState() == neon::VideoEngine::PreloadState::None);
        std::cout << "Prepared end transition " << transition + 1 << ": " << SDL_GetTicks() - started << " ms\n";
    }
    neon::Track next;
    next.path = L"\\\\next-video-fixture\\clips\\failed.avi";
    next.id = "unreadable";
    next.fileSize = next.modifiedTicks = 1;
    next.mediaKind = neon::MediaKind::Video;
    video.preload(next);
    CHECK(waitUntil([&] { return video.preloadState() == neon::VideoEngine::PreloadState::Failed; }, 1500));
    CHECK(!video.preloadError().empty());
    CHECK(video.playing() && !video.loading() && !video.takeFinished() && video.takeError().empty());

    next.id = "still-preparing";
    next.path = L"\\\\next-video-fixture\\clips\\pending.avi";
    preparation->release.store(false);
    video.preload(next);
    CHECK(waitUntil([&] { return preparation->calls == 6; }, 1500));
    video.stop();
    CHECK(video.play(next, 0, error)); // Adopt a pending copy without starting it again.
    pumpFor(100);
    CHECK(video.loading() && preparation->calls == 6);
    preparation->release.store(true);
    CHECK(waitUntil([&] { return !video.loading() && video.positionMs() > 0; }, 2500));
    CHECK(preparation->calls == 6 && video.takeError().empty());

    next.path = L"\\\\next-video-fixture\\clips\\discarded.avi";
    preparation->release.store(false);
    video.preload(next);
    CHECK(waitUntil([&] { return preparation->calls == 7; }, 1500));
    CHECK(video.play(current, 0, error)); // An explicit request supersedes the shuffle candidate.
    CHECK(waitUntil([&] { return preparation->cancelled.load(); }, 1500));
    CHECK(waitUntil([&] { return !video.loading() && video.positionMs() > 0; }, 2500));
    CHECK(video.takeError().empty() && !video.takeFinished());

    // Two drivers that ignore cancellation on one volume must not block a
    // healthy source on a different volume, nor an already cached/local clip.
    for (int i = 0; i < 2; ++i) {
        next.id = "stalled-" + std::to_string(i);
        next.path = L"\\\\blocked-video-fixture\\clips\\" + std::to_wstring(i) + L".avi";
        video.preload(next);
        CHECK(waitUntil([&] { return preparation->stalledEntered == i + 1; }, 1500));
        video.cancelPreload();
    }
    next.id = "healthy-drive";
    next.path = L"\\\\next-video-fixture\\clips\\healthy.avi";
    preparation->release.store(true);
    video.preload(next);
    CHECK(waitUntil([&] { return video.preloadState() == neon::VideoEngine::PreloadState::Ready; }, 1500));
    CHECK(preparation->stalledExited == 0);
    preparation->releaseStalled.store(true);
    CHECK(waitUntil([&] { return preparation->stalledExited == 2; }, 1500));
    video.shutdown();
}

int main(int argc, char** argv) {
    const bool ownsFixture = argc < 2;
    const auto fixture = ownsFixture
        ? std::filesystem::temp_directory_path() / "neon-jukebox-video-test.avi"
        : std::filesystem::path(argv[1]);
    if (ownsFixture) CHECK(writeTestAvi(fixture));
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        std::cerr << "SDL video initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Neon video test", 640, 360, SDL_WINDOW_HIDDEN);
    CHECK(window != nullptr);
    if (window && !ownsFixture && SDL_getenv("NEON_VIDEO_TEST_VISIBLE")) SDL_ShowWindow(window);
    if (window) {
        if (ownsFixture) {
            verifyUnresponsiveSource(window, fixture);
            verifyPreparedTransitions(window, fixture);
        }
        neon::VideoEngine video;
        neon::AudioEngine effects;
        std::string error;
        CHECK(effects.initialize(error, NEON_EFFECT_FIXTURES));
        CHECK(effects.takeEffectError().empty());
        effects.setEffectsEnabled(true);
        CHECK(video.initialize(window, error));
        CHECK(!video.takeSurfaceTouch());
        neon::Track track;
        track.id = "video-fixture";
        track.path = fixture;
        track.title = "Video fixture";
        track.mediaKind = neon::MediaKind::Video;
        CHECK(std::filesystem::is_regular_file(track.path));
        track.fileSize = std::filesystem::file_size(track.path);
        track.modifiedTicks = std::filesystem::last_write_time(track.path).time_since_epoch().count();
        if (argc > 2 && std::string_view(argv[2]) == "--playback-probe") {
            if (argc > 3 && std::string_view(argv[3]) == "--cancel-preparation") {
                CHECK(timedUiCall([&] { return video.play(track, 0, error); }));
                CHECK(waitUntil([&] { return video.loadingStatus().starts_with("PREPARING VIDEO"); }, 10000));
                const auto stopped = SDL_GetTicks();
                video.shutdown();
                std::cout << "Shutdown during preparation: " << SDL_GetTicks() - stopped << " ms" << std::endl;
                CHECK(SDL_GetTicks() - stopped < 2000);
                CHECK(video.initialize(window, error));
            }
            const auto began = SDL_GetTicks();
            CHECK(video.play(track, 0, error));
            CHECK(waitUntil([&] { return !video.loading(); }, 40000));
            const auto loaded = SDL_GetTicks() - began;
            const auto openError = video.takeError();
            std::cout << "Opened in " << loaded << " ms; error=" << openError << std::endl;
            CHECK(openError.empty());
            CHECK(video.playing());
            video.render({0, 0, 640, 360});
            CHECK(waitUntil([&] { return video.positionMs() >= 1500; }, 10000));
            std::cout << "Playback position " << video.positionMs() << " / " << video.durationMs() << std::endl;
            const auto target = video.durationMs() / 2;
            CHECK(video.seek(target));
            CHECK(waitUntil([&] { return video.positionMs() > target + 500; }, 10000));
            const auto playbackError = video.takeError();
            std::cout << "Playback after seek " << video.positionMs() << "; error=" << playbackError << std::endl;
            CHECK(playbackError.empty());
            CHECK(video.loadingStatus().empty());
            video.shutdown();
            effects.shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            std::cout << "Playback probe: " << failures << " failures\n";
            return failures ? 1 : 0;
        }
        CHECK(timedUiCall([&] { return video.play(track, 0, error); }));
        CHECK(waitUntil([&] { return !video.loading(); }));
        if (!error.empty()) std::cerr << error << '\n';
        CHECK(video.durationMs() > 0);
        if (video.durationMs() > 0) {
            pumpFor(120);
            video.render({0, 0, 640, 360});
            CHECK(video.playing());
            CHECK(waitUntil([&] { return video.positionMs() > 0; }, 5000));
            const auto beforeEffect = video.positionMs();
            CHECK(effects.playEffect(neon::UiSoundEffect::Coin));
            pumpFor(160);
            CHECK(effects.visualization().peakLeft > 0.01F);
            CHECK(video.playing());
            CHECK(video.positionMs() >= beforeEffect);
            CHECK(!effects.playing() && !effects.takeFinished());
            if (!ownsFixture) {
                bool reactive{};
                for (int attempt = 0; attempt < 60 && !reactive; ++attempt) {
                    pumpFor(50);
                    const auto frame = video.visualization();
                    reactive = frame.rmsLeft > 0.001F || frame.rmsRight > 0.001F ||
                        std::ranges::any_of(frame.bands,
                            [](float value) { return value > 0.001F; });
                }
                CHECK(reactive);
            } else {
                // The generated AVI intentionally has no audio, but exercising
                // the consumer verifies that an unavailable/quiet loopback
                // device remains a safe zero-level visualization.
                (void)video.visualization();
            }
            CHECK(video.pause());
            CHECK(video.paused());
            pumpFor(120); // Wait for the asynchronous decoder acknowledgement.
            const auto pausedPosition = video.positionMs();
            CHECK(effects.playEffect(neon::UiSoundEffect::PageTurn));
            pumpFor(80);
            CHECK(video.paused());
            CHECK(std::abs(video.positionMs() - pausedPosition) < 50);
            CHECK(video.seek(video.durationMs() / 2));
            CHECK(video.resume());
            CHECK(video.playing());
            CHECK(video.seek(std::max<std::int64_t>(0, video.durationMs() - 150)));
            bool reachedEnd{};
            for (int attempt = 0; attempt < 100 && !reachedEnd; ++attempt) {
                pumpFor(50);
                reachedEnd = video.takeFinished();
            }
            CHECK(reachedEnd);
        }
        // A burst of Skip/open/stop/seek commands is coalesced. Stale completion
        // or errors from discarded loads must never finish the newest video.
        for (int skip = 0; skip < 30; ++skip) {
            CHECK(timedUiCall([&] { return video.play(track, 0, error); }));
            timedUiCall([&] { video.stop(); return true; });
        }
        CHECK(video.play(track, 0, error));
        CHECK(video.seek(300));
        CHECK(video.pause());
        CHECK(waitUntil([&] { return !video.loading(); }));
        pumpFor(250);
        CHECK(video.paused() && !video.takeFinished());
        CHECK(waitUntil([&] { return std::abs(video.positionMs() - 300) < 150; }, 5000));
        CHECK(video.takeError().empty());
        CHECK(video.resume());
        for (int frame = 0; frame < 50; ++frame) {
            timedUiCall([&] {
                video.render(frame % 2 ? SDL_FRect{0, 0, 640, 360} : SDL_FRect{0, 0, 800, 450});
                (void)video.positionMs();
                (void)video.durationMs();
                (void)video.visualization();
                video.setVolume(0.25F);
                return true;
            });
            pumpFor(10);
        }
        CHECK(video.positionMs() > 300);
        neon::Track missing = track;
        missing.path = fixture.parent_path() / "missing-neon-video-fixture.mp4";
        CHECK(timedUiCall([&] { return video.play(missing, 0, error); }));
        CHECK(waitUntil([&] { return !video.loading(); }));
        CHECK(video.takeFinished() && !video.takeError().empty());
        CHECK(video.play(track, 0, error));
        CHECK(waitUntil([&] { return !video.loading(); }));
        CHECK(video.playing() && !video.takeFinished() && video.takeError().empty());
        timedUiCall([&] { video.stop(); return true; });
        CHECK(!video.playing() && !video.loading());
        pumpFor(150);
        CHECK(!video.takeFinished());
        CHECK(maximumUiCallMs < 100.0);
        std::cout << "Maximum video UI call: " << maximumUiCallMs << " ms\n";
        for (bool loading : {false, true}) {
            CHECK(video.initialize(window, error));
            CHECK(video.play(track, 0, error));
            if (!loading) CHECK(waitUntil([&] { return !video.loading(); }));
            else pumpFor(5);
            const auto started = SDL_GetTicks();
            video.requestShutdown();
            video.shutdown();
            const auto elapsed = SDL_GetTicks() - started;
            CHECK(elapsed < 2000);
            CHECK(!video.initialized() && !video.playing());
            std::cout << "Video shutdown (" << (loading ? "opening" : "playing") << "): " << elapsed << " ms\n";
            video.shutdown();
        }
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    if (ownsFixture) {
        std::error_code ec;
        std::filesystem::remove(fixture, ec);
    }
    if (failures == 0) std::cout << "Neon Jukebox video integration test passed.\n";
    return failures == 0 ? 0 : 1;
}
