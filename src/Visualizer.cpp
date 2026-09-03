#include "neon/Visualizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace neon {
namespace {

constexpr float pi = 3.14159265358979323846F;
constexpr SDL_Color ink{237, 245, 255, 255};
constexpr SDL_Color muted{137, 153, 182, 255};
constexpr SDL_Color cyan{0, 239, 255, 255};
constexpr SDL_Color blue{75, 156, 255, 255};
constexpr SDL_Color violet{157, 121, 255, 255};
constexpr SDL_Color pink{255, 44, 190, 255};
constexpr SDL_Color amber{255, 179, 77, 255};
constexpr SDL_Color green{45, 229, 141, 255};
constexpr SDL_Color red{255, 79, 104, 255};
constexpr SDL_Color navy{7, 12, 27, 255};
constexpr SDL_Color phosphor{93, 255, 184, 255};

SDL_Color withAlpha(SDL_Color color, Uint8 alpha) {
    color.a = alpha;
    return color;
}

SDL_Color mix(SDL_Color left, SDL_Color right, float amount) {
    amount = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [amount](Uint8 a, Uint8 b) {
        return static_cast<Uint8>(std::lround(static_cast<float>(a) * (1.0F - amount) +
                                              static_cast<float>(b) * amount));
    };
    return {channel(left.r, right.r), channel(left.g, right.g), channel(left.b, right.b),
            channel(left.a, right.a)};
}

SDL_Color hsv(float hue, float saturation = 0.86F, float value = 1.0F,
              Uint8 alpha = 255) {
    hue -= std::floor(hue);
    const float chroma = value * saturation;
    const float section = hue * 6.0F;
    const float x = chroma * (1.0F - std::abs(std::fmod(section, 2.0F) - 1.0F));
    float r{}, g{}, b{};
    if (section < 1.0F) { r = chroma; g = x; }
    else if (section < 2.0F) { r = x; g = chroma; }
    else if (section < 3.0F) { g = chroma; b = x; }
    else if (section < 4.0F) { g = x; b = chroma; }
    else if (section < 5.0F) { r = x; b = chroma; }
    else { r = chroma; b = x; }
    const float match = value - chroma;
    return {static_cast<Uint8>((r + match) * 255.0F),
            static_cast<Uint8>((g + match) * 255.0F),
            static_cast<Uint8>((b + match) * 255.0F), alpha};
}

SDL_Color magma(float value, Uint8 alpha = 255) {
    value = std::clamp(value, 0.0F, 1.0F);
    if (value < 0.26F) return mix({4, 3, 18, alpha}, {70, 16, 116, alpha}, value / 0.26F);
    if (value < 0.55F) return mix({70, 16, 116, alpha}, {201, 43, 102, alpha}, (value - 0.26F) / 0.29F);
    if (value < 0.80F) return mix({201, 43, 102, alpha}, {249, 120, 55, alpha}, (value - 0.55F) / 0.25F);
    return mix({249, 120, 55, alpha}, {255, 244, 171, alpha}, (value - 0.80F) / 0.20F);
}

void color(SDL_Renderer* renderer, SDL_Color value) {
    SDL_SetRenderDrawColor(renderer, value.r, value.g, value.b, value.a);
}

void fill(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color value) {
    color(renderer, value);
    SDL_RenderFillRect(renderer, &rect);
}

void line(SDL_Renderer* renderer, float x1, float y1, float x2, float y2, SDL_Color value) {
    color(renderer, value);
    SDL_RenderLine(renderer, x1, y1, x2, y2);
}

void polyline(SDL_Renderer* renderer, const std::vector<SDL_FPoint>& points,
              SDL_Color value, int width = 1) {
    if (points.size() < 2) return;
    color(renderer, value);
    const int half = std::max(0, width / 2);
    for (int offset = -half; offset <= half; ++offset) {
        if (offset == 0) {
            SDL_RenderLines(renderer, points.data(), static_cast<int>(points.size()));
            continue;
        }
        std::vector<SDL_FPoint> shifted = points;
        for (auto& point : shifted) point.y += static_cast<float>(offset);
        SDL_RenderLines(renderer, shifted.data(), static_cast<int>(shifted.size()));
    }
}

void glowLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2,
              SDL_Color value, float width = 2.0F) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length = std::max(0.001F, std::sqrt(dx * dx + dy * dy));
    const float nx = -dy / length;
    const float ny = dx / length;
    for (int layer = 5; layer >= 1; --layer) {
        const float offset = width * static_cast<float>(layer) * 0.42F;
        const Uint8 alpha = static_cast<Uint8>(8 + (5 - layer) * 5);
        line(renderer, x1 + nx * offset, y1 + ny * offset,
             x2 + nx * offset, y2 + ny * offset, withAlpha(value, alpha));
        line(renderer, x1 - nx * offset, y1 - ny * offset,
             x2 - nx * offset, y2 - ny * offset, withAlpha(value, alpha));
    }
    line(renderer, x1, y1, x2, y2, value);
}

void arc(SDL_Renderer* renderer, float cx, float cy, float radius, float start, float end,
         SDL_Color value, int width = 1, int segments = 72) {
    std::vector<SDL_FPoint> points;
    points.reserve(static_cast<std::size_t>(segments + 1));
    for (int i = 0; i <= segments; ++i) {
        const float angle = start + (end - start) * static_cast<float>(i) / static_cast<float>(segments);
        points.push_back({cx + std::cos(angle) * radius, cy + std::sin(angle) * radius});
    }
    polyline(renderer, points, value, width);
}

void circle(SDL_Renderer* renderer, float cx, float cy, float radius, SDL_Color value, int width = 1) {
    arc(renderer, cx, cy, radius, 0.0F, pi * 2.0F, value, width, 96);
}

void fillCircle(SDL_Renderer* renderer, float cx, float cy, float radius, SDL_Color value) {
    color(renderer, value);
    const int extent = static_cast<int>(std::ceil(radius));
    for (int y = -extent; y <= extent; ++y) {
        const float half = std::sqrt(std::max(0.0F, radius * radius - static_cast<float>(y * y)));
        SDL_RenderLine(renderer, cx - half, cy + static_cast<float>(y),
                      cx + half, cy + static_cast<float>(y));
    }
}

void base(SDL_Renderer* renderer, const SDL_FRect& rect, bool warm = false) {
    constexpr int strips = 18;
    for (int strip = 0; strip < strips; ++strip) {
        const float t = static_cast<float>(strip) / static_cast<float>(strips - 1);
        const SDL_Color top = warm ? SDL_Color{23, 18, 13, 255} : SDL_Color{5, 7, 19, 255};
        const SDL_Color bottom = warm ? SDL_Color{5, 6, 8, 255} : SDL_Color{9, 13, 28, 255};
        const SDL_FRect band{rect.x, rect.y + rect.h * t,
                             rect.w, rect.h / static_cast<float>(strips) + 1.0F};
        fill(renderer, band, mix(top, bottom, t));
    }
    if (!warm) {
        for (int row = 1; row < 6; ++row) {
            const float y = rect.y + rect.h * static_cast<float>(row) / 6.0F;
            line(renderer, rect.x, y, rect.x + rect.w, y, {84, 112, 153, 28});
        }
    }
    fill(renderer, {rect.x, rect.y, rect.w, 1.0F}, warm ? SDL_Color{210, 166, 91, 52}
                                                        : SDL_Color{86, 214, 255, 52});
    fill(renderer, {rect.x, rect.y + rect.h - 1.0F, rect.w, 1.0F}, {0, 0, 0, 180});
    fill(renderer, {rect.x, rect.y, 1.0F, rect.h}, {255, 255, 255, 24});
    fill(renderer, {rect.x + rect.w - 1.0F, rect.y, 1.0F, rect.h}, {0, 0, 0, 160});
}

float bandAt(const std::array<float, AudioVisualizationFrame::bandCount>& bands,
             std::size_t index, std::size_t count) {
    if (count <= 1) return bands.front();
    const float source = static_cast<float>(index) * static_cast<float>(bands.size() - 1) /
                         static_cast<float>(count - 1);
    const auto low = static_cast<std::size_t>(source);
    const auto high = std::min(low + 1, bands.size() - 1);
    return bands[low] + (bands[high] - bands[low]) * (source - static_cast<float>(low));
}

SDL_Color spectrumColor(float value) {
    if (value < 0.52F) return mix(cyan, violet, value / 0.52F);
    return mix(violet, pink, (value - 0.52F) / 0.48F);
}

void glowingRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color value) {
    fill(renderer, {rect.x - 3.0F, rect.y - 3.0F, rect.w + 6.0F, rect.h + 6.0F},
         withAlpha(value, 20));
    fill(renderer, {rect.x - 1.0F, rect.y - 1.0F, rect.w + 2.0F, rect.h + 2.0F},
         withAlpha(value, 55));
    fill(renderer, rect, withAlpha(value, 225));
}

void fillQuad(SDL_Renderer* renderer, const std::array<SDL_FPoint, 4>& points,
              SDL_Color value) {
    const SDL_FColor vertexColor{
        static_cast<float>(value.r) / 255.0F,
        static_cast<float>(value.g) / 255.0F,
        static_cast<float>(value.b) / 255.0F,
        static_cast<float>(value.a) / 255.0F
    };
    std::array<SDL_Vertex, 4> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        vertices[i].position = points[i];
        vertices[i].color = vertexColor;
    }
    constexpr std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

std::array<std::uint8_t, 7> pixelGlyph(char character) {
    switch (character) {
        case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
        case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
        case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
        case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case 'C': return {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
        case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
        case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case 'G': return {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F};
        case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
        case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
        case '+': return {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
        case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06};
        default: return {};
    }
}

void drawPixelText(SDL_Renderer* renderer, std::string_view text, float centerX, float top,
                   float pixel, SDL_Color value) {
    if (text.empty() || pixel <= 0.0F) return;
    const float width = (static_cast<float>(text.size()) * 6.0F - 1.0F) * pixel;
    float originX = centerX - width * 0.5F;
    for (const char character : text) {
        const auto glyph = pixelGlyph(character);
        for (std::size_t row = 0; row < glyph.size(); ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph[row] & (1U << (4 - column))) == 0) continue;
                fill(renderer, {originX + static_cast<float>(column) * pixel,
                                top + static_cast<float>(row) * pixel,
                                pixel + 0.15F, pixel + 0.15F}, value);
            }
        }
        originX += pixel * 6.0F;
    }
}

float amplitudeToVu(float amplitude) {
    const float decibels = 20.0F * std::log10(std::max(0.0001F, amplitude));
    return std::clamp((decibels + 20.0F) / 23.0F, 0.0F, 1.0F);
}

float amplitudeToDb(float amplitude) {
    return std::clamp(20.0F * std::log10(std::max(0.001F, amplitude)), -60.0F, 0.0F);
}

float amplitudeToDbLevel(float amplitude) {
    return (amplitudeToDb(amplitude) + 60.0F) / 60.0F;
}

std::string oneDecimal(float value) {
    std::array<char, 24> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.1f", static_cast<double>(value));
    return buffer.data();
}

std::string twoDecimalsSigned(float value) {
    std::array<char, 24> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%+.2f", static_cast<double>(value));
    return buffer.data();
}

std::size_t modeIndex(VisualizerMode mode) {
    return std::min(static_cast<std::size_t>(mode), visualizerModeCount - 1);
}

}  // namespace

void VisualizerRenderer::setTextRenderer(TextRenderer renderer) {
    textRenderer_ = std::move(renderer);
}

void VisualizerRenderer::pixelText(SDL_Renderer* renderer, std::string_view text,
                                   float centerX, float top, float pixel,
                                   SDL_Color value) const {
    if (!textRenderer_) {
        drawPixelText(renderer, text, centerX, top, pixel, value);
        return;
    }
    const float maxWidth = (static_cast<float>(text.size()) * 6.0F - 1.0F) * pixel;
    textRenderer_(text, centerX, top, maxWidth, pixel * 7.0F, value);
}

void VisualizerRenderer::update(const AudioVisualizationFrame& frame, std::uint64_t ticks) {
    if (ticks == lastUpdateTicks_) return;
    const float elapsed = lastUpdateTicks_ == 0
        ? 1.0F / 30.0F
        : std::clamp(static_cast<float>(ticks - lastUpdateTicks_) / 1000.0F, 0.0F, 0.1F);
    lastUpdateTicks_ = ticks;
    frame_ = frame;
    for (std::size_t i = 0; i < peakHold_.size(); ++i) {
        const float target = std::clamp(frame.bands[i], 0.0F, 1.0F);
        const float response = target > displayBands_[i] ? 18.0F : 5.2F;
        displayBands_[i] += (target - displayBands_[i]) * std::min(1.0F, elapsed * response);

        if (target >= gravityBands_[i]) {
            gravityBands_[i] = target;
            fallVelocity_[i] = 0.0F;
        } else {
            fallVelocity_[i] += elapsed * 1.75F;
            gravityBands_[i] = std::max(target, gravityBands_[i] - fallVelocity_[i] * elapsed);
        }

        if (displayBands_[i] >= peakHold_[i]) {
            peakHold_[i] = displayBands_[i];
            peakAge_[i] = 0.0F;
        } else {
            peakAge_[i] += elapsed;
            if (peakAge_[i] > 0.48F) {
                const float fall = (peakAge_[i] - 0.48F) * elapsed * 1.25F;
                peakHold_[i] = std::max(displayBands_[i], peakHold_[i] - fall);
            }
        }
    }
    const auto smooth = [elapsed](float current, float target) {
        const float rate = target > current ? 13.0F : 4.5F;
        return current + (target - current) * std::min(1.0F, elapsed * rate);
    };
    vuLeft_ = smooth(vuLeft_, std::max(frame.rmsLeft, frame.peakLeft * 0.72F));
    vuRight_ = smooth(vuRight_, std::max(frame.rmsRight, frame.peakRight * 0.72F));
    const auto updateVintagePeak = [elapsed](float& held, float target) {
        if (target >= held) {
            held = target;
        } else if (target < 0.001F) {
            held = std::max(0.0F, held - elapsed * 1.8F);
        }
    };
    updateVintagePeak(vintagePeakLeft_, std::max(frame.rmsLeft, frame.peakLeft));
    updateVintagePeak(vintagePeakRight_, std::max(frame.rmsRight, frame.peakRight));
    const auto updateOwPeak = [elapsed](float& held, float& age, float target) {
        if (target >= held) {
            held = target;
            age = 0.0F;
        } else {
            age += elapsed;
            if (age > 2.5F) held = std::max(target, held - elapsed * 0.48F);
        }
    };
    updateOwPeak(owPeakLeft_, owPeakAgeLeft_, amplitudeToDbLevel(frame.rmsLeft));
    updateOwPeak(owPeakRight_, owPeakAgeRight_, amplitudeToDbLevel(frame.rmsRight));
    if (frame.peakLeft >= 0.9999F || frame.peakRight >= 0.9999F) {
        owClipHold_ = 1.2F;
    } else {
        owClipHold_ = std::max(0.0F, owClipHold_ - elapsed);
    }
    const auto averageRange = [this](std::size_t first, std::size_t last) {
        float total{};
        for (std::size_t i = first; i < last; ++i) total += displayBands_[i];
        return total / static_cast<float>(last - first);
    };
    const float bass = averageRange(0, 12);
    const float mid = averageRange(12, 39);
    const float treble = averageRange(39, displayBands_.size());
    if (bass > bassEnvelope_ * 1.16F + 0.025F) beatPulse_ = 1.0F;
    bassEnvelope_ += (bass - bassEnvelope_) * std::min(1.0F, elapsed * (bass > bassEnvelope_ ? 10.0F : 2.2F));
    midEnvelope_ += (mid - midEnvelope_) * std::min(1.0F, elapsed * 5.0F);
    trebleEnvelope_ += (treble - trebleEnvelope_) * std::min(1.0F, elapsed * 5.0F);
    beatPulse_ = std::max(0.0F, beatPulse_ - elapsed * 2.7F);
    rotation_ = std::fmod(rotation_ + elapsed * (0.10F + bassEnvelope_ * 0.13F), pi * 2.0F);

    waveformHead_ = (waveformHead_ + waveformHistoryRows - 1) % waveformHistoryRows;
    leftHistory_[waveformHead_] = frame.leftWaveform;
    rightHistory_[waveformHead_] = frame.rightWaveform;
    waveformCount_ = std::min(waveformCount_ + 1, waveformHistoryRows);

    if (lastWaterfallTicks_ == 0 || ticks - lastWaterfallTicks_ >= 55) {
        lastWaterfallTicks_ = ticks;
        waterfallHead_ = (waterfallHead_ + waterfallRows - 1) % waterfallRows;
        waterfall_[waterfallHead_] = displayBands_;
        waterfallCount_ = std::min(waterfallCount_ + 1, waterfallRows);
    }
}

