#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

#include <SDL3/SDL_render.h>

#include "neon/Audio.hpp"
#include "neon/Models.hpp"

namespace neon {

class VisualizerRenderer {
public:
    using TextRenderer = std::function<void(std::string_view, float, float, float,
                                            float, SDL_Color)>;

    void update(const AudioVisualizationFrame& frame, std::uint64_t ticks);
    void draw(SDL_Renderer* renderer, const SDL_FRect& rect, VisualizerMode mode) const;
    void setTextRenderer(TextRenderer renderer);

    [[nodiscard]] static std::string_view name(VisualizerMode mode);
    [[nodiscard]] static std::string_view subtitle(VisualizerMode mode);

private:
    static constexpr std::size_t waterfallRows = 72;
    static constexpr std::size_t waveformHistoryRows = 10;

    void drawAurora(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawReferenceVu(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawNeonArcVu(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawMirrorStage(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawWaterfall(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawOrbitVinyl(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawStereoVector(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawSignalRibbon(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawStudioLed(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawPrecisionLevels(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawCavaMonstercat(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawPrismReflect(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawPhosphorScope(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawLissajousPro(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawRadialInferno(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawCircularWave(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawSpectrogramMagma(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawMilkdropMesh(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawParticleGalaxy(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawMasteringDashboard(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawVintageFlatVu(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawOwLevelMeter(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawRackmountSpectrum(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawGreenDbMeter(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawSpectrumSkyline(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawNeonMosaic(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawTripleSoundMeter(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void drawWarmTwinVu(SDL_Renderer* renderer, const SDL_FRect& rect) const;
    void pixelText(SDL_Renderer* renderer, std::string_view text, float centerX,
                   float top, float pixel, SDL_Color color) const;

    AudioVisualizationFrame frame_{};
    std::array<float, AudioVisualizationFrame::bandCount> displayBands_{};
    std::array<float, AudioVisualizationFrame::bandCount> gravityBands_{};
    std::array<float, AudioVisualizationFrame::bandCount> fallVelocity_{};
    std::array<float, AudioVisualizationFrame::bandCount> peakHold_{};
    std::array<float, AudioVisualizationFrame::bandCount> peakAge_{};
    std::array<std::array<float, AudioVisualizationFrame::bandCount>, waterfallRows> waterfall_{};
    std::array<std::array<float, AudioVisualizationFrame::waveformSampleCount>, waveformHistoryRows> leftHistory_{};
    std::array<std::array<float, AudioVisualizationFrame::waveformSampleCount>, waveformHistoryRows> rightHistory_{};
    std::size_t waterfallHead_{};
    std::size_t waterfallCount_{};
    std::size_t waveformHead_{};
    std::size_t waveformCount_{};
    float vuLeft_{};
    float vuRight_{};
    float vintagePeakLeft_{};
    float vintagePeakRight_{};
    float owPeakLeft_{};
    float owPeakRight_{};
    float owPeakAgeLeft_{};
    float owPeakAgeRight_{};
    float owClipHold_{};
    float bassEnvelope_{};
    float midEnvelope_{};
    float trebleEnvelope_{};
    float beatPulse_{};
    float rotation_{};
    std::uint64_t lastUpdateTicks_{};
    std::uint64_t lastWaterfallTicks_{};
    TextRenderer textRenderer_;
};

}  // namespace neon
