#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace neon::drawing {

// Feather in output pixels, including logical presentation and offscreen targets.
// A fixed logical fringe disappears when the cabinet is shown in a small window.
inline float pixelSize(SDL_Renderer* renderer) {
    float sx = 1, sy = 1;
    SDL_GetRenderScale(renderer, &sx, &sy);
    int width{}, height{};
    SDL_RendererLogicalPresentation mode{};
    SDL_GetRenderLogicalPresentation(renderer, &width, &height, &mode);
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED && width > 0 && height > 0) {
        SDL_FRect output{};
        SDL_GetRenderLogicalPresentationRect(renderer, &output);
        sx *= output.w / width;
        sy *= output.h / height;
    }
    return 1.0F / std::max(0.01F, std::min(std::abs(sx), std::abs(sy)));
}

inline SDL_FColor floatColor(SDL_Color color) {
    return {color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F};
}

inline SDL_FPoint normal(SDL_FPoint from, SDL_FPoint to) {
    const float dx = to.x - from.x, dy = to.y - from.y;
    const float length = std::hypot(dx, dy);
    return length > 0.00001F ? SDL_FPoint{-dy / length, dx / length} : SDL_FPoint{};
}

inline SDL_FPoint join(SDL_FPoint a, SDL_FPoint b) {
    const float denominator = std::max(0.25F, 1 + a.x * b.x + a.y * b.y);
    return {(a.x + b.x) / denominator, (a.y + b.y) / denominator};
}

inline void submit(SDL_Renderer* renderer, const std::vector<SDL_Vertex>& vertices,
                   const std::vector<int>& indices) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

inline void stroke(SDL_Renderer* renderer, std::span<const SDL_FPoint> points,
                   SDL_FColor color, float width = 0, bool closed = false) {
    if (points.size() < 2 || color.a <= 0) return;
    if (closed && std::hypot(points.front().x - points.back().x,
                            points.front().y - points.back().y) < 0.001F)
        points = points.first(points.size() - 1);
    if (points.size() < 2) return;
    const float pixel = pixelSize(renderer);
    if (width <= 0) width = pixel;
    const float halfCore = std::max(0.05F * pixel, (width - pixel) * 0.5F);
    const float outside = halfCore + pixel;
    color.a *= std::min(1.0F, width / (2 * halfCore + pixel));
    const std::array offsets{-outside, -halfCore, halfCore, outside};
    // Reuse storage: oscilloscope traces submit many paths every frame.
    thread_local std::vector<SDL_Vertex> vertices;
    thread_local std::vector<int> indices;
    vertices.clear(); indices.clear();
    vertices.reserve((points.size() + 2) * 4);
    indices.reserve((points.size() + 1) * 18);
    const auto row = [&](SDL_FPoint p, SDL_FPoint n, float coverage) {
        for (std::size_t band = 0; band < offsets.size(); ++band) {
            auto shade = color;
            shade.a *= band == 0 || band == 3 ? 0 : coverage;
            vertices.push_back({{p.x + n.x * offsets[band], p.y + n.y * offsets[band]}, shade, {}});
        }
    };
    if (!closed) {
        const auto n = normal(points[0], points[1]);
        row({points[0].x - n.y * pixel * 0.5F, points[0].y + n.x * pixel * 0.5F}, n, 0);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        SDL_FPoint n{};
        if (!closed && i == 0) n = normal(points[0], points[1]);
        else if (!closed && i + 1 == points.size()) n = normal(points[i - 1], points[i]);
        else n = join(normal(points[(i + points.size() - 1) % points.size()], points[i]),
                      normal(points[i], points[(i + 1) % points.size()]));
        row(points[i], n, 1);
    }
    if (!closed) {
        const auto n = normal(points[points.size() - 2], points.back());
        row({points.back().x + n.y * pixel * 0.5F, points.back().y - n.x * pixel * 0.5F}, n, 0);
    }
    const int rows = static_cast<int>(vertices.size() / 4);
    for (int i = 0; i < rows - (closed ? 0 : 1); ++i) {
        const int a = i * 4, b = ((i + 1) % rows) * 4;
        for (int band = 0; band < 3; ++band)
            indices.insert(indices.end(), {a + band, a + band + 1, b + band,
                b + band, a + band + 1, b + band + 1});
    }
    submit(renderer, vertices, indices);
}

inline void stroke(SDL_Renderer* renderer, std::span<const SDL_FPoint> points,
                   SDL_Color color, float width = 0, bool closed = false) {
    stroke(renderer, points, floatColor(color), width, closed);
}

inline void line(SDL_Renderer* renderer, float x, float y, float endX, float endY,
                 SDL_Color color, float width = 0) {
    const std::array points{SDL_FPoint{x, y}, SDL_FPoint{endX, endY}};
    stroke(renderer, points, color, width);
}

inline void arc(SDL_Renderer* renderer, float x, float y, float radius,
                float start, float end, SDL_Color color, float width = 0, int segments = 72) {
    const float sweep = end - start;
    const bool closed = std::abs(std::abs(sweep) - 6.2831853F) < 0.0001F;
    segments = std::max(segments, static_cast<int>(std::ceil(std::abs(sweep) *
        std::sqrt(std::max(1.0F, radius / pixelSize(renderer))))));
    thread_local std::vector<SDL_FPoint> points;
    points.clear();
    for (int i = 0; i < segments + (closed ? 0 : 1); ++i) {
        const float angle = start + sweep * i / segments;
        points.push_back({x + radius * std::cos(angle), y + radius * std::sin(angle)});
    }
    stroke(renderer, points, color, width, closed);
}