void VisualizerRenderer::draw(SDL_Renderer* renderer, const SDL_FRect& rect,
                              VisualizerMode mode) const {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    switch (mode) {
        case VisualizerMode::ReferenceVu: drawReferenceVu(renderer, rect); break;
        case VisualizerMode::NeonArcVu: drawNeonArcVu(renderer, rect); break;
        case VisualizerMode::MirrorStage: drawMirrorStage(renderer, rect); break;
        case VisualizerMode::ChromaticWaterfall: drawWaterfall(renderer, rect); break;
        case VisualizerMode::OrbitVinyl: drawOrbitVinyl(renderer, rect); break;
        case VisualizerMode::StereoVector: drawStereoVector(renderer, rect); break;
        case VisualizerMode::SignalRibbon: drawSignalRibbon(renderer, rect); break;
        case VisualizerMode::StudioLed: drawStudioLed(renderer, rect); break;
        case VisualizerMode::PrecisionLevels: drawPrecisionLevels(renderer, rect); break;
        case VisualizerMode::CavaMonstercat: drawCavaMonstercat(renderer, rect); break;
        case VisualizerMode::PrismReflect: drawPrismReflect(renderer, rect); break;
        case VisualizerMode::PhosphorScope: drawPhosphorScope(renderer, rect); break;
        case VisualizerMode::LissajousPro: drawLissajousPro(renderer, rect); break;
        case VisualizerMode::RadialInferno: drawRadialInferno(renderer, rect); break;
        case VisualizerMode::CircularWave: drawCircularWave(renderer, rect); break;
        case VisualizerMode::SpectrogramMagma: drawSpectrogramMagma(renderer, rect); break;
        case VisualizerMode::MilkdropMesh: drawMilkdropMesh(renderer, rect); break;
        case VisualizerMode::ParticleGalaxy: drawParticleGalaxy(renderer, rect); break;
        case VisualizerMode::MasteringDashboard: drawMasteringDashboard(renderer, rect); break;
        case VisualizerMode::VintageFlatVu: drawVintageFlatVu(renderer, rect); break;
        case VisualizerMode::OwLevelMeter: drawOwLevelMeter(renderer, rect); break;
        case VisualizerMode::RackmountSpectrum: drawRackmountSpectrum(renderer, rect); break;
        case VisualizerMode::GreenDbMeter: drawGreenDbMeter(renderer, rect); break;
        case VisualizerMode::SpectrumSkyline: drawSpectrumSkyline(renderer, rect); break;
        case VisualizerMode::NeonMosaic: drawNeonMosaic(renderer, rect); break;
        case VisualizerMode::TripleSoundMeter: drawTripleSoundMeter(renderer, rect); break;
        case VisualizerMode::WarmTwinVu: drawWarmTwinVu(renderer, rect); break;
        default: drawAurora(renderer, rect); break;
    }
}

std::string_view VisualizerRenderer::name(VisualizerMode mode) {
    static constexpr std::array<std::string_view, visualizerModeCount> names{
        "Aurora Spectrum", "Reference VU", "Neon Arc VU", "Mirror Stage",
        "Chromatic Waterfall", "Orbit Vinyl", "Stereo Vector", "Signal Ribbon",
        "Studio LED", "Precision Levels", "CAVA Gravity", "Prism Reflection",
        "Phosphor Oscilloscope", "Lissajous Studio", "Radial Inferno",
        "Circular Wave", "Magma Spectrogram", "MilkDrop Motion Mesh",
        "Particle Galaxy", "Mastering Dashboard", "Vintage Flat VU",
        "OW Level Meter", "Rackmount Spectrum", "Green dB Meter", "Spectrum Skyline",
        "Neon Mosaic", "Triple Sound Meter", "Warm Twin VU"};
    return names[modeIndex(mode)];
}

std::string_view VisualizerRenderer::subtitle(VisualizerMode mode) {
    static constexpr std::array<std::string_view, visualizerModeCount> subtitles{
        "48 logarithmic bands · glassy neon bars · peak hold",
        "Dual analog needles · warm ivory face · weighted inertia",
        "Segmented stereo arcs · cyan/pink channels · peak halo",
        "Center-split spectrum · balanced symmetric motion",
        "Frequency history · luminous heat trail · cinematic depth",
        "Album-centered radial bands · subtle vinyl rotation",
        "L/R phase scope · phosphor trail · mastering-room character",
        "Layered stereo waveform · fluid transients · restrained glow",
        "Classic RTA matrix · green, amber and red dB zones",
        "Broadcast L/R meters · RMS body · true-peak markers",
        "CAVA-inspired gravity · Monstercat spread · floating peaks",
        "audioMotion-inspired prism bars · luminance · floor reflection",
        "Pulse-inspired stabilized scope · phosphor persistence · trigger grid",
        "Stereo phase portrait · Catmull-style trace · directional color",
        "Inverted radial FFT · fire gradient · beat-reactive core",
        "GLava-inspired circular waveform · dual channel · spectral halo",
        "Log-frequency history · perceptual magma map · time persistence",
        "projectM-inspired warp grid · bass deformation · luminous feedback",
        "Beat-driven orbital particles · spectral color · depth trails",
        "Spectrum, scope, phase and meters · one mastering-room display",
        "Dual flat meters · grey live needles · red maximum hold · dB scale",
        "-60 to 0 dB stereo bars · 2.5 s peak hold · clip, spectrum and phase",
        "31-band 1U analyzer · rainbow LED matrix · peak caps · hardware faceplate",
        "Nine-band green phosphor meter · 60 Hz to 16 kHz · retro segmented display",
        "Three-zone LED skyline · green, amber and red · perspective floor reflection",
        "Independent chroma columns · segmented neon bars · pure black contrast",
        "Triple analog dB gauges · three miniature RTAs · tri-color scale",
        "Dual backlit VU windows · warm ivory faces · classic black needles"};
    return subtitles[modeIndex(mode)];
}

void VisualizerRenderer::drawAurora(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const std::size_t count = static_cast<std::size_t>(std::clamp(rect.w / 11.0F, 24.0F, 48.0F));
    const float gap = rect.w > 700.0F ? 5.0F : 3.0F;
    const float width = (rect.w - 24.0F - gap * static_cast<float>(count - 1)) /
                        static_cast<float>(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float level = std::max(0.025F, bandAt(displayBands_, i, count));
        const float height = level * (rect.h - 26.0F);
        const float x = rect.x + 12.0F + static_cast<float>(i) * (width + gap);
        const float y = rect.y + rect.h - 10.0F - height;
        constexpr int slices = 8;
        for (int slice = 0; slice < slices; ++slice) {
            const float t = static_cast<float>(slice) / static_cast<float>(slices - 1);
            const SDL_FRect part{x, y + height * t, width,
                                 height / static_cast<float>(slices) + 1.0F};
            glowingRect(renderer, part, spectrumColor(1.0F - t));
        }
        const float held = bandAt(peakHold_, i, count);
        fill(renderer, {x, rect.y + rect.h - 13.0F - held * (rect.h - 26.0F), width, 2.0F},
             withAlpha(ink, 190));
    }
}

void VisualizerRenderer::drawReferenceVu(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect, true);
    const float meterWidth = rect.w / 2.0F;
    for (int meter = 0; meter < 2; ++meter) {
        const SDL_FRect face{rect.x + meterWidth * static_cast<float>(meter) + 5.0F,
                             rect.y + 6.0F, meterWidth - 10.0F, rect.h - 12.0F};
        constexpr int faceStrips = 16;
        for (int strip = 0; strip < faceStrips; ++strip) {
            const float t = static_cast<float>(strip) / static_cast<float>(faceStrips - 1);
            fill(renderer, {face.x, face.y + face.h * t, face.w,
                            face.h / static_cast<float>(faceStrips) + 1.0F},
                 mix({28, 22, 13, 255}, {7, 8, 10, 255}, t));
        }
        color(renderer, {197, 154, 80, 105});
        SDL_RenderRect(renderer, &face);
        const float cx = rect.x + meterWidth * (static_cast<float>(meter) + 0.5F);
        const float cy = rect.y + rect.h * 0.84F;
        const float radius = std::min(meterWidth * 0.40F, rect.h * 0.67F);
        arc(renderer, cx, cy, radius + 2.0F, pi * 1.12F, pi * 1.88F, {255, 222, 166, 62}, 2);
        arc(renderer, cx, cy, radius * 0.82F, pi * 1.12F, pi * 1.88F, {255, 222, 166, 38});
        arc(renderer, cx, cy, radius + 3.0F, pi * 1.70F, pi * 1.88F, {255, 70, 64, 150}, 3);
        for (int tick = 0; tick <= 20; ++tick) {
            const float angle = pi * (1.12F + 0.038F * static_cast<float>(tick));
            const bool major = tick % 5 == 0;
            const float inner = radius - (major ? 15.0F : tick % 2 == 0 ? 10.0F : 6.0F);
            const SDL_Color tickColor = tick > 15 ? SDL_Color{255, 84, 72, 220}
                                                 : SDL_Color{255, 228, 185, 180};
            line(renderer, cx + std::cos(angle) * inner, cy + std::sin(angle) * inner,
                 cx + std::cos(angle) * radius, cy + std::sin(angle) * radius, tickColor);
        }
        const float value = std::clamp(meter == 0 ? vuLeft_ : vuRight_, 0.02F, 0.98F);
        const float angle = pi * (1.17F + 0.65F * value);
        const float nx = cx + std::cos(angle) * (radius - 9.0F);
        const float ny = cy + std::sin(angle) * (radius - 9.0F);
        glowLine(renderer, cx, cy, nx, ny, amber, 2.0F);
        fillCircle(renderer, cx, cy, std::max(4.0F, radius * 0.045F), {48, 36, 20, 255});
        circle(renderer, cx, cy, std::max(5.0F, radius * 0.055F), {232, 216, 187, 130});
        const float lampRadius = std::max(2.0F, std::min(face.w, face.h) * 0.022F);
        fillCircle(renderer, face.x + face.w - lampRadius * 2.4F, face.y + lampRadius * 2.4F,
                   lampRadius, value > 0.88F ? red : withAlpha(green, 95));
        fill(renderer, {face.x + face.w * 0.18F, face.y + face.h * 0.12F,
                        face.w * 0.64F, std::max(1.0F, face.h * 0.012F)}, {255, 234, 194, 32});
    }
}

void VisualizerRenderer::drawNeonArcVu(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const float meterWidth = rect.w / 2.0F;
    for (int meter = 0; meter < 2; ++meter) {
        const float cx = rect.x + meterWidth * (static_cast<float>(meter) + 0.5F);
        const float cy = rect.y + rect.h * 0.72F;
        const float radius = std::min(meterWidth * 0.37F, rect.h * 0.49F);
        const float value = std::clamp(meter == 0 ? vuLeft_ : vuRight_, 0.0F, 1.0F);
        const SDL_Color active = meter == 0 ? cyan : pink;
        for (int segment = 0; segment < 23; ++segment) {
            const float start = pi * (1.05F + static_cast<float>(segment) * 0.9F / 23.0F);
            const float end = start + pi * 0.025F;
            const bool on = static_cast<float>(segment) / 23.0F < value;
            if (on) arc(renderer, cx, cy, radius + 2.0F, start, end, withAlpha(active, 35), 7, 4);
            arc(renderer, cx, cy, radius, start, end,
                on ? active : SDL_Color{94, 112, 147, 42}, 4, 4);
        }
        fillCircle(renderer, cx, cy, std::max(3.0F, radius * 0.045F), withAlpha(active, 210));
        circle(renderer, cx, cy, radius * 0.22F, withAlpha(active, 65));
    }
}

void VisualizerRenderer::drawMirrorStage(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const float middle = rect.y + rect.h / 2.0F;
    line(renderer, rect.x + 10.0F, middle, rect.x + rect.w - 10.0F, middle,
         withAlpha(ink, 75));
    const std::size_t count = static_cast<std::size_t>(std::clamp(rect.w / 13.0F, 22.0F, 48.0F));
    const float gap = rect.w > 700.0F ? 5.0F : 3.0F;
    const float width = (rect.w - 26.0F - gap * static_cast<float>(count - 1)) /
                        static_cast<float>(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float level = bandAt(displayBands_, i, count);
        const float height = std::max(2.0F, level * rect.h * 0.39F);
        const float x = rect.x + 13.0F + static_cast<float>(i) * (width + gap);
        const SDL_Color value = mix(blue, pink, static_cast<float>(i) / static_cast<float>(count - 1));
        glowingRect(renderer, {x, middle - height, width, height - 2.0F}, withAlpha(value, 220));
        fill(renderer, {x, middle + 2.0F, width, height}, withAlpha(value, 88));
    }
}

void VisualizerRenderer::drawWaterfall(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const float cellWidth = rect.w / static_cast<float>(AudioVisualizationFrame::bandCount);
    const float cellHeight = rect.h / static_cast<float>(waterfallRows);
    for (std::size_t row = 0; row < waterfallRows; ++row) {
        const std::size_t sourceRow = (waterfallHead_ + row) % waterfallRows;
        const float age = static_cast<float>(row) / static_cast<float>(waterfallRows - 1);
        for (std::size_t column = 0; column < AudioVisualizationFrame::bandCount; ++column) {
            const float level = waterfall_[sourceRow][column] * (1.0F - age * 0.52F);
            const SDL_Color heat = level > 0.78F ? pink : level > 0.52F ? violet :
                                   level > 0.28F ? blue : cyan;
            const Uint8 alpha = static_cast<Uint8>(std::clamp(45.0F + level * 210.0F, 0.0F, 255.0F));
            fill(renderer, {rect.x + static_cast<float>(column) * cellWidth + 0.6F,
                            rect.y + static_cast<float>(row) * cellHeight + 0.6F,
                            cellWidth - 1.2F, cellHeight - 1.2F}, withAlpha(heat, alpha));
        }
    }
    for (int strip = 0; strip < 8; ++strip) {
        const float t = static_cast<float>(strip) / 7.0F;
        fill(renderer, {rect.x, rect.y + rect.h * (0.72F + t * 0.28F), rect.w,
                        rect.h * 0.28F / 8.0F + 1.0F},
             {5, 7, 19, static_cast<Uint8>(t * 155.0F)});
    }
}

void VisualizerRenderer::drawOrbitVinyl(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const float cx = rect.x + rect.w / 2.0F;
    const float cy = rect.y + rect.h / 2.0F;
    const float inner = std::min(rect.w, rect.h) * 0.23F;
    constexpr std::size_t spokes = 64;
    for (std::size_t i = 0; i < spokes; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(spokes) * pi * 2.0F + rotation_;
        const float level = bandAt(displayBands_, i, spokes) * std::min(rect.w, rect.h) * 0.19F;
        const SDL_Color value = i < spokes / 2 ? cyan : pink;
        line(renderer, cx + std::cos(angle) * (inner + 3.0F),
             cy + std::sin(angle) * (inner + 3.0F),
             cx + std::cos(angle) * (inner + level + 4.0F),
             cy + std::sin(angle) * (inner + level + 4.0F),
             withAlpha(value, static_cast<Uint8>(145.0F + level * 0.6F)));
    }
    fillCircle(renderer, cx, cy, inner, {3, 5, 12, 255});
    fillCircle(renderer, cx - inner * 0.16F, cy - inner * 0.16F, inner * 0.68F,
               {16, 23, 43, 255});
    for (float radius = inner * 0.34F; radius < inner; radius += std::max(4.0F, inner * 0.08F)) {
        circle(renderer, cx, cy, radius, {120, 145, 190, 48});
    }
    fillCircle(renderer, cx, cy, inner * 0.17F, withAlpha(pink, 45));
    fillCircle(renderer, cx, cy, inner * 0.13F, pink);
    fillCircle(renderer, cx, cy, inner * 0.025F, {5, 7, 18, 255});
}

void VisualizerRenderer::drawStereoVector(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const float cx = rect.x + rect.w / 2.0F;
    const float cy = rect.y + rect.h / 2.0F;
    const float radius = std::min(rect.w, rect.h) * 0.43F;
    line(renderer, cx, rect.y + 10.0F, cx, rect.y + rect.h - 10.0F, {105, 129, 168, 55});
    line(renderer, cx - radius, cy, cx + radius, cy, {105, 129, 168, 55});
    circle(renderer, cx, cy, radius, {105, 129, 168, 42});

    std::vector<SDL_FPoint> points;
    points.reserve(AudioVisualizationFrame::waveformSampleCount);
    for (std::size_t i = 0; i < AudioVisualizationFrame::waveformSampleCount; ++i) {
        const float left = frame_.leftWaveform[i];
        const float right = frame_.rightWaveform[i];
        const float x = (left - right) * 0.5F;
        const float y = (left + right) * 0.5F;
        points.push_back({cx + x * radius * 1.55F, cy - y * radius * 1.15F});
    }
    polyline(renderer, points, withAlpha(pink, 26), 11);
    polyline(renderer, points, withAlpha(cyan, 45), 6);
    polyline(renderer, points, withAlpha(cyan, 225), 2);
}

void VisualizerRenderer::drawSignalRibbon(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const float middle = rect.y + rect.h / 2.0F;
    line(renderer, rect.x, middle, rect.x + rect.w, middle, {121, 143, 180, 62});
    std::vector<SDL_FPoint> leftPoints;
    std::vector<SDL_FPoint> rightPoints;
    leftPoints.reserve(AudioVisualizationFrame::waveformSampleCount);
    rightPoints.reserve(AudioVisualizationFrame::waveformSampleCount);
    for (std::size_t i = 0; i < AudioVisualizationFrame::waveformSampleCount; ++i) {
        const float position = static_cast<float>(i) /
                               static_cast<float>(AudioVisualizationFrame::waveformSampleCount - 1);
        const float envelope = 0.28F + 0.72F * std::sin(pi * position);
        const float x = rect.x + position * rect.w;
        leftPoints.push_back({x, middle + frame_.leftWaveform[i] * rect.h * 0.39F * envelope});
        rightPoints.push_back({x, middle + frame_.rightWaveform[i] * rect.h * 0.39F * envelope});
    }
    polyline(renderer, leftPoints, withAlpha(cyan, 22), 13);
    polyline(renderer, rightPoints, withAlpha(pink, 22), 13);
    polyline(renderer, leftPoints, withAlpha(cyan, 215), 2);
    polyline(renderer, rightPoints, withAlpha(pink, 190), 2);
}

