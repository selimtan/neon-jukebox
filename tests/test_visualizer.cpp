#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "neon/Visualizer.hpp"
#include "neon/Drawing.hpp"

namespace {
struct GridSample { int columns{}; int rows{}; int minWidth{10000}; int minHeight{10000}; };

bool verifyAntialiasing() {
    bool passed = true;
    for (const int width : {342, 426, 640, 1280}) {
        auto* surface = SDL_CreateSurface(width, width / 2 + 20, SDL_PIXELFORMAT_RGBA32);
        auto* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        if (!renderer) { if (surface) SDL_DestroySurface(surface); return false; }
        SDL_SetRenderLogicalPresentation(renderer, 640, 320, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        const float pixel = neon::drawing::pixelSize(renderer);
        passed = passed && std::abs(pixel - 640.0F / width) < 0.001F;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        neon::drawing::line(renderer, 20.25F * pixel, 20.25F * pixel,
            200.25F * pixel, 74.25F * pixel, {255, 255, 255, 255});
        // Every column of a shallow diagonal must have continuous coverage,
        // with intermediate intensities instead of a staircase of solid pixels.
        SDL_RenderPresent(renderer);
        int partial = 0;
        for (int x = 25; x < 195; ++x) {
            const int centerY = static_cast<int>(30.25F + (x - 20.25F) * 0.3F);
            int energy = 0;
            for (int y = centerY - 3; y <= centerY + 3; ++y) {
                Uint8 r{}, g{}, b{}, a{};
                SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
                energy += r;
                if (r > 5 && r < 250) ++partial;
            }
            if (energy < 80 || energy > 540) passed = false;
        }
        passed = passed && partial > 170;
        // Offscreen targets have their own scale; page-turn snapshots must not
        // inherit the main window's feather width.
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        SDL_SetRenderScale(renderer, 2, 2);
        passed = passed && std::abs(neon::drawing::pixelSize(renderer) - 0.5F) < 0.001F;
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
    if (!passed) std::cerr << "Antialiased stroke coverage/scale checks failed\n";
    return passed;
}

bool verifyRetroPhosphor(const std::filesystem::path& output) {
    bool passed = true;
    for (const int width : {436, 233}) {
        const int height = width == 436 ? 134 : 72;
        auto* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
        auto* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        if (!renderer) { if (surface) SDL_DestroySurface(surface); return false; }
        neon::AudioVisualizationFrame frame;
        for (std::size_t i = 0; i < frame.leftWaveform.size(); ++i) {
            const float t = static_cast<float>(i) / (frame.leftWaveform.size() - 1);
            frame.leftWaveform[i] = 0.7F * std::sin(t * 30);
            frame.rightWaveform[i] = 0.6F * std::sin(t * 30 + 0.2F);
        }
        neon::VisualizerRenderer visualizer;
        for (std::uint64_t tick = 1000; tick <= 1400; tick += 40) visualizer.update(frame, tick);
        visualizer.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                        neon::VisualizerMode::RetroPhosphorScope);
        SDL_RenderPresent(renderer);
        int brightPixels = 0;
        for (int y = 3; y < height - 3; ++y) for (int x = 3; x < width - 3; ++x) {
            Uint8 r{}, g{}, b{}, a{};
            SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
            if (g > 180) {
                ++brightPixels;
                passed = passed && g > r && r > b + 20;
            }
        }
        Uint8 r{}, g{}, b{}, a{};
        SDL_ReadSurfacePixel(surface, 7, 7, &r, &g, &b, &a);
        passed = passed && g > r && g > b && g < 80 && brightPixels > width;
        if (!output.empty()) {
            const auto file = output.parent_path() / ("retro-phosphor-" + std::to_string(width) + ".bmp");
            passed = SDL_SaveBMP(surface, file.string().c_str()) && passed;
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
    if (!passed) std::cerr << "Retro phosphor palette/waveform checks failed\n";
    return passed;
}

bool verifyResponsiveGrid(const std::filesystem::path& output) {
    bool passed = true;
    const auto check = [&](bool condition, std::string_view message) {
        if (!condition) { std::cerr << "Responsive visualizer: " << message << '\n'; passed = false; }
    };
    const auto render = [&](int width, int height, float density, int singleBand, std::string_view suffix) {
        GridSample result;
        SDL_Surface* surface = SDL_CreateSurface(width + 20, height + 20, SDL_PIXELFORMAT_RGBA32);
        SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        check(renderer != nullptr, "renderer creation");
        if (!renderer) { if (surface) SDL_DestroySurface(surface); return result; }
        SDL_SetRenderDrawColor(renderer, 13, 29, 41, 255);
        SDL_RenderClear(renderer);
        neon::AudioVisualizationFrame frame;
        if (singleBand < 0) frame.bands.fill(1.0F);
        else frame.bands[static_cast<std::size_t>(singleBand)] = 1.0F;
        neon::VisualizerRenderer visualizer;
        for (std::uint64_t tick = 1000; tick <= 5600; tick += 60) visualizer.update(frame, tick);
        SDL_SetRenderScale(renderer, density, density);
        visualizer.draw(renderer, {10 / density, 10 / density, width / density, height / density},
                        neon::VisualizerMode::NeonMosaic, density);
        check(!SDL_RenderClipEnabled(renderer), "clip state restored");
        SDL_RenderPresent(renderer);
        std::vector<bool> columnLit(width, false), rowLit(height, false);
        for (int y = 0; y < height + 20; ++y) {
            for (int x = 0; x < width + 20; ++x) {
                Uint8 r{}, g{}, b{}, a{};
                SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
                if (x < 10 || y < 10 || x >= width + 10 || y >= height + 10) {
                    if (r != 13 || g != 29 || b != 41) {
                        check(false, "drawing escaped its panel");
                        y = height + 20;
                        break;
                    }
                } else if (std::max({r, g, b}) >= 70) {
                    columnLit[x - 10] = true;
                    rowLit[y - 10] = true;
                }
            }
        }
        const auto countRuns = [](const std::vector<bool>& mask, int& minimum) {
            int count = 0, length = 0;
            for (std::size_t i = 0; i <= mask.size(); ++i) {
                if (i < mask.size() && mask[i]) ++length;
                else if (length) { ++count; minimum = std::min(minimum, length); length = 0; }
            }
            return count;
        };
        result.columns = countRuns(columnLit, result.minWidth);
        result.rows = countRuns(rowLit, result.minHeight);
        if (!output.empty() && !suffix.empty()) {
            auto file = output.parent_path() / (output.stem().string() + "-" + std::string(suffix) + ".bmp");
            check(SDL_SaveBMP(surface, file.string().c_str()), "saving responsive snapshot");
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
        return result;
    };
    const auto large = render(1200, 440, 1, -1, "mosaic-large");
    const auto compact = render(436, 154, 1, -1, "mosaic-compact");
    const auto shallow = render(436, 68, 1, -1, "mosaic-shallow");
    const auto small = render(218, 77, 1, -1, "mosaic-small");
    const auto scaled = render(218, 77, 0.5F, -1, "");
    check(large.columns > compact.columns && compact.columns > small.columns, "columns must follow width");
    check(large.rows > compact.rows && compact.rows > shallow.rows, "segments must follow height");
    check(shallow.columns == compact.columns, "height alone must not change frequency columns");
    check(small.columns == scaled.columns && small.rows == scaled.rows, "layout must use physical pixels");
    for (const auto& grid : {compact, shallow, small}) {
        check(grid.columns >= 2 && grid.rows >= 2, "small panel must retain a useful grid");
        check(grid.minWidth >= 10 && grid.minHeight >= 5, "segments must remain legible");
    }
    for (int band : {0, 2, 31, 63}) {
        const auto peak = render(218, 77, 1, band, "");
        check(peak.columns > 0 && peak.rows > 0, "reduced columns must retain narrow frequency peaks");
    }
    std::cout << "Mosaic grids: large " << large.columns << 'x' << large.rows
              << ", compact " << compact.columns << 'x' << compact.rows
              << ", shallow " << shallow.columns << 'x' << shallow.rows
              << ", small " << small.columns << 'x' << small.rows << '\n';
    return passed;
}
} // namespace

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
    const int cellWidth = requestedMode == "compact" ? 436 : requestedMode == "tiny" ? 233 : 385;
    const int cellHeight = requestedMode == "compact" ? 154 : requestedMode == "tiny" ? 82 : 220;
    const int surfaceWidth = singleMode ? 1670 : (cellWidth + 20) * 4 + 20;
    const int surfaceHeight = singleMode
        ? 610
        : static_cast<int>((neon::visualizerModeCount + 3U) / 4U) * (cellHeight + 20) + 10;
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
            const float x = 20.0F + static_cast<float>(index % 4) * (cellWidth + 20.0F);
            const float y = 10.0F + static_cast<float>(index / 4) * (cellHeight + 20.0F);
            visualizer.draw(renderer, {x, y, static_cast<float>(cellWidth), static_cast<float>(cellHeight)},
                            static_cast<neon::VisualizerMode>(index));
        }
    }
    SDL_RenderPresent(renderer);

    bool saved = true;
    if (argc > 1 && argv[1]) {
        saved = SDL_SaveBMP(surface, argv[1]);
        if (!saved) std::cerr << "Visualizer snapshot failed: " << SDL_GetError() << '\n';
    }

    const bool responsive = verifyResponsiveGrid(argc > 1 && argv[1] ? std::filesystem::path(argv[1]) : std::filesystem::path{});
    const bool retro = verifyRetroPhosphor(argc > 1 && argv[1] ? std::filesystem::path(argv[1]) : std::filesystem::path{});
    const bool antialiasing = verifyAntialiasing();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    for (auto& [_, font] : fonts) if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    if (saved && !fontError && responsive && retro && antialiasing) {
        std::cout << (vintageOnly ? "Vintage Flat VU rendered successfully.\n"
                     : owOnly ? "OW Level Meter rendered successfully.\n"
                     : rackOnly ? "Rackmount Spectrum rendered successfully.\n"
                     : greenOnly ? "Green dB Meter rendered successfully.\n"
                     : skylineOnly ? "Spectrum Skyline rendered successfully.\n"
                     : mosaicOnly ? "Neon Mosaic rendered successfully.\n"
                     : tripleOnly ? "Triple Sound Meter rendered successfully.\n"
                     : warmOnly ? "Warm Twin VU rendered successfully.\n"
                                : "All twenty-nine visualizer modes rendered successfully.\n");
    }
    return saved && !fontError && responsive && retro && antialiasing ? 0 : 1;
}
