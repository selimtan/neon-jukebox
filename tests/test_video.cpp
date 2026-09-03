#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "neon/Video.hpp"

namespace {
int failures{};
#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #expression "\n"; \
        ++failures; \
    } \
} while (false)

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

int main(int argc, char** argv) {
    const bool ownsFixture = argc < 2;
    const auto fixture = ownsFixture
        ? std::filesystem::temp_directory_path() / "neon-jukebox-video-test.avi"
        : std::filesystem::path(argv[1]);
    if (ownsFixture) CHECK(writeTestAvi(fixture));
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::cerr << "SDL video initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Neon video test", 640, 360, SDL_WINDOW_HIDDEN);
    CHECK(window != nullptr);
    if (window) {
        neon::VideoEngine video;
        std::string error;
        CHECK(video.initialize(window, error));
        CHECK(!video.takeSurfaceTouch());
        neon::Track track;
        track.id = "video-fixture";
        track.path = fixture;
        track.title = "Video fixture";
        track.mediaKind = neon::MediaKind::Video;
        CHECK(std::filesystem::is_regular_file(track.path));
        CHECK(video.play(track, 0, error));
        if (!error.empty()) std::cerr << error << '\n';
        CHECK(video.durationMs() > 0);
        if (video.durationMs() > 0) {
            SDL_Delay(120);
            video.render({0, 0, 640, 360});
            CHECK(video.playing());
            CHECK(video.positionMs() > 0);
            if (!ownsFixture) {
                bool reactive{};
                for (int attempt = 0; attempt < 60 && !reactive; ++attempt) {
                    SDL_Delay(50);
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
            CHECK(video.seek(video.durationMs() / 2));
            CHECK(video.resume());
            CHECK(video.playing());
            CHECK(video.seek(std::max<std::int64_t>(0, video.durationMs() - 150)));
            bool reachedEnd{};
            for (int attempt = 0; attempt < 40 && !reachedEnd; ++attempt) {
                SDL_Delay(50);
                reachedEnd = video.takeFinished();
            }
            CHECK(reachedEnd);
        }
        video.stop();
        video.shutdown();
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