void VisualizerRenderer::drawStudioLed(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    constexpr std::size_t columns = AudioVisualizationFrame::bandCount;
    constexpr int rows = 10;
    const float pad = std::max(8.0F, rect.w * 0.025F);
    const float gap = rect.w > 700.0F ? 5.0F : 3.0F;
    const float width = (rect.w - pad * 2.0F - gap * static_cast<float>(columns - 1)) /
                        static_cast<float>(columns);
    const float height = (rect.h - 18.0F - gap * static_cast<float>(rows - 1)) /
                         static_cast<float>(rows);
    for (std::size_t column = 0; column < columns; ++column) {
        const int active = static_cast<int>(std::lround(displayBands_[column] * rows));
        for (int row = 0; row < rows; ++row) {
            const bool on = rows - row <= active;
            const SDL_Color zone = row < 2 ? red : row < 5 ? amber : green;
            const SDL_FRect led{rect.x + pad + static_cast<float>(column) * (width + gap),
                                rect.y + 8.0F + static_cast<float>(row) * (height + gap),
                                width, height};
            if (on) glowingRect(renderer, led, zone);
            else fill(renderer, led, {99, 119, 148, 28});
        }
    }
}

void VisualizerRenderer::drawPrecisionLevels(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const float left = rect.x + rect.w * 0.09F;
    const float right = rect.x + rect.w * 0.95F;
    const float meterWidth = right - left;
    for (int tick = 0; tick < 6; ++tick) {
        const float x = left + meterWidth * static_cast<float>(tick) / 5.0F;
        line(renderer, x, rect.y + rect.h * 0.14F, x, rect.y + rect.h * 0.9F,
             {108, 130, 168, 44});
    }
    const std::array<float, 2> values{frame_.rmsLeft, frame_.rmsRight};
    const std::array<float, 2> peaks{frame_.peakLeft, frame_.peakRight};
    for (int meter = 0; meter < 2; ++meter) {
        const float y = rect.y + rect.h * (meter == 0 ? 0.32F : 0.67F);
        const float height = std::max(12.0F, rect.h * 0.16F);
        fill(renderer, {left, y, meterWidth, height}, {102, 123, 158, 38});
        constexpr int segments = 42;
        for (int segment = 0; segment < segments; ++segment) {
            const float t = static_cast<float>(segment) / static_cast<float>(segments - 1);
            if (t > values[static_cast<std::size_t>(meter)]) break;
            const SDL_Color value = t < 0.72F ? mix(cyan, blue, t / 0.72F)
                                  : t < 0.9F ? mix(blue, amber, (t - 0.72F) / 0.18F)
                                             : mix(amber, pink, (t - 0.9F) / 0.1F);
            const float segmentWidth = meterWidth / static_cast<float>(segments);
            glowingRect(renderer, {left + static_cast<float>(segment) * segmentWidth,
                                    y, std::max(1.0F, segmentWidth - 2.0F), height}, value);
        }
        const float peakX = left + meterWidth * std::clamp(peaks[static_cast<std::size_t>(meter)], 0.0F, 1.0F);
        fill(renderer, {peakX, y - 3.0F, 3.0F, height + 6.0F}, ink);
        const SDL_Color channel = meter == 0 ? cyan : pink;
        fill(renderer, {rect.x + rect.w * 0.035F, y, 5.0F, height}, channel);
    }
}

void VisualizerRenderer::drawCavaMonstercat(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const std::size_t count = static_cast<std::size_t>(std::clamp(rect.w / 13.0F, 24.0F, 58.0F));
    std::vector<float> levels(count);
    for (std::size_t i = 0; i < count; ++i) levels[i] = bandAt(gravityBands_, i, count);
    for (std::size_t source = 0; source < count; ++source) {
        for (std::size_t target = 0; target < count; ++target) {
            const auto distance = static_cast<float>(source > target ? source - target : target - source);
            levels[target] = std::max(levels[target], levels[source] / std::pow(1.72F, distance));
        }
    }
    const float pad = std::max(8.0F, rect.w * 0.018F);
    const float gap = std::clamp(rect.w / 360.0F, 2.0F, 5.0F);
    const float width = (rect.w - pad * 2.0F - gap * static_cast<float>(count - 1)) /
                        static_cast<float>(count);
    const float floor = rect.y + rect.h * 0.91F;
    for (int grid = 1; grid < 5; ++grid) {
        const float y = rect.y + rect.h * (0.08F + static_cast<float>(grid) * 0.17F);
        line(renderer, rect.x + pad, y, rect.x + rect.w - pad, y, {90, 116, 157, 34});
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float level = std::max(0.012F, levels[i]);
        const float height = level * rect.h * 0.78F;
        const float x = rect.x + pad + static_cast<float>(i) * (width + gap);
        const SDL_Color value = mix(cyan, pink, std::pow(static_cast<float>(i) /
                                                         static_cast<float>(count - 1), 1.25F));
        fill(renderer, {x - 2.0F, floor - height - 2.0F, width + 4.0F, height + 4.0F},
             withAlpha(value, 18));
        constexpr int slices = 12;
        for (int slice = 0; slice < slices; ++slice) {
            const float t = static_cast<float>(slice) / static_cast<float>(slices);
            const float y = floor - height + height * t;
            fill(renderer, {x, y, width, height / static_cast<float>(slices) + 1.0F},
                 mix(withAlpha(ink, 235), withAlpha(value, 220), std::sqrt(t)));
        }
        const float held = bandAt(peakHold_, i, count);
        const float peakY = floor - held * rect.h * 0.78F;
        fill(renderer, {x, peakY, width, std::max(1.0F, rect.h * 0.006F)}, withAlpha(ink, 220));
        fill(renderer, {x, floor + 3.0F, width, level * rect.h * 0.055F}, withAlpha(value, 50));
    }
}

void VisualizerRenderer::drawPrismReflect(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    base(renderer, rect);
    const std::size_t count = static_cast<std::size_t>(std::clamp(rect.w / 10.0F, 32.0F, 64.0F));
    const float pad = std::max(8.0F, rect.w * 0.018F);
    const float gap = std::clamp(rect.w / 520.0F, 1.5F, 4.0F);
    const float width = (rect.w - pad * 2.0F - gap * static_cast<float>(count - 1)) /
                        static_cast<float>(count);
    const float baseline = rect.y + rect.h * 0.72F;
    const float analyzerHeight = rect.h * 0.62F;
    for (std::size_t i = 0; i < count; ++i) {
        const float level = std::max(0.008F, bandAt(displayBands_, i, count));
        const float height = analyzerHeight * level;
        const float x = rect.x + pad + static_cast<float>(i) * (width + gap);
        const SDL_Color value = hsv(0.52F + static_cast<float>(i) * 0.72F /
                                             static_cast<float>(count - 1), 0.78F, 1.0F);
        fill(renderer, {x - 2.0F, baseline - height, width + 4.0F, height}, withAlpha(value, 20));
        fill(renderer, {x, baseline - height, width, height},
             withAlpha(value, static_cast<Uint8>(120.0F + level * 135.0F)));
        fill(renderer, {x, baseline - height, std::max(1.0F, width * 0.28F), height},
             withAlpha(ink, 58));
        const float reflected = std::min(rect.h * 0.23F, height * 0.38F);
        constexpr int fades = 8;
        for (int fade = 0; fade < fades; ++fade) {
            const float t = static_cast<float>(fade) / static_cast<float>(fades);
            fill(renderer, {x, baseline + 4.0F + t * reflected, width,
                            reflected / static_cast<float>(fades) + 1.0F},
                 withAlpha(value, static_cast<Uint8>((1.0F - t) * 72.0F)));
        }
        const float held = bandAt(peakHold_, i, count);
        fill(renderer, {x, baseline - held * analyzerHeight - 3.0F, width, 2.0F},
             withAlpha(ink, 205));
    }
    line(renderer, rect.x + pad, baseline, rect.x + rect.w - pad, baseline, {221, 241, 255, 92});
}

void VisualizerRenderer::drawPhosphorScope(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {2, 12, 12, 255});
    for (int column = 0; column <= 10; ++column) {
        const float x = rect.x + rect.w * static_cast<float>(column) / 10.0F;
        line(renderer, x, rect.y, x, rect.y + rect.h,
             column == 5 ? SDL_Color{79, 203, 161, 58} : SDL_Color{53, 144, 116, 28});
    }
    for (int row = 0; row <= 8; ++row) {
        const float y = rect.y + rect.h * static_cast<float>(row) / 8.0F;
        line(renderer, rect.x, y, rect.x + rect.w, y,
             row == 4 ? SDL_Color{79, 203, 161, 58} : SDL_Color{53, 144, 116, 28});
    }
    for (std::size_t age = std::min(waveformCount_, waveformHistoryRows); age-- > 0;) {
        const auto source = (waveformHead_ + age) % waveformHistoryRows;
        const float fade = 1.0F - static_cast<float>(age) /
                                      static_cast<float>(waveformHistoryRows + 1);
        const auto& leftWave = leftHistory_[source];
        const auto& rightWave = rightHistory_[source];
        std::size_t trigger{};
        for (std::size_t i = 1; i < leftWave.size() / 3; ++i) {
            if (leftWave[i - 1] < 0.0F && leftWave[i] >= 0.0F) { trigger = i; break; }
        }
        std::vector<SDL_FPoint> points;
        points.reserve(leftWave.size());
        for (std::size_t i = 0; i < leftWave.size(); ++i) {
            const auto index = std::min(trigger + i, leftWave.size() - 1);
            const float position = static_cast<float>(i) / static_cast<float>(leftWave.size() - 1);
            const float edge = std::sin(pi * std::clamp(position, 0.0F, 1.0F));
            const float sample = (leftWave[index] + rightWave[index]) * 0.5F;
            points.push_back({rect.x + position * rect.w,
                              rect.y + rect.h * 0.5F - sample * edge * rect.h * 0.43F});
        }
        polyline(renderer, points, withAlpha(phosphor, static_cast<Uint8>(10.0F + fade * 42.0F)),
                 age == 0 ? 9 : 3);
        if (age == 0) polyline(renderer, points, {184, 255, 222, 245}, 2);
    }
    fill(renderer, {rect.x, rect.y, rect.w, 2.0F}, {148, 255, 219, 65});
}

void VisualizerRenderer::drawLissajousPro(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {3, 6, 17, 255});
    const float cx = rect.x + rect.w * 0.5F;
    const float cy = rect.y + rect.h * 0.5F;
    const float radius = std::min(rect.w, rect.h) * 0.44F;
    circle(renderer, cx, cy, radius, {112, 139, 182, 45});
    circle(renderer, cx, cy, radius * 0.5F, {112, 139, 182, 28});
    line(renderer, cx - radius, cy, cx + radius, cy, {112, 139, 182, 34});
    line(renderer, cx, cy - radius, cx, cy + radius, {112, 139, 182, 34});
    const auto trails = std::min(waveformCount_, waveformHistoryRows);
    for (std::size_t age = trails; age-- > 0;) {
        const auto source = (waveformHead_ + age) % waveformHistoryRows;
        std::vector<SDL_FPoint> points;
        points.reserve(AudioVisualizationFrame::waveformSampleCount);
        for (std::size_t i = 0; i < AudioVisualizationFrame::waveformSampleCount; ++i) {
            const float leftSample = leftHistory_[source][i];
            const float rightSample = rightHistory_[source][i];
            points.push_back({cx + leftSample * radius, cy - rightSample * radius});
        }
        const float fade = 1.0F - static_cast<float>(age) / static_cast<float>(trails + 1);
        polyline(renderer, points, withAlpha(mix(cyan, pink, 0.48F),
                                             static_cast<Uint8>(8.0F + fade * 50.0F)), age == 0 ? 8 : 2);
        if (age == 0) {
            for (std::size_t i = 1; i < points.size(); ++i) {
                const float dx = points[i].x - points[i - 1].x;
                const float dy = points[i].y - points[i - 1].y;
                const float hue = std::atan2(dy, dx) / (pi * 2.0F) + 0.63F;
                line(renderer, points[i - 1].x, points[i - 1].y, points[i].x, points[i].y,
                     hsv(hue, 0.72F, 1.0F, 225));
            }
        }
    }
}

void VisualizerRenderer::drawRadialInferno(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {5, 3, 12, 255});
    const float cx = rect.x + rect.w * 0.5F;
    const float cy = rect.y + rect.h * 0.5F;
    const float scale = std::min(rect.w, rect.h);
    const float inner = scale * (0.18F + beatPulse_ * 0.018F);
    constexpr std::size_t segments = 72;
    for (std::size_t i = 0; i < segments; ++i) {
        const float fraction = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = fraction * pi * 2.0F - pi * 0.5F + rotation_ * 0.28F;
        const float level = std::pow(bandAt(displayBands_, i, segments), 0.82F);
        const float outer = inner + scale * (0.07F + level * 0.25F);
        const SDL_Color value = magma(std::clamp(0.25F + level * 0.72F, 0.0F, 1.0F));
        glowLine(renderer, cx + std::cos(angle) * (inner + 3.0F),
                 cy + std::sin(angle) * (inner + 3.0F),
                 cx + std::cos(angle) * outer, cy + std::sin(angle) * outer,
                 withAlpha(value, 220), std::max(1.0F, scale * 0.006F));
    }
    for (int ring = 5; ring >= 1; --ring) {
        fillCircle(renderer, cx, cy, inner * (0.54F + static_cast<float>(ring) * 0.085F),
                   withAlpha(mix(pink, amber, static_cast<float>(ring) / 5.0F),
                             static_cast<Uint8>(9 + beatPulse_ * 10.0F)));
    }
    circle(renderer, cx, cy, inner, {255, 159, 82, 190}, 2);
    fillCircle(renderer, cx, cy, inner * 0.14F, {255, 223, 153, 220});
}

void VisualizerRenderer::drawCircularWave(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {2, 7, 18, 255});
    const float cx = rect.x + rect.w * 0.5F;
    const float cy = rect.y + rect.h * 0.5F;
    const float scale = std::min(rect.w, rect.h);
    const float radius = scale * 0.285F;
    for (int grid = 1; grid <= 3; ++grid) {
        circle(renderer, cx, cy, radius * static_cast<float>(grid) / 3.0F,
               {89, 133, 180, static_cast<Uint8>(18 + grid * 6)});
    }
    const std::size_t pointCount = 192;
    for (int channel = 0; channel < 2; ++channel) {
        std::vector<SDL_FPoint> points;
        points.reserve(pointCount + 1);
        for (std::size_t i = 0; i <= pointCount; ++i) {
            const std::size_t index = (i % pointCount) * AudioVisualizationFrame::waveformSampleCount / pointCount;
            const float sample = channel == 0 ? frame_.leftWaveform[index] : frame_.rightWaveform[index];
            const float spectral = bandAt(displayBands_, i % pointCount, pointCount);
            const float angle = static_cast<float>(i) / static_cast<float>(pointCount) * pi * 2.0F +
                                (channel == 0 ? rotation_ : -rotation_) * 0.17F;
            const float r = radius + sample * scale * 0.10F + spectral * scale * 0.035F *
                            (channel == 0 ? 1.0F : -1.0F);
            points.push_back({cx + std::cos(angle) * r, cy + std::sin(angle) * r});
        }
        const SDL_Color value = channel == 0 ? cyan : pink;
        polyline(renderer, points, withAlpha(value, 22), 11);
        polyline(renderer, points, withAlpha(value, 235), 2);
    }
    fillCircle(renderer, cx, cy, radius * 0.08F, withAlpha(ink, 210));
}

void VisualizerRenderer::drawSpectrogramMagma(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {3, 3, 12, 255});
    const float rowHeight = rect.h / static_cast<float>(waterfallRows);
    const float columnWidth = rect.w / static_cast<float>(AudioVisualizationFrame::bandCount);
    for (std::size_t row = 0; row < waterfallRows; ++row) {
        const auto source = (waterfallHead_ + row) % waterfallRows;
        const float age = static_cast<float>(row) / static_cast<float>(waterfallRows - 1);
        for (std::size_t band = 0; band < AudioVisualizationFrame::bandCount; ++band) {
            const float raw = waterfall_[source][band];
            const float intensity = std::clamp(std::pow(raw, 0.72F) * (1.0F - age * 0.28F), 0.0F, 1.0F);
            fill(renderer, {rect.x + static_cast<float>(band) * columnWidth,
                            rect.y + static_cast<float>(row) * rowHeight,
                            columnWidth + 0.5F, rowHeight + 0.5F}, magma(intensity));
        }
    }
    fill(renderer, {rect.x, rect.y, rect.w, std::max(2.0F, rowHeight)}, {255, 230, 172, 82});
    for (int marker = 1; marker < 8; ++marker) {
        const float x = rect.x + rect.w * static_cast<float>(marker) / 8.0F;
        line(renderer, x, rect.y, x, rect.y + rect.h, {255, 255, 255, 12});
    }
}

