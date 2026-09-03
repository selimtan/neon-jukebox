#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <unordered_map>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "neon/Visualizer.hpp"

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL video initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }
    if (!TTF_Init()) {
        std::cerr << "SDL_ttf initialization failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }

    const std::string_view requestedMode = argc > 2 && argv[2] ? std::string_view(argv[2]) : std::string_view{};
    const bool vintageOnly = requestedMode == "vintage";
    const bool owOnly = requestedMode == "ow";
    const bool rackOnly = requestedMode == "rack";
    const bool greenOnly = requestedMode == "green";
    const bool skylineOnly = requestedMode == "skyline";
    const bool mosaicOnly = requestedMode == "mosaic";
    const bool tripleOnly = requestedMode == "triple";
    const bool warmOnly = requestedMode == "warm";
    const bool singleMode = vintageOnly || owOnly || rackOnly || greenOnly || skylineOnly ||
                            mosaicOnly || tripleOnly || warmOnly;
    const int surfaceWidth = singleMode ? 1670 : 1640;
    const int surfaceHeight = singleMode
        ? 610
        : static_cast<int>((neon::visualizerModeCount + 3U) / 4U) * 240 + 10;
    SDL_Surface* surface = SDL_CreateSurface(surfaceWidth, surfaceHeight, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!surface || !renderer) {
        std::cerr << "Software renderer creation failed: " << SDL_GetError() << '\n';
        if (surface) SDL_DestroySurface(surface);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    neon::AudioVisualizationFrame frame;
    for (std::size_t i = 0; i < frame.bands.size(); ++i) {
        const float position = static_cast<float>(i) / static_cast<float>(frame.bands.size() - 1);
        frame.bands[i] = std::clamp(0.18F + 0.7F * std::abs(std::sin(position * 9.0F)), 0.0F, 1.0F);
    }
    for (std::size_t i = 0; i < frame.leftWaveform.size(); ++i) {
        const float position = static_cast<float>(i) / static_cast<float>(frame.leftWaveform.size() - 1);
        frame.leftWaveform[i] = std::sin(position * 38.0F) * std::sin(position * 3.14159265F) * 0.72F;
        frame.rightWaveform[i] = std::sin(position * 41.0F + 0.7F) * std::sin(position * 3.14159265F) * 0.62F;
    }
    frame.rmsLeft = 0.68F;
    frame.rmsRight = 0.57F;
    frame.peakLeft = 0.86F;
    frame.peakRight = 0.76F;

    neon::VisualizerRenderer visualizer;
    const std::filesystem::path monoFont = std::filesystem::exists("C:/Windows/Fonts/consola.ttf")
        ? std::filesystem::path("C:/Windows/Fonts/consola.ttf")
        : std::filesystem::path("C:/Windows/Fonts/segoeui.ttf");
    std::unordered_map<int, TTF_Font*> fonts;
    bool fontError = false;
    visualizer.setTextRenderer(
        [&](std::string_view value, float centerX, float top, float maxWidth,
            float maxHeight, SDL_Color color) {
            const int size = std::max(5, static_cast<int>(std::ceil(maxHeight * 1.25F)));
            TTF_Font*& font = fonts[size];
            if (!font) font = TTF_OpenFont(monoFont.string().c_str(), static_cast<float>(size));
            SDL_Surface* textSurface = font
                ? TTF_RenderText_Blended(font, value.data(), value.size(), color) : nullptr;
            if (!textSurface) {
                fontError = true;
                return;
            }
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, textSurface);
            float fit = 1.0F;
            if (static_cast<float>(textSurface->w) > maxWidth)
                fit = std::min(fit, maxWidth / static_cast<float>(textSurface->w));
            if (static_cast<float>(textSurface->h) > maxHeight)
                fit = std::min(fit, maxHeight / static_cast<float>(textSurface->h));
            const float width = static_cast<float>(textSurface->w) * fit;
            const float height = static_cast<float>(textSurface->h) * fit;
            SDL_DestroySurface(textSurface);
            if (!texture) {
                fontError = true;
                return;
            }
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            const SDL_FRect destination{centerX - width * 0.5F, top, width, height};
            SDL_RenderTexture(renderer, texture, nullptr, &destination);
            SDL_DestroyTexture(texture);
        });
    for (std::uint64_t tick = 1000; tick <= 5600; tick += 60) visualizer.update(frame, tick);
    SDL_SetRenderDrawColor(renderer, 3, 5, 14, 255);
    SDL_RenderClear(renderer);
    if (singleMode) {
        const auto mode = vintageOnly ? neon::VisualizerMode::VintageFlatVu
                        : owOnly ? neon::VisualizerMode::OwLevelMeter
                        : rackOnly ? neon::VisualizerMode::RackmountSpectrum
                        : greenOnly ? neon::VisualizerMode::GreenDbMeter
                        : skylineOnly ? neon::VisualizerMode::SpectrumSkyline
                        : mosaicOnly ? neon::VisualizerMode::NeonMosaic
                        : tripleOnly ? neon::VisualizerMode::TripleSoundMeter
                                     : neon::VisualizerMode::WarmTwinVu;
        visualizer.draw(renderer, {0.0F, 0.0F, 1670.0F, 610.0F}, mode);
    } else {
        for (std::size_t index = 0; index < neon::visualizerModeCount; ++index) {
            const float x = 20.0F + static_cast<float>(index % 4) * 405.0F;
            const float y = 10.0F + static_cast<float>(index / 4) * 240.0F;
            visualizer.draw(renderer, {x, y, 385.0F, 220.0F},
                            static_cast<neon::VisualizerMode>(index));
        }
    }
    SDL_RenderPresent(renderer);

    bool saved = true;
    if (argc > 1 && argv[1]) {
        saved = SDL_SaveBMP(surface, argv[1]);
        if (!saved) std::cerr << "Visualizer snapshot failed: " << SDL_GetError() << '\n';
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    for (auto& [_, font] : fonts) if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    if (saved && !fontError) {
        std::cout << (vintageOnly ? "Vintage Flat VU rendered successfully.\n"
                     : owOnly ? "OW Level Meter rendered successfully.\n"
                     : rackOnly ? "Rackmount Spectrum rendered successfully.\n"
                     : greenOnly ? "Green dB Meter rendered successfully.\n"
                     : skylineOnly ? "Spectrum Skyline rendered successfully.\n"
                     : mosaicOnly ? "Neon Mosaic rendered successfully.\n"
                     : tripleOnly ? "Triple Sound Meter rendered successfully.\n"
                     : warmOnly ? "Warm Twin VU rendered successfully.\n"
                                : "All twenty-eight visualizer modes rendered successfully.\n");
    }
    return saved && !fontError ? 0 : 1;
}