inline void disc(SDL_Renderer* renderer, float x, float y, float radius, SDL_Color color) {
    if (radius <= 0) return;
    const float halfPixel = pixelSize(renderer) * 0.5F;
    const float inner = std::max(0.0F, radius - halfPixel), outer = radius + halfPixel;
    const int segments = std::clamp(static_cast<int>(std::ceil(6.2831853F *
        std::sqrt(std::max(1.0F, radius / (halfPixel * 2))))), 24, 256);
    const auto solid = floatColor(color);
    auto transparent = solid; transparent.a = 0;
    thread_local std::vector<SDL_Vertex> vertices;
    thread_local std::vector<int> indices;
    vertices.clear(); indices.clear();
    vertices.push_back({{x, y}, solid, {}});
    for (int i = 0; i <= segments; ++i) {
        const float angle = 6.2831853F * i / segments;
        const float c = std::cos(angle), s = std::sin(angle);
        vertices.push_back({{x + inner * c, y + inner * s}, solid, {}});
        vertices.push_back({{x + outer * c, y + outer * s}, transparent, {}});
        if (i < segments) {
            const int a = 1 + i * 2;
            indices.insert(indices.end(), {0, a, a + 2, a, a + 1, a + 2, a + 1, a + 3, a + 2});
        }
    }
    submit(renderer, vertices, indices);
}

// Inward geometry keeps all four borders present under clipping and downscaling.
inline void outline(SDL_Renderer* renderer, SDL_FRect rect, SDL_Color color, float width = 0) {
    const float pixel = pixelSize(renderer);
    if (width <= 0) width = pixel;
    const float inset = width * 0.5F + pixel * 0.5F;
    if (rect.w <= inset * 2 || rect.h <= inset * 2) return;
    const std::array<SDL_FPoint, 4> points{{{rect.x + inset, rect.y + inset},
        {rect.x + rect.w - inset, rect.y + inset},
        {rect.x + rect.w - inset, rect.y + rect.h - inset},
        {rect.x + inset, rect.y + rect.h - inset}}};
    stroke(renderer, points, color, width, true);
}

inline void outline(SDL_Renderer* renderer, const SDL_FRect* rect) {
    SDL_Color color{};
    SDL_GetRenderDrawColor(renderer, &color.r, &color.g, &color.b, &color.a);
    outline(renderer, *rect, color);
}

inline void convex(SDL_Renderer* renderer, std::span<const SDL_FPoint> points, SDL_Color color) {
    if (points.size() < 3) return;
    const float feather = pixelSize(renderer) * 0.5F;
    float area = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size()];
        area += a.x * b.y - b.x * a.y;
    }
    const float inward = area > 0 ? 1.0F : -1.0F;
    const auto solid = floatColor(color);
    auto transparent = solid; transparent.a = 0;
    thread_local std::vector<SDL_Vertex> vertices;
    thread_local std::vector<int> indices;
    vertices.clear(); indices.clear();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto p = points[i];
        const auto n = join(normal(points[(i + points.size() - 1) % points.size()], p),
                            normal(p, points[(i + 1) % points.size()]));
        vertices.push_back({{p.x + n.x * feather * inward, p.y + n.y * feather * inward}, solid, {}});
        vertices.push_back({{p.x - n.x * feather * inward, p.y - n.y * feather * inward}, transparent, {}});
        const int a = static_cast<int>(i * 2), b = static_cast<int>(((i + 1) % points.size()) * 2);
        indices.insert(indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        if (i > 0 && i + 1 < points.size()) indices.insert(indices.end(), {0, a, a + 2});
    }
    submit(renderer, vertices, indices);
}

// Preserve the mesh's gradients/UVs, adding coverage only at its outside boundary.
inline void mesh(SDL_Renderer* renderer, SDL_Texture* texture,
                 std::span<const SDL_Vertex> source, std::span<const int> triangles,
                 std::span<const int> boundary) {
    if (boundary.size() < 3) return;
    thread_local std::vector<SDL_Vertex> vertices;
    thread_local std::vector<int> indices;
    vertices.assign(source.begin(), source.end());
    indices.assign(triangles.begin(), triangles.end());
    float area = 0;
    for (std::size_t i = 0; i < boundary.size(); ++i) {
        const auto a = source[boundary[i]].position;
        const auto b = source[boundary[(i + 1) % boundary.size()]].position;
        area += a.x * b.y - b.x * a.y;
    }
    const float inset = pixelSize(renderer) * (area > 0 ? 0.5F : -0.5F);
    const int outside = static_cast<int>(source.size());
    for (std::size_t i = 0; i < boundary.size(); ++i) {
        const auto p = source[boundary[i]].position;
        const auto n = join(normal(source[boundary[(i + boundary.size() - 1) % boundary.size()]].position, p),
            normal(p, source[boundary[(i + 1) % boundary.size()]].position));
        vertices[boundary[i]].position = {p.x + n.x * inset, p.y + n.y * inset};
        auto edge = source[boundary[i]];
        edge.position = {p.x - n.x * inset, p.y - n.y * inset};
        edge.color.a = 0;
        vertices.push_back(edge);
        const int next = static_cast<int>((i + 1) % boundary.size());
        const int a = boundary[i], b = boundary[next], c = outside + static_cast<int>(i), d = outside + next;
        indices.insert(indices.end(), {a, c, b, b, c, d});
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_BlendMode previous = SDL_BLENDMODE_NONE;
    if (texture) {
        SDL_GetTextureBlendMode(texture, &previous);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    SDL_RenderGeometry(renderer, texture, vertices.data(), static_cast<int>(vertices.size()),
                       indices.data(), static_cast<int>(indices.size()));
    if (texture) SDL_SetTextureBlendMode(texture, previous);
}

} // namespace neon::drawing