void VisualizerRenderer::drawMilkdropMesh(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {3, 4, 17, 255});
    const float cx = rect.x + rect.w * 0.5F;
    const float cy = rect.y + rect.h * 0.48F;
    constexpr int columns = 24;
    constexpr int rows = 15;
    std::array<std::array<SDL_FPoint, columns>, rows> mesh{};
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const float nx = static_cast<float>(column) / static_cast<float>(columns - 1) * 2.0F - 1.0F;
            const float ny = static_cast<float>(row) / static_cast<float>(rows - 1) * 2.0F - 1.0F;
            const float distance = std::sqrt(nx * nx + ny * ny);
            const float angle = std::atan2(ny, nx) + rotation_ * 0.35F;
            const float warp = std::sin(distance * 11.0F - rotation_ * 8.0F + angle * 2.0F) *
                               (0.025F + bassEnvelope_ * 0.065F);
            const float swirl = (0.025F + midEnvelope_ * 0.05F) * std::sin(distance * 5.0F + rotation_ * 3.0F);
            const float wx = nx * (1.0F + warp) - ny * swirl;
            const float wy = ny * (1.0F + warp) + nx * swirl;
            mesh[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] =
                {cx + wx * rect.w * 0.49F, cy + wy * rect.h * 0.49F};
        }
    }
    for (int row = 0; row < rows; ++row) {
        std::vector<SDL_FPoint> points(mesh[static_cast<std::size_t>(row)].begin(),
                                      mesh[static_cast<std::size_t>(row)].end());
        polyline(renderer, points, withAlpha(mix(blue, pink, static_cast<float>(row) /
                                                          static_cast<float>(rows - 1)), 70), 1);
    }
    for (int column = 0; column < columns; ++column) {
        std::vector<SDL_FPoint> points;
        for (int row = 0; row < rows; ++row) points.push_back(mesh[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]);
        polyline(renderer, points, withAlpha(mix(cyan, violet, static_cast<float>(column) /
                                                          static_cast<float>(columns - 1)), 48), 1);
    }
    std::vector<SDL_FPoint> wave;
    constexpr std::size_t count = 128;
    for (std::size_t i = 0; i <= count; ++i) {
        const auto sample = frame_.leftWaveform[(i % count) * frame_.leftWaveform.size() / count];
        const float angle = static_cast<float>(i) / static_cast<float>(count) * pi * 2.0F + rotation_;
        const float radius = std::min(rect.w, rect.h) * (0.13F + sample * 0.06F + beatPulse_ * 0.018F);
        wave.push_back({cx + std::cos(angle) * radius, cy + std::sin(angle) * radius});
    }
    polyline(renderer, wave, withAlpha(pink, 28), 13);
    polyline(renderer, wave, {216, 180, 255, 225}, 2);
}

void VisualizerRenderer::drawParticleGalaxy(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {2, 3, 14, 255});
    const float cx = rect.x + rect.w * 0.5F;
    const float cy = rect.y + rect.h * 0.5F;
    const float scale = std::min(rect.w, rect.h);
    constexpr std::size_t particles = 120;
    constexpr float golden = 2.39996323F;
    for (std::size_t i = particles; i-- > 0;) {
        const float seed = static_cast<float>(i) / static_cast<float>(particles - 1);
        const float level = bandAt(displayBands_, i, particles);
        const float depth = 0.28F + 0.72F * std::abs(std::sin(seed * 17.0F + rotation_));
        const float angle = static_cast<float>(i) * golden + rotation_ * (0.7F + seed * 1.8F);
        const float radius = scale * (0.055F + seed * 0.43F + level * 0.065F);
        const float flatten = 0.48F + 0.25F * std::sin(rotation_ + seed * 8.0F);
        const float x = cx + std::cos(angle) * radius;
        const float y = cy + std::sin(angle) * radius * flatten;
        const float previousAngle = angle - (0.025F + level * 0.08F);
        const SDL_Color value = hsv(0.53F + seed * 0.38F + rotation_ * 0.025F,
                                    0.72F, 0.75F + depth * 0.25F,
                                    static_cast<Uint8>(90.0F + depth * 160.0F));
        line(renderer, cx + std::cos(previousAngle) * radius,
             cy + std::sin(previousAngle) * radius * flatten, x, y, withAlpha(value, 82));
        fillCircle(renderer, x, y, std::max(0.8F, scale * (0.0025F + level * 0.006F)), value);
    }
    for (int halo = 5; halo >= 1; --halo) {
        fillCircle(renderer, cx, cy, scale * (0.015F + static_cast<float>(halo) * 0.008F),
                   withAlpha(mix(cyan, pink, beatPulse_), static_cast<Uint8>(8 + beatPulse_ * 12.0F)));
    }
    fillCircle(renderer, cx, cy, scale * 0.012F, ink);
}

void VisualizerRenderer::drawMasteringDashboard(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    fill(renderer, rect, {5, 8, 17, 255});
    const float pad = std::max(6.0F, rect.w * 0.012F);
    const SDL_FRect scope{rect.x + pad, rect.y + pad, rect.w * 0.56F, rect.h * 0.42F};
    const SDL_FRect spectrum{rect.x + pad, rect.y + rect.h * 0.49F,
                             rect.w * 0.56F, rect.h * 0.45F};
    const SDL_FRect phase{rect.x + rect.w * 0.60F, rect.y + pad,
                          rect.w * 0.23F, rect.h - pad * 2.0F};
    const SDL_FRect meters{rect.x + rect.w * 0.86F, rect.y + pad,
                           rect.w * 0.11F, rect.h - pad * 2.0F};
    for (const auto& panelRect : {scope, spectrum, phase, meters}) {
        fill(renderer, panelRect, {2, 5, 12, 255});
        color(renderer, {87, 117, 158, 68});
        SDL_RenderRect(renderer, &panelRect);
    }

    for (int row = 1; row < 4; ++row) {
        const float y = scope.y + scope.h * static_cast<float>(row) / 4.0F;
        line(renderer, scope.x, y, scope.x + scope.w, y, {73, 124, 137, 30});
    }
    std::vector<SDL_FPoint> wave;
    wave.reserve(frame_.leftWaveform.size());
    for (std::size_t i = 0; i < frame_.leftWaveform.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(frame_.leftWaveform.size() - 1);
        wave.push_back({scope.x + t * scope.w,
                        scope.y + scope.h * 0.5F - (frame_.leftWaveform[i] + frame_.rightWaveform[i]) *
                                                       scope.h * 0.22F});
    }
    polyline(renderer, wave, withAlpha(phosphor, 24), 7);
    polyline(renderer, wave, withAlpha(phosphor, 225), 1);

    constexpr std::size_t bars = 32;
    const float barWidth = spectrum.w / static_cast<float>(bars);
    for (std::size_t i = 0; i < bars; ++i) {
        const float level = bandAt(displayBands_, i, bars);
        const float h = level * spectrum.h * 0.91F;
        const SDL_Color value = mix(cyan, violet, static_cast<float>(i) / static_cast<float>(bars - 1));
        fill(renderer, {spectrum.x + static_cast<float>(i) * barWidth + 1.0F,
                        spectrum.y + spectrum.h - h, std::max(1.0F, barWidth - 2.0F), h}, value);
    }

    const float pcx = phase.x + phase.w * 0.5F;
    const float pcy = phase.y + phase.h * 0.5F;
    const float pr = std::min(phase.w, phase.h) * 0.43F;
    circle(renderer, pcx, pcy, pr, {103, 137, 177, 55});
    line(renderer, pcx - pr, pcy, pcx + pr, pcy, {103, 137, 177, 38});
    line(renderer, pcx, pcy - pr, pcx, pcy + pr, {103, 137, 177, 38});
    std::vector<SDL_FPoint> phaseLine;
    for (std::size_t i = 0; i < frame_.leftWaveform.size(); ++i) {
        phaseLine.push_back({pcx + frame_.leftWaveform[i] * pr,
                             pcy - frame_.rightWaveform[i] * pr});
    }
    polyline(renderer, phaseLine, withAlpha(cyan, 190), 1);

    const std::array<float, 2> levels{vuLeft_, vuRight_};
    const float meterWidth = meters.w * 0.28F;
    for (int channel = 0; channel < 2; ++channel) {
        const float x = meters.x + meters.w * (channel == 0 ? 0.18F : 0.56F);
        fill(renderer, {x, meters.y + 4.0F, meterWidth, meters.h - 8.0F}, {29, 39, 52, 255});
        constexpr int segments = 24;
        for (int segment = 0; segment < segments; ++segment) {
            const float t = static_cast<float>(segment) / static_cast<float>(segments - 1);
            const bool on = t <= levels[static_cast<std::size_t>(channel)];
            const SDL_Color zone = t > 0.90F ? red : t > 0.72F ? amber : green;
            const float y = meters.y + meters.h - 7.0F -
                            (static_cast<float>(segment) + 1.0F) * (meters.h - 12.0F) /
                                static_cast<float>(segments);
            fill(renderer, {x + 1.0F, y, meterWidth - 2.0F,
                            (meters.h - 12.0F) / static_cast<float>(segments) - 1.0F},
                 on ? zone : withAlpha(zone, 25));
        }
    }
}

void VisualizerRenderer::drawVintageFlatVu(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 20.0F) return;

    // Brushed charcoal enclosure and a narrow center seam reproduce the compact
    // two-channel instrument layout without relying on external bitmap assets.
    constexpr int enclosureStrips = 20;
    for (int strip = 0; strip < enclosureStrips; ++strip) {
        const float t = static_cast<float>(strip) / static_cast<float>(enclosureStrips - 1);
        const SDL_Color shade = mix({62, 64, 66, 255}, {20, 21, 23, 255}, t);
        fill(renderer, {rect.x, rect.y + t * rect.h, rect.w,
                        rect.h / static_cast<float>(enclosureStrips) + 1.0F}, shade);
    }
    fill(renderer, {rect.x, rect.y, rect.w, std::max(1.0F, rect.h * 0.009F)},
         {181, 184, 186, 90});
    fill(renderer, {rect.x, rect.y + rect.h - std::max(1.0F, rect.h * 0.012F),
                    rect.w, std::max(1.0F, rect.h * 0.012F)}, {0, 0, 0, 190});

    const float outerPad = std::max(3.0F, std::min(rect.w, rect.h) * 0.018F);
    const float gap = std::max(3.0F, rect.w * 0.008F);
    const float meterWidth = (rect.w - outerPad * 2.0F - gap) * 0.5F;
    const std::array<float, 2> liveValues{vuLeft_, vuRight_};
    const std::array<float, 2> heldValues{vintagePeakLeft_, vintagePeakRight_};

    for (int meter = 0; meter < 2; ++meter) {
        const float outerX = rect.x + outerPad + static_cast<float>(meter) * (meterWidth + gap);
        const SDL_FRect outer{outerX, rect.y + outerPad, meterWidth, rect.h - outerPad * 2.0F};
        fill(renderer, outer, {8, 9, 10, 255});
        color(renderer, {153, 157, 160, 125});
        SDL_RenderRect(renderer, &outer);

        const float bevel = std::max(2.0F, std::min(outer.w, outer.h) * 0.012F);
        fill(renderer, {outer.x + bevel, outer.y + bevel, outer.w - bevel * 2.0F,
                        outer.h - bevel * 2.0F}, {34, 35, 36, 255});
        const SDL_FRect face{outer.x + bevel * 2.1F, outer.y + bevel * 2.1F,
                             outer.w - bevel * 4.2F, outer.h - bevel * 4.2F};
        constexpr int faceStrips = 18;
        for (int strip = 0; strip < faceStrips; ++strip) {
            const float t = static_cast<float>(strip) / static_cast<float>(faceStrips - 1);
            const SDL_Color shade = mix({8, 11, 12, 255}, {1, 2, 3, 255}, t);
            fill(renderer, {face.x, face.y + face.h * t, face.w,
                            face.h / static_cast<float>(faceStrips) + 1.0F}, shade);
        }

        const bool detailed = face.w >= 245.0F && face.h >= 145.0F;
        const float tinyPixel = std::clamp(std::min(face.w / 350.0F, face.h / 185.0F), 1.0F, 3.2F);
        const float titlePixel = std::clamp(std::min(face.w / 150.0F, face.h / 82.0F), 1.0F, 5.2F);
        pixelText(renderer, "VU", face.x + face.w * 0.5F, face.y + face.h * 0.035F,
                  titlePixel, {35, 213, 132, 230});

        // Fourteen tapered blocks form the characteristic green/red flat scale.
        constexpr int segments = 14;
        const float scaleTop = face.y + face.h * 0.235F;
        const float scaleBottom = face.y + face.h * 0.385F;
        const float topLeft = face.x + face.w * 0.055F;
        const float topRight = face.x + face.w * 0.945F;
        const float bottomLeft = face.x + face.w * 0.115F;
        const float bottomRight = face.x + face.w * 0.885F;
        for (int segment = 0; segment < segments; ++segment) {
            const float t0 = (static_cast<float>(segment) + 0.055F) / static_cast<float>(segments);
            const float t1 = (static_cast<float>(segment + 1) - 0.055F) / static_cast<float>(segments);
            const float x0Top = topLeft + (topRight - topLeft) * t0;
            const float x1Top = topLeft + (topRight - topLeft) * t1;
            const float x0Bottom = bottomLeft + (bottomRight - bottomLeft) * t0;
            const float x1Bottom = bottomLeft + (bottomRight - bottomLeft) * t1;
            const SDL_Color zone = segment < 10
                ? mix(SDL_Color{20, 129, 80, 255}, SDL_Color{35, 213, 132, 255},
                      0.62F + static_cast<float>(segment % 2) * 0.12F)
                : mix(SDL_Color{170, 37, 43, 255}, SDL_Color{238, 75, 75, 255},
                      0.66F + static_cast<float>(segment % 2) * 0.10F);
            fillQuad(renderer, {{{x0Top, scaleTop}, {x1Top, scaleTop},
                                 {x1Bottom, scaleBottom}, {x0Bottom, scaleBottom}}}, zone);
            line(renderer, x0Top, scaleTop, x1Top, scaleTop, withAlpha(ink, 58));
        }

        if (detailed) {
            static constexpr std::array<float, 11> markerPositions{
                0.015F, 0.17F, 0.30F, 0.40F, 0.50F, 0.585F,
                0.66F, 0.72F, 0.79F, 0.87F, 0.955F
            };
            static constexpr std::array<std::string_view, 11> markerLabels{
                "-20", "-10", "-7", "-5", "-3", "-2", "-1", "0", "+1", "+2", "+3"
            };
            for (std::size_t marker = 0; marker < markerPositions.size(); ++marker) {
                const float t = markerPositions[marker];
                const float x = topLeft + (topRight - topLeft) * t;
                const SDL_Color zone = marker < 8 ? SDL_Color{35, 213, 132, 235}
                                                  : SDL_Color{238, 75, 75, 235};
                pixelText(renderer, markerLabels[marker], x, face.y + face.h * 0.125F,
                          tinyPixel, zone);
            }

            const float referenceY = face.y + face.h * 0.43F;
            const float referenceLeft = face.x + face.w * 0.17F;
            const float referenceRight = face.x + face.w * 0.84F;
            const float zeroX = referenceLeft + (referenceRight - referenceLeft) * 0.72F;
            line(renderer, referenceLeft, referenceY, zeroX, referenceY,
                 {117, 123, 126, 205});
            line(renderer, zeroX, referenceY, referenceRight, referenceY,
                 {238, 75, 75, 220});
            static constexpr std::array<float, 6> lowerPositions{0.0F, 0.18F, 0.39F, 0.60F, 0.78F, 1.0F};
            static constexpr std::array<std::string_view, 6> lowerLabels{"0", "10", "30", "60", "80", "100"};
            for (std::size_t marker = 0; marker < lowerPositions.size(); ++marker) {
                const float x = referenceLeft + (referenceRight - referenceLeft) * lowerPositions[marker];
                line(renderer, x, referenceY - face.h * 0.008F, x, referenceY + face.h * 0.012F,
                     {139, 145, 148, 165});
                pixelText(renderer, lowerLabels[marker], x, referenceY + face.h * 0.025F,
                          tinyPixel * 0.82F, {135, 142, 145, 210});
            }
        }

        const float pivotX = face.x + face.w * 0.5F;
        const float pivotY = face.y + face.h * 0.94F;
        const float needleLength = std::min(face.h * 0.73F, face.w * 0.60F);
        constexpr float startAngle = -139.0F * pi / 180.0F;
        constexpr float endAngle = -43.0F * pi / 180.0F;
        const auto needleEnd = [&](float amplitude) {
            const float normalized = amplitudeToVu(amplitude);
            const float angle = startAngle + (endAngle - startAngle) * normalized;
            return SDL_FPoint{pivotX + std::cos(angle) * needleLength,
                              pivotY + std::sin(angle) * needleLength};
        };

        const auto peakEnd = needleEnd(heldValues[static_cast<std::size_t>(meter)]);
        line(renderer, pivotX + 1.5F, pivotY + 1.5F, peakEnd.x + 1.5F, peakEnd.y + 1.5F,
             {0, 0, 0, 195});
        glowLine(renderer, pivotX, pivotY, peakEnd.x, peakEnd.y,
                 {238, 48, 54, 235}, std::max(1.0F, face.w * 0.0022F));

        const auto liveEnd = needleEnd(liveValues[static_cast<std::size_t>(meter)]);
        line(renderer, pivotX + 2.0F, pivotY + 2.0F, liveEnd.x + 2.0F, liveEnd.y + 2.0F,
             {0, 0, 0, 220});
        glowLine(renderer, pivotX, pivotY, liveEnd.x, liveEnd.y,
                 {198, 203, 205, 255}, std::max(1.2F, face.w * 0.0030F));

        const float hubRadius = std::max(3.0F, std::min(face.w, face.h) * 0.035F);
        fillCircle(renderer, pivotX, pivotY, hubRadius * 1.24F, {2, 3, 4, 255});
        fillCircle(renderer, pivotX, pivotY, hubRadius, {63, 66, 68, 255});
        circle(renderer, pivotX, pivotY, hubRadius, {206, 210, 211, 125});
        fillCircle(renderer, pivotX - hubRadius * 0.18F, pivotY - hubRadius * 0.22F,
                   hubRadius * 0.26F, {214, 218, 219, 180});

        if (detailed) {
            pixelText(renderer, meter == 0 ? "LEFT CHANNEL" : "RIGHT CHANNEL",
                      face.x + face.w * 0.5F, face.y + face.h * 0.825F,
                      tinyPixel * 0.78F, {35, 213, 132, 190});
            pixelText(renderer, "DB", face.x + face.w * 0.92F,
                      face.y + face.h * 0.455F, tinyPixel * 0.78F,
                      {238, 75, 75, 175});
        } else {
            pixelText(renderer, meter == 0 ? "L" : "R", face.x + face.w * 0.12F,
                      face.y + face.h * 0.80F, std::max(0.9F, titlePixel * 0.55F),
                      {35, 213, 132, 180});
        }

        // A restrained glass reflection gives the flat scale physical depth.
        fillQuad(renderer, {{{face.x + face.w * 0.03F, face.y + face.h * 0.04F},
                             {face.x + face.w * 0.31F, face.y + face.h * 0.04F},
                             {face.x + face.w * 0.64F, face.y + face.h * 0.72F},
                             {face.x + face.w * 0.43F, face.y + face.h * 0.72F}}},
                 {255, 255, 255, 10});
        fill(renderer, {face.x, face.y, face.w, std::max(1.0F, face.h * 0.008F)},
             {255, 255, 255, 42});
        color(renderer, {146, 150, 151, 80});
        SDL_RenderRect(renderer, &face);
    }

    fill(renderer, {rect.x + rect.w * 0.5F - gap * 0.18F, rect.y + outerPad,
                    gap * 0.36F, rect.h - outerPad * 2.0F}, {0, 0, 0, 155});
}

void VisualizerRenderer::drawOwLevelMeter(SDL_Renderer* renderer, const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 24.0F) return;
    constexpr SDL_Color appBackground{28, 28, 32, 255};
    constexpr SDL_Color cardBackground{38, 38, 43, 255};
    constexpr SDL_Color elementBackground{48, 48, 55, 255};
    constexpr SDL_Color border{61, 61, 69, 255};
    constexpr SDL_Color textMain{250, 250, 250, 255};
    constexpr SDL_Color textMuted{161, 161, 170, 255};
    constexpr SDL_Color accent{99, 102, 241, 255};
    constexpr SDL_Color success{34, 197, 94, 255};
    constexpr SDL_Color warning{245, 158, 11, 255};
    constexpr SDL_Color danger{239, 68, 68, 255};

    fill(renderer, rect, appBackground);
    const auto drawPanel = [&](const SDL_FRect& panelRect) {
        fill(renderer, panelRect, cardBackground);
        color(renderer, border);
        SDL_RenderRect(renderer, &panelRect);
        fill(renderer, {panelRect.x + 1.0F, panelRect.y + 1.0F,
                        panelRect.w - 2.0F, 1.0F}, {255, 255, 255, 12});
    };

    const auto drawStereoMeter = [&](const SDL_FRect& card, bool compact) {
        drawPanel(card);
        const float labelPixel = std::clamp(std::min(card.w / 260.0F, card.h / 150.0F),
                                            0.85F, 2.8F);
        const float valuePixel = std::clamp(std::min(card.w / 125.0F, card.h / 72.0F),
                                            1.1F, 5.0F);
        pixelText(renderer, compact ? "OW LEVEL" : "LEVEL METER",
                  card.x + card.w * 0.5F, card.y + card.h * 0.045F,
                  labelPixel, textMuted);

        const float maxDb = std::max(amplitudeToDb(vuLeft_), amplitudeToDb(vuRight_));
        const SDL_Color valueColor = maxDb >= -6.0F ? danger : maxDb >= -12.0F ? warning : textMain;
        pixelText(renderer, oneDecimal(maxDb) + " DB", card.x + card.w * 0.5F,
                  card.y + card.h * (compact ? 0.14F : 0.135F), valuePixel, valueColor);

        const bool clipping = owClipHold_ > 0.0F;
        const SDL_FRect clipRect{card.x + card.w * 0.77F, card.y + card.h * 0.04F,
                                 card.w * 0.18F, card.h * 0.095F};
        fill(renderer, clipRect, clipping ? danger : elementBackground);
        color(renderer, clipping ? danger : border);
        SDL_RenderRect(renderer, &clipRect);
        if (!compact || card.w > 300.0F) {
            pixelText(renderer, "CLIP", clipRect.x + clipRect.w * 0.5F,
                      clipRect.y + clipRect.h * 0.23F, labelPixel * 0.72F,
                      clipping ? textMain : textMuted);
        }

        const float barTop = card.y + card.h * (compact ? 0.29F : 0.285F);
        const float barBottom = card.y + card.h * 0.91F;
        const float barHeight = std::max(8.0F, barBottom - barTop);
        const float barWidth = card.w * (compact ? 0.255F : 0.205F);
        const std::array<float, 2> xPositions{
            card.x + card.w * (compact ? 0.095F : 0.13F),
            card.x + card.w * (compact ? 0.65F : 0.665F)
        };
        const std::array<float, 2> levels{
            amplitudeToDbLevel(vuLeft_), amplitudeToDbLevel(vuRight_)
        };
        const std::array<float, 2> peaks{owPeakLeft_, owPeakRight_};

        constexpr int slices = 60;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float x = xPositions[channel];
            pixelText(renderer, channel == 0 ? "L" : "R", x + barWidth * 0.5F,
                      barTop - labelPixel * 10.0F, labelPixel, textMuted);
            fill(renderer, {x, barTop, barWidth, barHeight}, elementBackground);
            for (int slice = 0; slice < slices; ++slice) {
                const float t0 = static_cast<float>(slice) / static_cast<float>(slices);
                const float t1 = static_cast<float>(slice + 1) / static_cast<float>(slices);
                if (t0 > levels[channel]) break;
                const SDL_Color zone = t0 >= 0.80F ? danger : t0 >= 0.60F ? warning : success;
                const float y = barBottom - t1 * barHeight;
                fill(renderer, {x + 1.0F, y, barWidth - 2.0F,
                                barHeight / static_cast<float>(slices) + 1.0F}, zone);
            }
            const float peakY = barBottom - std::clamp(peaks[channel], 0.0F, 1.0F) * barHeight;
            if (peaks[channel] > 0.01F) {
                fill(renderer, {x + 2.0F, peakY - 1.0F, barWidth - 4.0F,
                                std::max(2.0F, card.h * 0.006F)}, textMain);
                fill(renderer, {x + 3.0F, peakY - 3.0F, barWidth - 6.0F, 1.0F},
                     {255, 255, 255, 55});
            }
            color(renderer, border);
            const SDL_FRect outline{x, barTop, barWidth, barHeight};
            SDL_RenderRect(renderer, &outline);
        }

        static constexpr std::array<int, 7> dbMarks{0, -6, -12, -20, -30, -40, -60};
        const float scaleX = card.x + card.w * 0.5F;
        for (const int db : dbMarks) {
            const float level = (static_cast<float>(db) + 60.0F) / 60.0F;
            const float y = barBottom - level * barHeight;
            line(renderer, scaleX - card.w * 0.032F, y, scaleX + card.w * 0.032F, y,
                 withAlpha(textMuted, 90));
            if (!compact || card.w > 300.0F) {
                pixelText(renderer, std::to_string(db), scaleX, y - labelPixel * 3.2F,
                          labelPixel * 0.62F, withAlpha(textMuted, 220));
            }
        }
    };

    const bool dashboard = rect.w >= 760.0F && rect.h >= 360.0F;
    const float pad = std::max(5.0F, std::min(rect.w, rect.h) * 0.022F);
    if (!dashboard) {
        drawStereoMeter({rect.x + pad, rect.y + pad, rect.w - pad * 2.0F,
                         rect.h - pad * 2.0F}, true);
        return;
    }

    const float headerHeight = rect.h * 0.105F;
    pixelText(renderer, "OW LEVEL METER", rect.x + rect.w * 0.18F,
              rect.y + headerHeight * 0.31F,
              std::clamp(std::min(rect.w / 620.0F, rect.h / 210.0F), 1.2F, 3.7F),
              textMain);
    fillCircle(renderer, rect.x + rect.w - pad * 1.8F, rect.y + headerHeight * 0.5F,
               std::max(3.0F, headerHeight * 0.09F), success);
    pixelText(renderer, "SIGNAL", rect.x + rect.w - pad * 5.2F,
              rect.y + headerHeight * 0.38F,
              std::clamp(rect.h / 330.0F, 1.0F, 2.2F), textMuted);
    line(renderer, rect.x, rect.y + headerHeight, rect.x + rect.w,
         rect.y + headerHeight, border);

    const float contentTop = rect.y + headerHeight + pad;
    const float availableHeight = rect.h - headerHeight - pad * 3.0F;
    const float topHeight = availableHeight * 0.62F;
    const float bottomHeight = availableHeight - topHeight - pad;
    const float meterWidth = rect.w * 0.34F;
    const SDL_FRect meterCard{rect.x + pad, contentTop, meterWidth - pad * 0.5F, topHeight};
    const SDL_FRect spectrumCard{rect.x + meterWidth + pad * 0.5F, contentTop,
                                 rect.w - meterWidth - pad * 1.5F, topHeight};
    drawStereoMeter(meterCard, false);

    drawPanel(spectrumCard);
    const float smallPixel = std::clamp(std::min(rect.w / 680.0F, rect.h / 285.0F), 1.0F, 2.7F);
    pixelText(renderer, "SPECTRUM", spectrumCard.x + spectrumCard.w * 0.12F,
              spectrumCard.y + spectrumCard.h * 0.055F, smallPixel, textMuted);
    const SDL_FRect plot{spectrumCard.x + spectrumCard.w * 0.045F,
                         spectrumCard.y + spectrumCard.h * 0.20F,
                         spectrumCard.w * 0.91F, spectrumCard.h * 0.69F};
    fill(renderer, plot, {22, 22, 27, 255});
    for (int row = 0; row <= 4; ++row) {
        const float y = plot.y + plot.h * static_cast<float>(row) / 4.0F;
        line(renderer, plot.x, y, plot.x + plot.w, y, withAlpha(border, 115));
    }
    for (int column = 0; column <= 4; ++column) {
        const float x = plot.x + plot.w * static_cast<float>(column) / 4.0F;
        line(renderer, x, plot.y, x, plot.y + plot.h, withAlpha(border, 70));
    }
    constexpr std::size_t spectrumBars = 48;
    const float barStep = plot.w / static_cast<float>(spectrumBars);
    std::size_t dominantBand{};
    float dominantLevel{};
    for (std::size_t index = 0; index < spectrumBars; ++index) {
        const float level = bandAt(displayBands_, index, spectrumBars);
        if (level > dominantLevel) {
            dominantLevel = level;
            dominantBand = index;
        }
        const float height = std::max(1.0F, level * plot.h * 0.94F);
        const float frequencyPosition = static_cast<float>(index) /
                                        static_cast<float>(spectrumBars - 1);
        const SDL_Color barColor = mix(accent, {168, 85, 247, 255}, frequencyPosition);
        fill(renderer, {plot.x + static_cast<float>(index) * barStep + 1.0F,
                        plot.y + plot.h - height,
                        std::max(1.0F, barStep - 2.0F), height}, barColor);
        if (level > 0.87F) {
            fill(renderer, {plot.x + static_cast<float>(index) * barStep + 1.0F,
                            plot.y + plot.h - height,
                            std::max(1.0F, barStep - 2.0F), std::max(1.0F, height * 0.11F)}, danger);
        }
    }
    const float dominantPosition = static_cast<float>(dominantBand) /
                                   static_cast<float>(spectrumBars - 1);
    const int dominantFrequency = static_cast<int>(std::lround(
        45.0F * std::pow(18000.0F / 45.0F, dominantPosition)));
    const std::string frequencyText = std::to_string(dominantFrequency) + " HZ";
    pixelText(renderer, frequencyText, spectrumCard.x + spectrumCard.w * 0.84F,
              spectrumCard.y + spectrumCard.h * 0.055F, smallPixel,
              dominantLevel > 0.0F ? accent : textMuted);

    const float bottomTop = contentTop + topHeight + pad;
    const float bottomGap = pad;
    const float bottomWidth = (rect.w - pad * 2.0F - bottomGap * 2.0F) / 3.0F;
    const SDL_FRect loudness{rect.x + pad, bottomTop, bottomWidth, bottomHeight};
    const SDL_FRect phase{loudness.x + bottomWidth + bottomGap, bottomTop, bottomWidth, bottomHeight};
    const SDL_FRect analysis{phase.x + bottomWidth + bottomGap, bottomTop, bottomWidth, bottomHeight};
    drawPanel(loudness);
    drawPanel(phase);
    drawPanel(analysis);

    pixelText(renderer, "LOUDNESS", loudness.x + loudness.w * 0.19F,
              loudness.y + loudness.h * 0.10F, smallPixel * 0.82F, textMuted);
    const float averageAmplitude = (vuLeft_ + vuRight_) * 0.5F;
    pixelText(renderer, oneDecimal(amplitudeToDb(averageAmplitude)) + " DB",
              loudness.x + loudness.w * 0.5F, loudness.y + loudness.h * 0.34F,
              smallPixel * 1.35F, textMain);
    const std::array<float, 2> loudnessLevels{
        amplitudeToDbLevel(vuLeft_), amplitudeToDbLevel(vuRight_)
    };
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const SDL_FRect track{loudness.x + loudness.w * 0.13F,
                              loudness.y + loudness.h * (0.65F + static_cast<float>(channel) * 0.16F),
                              loudness.w * 0.74F, loudness.h * 0.075F};
        fill(renderer, track, elementBackground);
        const SDL_Color zone = loudnessLevels[channel] > 0.90F ? danger
                                  : loudnessLevels[channel] > 0.78F ? warning : success;
        fill(renderer, {track.x, track.y, track.w * loudnessLevels[channel], track.h}, zone);
        color(renderer, border);
        SDL_RenderRect(renderer, &track);
        pixelText(renderer, channel == 0 ? "L" : "R", loudness.x + loudness.w * 0.075F,
                  track.y, smallPixel * 0.62F, textMuted);
    }

    float cross{};
    float leftEnergy{};
    float rightEnergy{};
    for (std::size_t index = 0; index < frame_.leftWaveform.size(); ++index) {
        cross += frame_.leftWaveform[index] * frame_.rightWaveform[index];
        leftEnergy += frame_.leftWaveform[index] * frame_.leftWaveform[index];
        rightEnergy += frame_.rightWaveform[index] * frame_.rightWaveform[index];
    }
    const float correlation = leftEnergy > 0.00001F && rightEnergy > 0.00001F
        ? std::clamp(cross / std::sqrt(leftEnergy * rightEnergy), -1.0F, 1.0F) : 0.0F;
    pixelText(renderer, "STEREO PHASE", phase.x + phase.w * 0.26F,
              phase.y + phase.h * 0.10F, smallPixel * 0.74F, textMuted);
    const SDL_FRect phasePlot{phase.x + phase.w * 0.08F, phase.y + phase.h * 0.27F,
                              phase.w * 0.48F, phase.h * 0.48F};
    fill(renderer, phasePlot, {22, 22, 27, 255});
    line(renderer, phasePlot.x, phasePlot.y + phasePlot.h * 0.5F,
         phasePlot.x + phasePlot.w, phasePlot.y + phasePlot.h * 0.5F, withAlpha(border, 100));
    line(renderer, phasePlot.x + phasePlot.w * 0.5F, phasePlot.y,
         phasePlot.x + phasePlot.w * 0.5F, phasePlot.y + phasePlot.h, withAlpha(border, 100));
    std::vector<SDL_FPoint> phaseTrace;
    phaseTrace.reserve(frame_.leftWaveform.size());
    for (std::size_t index = 0; index < frame_.leftWaveform.size(); index += 2) {
        phaseTrace.push_back({phasePlot.x + phasePlot.w * (0.5F + frame_.leftWaveform[index] * 0.43F),
                              phasePlot.y + phasePlot.h * (0.5F - frame_.rightWaveform[index] * 0.43F)});
    }
    polyline(renderer, phaseTrace, withAlpha(accent, 205), 1);
    pixelText(renderer, twoDecimalsSigned(correlation), phase.x + phase.w * 0.76F,
              phase.y + phase.h * 0.34F, smallPixel * 1.05F,
              correlation < 0.0F ? danger : success);
    const SDL_FRect gauge{phase.x + phase.w * 0.62F, phase.y + phase.h * 0.64F,
                          phase.w * 0.30F, phase.h * 0.065F};
    fill(renderer, gauge, elementBackground);
    fill(renderer, {gauge.x, gauge.y, gauge.w * 0.5F, gauge.h}, withAlpha(danger, 115));
    fill(renderer, {gauge.x + gauge.w * 0.5F, gauge.y, gauge.w * 0.5F, gauge.h}, withAlpha(success, 115));
    const float cursorX = gauge.x + (correlation + 1.0F) * 0.5F * gauge.w;
    fill(renderer, {cursorX - 1.5F, gauge.y - 3.0F, 3.0F, gauge.h + 6.0F}, textMain);

    pixelText(renderer, "ANALYSIS", analysis.x + analysis.w * 0.18F,
              analysis.y + analysis.h * 0.10F, smallPixel * 0.82F, textMuted);
    pixelText(renderer, "DOMINANT", analysis.x + analysis.w * 0.20F,
              analysis.y + analysis.h * 0.35F, smallPixel * 0.67F, textMuted);
    pixelText(renderer, frequencyText, analysis.x + analysis.w * 0.63F,
              analysis.y + analysis.h * 0.31F, smallPixel * 1.0F, accent);
    line(renderer, analysis.x + analysis.w * 0.08F, analysis.y + analysis.h * 0.56F,
         analysis.x + analysis.w * 0.92F, analysis.y + analysis.h * 0.56F, border);
    const bool signalPresent = std::max(vuLeft_, vuRight_) > 0.0032F;
    fillCircle(renderer, analysis.x + analysis.w * 0.15F, analysis.y + analysis.h * 0.75F,
               std::max(3.0F, analysis.h * 0.035F), signalPresent ? success : danger);
    pixelText(renderer, signalPresent ? "SIGNAL" : "SILENCE",
              analysis.x + analysis.w * 0.56F, analysis.y + analysis.h * 0.70F,
              smallPixel * 0.82F, signalPresent ? success : danger);
}

void VisualizerRenderer::drawRackmountSpectrum(SDL_Renderer* renderer,
                                                const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 24.0F) return;

    // A straight-on interpretation of the supplied 1U rack analyzer. Keeping the
    // hardware shallow in the full-screen view preserves the characteristic wide
    // proportions while the compact card uses more height so its LEDs remain legible.
    constexpr int backgroundStrips = 24;
    for (int strip = 0; strip < backgroundStrips; ++strip) {
        const float t = static_cast<float>(strip) /
                        static_cast<float>(backgroundStrips - 1);
        fill(renderer,
             {rect.x, rect.y + rect.h * t, rect.w,
              rect.h / static_cast<float>(backgroundStrips) + 1.0F},
             mix({2, 3, 6, 255}, {8, 11, 17, 255},
                 1.0F - std::abs(t * 2.0F - 1.0F)));
    }

    const bool detailed = rect.w >= 760.0F && rect.h >= 300.0F;
    const float outerPad = std::max(5.0F, std::min(rect.w, rect.h) * 0.035F);
    const float panelWidth = rect.w - outerPad * 2.0F;
    const float panelHeight = detailed
        ? std::min(rect.h * 0.56F, panelWidth * 0.195F)
        : rect.h - outerPad * 2.0F;
    const SDL_FRect panel{rect.x + outerPad,
                          rect.y + (rect.h - panelHeight) * 0.5F,
                          panelWidth, panelHeight};

    // Drop shadow and brushed-black anodized enclosure.
    fill(renderer, {panel.x + panel.h * 0.035F, panel.y + panel.h * 0.055F,
                    panel.w, panel.h}, {0, 0, 0, 150});
    constexpr int faceStrips = 32;
    for (int strip = 0; strip < faceStrips; ++strip) {
        const float t = static_cast<float>(strip) / static_cast<float>(faceStrips - 1);
        const SDL_Color faceColor = t < 0.48F
            ? mix({30, 34, 38, 255}, {10, 12, 15, 255}, t / 0.48F)
            : mix({10, 12, 15, 255}, {23, 26, 29, 255}, (t - 0.48F) / 0.52F);
        fill(renderer, {panel.x, panel.y + panel.h * t, panel.w,
                        panel.h / static_cast<float>(faceStrips) + 1.0F}, faceColor);
    }
    fill(renderer, {panel.x, panel.y, panel.w, std::max(1.0F, panel.h * 0.012F)},
         {104, 112, 119, 110});
    fill(renderer, {panel.x, panel.y + panel.h - std::max(2.0F, panel.h * 0.018F),
                    panel.w, std::max(2.0F, panel.h * 0.018F)}, {0, 0, 0, 220});
    color(renderer, {1, 2, 3, 255});
    SDL_RenderRect(renderer, &panel);

    const float earWidth = panel.w * (detailed ? 0.038F : 0.055F);
    const std::array<SDL_FRect, 2> ears{{
        {panel.x, panel.y, earWidth, panel.h},
        {panel.x + panel.w - earWidth, panel.y, earWidth, panel.h}
    }};
    for (const auto& ear : ears) {
        fill(renderer, ear, {13, 15, 17, 255});
        fill(renderer, {ear.x, ear.y, ear.w, std::max(1.0F, panel.h * 0.016F)},
             {93, 99, 104, 80});
        const float screwRadius = std::clamp(panel.h * 0.026F, 2.2F, 9.0F);
        for (const float screwY : {ear.y + ear.h * 0.20F, ear.y + ear.h * 0.80F}) {
            const float screwX = ear.x + ear.w * 0.50F;
            fillCircle(renderer, screwX, screwY, screwRadius * 1.35F, {2, 3, 4, 255});
            fillCircle(renderer, screwX, screwY, screwRadius, {76, 81, 84, 255});
            circle(renderer, screwX, screwY, screwRadius, {151, 156, 157, 115});
            line(renderer, screwX - screwRadius * 0.52F, screwY + screwRadius * 0.18F,
                 screwX + screwRadius * 0.52F, screwY - screwRadius * 0.18F,
                 {19, 21, 22, 230});
        }
    }

    const float bodyLeft = panel.x + earWidth;
    const float bodyWidth = panel.w - earWidth * 2.0F;
    const float knobZoneWidth = bodyWidth * (detailed ? 0.135F : 0.185F);
    const float knobCenterX = bodyLeft + knobZoneWidth * 0.48F;
    const float knobCenterY = panel.y + panel.h * 0.56F;
    const float knobRadius = std::min(panel.h * 0.235F, knobZoneWidth * 0.30F);

    // Calibrated tick ring and machined aluminium level control.
    constexpr int knobTicks = 17;
    for (int tick = 0; tick < knobTicks; ++tick) {
        const float t = static_cast<float>(tick) / static_cast<float>(knobTicks - 1);
        const float angle = -2.42F + t * 4.84F;
        const float innerRadius = knobRadius * (tick % 4 == 0 ? 1.27F : 1.32F);
        const float outerRadius = knobRadius * 1.44F;
        line(renderer,
             knobCenterX + std::cos(angle) * innerRadius,
             knobCenterY + std::sin(angle) * innerRadius,
             knobCenterX + std::cos(angle) * outerRadius,
             knobCenterY + std::sin(angle) * outerRadius,
             tick % 4 == 0 ? SDL_Color{184, 188, 188, 150}
                           : SDL_Color{87, 91, 92, 115});
    }
    fillCircle(renderer, knobCenterX + knobRadius * 0.08F,
               knobCenterY + knobRadius * 0.12F, knobRadius * 1.12F,
               {0, 0, 0, 175});
    fillCircle(renderer, knobCenterX, knobCenterY, knobRadius * 1.06F,
               {42, 45, 46, 255});
    constexpr int knobRings = 14;
    for (int ring = knobRings; ring >= 1; --ring) {
        const float t = static_cast<float>(ring) / static_cast<float>(knobRings);
        const float shade = 0.25F + (1.0F - t) * 0.58F;
        const Uint8 channel = static_cast<Uint8>(std::lround(255.0F * shade));
        fillCircle(renderer, knobCenterX - knobRadius * (1.0F - t) * 0.10F,
                   knobCenterY - knobRadius * (1.0F - t) * 0.13F,
                   knobRadius * t,
                   {channel, static_cast<Uint8>(std::min(255, channel + 3)),
                    static_cast<Uint8>(std::min(255, channel + 4)), 255});
    }
    for (int groove = 0; groove < 28; ++groove) {
        const float angle = static_cast<float>(groove) / 28.0F * pi * 2.0F;
        line(renderer,
             knobCenterX + std::cos(angle) * knobRadius * 0.88F,
             knobCenterY + std::sin(angle) * knobRadius * 0.88F,
             knobCenterX + std::cos(angle) * knobRadius * 1.03F,
             knobCenterY + std::sin(angle) * knobRadius * 1.03F,
             {23, 25, 26, 150});
    }
    circle(renderer, knobCenterX, knobCenterY, knobRadius * 1.06F,
           {203, 207, 205, 135}, 2);
    const float knobLevel = std::clamp((vuLeft_ + vuRight_) * 0.5F, 0.0F, 1.0F);
    const float knobAngle = -2.22F + knobLevel * 4.44F;
    line(renderer, knobCenterX + std::cos(knobAngle) * knobRadius * 0.42F,
         knobCenterY + std::sin(knobAngle) * knobRadius * 0.42F,
         knobCenterX + std::cos(knobAngle) * knobRadius * 0.82F,
         knobCenterY + std::sin(knobAngle) * knobRadius * 0.82F,
         {239, 243, 238, 225});

    const float tinyPixel = std::clamp(panel.h / 155.0F, 0.72F, 2.1F);
    if (detailed || rect.w > 330.0F) {
        pixelText(renderer, "LEVEL", knobCenterX, panel.y + panel.h * 0.12F,
                  tinyPixel * 0.78F, {139, 145, 147, 185});
    }

    const float statusWidth = bodyWidth * (detailed ? 0.048F : 0.055F);
    const SDL_FRect glass{bodyLeft + knobZoneWidth,
                          panel.y + panel.h * 0.105F,
                          bodyWidth - knobZoneWidth - statusWidth,
                          panel.h * 0.77F};
    fill(renderer, {glass.x - panel.h * 0.016F, glass.y - panel.h * 0.018F,
                    glass.w + panel.h * 0.032F, glass.h + panel.h * 0.036F},
         {1, 2, 3, 255});
    fill(renderer, glass, {3, 6, 8, 255});
    fill(renderer, {glass.x, glass.y, glass.w, std::max(1.0F, glass.h * 0.025F)},
         {47, 57, 60, 140});

    const float titleHeight = detailed ? glass.h * 0.14F : glass.h * 0.09F;
    if (detailed) {
        pixelText(renderer, "MUSIC SPECTRUM ANALYZER", glass.x + glass.w * 0.5F,
                  glass.y + glass.h * 0.035F, tinyPixel * 0.76F,
                  {115, 126, 130, 165});
    }
    const float labelHeight = detailed ? glass.h * 0.14F : glass.h * 0.055F;
    const SDL_FRect matrix{glass.x + glass.w * 0.018F,
                           glass.y + titleHeight,
                           glass.w * 0.964F,
                           glass.h - titleHeight - labelHeight};
    const std::size_t bandCount = detailed ? 31U : (rect.w >= 300.0F ? 24U : 18U);
    const int rowCount = detailed ? 18 : 16;
    const float bandStep = matrix.w / static_cast<float>(bandCount);
    const float rowStep = matrix.h / static_cast<float>(rowCount);
    const float ledWidth = std::max(1.0F, bandStep * 0.69F);
    const float ledHeight = std::max(1.0F, rowStep * 0.69F);

    for (std::size_t band = 0; band < bandCount; ++band) {
        const float frequencyPosition = bandCount > 1
            ? static_cast<float>(band) / static_cast<float>(bandCount - 1) : 0.0F;
        const SDL_Color ledColor = hsv(0.015F + frequencyPosition * 0.78F, 0.83F, 1.0F);
        const float level = std::clamp(std::pow(bandAt(displayBands_, band, bandCount), 0.82F),
                                       0.0F, 1.0F);
        const int litRows = std::clamp(static_cast<int>(std::ceil(level * rowCount)), 0, rowCount);
        const float peak = std::clamp(bandAt(peakHold_, band, bandCount), 0.0F, 1.0F);
        const int peakRow = std::clamp(static_cast<int>(std::ceil(peak * rowCount)) - 1,
                                       0, rowCount - 1);
        const float x = matrix.x + static_cast<float>(band) * bandStep +
                        (bandStep - ledWidth) * 0.5F;
        for (int row = 0; row < rowCount; ++row) {
            const float y = matrix.y + matrix.h - static_cast<float>(row + 1) * rowStep +
                            (rowStep - ledHeight) * 0.5F;
            const SDL_FRect segment{x, y, ledWidth, ledHeight};
            const bool active = row < litRows;
            if (!active) {
                fill(renderer, segment,
                     {static_cast<Uint8>(4 + ledColor.r / 28),
                      static_cast<Uint8>(6 + ledColor.g / 28),
                      static_cast<Uint8>(7 + ledColor.b / 28), 255});
                fill(renderer, {segment.x + 1.0F, segment.y + 1.0F,
                                std::max(0.0F, segment.w - 2.0F), 1.0F},
                     withAlpha(ledColor, 16));
                continue;
            }

            const bool isPeak = row == peakRow && peak > 0.02F;
            fill(renderer, {segment.x - bandStep * 0.17F, segment.y - rowStep * 0.20F,
                            segment.w + bandStep * 0.34F,
                            segment.h + rowStep * 0.40F},
                 withAlpha(ledColor, isPeak ? 45 : 24));
            fill(renderer, segment, isPeak ? mix(ledColor, ink, 0.42F) : ledColor);
            fill(renderer, {segment.x + segment.w * 0.08F,
                            segment.y + segment.h * 0.10F,
                            segment.w * 0.84F,
                            std::max(1.0F, segment.h * 0.18F)},
                 {255, 255, 255, static_cast<Uint8>(isPeak ? 150 : 82)});
        }
    }

    // Frequency legends sit behind the glass, as on a physical real-time analyzer.
    if (detailed) {
        static constexpr std::array<std::string_view, 10> labels{
            "32", "63", "125", "250", "500", "1K", "2K", "4K", "8K", "16K"
        };
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const float t = static_cast<float>(index) /
                            static_cast<float>(labels.size() - 1);
            pixelText(renderer, labels[index], matrix.x + matrix.w * t,
                      glass.y + glass.h * 0.895F, tinyPixel * 0.58F,
                      {102, 113, 118, 175});
        }
        pixelText(renderer, "HZ", glass.x + glass.w * 0.985F,
                  glass.y + glass.h * 0.895F, tinyPixel * 0.54F,
                  {102, 113, 118, 150});
    }

    // Restrained glass reflections, status light and a physical power rocker.
    fillQuad(renderer, {{{glass.x + glass.w * 0.02F, glass.y + glass.h * 0.025F},
                         {glass.x + glass.w * 0.30F, glass.y + glass.h * 0.025F},
                         {glass.x + glass.w * 0.48F, glass.y + glass.h * 0.72F},
                         {glass.x + glass.w * 0.25F, glass.y + glass.h * 0.72F}}},
             {255, 255, 255, 8});
    color(renderer, {71, 80, 84, 100});
    SDL_RenderRect(renderer, &glass);

    const float statusCenterX = glass.x + glass.w + statusWidth * 0.54F;
    const float statusCenterY = panel.y + panel.h * 0.46F;
    const float statusRadius = std::clamp(panel.h * 0.018F, 1.6F, 6.0F);
    fillCircle(renderer, statusCenterX, statusCenterY, statusRadius * 2.3F,
               {39, 255, 152, 16});
    fillCircle(renderer, statusCenterX, statusCenterY, statusRadius,
               std::max(vuLeft_, vuRight_) > 0.004F ? SDL_Color{42, 236, 145, 255}
                                                    : SDL_Color{45, 72, 58, 255});
    if (detailed) {
        pixelText(renderer, "SIGNAL", statusCenterX, statusCenterY + panel.h * 0.075F,
                  tinyPixel * 0.52F, {106, 116, 117, 160});
    }
    const SDL_FRect rocker{statusCenterX - statusWidth * 0.20F,
                           panel.y + panel.h * 0.68F,
                           statusWidth * 0.40F, panel.h * 0.12F};
    fill(renderer, rocker, {3, 4, 5, 255});
    fill(renderer, {rocker.x + 1.0F, rocker.y + 1.0F,
                    std::max(1.0F, rocker.w - 2.0F), rocker.h * 0.42F},
         {50, 54, 56, 255});
    color(renderer, {76, 81, 83, 100});
    SDL_RenderRect(renderer, &rocker);
}

void VisualizerRenderer::drawGreenDbMeter(SDL_Renderer* renderer,
                                          const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 24.0F) return;

    // Minimal CRT-like face from the supplied reference: no decorative chrome,
    // only a deep charcoal panel, nine calibrated columns and green phosphor ink.
    constexpr int backgroundStrips = 20;
    for (int strip = 0; strip < backgroundStrips; ++strip) {
        const float t = static_cast<float>(strip) /
                        static_cast<float>(backgroundStrips - 1);
        fill(renderer,
             {rect.x, rect.y + rect.h * t, rect.w,
              rect.h / static_cast<float>(backgroundStrips) + 1.0F},
             mix({28, 29, 29, 255}, {35, 36, 36, 255},
                 1.0F - std::abs(t * 2.0F - 1.0F)));
    }

    const bool detailed = rect.w >= 760.0F && rect.h >= 300.0F;
    const float sidePad = rect.w * (detailed ? 0.075F : 0.035F);
    const float contentWidth = rect.w - sidePad * 2.0F;
    const float legendWidth = contentWidth * (detailed ? 0.105F : 0.125F);
    const float barsLeft = rect.x + sidePad + legendWidth;
    const float barsWidth = contentWidth - legendWidth;
    const float graphTop = rect.y + rect.h * (detailed ? 0.105F : 0.075F);
    const float graphBottom = rect.y + rect.h * (detailed ? 0.765F : 0.745F);
    const float graphHeight = std::max(8.0F, graphBottom - graphTop);
    constexpr std::size_t bandCount = 9;
    constexpr int segmentCount = 14;
    const float bandStep = barsWidth / static_cast<float>(bandCount);
    const float segmentStep = graphHeight / static_cast<float>(segmentCount);
    const float segmentWidth = bandStep * (detailed ? 0.68F : 0.72F);
    const float segmentHeight = std::max(1.0F, segmentStep * 0.70F);

    for (std::size_t band = 0; band < bandCount; ++band) {
        const float rawLevel = bandAt(displayBands_, band, bandCount);
        const float level = std::clamp(std::pow(rawLevel, 0.82F) * 0.94F, 0.0F, 1.0F);
        const int litSegments = std::clamp(
            static_cast<int>(std::ceil(level * static_cast<float>(segmentCount))),
            0, segmentCount);
        const float x = barsLeft + static_cast<float>(band) * bandStep +
                        (bandStep - segmentWidth) * 0.5F;

        for (int segment = 0; segment < segmentCount; ++segment) {
            const float y = graphBottom - static_cast<float>(segment + 1) * segmentStep +
                            (segmentStep - segmentHeight) * 0.5F;
            const SDL_FRect block{x, y, segmentWidth, segmentHeight};
            const float heightPosition = static_cast<float>(segment) /
                                         static_cast<float>(segmentCount - 1);
            const SDL_Color phosphorColor = mix({103, 255, 128, 255},
                                                 {28, 102, 48, 255},
                                                 heightPosition * 0.92F);
            if (segment >= litSegments) {
                // Barely-visible inactive glass keeps the stepped scale readable
                // without losing the clean black space of the reference.
                fill(renderer, block,
                     {static_cast<Uint8>(4 + phosphorColor.r / 42),
                      static_cast<Uint8>(7 + phosphorColor.g / 42),
                      static_cast<Uint8>(5 + phosphorColor.b / 42), 120});
                continue;
            }

            fill(renderer, {block.x - bandStep * 0.045F,
                            block.y - segmentStep * 0.08F,
                            block.w + bandStep * 0.09F,
                            block.h + segmentStep * 0.16F},
                 withAlpha(phosphorColor, 18));
            fill(renderer, block, phosphorColor);
            fill(renderer, {block.x + 1.0F, block.y + 1.0F,
                            std::max(0.0F, block.w - 2.0F),
                            std::max(1.0F, block.h * 0.18F)},
                 {190, 255, 194, static_cast<Uint8>(120 - heightPosition * 45.0F)});
            color(renderer, {161, 255, 174, 120});
            SDL_RenderRect(renderer, &block);
        }
    }

    static constexpr std::array<std::string_view, bandCount> labels{
        "60", "120", "250", "500", "1K", "2K", "4K", "8K", "16K"
    };
    const float textPixel = std::clamp(
        std::min(bandStep / 24.0F, rect.h / 185.0F), 0.68F, 3.0F);
    const float labelTop = rect.y + rect.h * (detailed ? 0.805F : 0.79F);
    const SDL_Color labelColor{163, 255, 175, 235};
    pixelText(renderer, "DB METER",
              rect.x + sidePad + legendWidth * 0.46F,
              labelTop, textPixel * (detailed ? 0.92F : 0.76F), labelColor);
    for (std::size_t band = 0; band < bandCount; ++band) {
        pixelText(renderer, labels[band],
                  barsLeft + (static_cast<float>(band) + 0.5F) * bandStep,
                  labelTop, textPixel, labelColor);
    }

    // Fine scanlines and a restrained edge reflection make the result feel like
    // a photographed display without obscuring the clean segmented geometry.
    const float scanlineStep = detailed ? 5.0F : 3.0F;
    for (float y = rect.y + 1.0F; y < rect.y + rect.h; y += scanlineStep) {
        line(renderer, rect.x, y, rect.x + rect.w, y, {0, 0, 0, 10});
    }
    fill(renderer, {rect.x, rect.y, rect.w, 1.0F}, {152, 255, 170, 18});
    fill(renderer, {rect.x, rect.y + rect.h - 1.0F, rect.w, 1.0F}, {0, 0, 0, 150});
}

void VisualizerRenderer::drawSpectrumSkyline(SDL_Renderer* renderer,
                                              const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 24.0F) return;

    fill(renderer, rect, {0, 1, 1, 255});
    const bool detailed = rect.w >= 760.0F && rect.h >= 300.0F;
    const std::size_t barCount = detailed ? 36U : 28U;
    const int segmentCount = detailed ? 20 : 16;
    const float sidePad = rect.w * (detailed ? 0.025F : 0.018F);
    const float graphTop = rect.y + rect.h * 0.055F;
    const float horizon = rect.y + rect.h * 0.735F;
    const float graphHeight = horizon - graphTop;
    const float floorBottom = rect.y + rect.h * 0.975F;
    const float floorHeight = floorBottom - horizon;
    const float usableWidth = rect.w - sidePad * 2.0F;
    const float barStep = usableWidth / static_cast<float>(barCount);
    const float barWidth = std::max(1.0F, barStep * 0.69F);
    const float segmentStep = graphHeight / static_cast<float>(segmentCount);
    const float segmentHeight = std::max(1.0F, segmentStep * 0.67F);
    const float centerX = rect.x + rect.w * 0.5F;

    const auto zoneColor = [](float heightPosition, Uint8 alpha = 255) {
        SDL_Color value;
        if (heightPosition < 0.42F) {
            value = mix({16, 201, 50, 255}, {111, 255, 36, 255},
                        heightPosition / 0.42F);
        } else if (heightPosition < 0.64F) {
            value = mix({111, 255, 36, 255}, {255, 232, 32, 255},
                        (heightPosition - 0.42F) / 0.22F);
        } else if (heightPosition < 0.78F) {
            value = mix({255, 232, 32, 255}, {255, 129, 17, 255},
                        (heightPosition - 0.64F) / 0.14F);
        } else {
            value = mix({255, 79, 15, 255}, {244, 25, 9, 255},
                        (heightPosition - 0.78F) / 0.22F);
        }
        value.a = alpha;
        return value;
    };

    std::array<float, 48> levels{};
    std::array<int, 48> litSegments{};
    for (std::size_t bar = 0; bar < barCount; ++bar) {
        levels[bar] = std::clamp(
            std::pow(bandAt(displayBands_, bar, barCount), 0.78F) * 0.98F,
            0.0F, 1.0F);
        litSegments[bar] = std::clamp(
            static_cast<int>(std::ceil(levels[bar] * static_cast<float>(segmentCount))),
            0, segmentCount);
    }

    // Perspective grid and dim mirrored LEDs form the black glass floor seen in
    // the reference, converging at the spectrum baseline.
    for (int row = 1; row <= 7; ++row) {
        const float t = static_cast<float>(row) / 7.0F;
        const float curved = t * t;
        const float y = horizon + floorHeight * curved;
        line(renderer, rect.x, y, rect.x + rect.w, y,
             mix({23, 78, 25, 45}, {139, 69, 9, 18}, t));
    }
    for (std::size_t boundary = 0; boundary <= barCount; boundary += 2) {
        const float horizonX = rect.x + sidePad + static_cast<float>(boundary) * barStep;
        const float bottomX = centerX + (horizonX - centerX) * 1.34F;
        line(renderer, horizonX, horizon, bottomX, floorBottom, {30, 88, 31, 30});
    }

    for (std::size_t bar = 0; bar < barCount; ++bar) {
        const float x = rect.x + sidePad + static_cast<float>(bar) * barStep +
                        (barStep - barWidth) * 0.5F;
        for (int segment = 0; segment < litSegments[bar]; ++segment) {
            const float depth = static_cast<float>(segment) /
                                static_cast<float>(segmentCount);
            const float reflectedTop = horizon + depth * floorHeight * 0.88F +
                                       segmentStep * 0.10F;
            const float reflectedBottom = reflectedTop + segmentHeight *
                                           (0.53F + depth * 0.10F);
            const float expansion = depth * barStep * 0.17F;
            const float shift = (x + barWidth * 0.5F - centerX) * depth * 0.10F -
                                rect.w * depth * 0.012F;
            const SDL_Color reflectedColor = zoneColor(
                static_cast<float>(segment) / static_cast<float>(segmentCount - 1),
                static_cast<Uint8>(std::clamp(54.0F * (1.0F - depth), 5.0F, 54.0F)));
            fillQuad(renderer,
                     {{{x, reflectedTop}, {x + barWidth, reflectedTop},
                       {x + shift + barWidth + expansion, reflectedBottom},
                       {x + shift - expansion, reflectedBottom}}},
                     reflectedColor);
        }
    }

    for (std::size_t bar = 0; bar < barCount; ++bar) {
        const float x = rect.x + sidePad + static_cast<float>(bar) * barStep +
                        (barStep - barWidth) * 0.5F;
        const float held = std::clamp(bandAt(peakHold_, bar, barCount), 0.0F, 1.0F);
        const int heldSegment = std::clamp(
            static_cast<int>(std::ceil(held * static_cast<float>(segmentCount))) - 1,
            0, segmentCount - 1);
        for (int segment = 0; segment < segmentCount; ++segment) {
            const float heightPosition = static_cast<float>(segment) /
                                         static_cast<float>(segmentCount - 1);
            const float y = horizon - static_cast<float>(segment + 1) * segmentStep +
                            (segmentStep - segmentHeight) * 0.5F;
            const SDL_FRect led{x, y, barWidth, segmentHeight};
            if (segment >= litSegments[bar]) {
                // The source leaves unlit space almost completely black; this tiny
                // green trace only reveals the physical LED matrix on close view.
                fill(renderer, led, {1, 8, 3, 115});
                continue;
            }

            const SDL_Color activeColor = zoneColor(heightPosition);
            const bool peakSegment = segment == heldSegment && held > 0.015F;
            fill(renderer, {led.x - barStep * 0.09F, led.y - segmentStep * 0.12F,
                            led.w + barStep * 0.18F, led.h + segmentStep * 0.24F},
                 withAlpha(activeColor, peakSegment ? 43 : 24));
            fill(renderer, led, peakSegment ? mix(activeColor, ink, 0.18F) : activeColor);
            fill(renderer, {led.x + led.w * 0.08F, led.y + led.h * 0.08F,
                            led.w * 0.84F, std::max(1.0F, led.h * 0.17F)},
                 {255, 255, 205, static_cast<Uint8>(peakSegment ? 105 : 65)});
        }
    }

    fill(renderer, {rect.x, horizon - 1.0F, rect.w, 1.0F}, {99, 255, 48, 34});
    fill(renderer, {rect.x, rect.y, rect.w, 1.0F}, {255, 255, 255, 8});
}

void VisualizerRenderer::drawNeonMosaic(SDL_Renderer* renderer,
                                        const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 24.0F) return;

    fill(renderer, rect, {0, 0, 1, 255});
    const bool detailed = rect.w >= 760.0F && rect.h >= 300.0F;
    const std::size_t barCount = detailed ? 24U : 16U;
    const int segmentCount = detailed ? 22 : 17;
    const float sidePad = rect.w * (detailed ? 0.012F : 0.016F);
    const float topPad = rect.h * 0.018F;
    const float bottom = rect.y + rect.h * 0.985F;
    const float graphHeight = bottom - (rect.y + topPad);
    const float step = (rect.w - sidePad * 2.0F) / static_cast<float>(barCount);
    const float barWidth = std::max(1.0F, step * 0.68F);
    const float segmentStep = graphHeight / static_cast<float>(segmentCount);
    const float segmentHeight = std::max(1.0F, segmentStep * 0.68F);

    // Intentionally irregular color order mirrors the reference: each frequency
    // owns a distinct tube color rather than sharing one continuous rainbow.
    static constexpr std::array<SDL_Color, 16> palette{{
        {72, 18, 238, 255}, {255, 9, 169, 255}, {210, 43, 14, 255},
        {91, 13, 162, 255}, {150, 188, 14, 255}, {255, 132, 19, 255},
        {111, 14, 90, 255}, {247, 0, 239, 255}, {218, 11, 40, 255},
        {130, 190, 34, 255}, {90, 20, 211, 255}, {252, 0, 175, 255},
        {255, 93, 83, 255}, {179, 114, 9, 255}, {170, 16, 8, 255},
        {205, 54, 111, 255}
    }};

    for (std::size_t bar = 0; bar < barCount; ++bar) {
        // The offset prevents adjacent repeating palette cycles from looking tiled.
        const SDL_Color tubeColor = palette[(bar * 5U + bar / palette.size()) % palette.size()];
        const float rawLevel = bandAt(displayBands_, bar, barCount);
        const float level = std::clamp(std::pow(rawLevel, 0.80F), 0.0F, 1.0F);
        const int litSegments = std::clamp(
            static_cast<int>(std::ceil(level * static_cast<float>(segmentCount))),
            0, segmentCount);
        const float held = std::clamp(bandAt(peakHold_, bar, barCount), 0.0F, 1.0F);
        const int heldSegment = std::clamp(
            static_cast<int>(std::ceil(held * static_cast<float>(segmentCount))) - 1,
            0, segmentCount - 1);
        const float x = rect.x + sidePad + static_cast<float>(bar) * step +
                        (step - barWidth) * 0.5F;

        for (int segment = 0; segment < segmentCount; ++segment) {
            const float heightPosition = static_cast<float>(segment) /
                                         static_cast<float>(segmentCount - 1);
            const float y = bottom - static_cast<float>(segment + 1) * segmentStep +
                            (segmentStep - segmentHeight) * 0.5F;
            const SDL_FRect block{x, y, barWidth, segmentHeight};

            if (segment >= litSegments) {
                // Faint colored glass at the top keeps the hardware segmentation
                // visible while remaining almost indistinguishable from black.
                fill(renderer, block,
                     {static_cast<Uint8>(tubeColor.r / 15),
                      static_cast<Uint8>(tubeColor.g / 15),
                      static_cast<Uint8>(tubeColor.b / 15), 145});
                continue;
            }

            const float brightness = 0.42F + (1.0F - heightPosition) * 0.58F;
            SDL_Color active = mix({4, 0, 7, 255}, tubeColor, brightness);
            const bool peakSegment = segment == heldSegment && held > 0.015F;
            if (peakSegment) active = mix(active, {255, 214, 214, 255}, 0.23F);
            fill(renderer, {block.x - step * 0.08F, block.y - segmentStep * 0.10F,
                            block.w + step * 0.16F, block.h + segmentStep * 0.20F},
                 withAlpha(active, peakSegment ? 45 : 25));
            fill(renderer, block, active);
            fill(renderer, {block.x + block.w * 0.06F, block.y + block.h * 0.08F,
                            block.w * 0.88F, std::max(1.0F, block.h * 0.15F)},
                 {255, 204, 255, static_cast<Uint8>(peakSegment ? 105 : 54)});
        }
    }

    // The source is deliberately borderless and tightly cropped.
    fill(renderer, {rect.x, rect.y, rect.w, 1.0F}, {255, 0, 190, 10});
}

void VisualizerRenderer::drawTripleSoundMeter(SDL_Renderer* renderer,
                                               const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 24.0F) return;

    constexpr int backgroundStrips = 20;
    for (int strip = 0; strip < backgroundStrips; ++strip) {
        const float t = static_cast<float>(strip) /
                        static_cast<float>(backgroundStrips - 1);
        fill(renderer,
             {rect.x, rect.y + rect.h * t, rect.w,
              rect.h / static_cast<float>(backgroundStrips) + 1.0F},
             mix({0, 0, 0, 255}, {9, 10, 11, 255},
                 1.0F - std::abs(t * 2.0F - 1.0F)));
    }

    const bool detailed = rect.w >= 760.0F && rect.h >= 300.0F;
    const float sidePad = rect.w * (detailed ? 0.035F : 0.020F);
    const float gap = rect.w * (detailed ? 0.020F : 0.012F);
    const float columnWidth = (rect.w - sidePad * 2.0F - gap * 2.0F) / 3.0F;
    const std::array<float, 3> meterLevels{
        amplitudeToVu(vuLeft_),
        amplitudeToVu((vuLeft_ + vuRight_) * 0.5F),
        amplitudeToVu(vuRight_)
    };
    const std::array<SDL_Color, 3> statusColors{
        SDL_Color{46, 238, 74, 255},
        SDL_Color{255, 126, 22, 255},
        SDL_Color{255, 43, 43, 255}
    };
    const std::array<std::string_view, 3> channelNames{
        "LEFT CHANNEL", "AVERAGE", "RIGHT CHANNEL"
    };

    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float columnX = rect.x + sidePad + static_cast<float>(channel) *
                              (columnWidth + gap);

        // Miniature real-time analyzer with a thick smoked-glass bezel.
        const SDL_FRect screenOuter{columnX + columnWidth * 0.08F,
                                    rect.y + rect.h * 0.045F,
                                    columnWidth * 0.84F,
                                    rect.h * (detailed ? 0.225F : 0.245F)};
        fill(renderer, {screenOuter.x + 3.0F, screenOuter.y + 4.0F,
                        screenOuter.w, screenOuter.h}, {0, 0, 0, 175});
        fill(renderer, screenOuter, {37, 40, 42, 255});
        fill(renderer, {screenOuter.x + 3.0F, screenOuter.y + 3.0F,
                        screenOuter.w - 6.0F, screenOuter.h - 6.0F},
             {13, 15, 17, 255});
        const SDL_FRect screen{screenOuter.x + screenOuter.w * 0.065F,
                               screenOuter.y + screenOuter.h * 0.12F,
                               screenOuter.w * 0.87F,
                               screenOuter.h * 0.76F};
        fill(renderer, screen, {20, 24, 25, 255});
        const std::size_t miniBars = detailed ? 14U : 10U;
        constexpr int miniSegments = 8;
        const float miniStep = screen.w / static_cast<float>(miniBars);
        const float miniWidth = miniStep * 0.68F;
        const float miniRowStep = screen.h / static_cast<float>(miniSegments);
        const float miniHeight = std::max(1.0F, miniRowStep * 0.63F);
        for (std::size_t bar = 0; bar < miniBars; ++bar) {
            const std::size_t shifted = (bar + channel * 2U) % miniBars;
            const float bandLevel = bandAt(displayBands_, shifted, miniBars);
            const float channelScale = 0.79F + meterLevels[channel] * 0.25F;
            const int activeRows = std::clamp(
                static_cast<int>(std::ceil(std::pow(bandLevel, 0.82F) *
                                           channelScale * miniSegments)),
                0, miniSegments);
            const float x = screen.x + static_cast<float>(bar) * miniStep +
                            (miniStep - miniWidth) * 0.5F;
            for (int row = 0; row < miniSegments; ++row) {
                const float y = screen.y + screen.h - static_cast<float>(row + 1) *
                                miniRowStep + (miniRowStep - miniHeight) * 0.5F;
                const SDL_FRect led{x, y, miniWidth, miniHeight};
                SDL_Color ledColor = row < 4 ? SDL_Color{42, 229, 70, 255}
                                   : row < 6 ? SDL_Color{220, 241, 36, 255}
                                   : row < 7 ? SDL_Color{255, 153, 25, 255}
                                             : SDL_Color{255, 64, 30, 255};
                if (row < activeRows) {
                    fill(renderer, {led.x - 1.0F, led.y - 1.0F,
                                    led.w + 2.0F, led.h + 2.0F},
                         withAlpha(ledColor, 24));
                    fill(renderer, led, ledColor);
                } else {
                    fill(renderer, led,
                         {static_cast<Uint8>(ledColor.r / 20),
                          static_cast<Uint8>(ledColor.g / 20),
                          static_cast<Uint8>(ledColor.b / 20), 180});
                }
            }
        }
        fillQuad(renderer, {{{screen.x + screen.w * 0.04F, screen.y},
                             {screen.x + screen.w * 0.36F, screen.y},
                             {screen.x + screen.w * 0.55F, screen.y + screen.h},
                             {screen.x + screen.w * 0.25F, screen.y + screen.h}}},
                 {255, 255, 255, 9});
        color(renderer, {100, 105, 108, 115});
        SDL_RenderRect(renderer, &screenOuter);

        // Deep machined bezel and dark radial meter face.
        const float gaugeRadius = std::min(columnWidth * (detailed ? 0.355F : 0.39F),
                                           rect.h * (detailed ? 0.270F : 0.255F));
        const float gaugeX = columnX + columnWidth * 0.5F;
        const float gaugeY = rect.y + rect.h * (detailed ? 0.625F : 0.665F);
        fillCircle(renderer, gaugeX + gaugeRadius * 0.05F,
                   gaugeY + gaugeRadius * 0.075F, gaugeRadius * 1.08F,
                   {0, 0, 0, 190});
        fillCircle(renderer, gaugeX, gaugeY, gaugeRadius * 1.07F,
                   {16, 17, 18, 255});
        circle(renderer, gaugeX, gaugeY, gaugeRadius * 1.07F,
               {112, 116, 118, 160}, 2);
        circle(renderer, gaugeX, gaugeY, gaugeRadius * 1.015F,
               {50, 53, 55, 255}, 3);
        constexpr int faceRings = 18;
        for (int ring = faceRings; ring >= 1; --ring) {
            const float t = static_cast<float>(ring) / static_cast<float>(faceRings);
            const Uint8 shade = static_cast<Uint8>(17.0F + (1.0F - t) * 18.0F);
            fillCircle(renderer, gaugeX - gaugeRadius * (1.0F - t) * 0.08F,
                       gaugeY - gaugeRadius * (1.0F - t) * 0.12F,
                       gaugeRadius * 0.96F * t,
                       {shade, shade, static_cast<Uint8>(shade + 1), 255});
        }

        const float startAngle = pi + 0.22F;
        const float endAngle = pi * 2.0F - 0.22F;
        arc(renderer, gaugeX, gaugeY + gaugeRadius * 0.10F,
            gaugeRadius * 0.77F, startAngle, endAngle,
            {185, 189, 190, 115}, 1, 72);
        constexpr int ticks = 18;
        const float scaleCenterY = gaugeY + gaugeRadius * 0.10F;
        const float labelPixel = std::clamp(gaugeRadius / 72.0F, 0.58F, 2.15F);
        for (int tick = 0; tick <= ticks; ++tick) {
            const float t = static_cast<float>(tick) / static_cast<float>(ticks);
            const float angle = startAngle + (endAngle - startAngle) * t;
            const bool major = tick % 3 == 0;
            const float innerRadius = gaugeRadius * (major ? 0.67F : 0.705F);
            const float outerRadius = gaugeRadius * 0.79F;
            const SDL_Color tickColor = t < 0.62F ? SDL_Color{165, 237, 83, 255}
                                       : t < 0.82F ? SDL_Color{255, 162, 53, 255}
                                                   : SDL_Color{248, 61, 62, 255};
            line(renderer,
                 gaugeX + std::cos(angle) * innerRadius,
                 scaleCenterY + std::sin(angle) * innerRadius,
                 gaugeX + std::cos(angle) * outerRadius,
                 scaleCenterY + std::sin(angle) * outerRadius,
                 tickColor);
            if (major && (detailed || tick % 6 == 0)) {
                pixelText(renderer, std::to_string(tick * 10),
                          gaugeX + std::cos(angle) * gaugeRadius * 0.56F,
                          scaleCenterY + std::sin(angle) * gaugeRadius * 0.56F -
                              labelPixel * 3.5F,
                          labelPixel * 0.64F, {205, 211, 213, 205});
            }
        }

        const float pivotY = gaugeY + gaugeRadius * 0.16F;
        const float needleAngle = startAngle + (endAngle - startAngle) *
                                  std::clamp(meterLevels[channel], 0.0F, 1.0F);
        const float needleLength = gaugeRadius * 0.73F;
        line(renderer, gaugeX + 2.0F, pivotY + 3.0F,
             gaugeX + std::cos(needleAngle) * needleLength + 2.0F,
             pivotY + std::sin(needleAngle) * needleLength + 3.0F,
             {0, 0, 0, 180});
        glowLine(renderer, gaugeX, pivotY,
                 gaugeX + std::cos(needleAngle) * needleLength,
                 pivotY + std::sin(needleAngle) * needleLength,
                 {255, 48, 48, 255}, std::max(1.0F, gaugeRadius * 0.018F));
        fillCircle(renderer, gaugeX, pivotY, gaugeRadius * 0.060F,
                   {26, 27, 28, 255});
        fillCircle(renderer, gaugeX - gaugeRadius * 0.012F,
                   pivotY - gaugeRadius * 0.018F, gaugeRadius * 0.032F,
                   {150, 153, 153, 255});

        const float titlePixel = std::clamp(gaugeRadius / 45.0F, 0.75F, 3.2F);
        pixelText(renderer, "DB", gaugeX, gaugeY + gaugeRadius * 0.33F,
                  titlePixel, {230, 233, 234, 240});
        pixelText(renderer, channelNames[channel], gaugeX,
                  gaugeY + gaugeRadius * 0.58F,
                  titlePixel * (detailed ? 0.43F : 0.34F),
                  {190, 194, 196, 215});

        const float lampX = gaugeX - gaugeRadius * 0.60F;
        const float lampY = gaugeY + gaugeRadius * 0.34F;
        fillCircle(renderer, lampX, lampY, gaugeRadius * 0.085F,
                   {0, 0, 0, 220});
        fillCircle(renderer, lampX, lampY, gaugeRadius * 0.048F,
                   statusColors[channel]);
        circle(renderer, lampX, lampY, gaugeRadius * 0.055F,
               withAlpha(statusColors[channel], 120));
    }
}

void VisualizerRenderer::drawWarmTwinVu(SDL_Renderer* renderer,
                                        const SDL_FRect& rect) const {
    if (rect.w < 24.0F || rect.h < 24.0F) return;

    // Near-black studio background keeps the illuminated ivory faces dominant.
    constexpr int backgroundStrips = 18;
    for (int strip = 0; strip < backgroundStrips; ++strip) {
        const float t = static_cast<float>(strip) /
                        static_cast<float>(backgroundStrips - 1);
        fill(renderer,
             {rect.x, rect.y + rect.h * t, rect.w,
              rect.h / static_cast<float>(backgroundStrips) + 1.0F},
             mix({4, 4, 5, 255}, {13, 10, 8, 255},
                 1.0F - std::abs(t * 2.0F - 1.0F)));
    }

    const bool detailed = rect.w >= 760.0F && rect.h >= 300.0F;
    const float outerPad = std::max(4.0F, std::min(rect.w, rect.h) * 0.055F);
    const float panelWidth = rect.w - outerPad * 2.0F;
    const float panelHeight = detailed
        ? std::min(rect.h * 0.82F, panelWidth * 0.34F)
        : rect.h - outerPad * 1.35F;
    const SDL_FRect panel{rect.x + outerPad,
                          rect.y + (rect.h - panelHeight) * 0.5F,
                          panelWidth, panelHeight};

    fill(renderer, {panel.x + panel.h * 0.035F, panel.y + panel.h * 0.060F,
                    panel.w, panel.h}, {0, 0, 0, 175});
    fill(renderer, panel, {4, 5, 6, 255});
    fill(renderer, {panel.x + panel.h * 0.018F, panel.y + panel.h * 0.018F,
                    panel.w - panel.h * 0.036F, panel.h - panel.h * 0.036F},
         {29, 28, 26, 255});
    fill(renderer, {panel.x + panel.h * 0.032F, panel.y + panel.h * 0.036F,
                    panel.w - panel.h * 0.064F, panel.h - panel.h * 0.072F},
         {8, 9, 10, 255});
    fill(renderer, {panel.x, panel.y, panel.w, std::max(1.0F, panel.h * 0.012F)},
         {126, 127, 124, 105});
    fill(renderer, {panel.x, panel.y + panel.h - std::max(2.0F, panel.h * 0.020F),
                    panel.w, std::max(2.0F, panel.h * 0.020F)}, {0, 0, 0, 230});
    color(renderer, {99, 101, 100, 125});
    SDL_RenderRect(renderer, &panel);

    const float innerPad = panel.h * 0.060F;
    const float centerGap = panel.w * 0.012F;
    const float meterWidth = (panel.w - innerPad * 2.0F - centerGap) * 0.5F;
    const std::array<float, 2> values{vuLeft_, vuRight_};
    static constexpr std::array<float, 11> markerPositions{
        0.02F, 0.17F, 0.29F, 0.39F, 0.49F, 0.58F,
        0.66F, 0.73F, 0.80F, 0.88F, 0.97F
    };
    static constexpr std::array<std::string_view, 11> markerLabels{
        "20", "10", "7", "5", "3", "2", "1", "0", "1", "2", "3"
    };

    for (std::size_t meter = 0; meter < 2; ++meter) {
        const SDL_FRect bezel{panel.x + innerPad + static_cast<float>(meter) *
                                 (meterWidth + centerGap),
                              panel.y + innerPad,
                              meterWidth, panel.h - innerPad * 2.0F};
        fill(renderer, {bezel.x + 2.0F, bezel.y + 4.0F, bezel.w, bezel.h},
             {0, 0, 0, 180});
        fill(renderer, bezel, {58, 54, 47, 255});
        fill(renderer, {bezel.x + bezel.w * 0.012F, bezel.y + bezel.h * 0.018F,
                        bezel.w * 0.976F, bezel.h * 0.964F}, {15, 15, 15, 255});
        fill(renderer, {bezel.x + bezel.w * 0.028F, bezel.y + bezel.h * 0.042F,
                        bezel.w * 0.944F, bezel.h * 0.916F}, {82, 70, 53, 255});

        const SDL_FRect face{bezel.x + bezel.w * 0.050F,
                             bezel.y + bezel.h * 0.072F,
                             bezel.w * 0.900F,
                             bezel.h * 0.850F};
        constexpr int faceStrips = 28;
        for (int strip = 0; strip < faceStrips; ++strip) {
            const float t = static_cast<float>(strip) / static_cast<float>(faceStrips - 1);
            const float centerLight = 1.0F - std::abs(t * 2.0F - 1.0F);
            const SDL_Color warmFace = mix({211, 176, 112, 255},
                                           {255, 236, 181, 255},
                                           0.42F + centerLight * 0.52F);
            fill(renderer, {face.x, face.y + face.h * t, face.w,
                            face.h / static_cast<float>(faceStrips) + 1.0F},
                 warmFace);
        }
        fill(renderer, {face.x, face.y, face.w, face.h * 0.055F},
             {91, 67, 39, 42});
        fill(renderer, {face.x, face.y + face.h * 0.94F, face.w, face.h * 0.06F},
             {88, 55, 28, 48});
        fill(renderer, {face.x, face.y, face.w * 0.035F, face.h},
             {89, 60, 35, 34});
        fill(renderer, {face.x + face.w * 0.965F, face.y, face.w * 0.035F, face.h},
             {89, 60, 35, 34});

        const float centerX = face.x + face.w * 0.5F;
        const float pivotY = face.y + face.h * 0.865F;
        const float radius = std::min(face.w * 0.455F, face.h * 0.73F);
        constexpr float startAngle = pi * 1.12F;
        constexpr float endAngle = pi * 1.88F;
        const float textPixel = std::clamp(std::min(face.w / 300.0F, face.h / 155.0F),
                                           0.72F, 3.2F);
        const float titlePixel = std::clamp(std::min(face.w / 145.0F, face.h / 76.0F),
                                            0.85F, 5.0F);

        arc(renderer, centerX, pivotY, radius, startAngle, endAngle,
            {44, 34, 22, 220}, 2, 84);
        arc(renderer, centerX, pivotY, radius * 0.88F, startAngle, endAngle,
            {80, 55, 29, 125}, 1, 84);
        arc(renderer, centerX, pivotY, radius * 1.012F,
            startAngle + (endAngle - startAngle) * 0.73F, endAngle,
            {188, 44, 34, 220}, 3, 32);

        constexpr int ticks = 30;
        for (int tick = 0; tick <= ticks; ++tick) {
            const float t = static_cast<float>(tick) / static_cast<float>(ticks);
            const float angle = startAngle + (endAngle - startAngle) * t;
            const bool major = tick % 3 == 0;
            const float inner = radius * (major ? 0.86F : 0.91F);
            const SDL_Color tickColor = t >= 0.73F ? SDL_Color{177, 42, 32, 235}
                                                   : SDL_Color{40, 31, 22, 225};
            line(renderer, centerX + std::cos(angle) * inner,
                 pivotY + std::sin(angle) * inner,
                 centerX + std::cos(angle) * radius,
                 pivotY + std::sin(angle) * radius, tickColor);
        }

        for (std::size_t marker = 0; marker < markerPositions.size(); ++marker) {
            if (!detailed && marker % 2 == 1) continue;
            const float t = markerPositions[marker];
            const float angle = startAngle + (endAngle - startAngle) * t;
            const SDL_Color markerColor = t >= 0.73F ? SDL_Color{163, 35, 29, 245}
                                                     : SDL_Color{32, 27, 22, 245};
            pixelText(renderer, markerLabels[marker],
                      centerX + std::cos(angle) * radius * 0.74F,
                      pivotY + std::sin(angle) * radius * 0.74F - textPixel * 3.5F,
                      textPixel * 0.78F, markerColor);
        }

        pixelText(renderer, "VU", face.x + face.w * 0.13F,
                  face.y + face.h * 0.075F, titlePixel * 0.70F,
                  {35, 28, 21, 245});
        pixelText(renderer, meter == 0 ? "L" : "R", face.x + face.w * 0.87F,
                  face.y + face.h * 0.075F, titlePixel * 0.55F,
                  {121, 30, 26, 235});

        const float normalized = amplitudeToVu(values[meter]);
        const float needleAngle = startAngle + (endAngle - startAngle) * normalized;
        const float needleLength = radius * 0.93F;
        const float needleX = centerX + std::cos(needleAngle) * needleLength;
        const float needleY = pivotY + std::sin(needleAngle) * needleLength;
        line(renderer, centerX + 2.0F, pivotY + 3.0F,
             needleX + 2.0F, needleY + 3.0F, {74, 45, 26, 110});
        const float needleWidth = std::max(1.0F, radius * 0.009F);
        for (int offset = -1; offset <= 1; ++offset) {
            line(renderer, centerX + static_cast<float>(offset) * needleWidth,
                 pivotY, needleX + static_cast<float>(offset) * needleWidth * 0.30F,
                 needleY, {20, 18, 16, 255});
        }
        fillCircle(renderer, centerX, pivotY, std::max(3.0F, radius * 0.052F),
                   {29, 27, 23, 255});
        fillCircle(renderer, centerX - radius * 0.010F,
                   pivotY - radius * 0.014F, std::max(1.5F, radius * 0.022F),
                   {119, 108, 90, 255});
        pixelText(renderer, "NEON AUDIO", centerX,
                  face.y + face.h * 0.69F, textPixel * 0.66F,
                  {55, 43, 31, 215});

        fillQuad(renderer, {{{face.x + face.w * 0.03F, face.y + face.h * 0.025F},
                             {face.x + face.w * 0.30F, face.y + face.h * 0.025F},
                             {face.x + face.w * 0.48F, face.y + face.h * 0.73F},
                             {face.x + face.w * 0.25F, face.y + face.h * 0.73F}}},
                 {255, 255, 255, 18});
        color(renderer, {74, 58, 39, 165});
        SDL_RenderRect(renderer, &face);
        color(renderer, {121, 109, 89, 115});
        SDL_RenderRect(renderer, &bezel);
    }

    fill(renderer, {panel.x + panel.w * 0.5F - centerGap * 0.18F,
                    panel.y + innerPad * 0.45F,
                    centerGap * 0.36F, panel.h - innerPad * 0.90F},
         {0, 0, 0, 190});
}

}  // namespace neon
