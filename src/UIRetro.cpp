#include "neon/UI.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "neon/Drawing.hpp"
#include "neon/Library.hpp"
#include "neon/Utils.hpp"

namespace neon {
namespace {

constexpr SDL_Color ink{38, 39, 32, 255};
constexpr SDL_Color faded{99, 99, 86, 255};
constexpr SDL_Color paper{247, 244, 224, 255};
constexpr SDL_Color red{158, 32, 24, 255};
constexpr SDL_Color green{170, 230, 113, 255};
constexpr SDL_Color dimGreen{106, 151, 86, 255};
constexpr SDL_FRect leftLeaf{42, 172, 660, 804};
constexpr SDL_FRect rightLeaf{702, 172, 678, 804};
constexpr float firstCardY = leftLeaf.y + 18;
constexpr float pairGap = 12;
constexpr float pairHeight = (leftLeaf.h - 38 - 4 * pairGap) / 5;
constexpr float cardHeight = pairHeight / 2;
constexpr float hingeX = 702;
constexpr std::uint64_t pageTurnMs = 680;

SDL_FRect videoCardRect(std::size_t slot) {
    return {slot == 0 ? 100.0F : 788.0F, 198, 516, 744};
}

void disc(SDL_Renderer* renderer, float x, float y, float radius, SDL_Color color) {
    drawing::disc(renderer, x, y, radius, color);
}

float cardTop(std::size_t row) {
    return firstCardY + static_cast<float>(row / 2) * (pairHeight + pairGap) +
           static_cast<float>(row % 2) * cardHeight;
}

void fill(SDL_Renderer* renderer, SDL_FRect rect, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

void line(SDL_Renderer* renderer, float x, float y, float endX, float endY, SDL_Color color) {
    drawing::line(renderer, x, y, endX, endY, color);
}

void gradient(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color top, SDL_Color bottom,
              bool horizontal = false) {
    const auto fc = [](SDL_Color color) {
        return SDL_FColor{color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F};
    };
    const std::array<SDL_Vertex, 4> vertices{{
        {{rect.x, rect.y}, fc(top), {}},
        {{rect.x + rect.w, rect.y}, fc(horizontal ? bottom : top), {}},
        {{rect.x + rect.w, rect.y + rect.h}, fc(bottom), {}},
        {{rect.x, rect.y + rect.h}, fc(horizontal ? top : bottom), {}}
    }};
    constexpr std::array indices{0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), 4, indices.data(), 6);
}

void screw(SDL_Renderer* renderer, float x, float y) {
    disc(renderer, x, y, 5, {94, 100, 95, 255});
    drawing::arc(renderer, x, y, 3.5F, 3.1415927F, 6.2831853F,
                 {216, 218, 208, 255}, 2.0F);
    line(renderer, x - 3, y + 2, x + 3, y - 2, {52, 57, 53, 255});
}

void catalogueHinge(SDL_Renderer* renderer) {
    const float top = leftLeaf.y;
    const float bottom = top + leftLeaf.h;
    constexpr float radius = 12;

    // Recessed channels join the page frames to the rolled metal spine.
    gradient(renderer, {hingeX - 18, top + 2, 7, leftLeaf.h - 4},
             {219, 221, 217, 255}, {69, 73, 71, 255}, true);
    gradient(renderer, {hingeX + 11, top + 2, 7, leftLeaf.h - 4},
             {75, 79, 77, 255}, {227, 229, 225, 255}, true);
    line(renderer, hingeX - 17, top + 3, hingeX - 17, bottom - 3, {248, 249, 245, 230});
    line(renderer, hingeX + 17, top + 3, hingeX + 17, bottom - 3, {250, 251, 247, 230});

    // Narrow highlights and broad grey reflections make a round chrome barrel.
    // Its rounded ends and curved knuckle seams follow the supplied reference.
    struct Reflection { float x; float light; };
    constexpr std::array<Reflection, 11> reflections{{
        {-1.0F, 70}, {-0.92F, 176}, {-0.76F, 246}, {-0.60F, 229},
        {-0.30F, 187}, {0.10F, 206}, {0.44F, 222}, {0.58F, 251},
        {0.72F, 230}, {0.86F, 176}, {1.0F, 79}
    }};
    constexpr int strips = 64;
    std::array<SDL_Vertex, (strips + 1) * 2> barrel{};
    std::array<int, strips * 6> indices{};
    for (int i = 0; i <= strips; ++i) {
        const float x = -1 + 2.0F * i / strips;
        std::size_t stop = 1;
        while (stop + 1 < reflections.size() && x > reflections[stop].x) ++stop;
        const auto& a = reflections[stop - 1];
        const auto& b = reflections[stop];
        const float t = (x - a.x) / (b.x - a.x);
        const float light = (a.light + (b.light - a.light) * t) / 255.0F;
        const SDL_FColor color{light, light, light * 0.99F, 1};
        const float cap = 4.0F * (1 - std::sqrt(std::max(0.0F, 1 - x * x)));
        barrel[i * 2] = {{hingeX + radius * x, top + cap}, color, {}};
        barrel[i * 2 + 1] = {{hingeX + radius * x, bottom - cap}, color, {}};
        if (i < strips) {
            const int v = i * 2;
            const std::array<int, 6> quad{v, v + 1, v + 2, v + 2, v + 1, v + 3};
            std::copy(quad.begin(), quad.end(), indices.begin() + i * 6);
        }
    }
    std::array<int, (strips + 1) * 2> boundary{};
    for (int i = 0; i <= strips; ++i) {
        boundary[i] = i * 2;
        boundary[strips + 1 + i] = (strips - i) * 2 + 1;
    }
    drawing::mesh(renderer, nullptr, barrel, indices, boundary);

    // Thin paired arcs wrap around the cylinder; they are seams, not crossbars.
    const auto arc = [&](float y, float thickness, SDL_FColor color, float bow) {
        std::array<SDL_FPoint, strips + 1> points{};
        for (int i = 0; i <= strips; ++i) {
            // Angular sampling keeps the steep ends of each seam smooth too.
            const float angle = 3.1415927F * i / strips;
            const float x = -std::cos(angle);
            points[i] = {hingeX + radius * x,
                y - bow * std::sin(angle) - 0.6F * x + thickness * 0.5F};
        }
        drawing::stroke(renderer, points, color, thickness);
    };
    arc(top + 4, 1.0F, {0.98F, 0.98F, 0.96F, 0.9F}, 3.2F);
    for (int joint = 0; joint < 9; ++joint) {
        const float y = top + leftLeaf.h * (71.0F + 73.0F * joint) / 685.0F;
        arc(y + 2.1F, 1.4F, {0.20F, 0.21F, 0.20F, 0.80F}, 4.5F);
        arc(y + 0.8F, 1.2F, {0.98F, 0.98F, 0.96F, 0.95F}, 4.5F);
        arc(y, 0.9F, {0.23F, 0.24F, 0.23F, 0.95F}, 4.5F);
        // Short turned lips nest into the side channels without spanning a page.
        line(renderer, hingeX - radius - 1, y + 1, hingeX - radius - 1, y + 7,
             {91, 94, 91, 225});
        line(renderer, hingeX + radius + 1, y, hingeX + radius + 1, y + 6,
             {249, 250, 246, 235});
    }
    arc(bottom - 4, 1.1F, {0.30F, 0.31F, 0.30F, 0.9F}, -2.8F);
}

void selectorOutline(SDL_Renderer* renderer, float x, float y, float outputScale) {
    constexpr std::array<SDL_FPoint, 5> corners{{{0, 0}, {25, 0}, {39, 20}, {25, 40}, {0, 40}}};
    // Closed, joined stroke geometry avoids the dropped subpixel segments of
    // SDL_RenderLines. Keep a visible core and a one-pixel alpha fringe at any size.
    const float pixel = 1.0F / std::max(0.25F, outputScale);
    const float core = 0.35F * pixel;
    const float feather = 0.70F * pixel;
    const std::array<float, 4> offsets{-core - feather, -core, core, core + feather};
    std::array<SDL_Vertex, corners.size() * offsets.size()> vertices{};
    std::array<int, corners.size() * 18> indices{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto& previous = corners[(i + corners.size() - 1) % corners.size()];
        const auto& point = corners[i];
        const auto& next = corners[(i + 1) % corners.size()];
        const auto normal = [](SDL_FPoint from, SDL_FPoint to) {
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float length = std::hypot(dx, dy);
            return SDL_FPoint{dy / length, -dx / length};
        };
        const auto a = normal(previous, point);
        const auto b = normal(point, next);
        const float denominator = 1 + a.x * b.x + a.y * b.y;
        const SDL_FPoint miter{(a.x + b.x) / denominator, (a.y + b.y) / denominator};
        for (std::size_t band = 0; band < offsets.size(); ++band) {
            const float alpha = band == 0 || band == offsets.size() - 1 ? 0.0F : 1.0F;
            vertices[i * 4 + band] = {
                {x + point.x + miter.x * offsets[band], y + point.y + miter.y * offsets[band]},
                {166 / 255.0F, 57 / 255.0F, 45 / 255.0F, alpha}, {}};
        }
        const int current = static_cast<int>(i * 4);
        const int following = static_cast<int>(((i + 1) % corners.size()) * 4);
        for (int band = 0; band < 3; ++band) {
            const std::array<int, 6> quad{current + band, current + band + 1, following + band,
                following + band, current + band + 1, following + band + 1};
            std::copy(quad.begin(), quad.end(), indices.begin() + i * 18 + band * 6);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

void copyLeaf(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect& leaf) {
    const SDL_FRect source{leaf.x * texture->w / 1920.0F, leaf.y * texture->h / 1080.0F,
                          leaf.w * texture->w / 1920.0F, leaf.h * texture->h / 1080.0F};
    SDL_RenderTexture(renderer, texture, &source, &leaf);
}

void turningLeaf(SDL_Renderer* renderer, SDL_Texture* texture, bool sourceRight,
                 int direction, float progress) {
    // The lifted corners project above the resting page frame and in front of
    // the artist index. Clip only at the outer cabinet, not the page opening.
    SDL_Rect previousClip{};
    const bool clipped = SDL_RenderClipEnabled(renderer);
    SDL_GetRenderClipRect(renderer, &previousClip);
    SDL_Rect cabinetClip{20, 20, 1384, 1040};
    if (clipped && !SDL_GetRectIntersection(&previousClip, &cabinetClip, &cabinetClip)) return;
    SDL_SetRenderClipRect(renderer, &cabinetClip);
    constexpr float pi = 3.14159265358979323846F;
    // A rigid leaf rotates about its spine. Subdivision keeps the printed labels
    // in perspective rather than stretching a single affine-textured quad.
    const float eased = progress * progress * (3.0F - 2.0F * progress);
    const float angle = eased * pi;
    const float cosine = std::cos(angle) * static_cast<float>(direction);
    const float lift = std::sin(angle);
    const auto& leaf = sourceRight ? rightLeaf : leftLeaf;
    constexpr int strips = 48;
    std::array<SDL_Vertex, (strips + 1) * 2> vertices{};
    std::array<int, strips * 6> indices{};
    const float centerY = leaf.y + leaf.h * 0.5F;
    const float shade = 1.0F - 0.43F * lift;
    for (int i = 0; i <= strips; ++i) {
        const float amount = static_cast<float>(i) / strips;
        const float radius = amount * leaf.w;
        // Positive depth lifts the metal leaf toward the viewer in both directions.
        constexpr float cameraDistance = 4800.0F;
        const float perspective = cameraDistance / (cameraDistance - radius * lift);
        const float x = hingeX + radius * cosine * perspective;
        const float halfHeight = leaf.h * 0.5F * perspective;
        const float u = (hingeX + (sourceRight ? radius : -radius)) / 1920.0F;
        const float light = shade + 0.10F * lift * (1.0F - amount);
        const SDL_FColor color{light, light, light * (1.0F - 0.035F * lift), 1};
        vertices[i * 2] = {{x, centerY - halfHeight}, color, {u, leaf.y / 1080.0F}};
        vertices[i * 2 + 1] = {{x, centerY + halfHeight}, color,
                              {u, (leaf.y + leaf.h) / 1080.0F}};
        if (i < strips) {
            const int a = i * 2;
            const std::array<int, 6> strip{a, a + 1, a + 2, a + 2, a + 1, a + 3};
            std::copy(strip.begin(), strip.end(), indices.begin() + i * 6);
        }
    }

    const auto outerTop = vertices[strips * 2].position;
    const auto outerBottom = vertices[strips * 2 + 1].position;
    const float shadowOffset = (cosine >= 0 ? 1.0F : -1.0F) * (12.0F + 34.0F * lift);
    const SDL_FColor shadow{0.035F, 0.04F, 0.03F, 0.30F * lift};
    const std::array<SDL_Vertex, 4> shadowVertices{{
        {{hingeX, leaf.y}, {0, 0, 0, 0}, {}},
        {{outerTop.x + shadowOffset, outerTop.y + 12 * lift}, shadow, {}},
        {{outerBottom.x + shadowOffset, outerBottom.y + 12 * lift}, shadow, {}},
        {{hingeX, leaf.y + leaf.h}, {0, 0, 0, 0}, {}}
    }};
    constexpr std::array shadowIndices{0, 1, 2, 0, 2, 3};
    constexpr std::array shadowBoundary{0, 1, 2, 3};
    drawing::mesh(renderer, nullptr, shadowVertices, shadowIndices, shadowBoundary);
    // Keep the fixed spine visible through the transparent gap beside the leaf.
    // Only the actual metal page, not its shadow or an opaque filler, covers it.
    catalogueHinge(renderer);
    std::array<int, (strips + 1) * 2> boundary{};
    for (int i = 0; i <= strips; ++i) {
        boundary[i] = i * 2;
        boundary[strips + 1 + i] = (strips - i) * 2 + 1;
    }
    drawing::mesh(renderer, texture, vertices, indices, boundary);

    // A narrow chrome edge stays visible even when the leaf is edge-on.
    const float edge = 1.0F + 2.0F * lift;
    fill(renderer, {outerTop.x - edge, outerTop.y, edge * 2,
                    outerBottom.y - outerTop.y}, {89, 98, 90, 255});
    line(renderer, outerTop.x, outerTop.y, outerBottom.x, outerBottom.y, {246, 247, 220, 255});
    line(renderer, hingeX, leaf.y, outerTop.x, outerTop.y, {237, 239, 220, 255});
    line(renderer, hingeX, leaf.y + leaf.h, outerBottom.x, outerBottom.y, {66, 77, 67, 255});
    SDL_SetRenderClipRect(renderer, clipped ? &previousClip : nullptr);
}

}  // namespace

bool UI::beginPageTurn(bool forward) {
    if (retroTurnDirection_) return false;
    if (theme_ == Theme::Retro && retroCatalogueReady_) {
        retroTurnDirection_ = forward ? 1 : -1;
        retroTurnPending_ = true;
        retroTurnSoundPlayed_ = false;
        retroTurnSoundPending_ = false;
    }
    return true;
}

bool UI::takePageTurnSound() { return std::exchange(retroTurnSoundPending_, false); }

void UI::resetRetroCatalogue() {
    for (auto& target : retroCatalogueTargets_) {
        if (target.texture) SDL_DestroyTexture(target.texture);
        target = {};
    }
    retroCatalogueReady_ = false;
    retroCatalogueLibrary_ = nullptr;
    retroTurnDirection_ = 0;
    retroTurnPending_ = false;
    retroTurnSoundPlayed_ = false;
    retroTurnSoundPending_ = false;
    retroInputBlockedUntilNs_ = 0;
}

void UI::drawRetroCatalogue(const UiModel& model, bool enabled, std::uint64_t ticks) {
    const auto drawStationary = [&](bool interactive) {
        drawRetroCards(model, interactive);
        catalogueHinge(renderer_);
    };
    const bool unobstructed = model.mode == UiMode::Browse && !model.keyboardOpen &&
        !model.genreMenuOpen && !model.playNowPrompt && !model.visualizerOpen;
    if (!unobstructed) { drawStationary(false); return; }

    const int width = static_cast<int>(std::ceil(1920 * outputScale_));
    const int height = static_cast<int>(std::ceil(1080 * outputScale_));
    if (retroCatalogueTargets_[0].texture &&
        (retroCatalogueTargets_[0].width != width || retroCatalogueTargets_[0].height != height))
        resetRetroCatalogue();
    for (auto& target : retroCatalogueTargets_) {
        if (target.texture) continue;
        target.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_TARGET, width, height);
        if (!target.texture) {
            resetRetroCatalogue();
            drawStationary(enabled);
            return;
        }
        target.width = width;
        target.height = height;
        SDL_SetTextureBlendMode(target.texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(target.texture, SDL_SCALEMODE_LINEAR);
    }

    const std::size_t count = model.filtered ? model.filtered->size() : 0;
    const auto capacity = themePageSize(Theme::Retro, model.libraryFilter == LibraryFilter::Video);
    const std::size_t trackCount = model.library ? model.library->tracks.size() : 0;
    const std::string filter = model.search + '\0' + model.selectedGenre + '\0' +
                              std::to_string(static_cast<int>(model.libraryFilter)) + '\0' +
                              model.selectedArtistInitial;
    const bool sameCollection = retroCatalogueReady_ && retroCatalogueLibrary_ == model.library &&
        count > 0 && trackCount > 0 &&
        retroCatalogueFilter_ == filter;
    const bool adjacent = retroTurnDirection_ > 0 ? model.page == retroCataloguePage_ + 1 :
        retroCataloguePage_ > 0 && model.page + 1 == retroCataloguePage_;
    // Static title strips and CD geometry are reused until their content changes.
    std::size_t signature = 0;
    const auto mix = [&](std::size_t value) {
        signature ^= value + 0x9e3779b9 + (signature << 6) + (signature >> 2);
    };
    const auto mixText = [&](std::string_view value) { mix(std::hash<std::string_view>{}(value)); };
    mix(count); mix(trackCount); mix(model.scanning);
    mix(static_cast<std::size_t>(model.nowPlayingArtworkMode));
    if (model.selectedTrack) mixText(model.selectedTrack->id);
    if (model.library && model.filtered) {
        const auto first = model.page * capacity;
        for (auto slot = first; slot < std::min(first + capacity, count); ++slot) {
            const auto index = (*model.filtered)[slot];
            if (index >= trackCount) continue;
            const auto& track = model.library->tracks[index];
            mix(model.libraryFilter == LibraryFilter::Video ? videoArtwork_.revision(track) : artwork_.revision(track));
            mix(index);
            mixText(track.id); mixText(track.title); mixText(track.artist);
            mixText(track.genre); mixText(track.album);
            mix(static_cast<std::size_t>(track.durationMs)); mix(track.albumYear);
            mix(track.favorite); mix(static_cast<std::size_t>(track.mediaKind));
            mix(static_cast<std::size_t>(track.modifiedTicks)); mix(track.fileSize);
            mix(track.hasEmbeddedArtwork);
            if (track.sidecarArtwork) mixText(pathToUtf8(*track.sidecarArtwork));
            if (track.onlineArtwork) mixText(pathToUtf8(*track.onlineArtwork));
        }
    }
    const bool redraw = !sameCollection || model.page != retroCataloguePage_ ||
                        signature != retroCatalogueSignature_ || retroTurnPending_;
    if (retroTurnPending_ && sameCollection && adjacent) {
        std::swap(retroCatalogueTargets_[0], retroCatalogueTargets_[1]);
        retroTurnElapsed_ = 0;
        retroTurnLastTick_ = ticks;
    } else if (!sameCollection || model.page != retroCataloguePage_ || retroTurnPending_) {
        retroTurnDirection_ = 0;
        retroTurnSoundPending_ = false;
    }
    retroTurnPending_ = false;
    if (retroTurnDirection_) {
        // Advance by displayed frames: a slow decode/OS stall cannot consume
        // the whole turn before the user has seen any of its intermediate poses.
        const auto elapsed = ticks >= retroTurnLastTick_ ? ticks - retroTurnLastTick_ : 0;
        retroTurnElapsed_ += std::min<std::uint64_t>(elapsed, 50);
        retroTurnLastTick_ = ticks;
        if (!retroTurnSoundPlayed_ && retroTurnElapsed_ >= pageTurnMs * 65 / 100) {
            retroTurnSoundPlayed_ = true;
            retroTurnSoundPending_ = true;
        }
        if (retroTurnElapsed_ >= pageTurnMs) {
            retroTurnDirection_ = 0;
            retroInputBlockedUntilNs_ = ticks * 1000000;
        }
    }

    // Freeze the previous spread, while the destination can still receive artwork.
    // SDL keeps logical size, viewport and clipping state separately per target.
    SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer_);
    auto* current = retroCatalogueTargets_[0].texture;
    if (redraw) {
        if (!SDL_SetRenderTarget(renderer_, current)) {
            resetRetroCatalogue();
            drawStationary(enabled);
            return;
        }
        SDL_SetRenderLogicalPresentation(renderer_, 1920, 1080, SDL_LOGICAL_PRESENTATION_STRETCH);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        // The gap between each page frame and its pivot must stay transparent:
        // filling it creates a flat grey strip over the hinge as soon as a turn starts.
        drawRetroCards(model, false);
        SDL_SetRenderTarget(renderer_, previousTarget);
    }

    if (!retroTurnDirection_) {
        const SDL_FRect full{0, 0, 1920, 1080};
        SDL_RenderTexture(renderer_, current, nullptr, &full);
        catalogueHinge(renderer_);
    } else {
        auto* previous = retroCatalogueTargets_[1].texture;
        const bool forward = retroTurnDirection_ > 0;
        copyLeaf(renderer_, forward ? previous : current, leftLeaf);
        copyLeaf(renderer_, forward ? current : previous, rightLeaf);
        // The turn draws the fixed hinge between the shadow and the actual leaf.
        const float progress = static_cast<float>(retroTurnElapsed_) / pageTurnMs;
        const bool front = progress < 0.5F;
        turningLeaf(renderer_, front ? previous : current, forward == front,
                    retroTurnDirection_, progress);
    }
    retroCatalogueReady_ = true;
    retroCatalogueLibrary_ = model.library;
    retroCatalogueCount_ = count;
    retroCatalogueTrackCount_ = trackCount;
    retroCatalogueFilter_ = filter;
    retroCataloguePage_ = model.page;
    retroCatalogueSignature_ = signature;
    if (enabled && !retroTurnDirection_ && model.library && model.filtered) {
        const auto first = model.page * capacity;
        for (std::size_t slot = 0; slot < capacity && first + slot < count; ++slot) {
            const auto index = (*model.filtered)[first + slot];
            if (index >= trackCount) continue;
            const auto hit = model.libraryFilter == LibraryFilter::Video ? videoCardRect(slot) :
                SDL_FRect{slot < 10 ? 56.0F : 734.0F, cardTop(slot % 10),
                          slot < 10 ? 616.0F : 632.0F, cardHeight};
            addHit(hit, {UiActionKind::SelectTrack, index});
        }
    }
}

void UI::chrome(const SDL_FRect& rect, bool screws) {
    fill(renderer_, {rect.x + 3, rect.y + 5, rect.w, rect.h}, {17, 24, 21, 255});
    gradient(renderer_, rect, {231, 234, 222, 255}, {139, 147, 141, 255});
    const SDL_FRect inset{rect.x + 6, rect.y + 6, rect.w - 12, rect.h - 12};
    gradient(renderer_, inset, {183, 190, 181, 255}, {205, 210, 198, 255});
    for (float y = inset.y + 2; y < inset.y + inset.h; y += 4)
        line(renderer_, inset.x, y, inset.x + inset.w, y, {240, 245, 228, 15});
    line(renderer_, rect.x + 1, rect.y + 1, rect.x + rect.w - 1, rect.y + 1, {249, 250, 239, 255});
    line(renderer_, rect.x + 1, rect.y, rect.x + 1, rect.y + rect.h, {238, 243, 228, 255});
    line(renderer_, rect.x + rect.w - 1, rect.y, rect.x + rect.w - 1, rect.y + rect.h, {83, 92, 85, 255});
    line(renderer_, rect.x, rect.y + rect.h - 1, rect.x + rect.w, rect.y + rect.h - 1, {82, 88, 82, 255});
    if (screws) {
        for (float x : {rect.x + 13, rect.x + rect.w - 13})
            for (float y : {rect.y + 13, rect.y + rect.h - 13}) screw(renderer_, x, y);
    }
}

void UI::phosphor(const SDL_FRect& rect) {
    fill(renderer_, rect, {67, 78, 57, 255});
    fill(renderer_, {rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4}, {7, 19, 12, 255});
    gradient(renderer_, {rect.x + 5, rect.y + 5, rect.w - 10, rect.h - 10},
             {16, 37, 22, 255}, {30, 60, 29, 255});
    for (float y = rect.y + 7; y < rect.y + rect.h - 5; y += 4)
        line(renderer_, rect.x + 5, y, rect.x + rect.w - 5, y, {0, 5, 0, 35});
    line(renderer_, rect.x, rect.y + rect.h, rect.x + rect.w, rect.y + rect.h, {235, 238, 216, 255});
}

void UI::retroButton(const SDL_FRect& rect, std::string_view label, UiAction action,
                     bool enabled, bool selected) {
    const auto sameAction = [&](const std::optional<UiAction>& other) {
        return other && other->kind == action.kind && other->index == action.index &&
            other->character == action.character;
    };
    const bool hovered = enabled && contains(rect, pointerX_, pointerY_);
    const bool pressed = enabled && ((hovered && pointerDown_ && sameAction(pressedAction_)) ||
                                     sameAction(feedbackAction_));
    const bool lit = enabled && (selected || hovered || pressed);
    fill(renderer_, rect, {60, 62, 50, 255});
    fill(renderer_, {rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4},
         lit ? SDL_Color{243, 205, 84, 255} : SDL_Color{230, 225, 201, 255});
    const SDL_FRect key{rect.x + 5, rect.y + 5, rect.w - 10, rect.h - 10};
    const SDL_Color top = !enabled ? SDL_Color{112, 79, 66, 255} : pressed
        ? SDL_Color{106, 22, 18, 255} : lit ? SDL_Color{230, 66, 41, 255} : SDL_Color{166, 39, 27, 255};
    const SDL_Color bottom = !enabled ? SDL_Color{63, 43, 37, 255} : pressed
        ? SDL_Color{190, 46, 28, 255} : SDL_Color{78, 14, 12, 255};
    gradient(renderer_, key, top, bottom);
    line(renderer_, key.x + 2, key.y + 2, key.x + key.w - 2, key.y + 2,
         pressed ? SDL_Color{57, 12, 10, 255} : SDL_Color{245, 131, 92, 160});
    line(renderer_, key.x + 2, key.y + key.h - 2, key.x + key.w - 2, key.y + key.h - 2,
         pressed ? SDL_Color{239, 124, 76, 230} : SDL_Color{51, 12, 10, 255});
    const SDL_Color labelColor = lit ? SDL_Color{255, 242, 179, 255} : enabled ? paper : SDL_Color{179, 155, 137, 255};
    const float labelHeight = std::min(28.0F, rect.h - 20.0F);
    text(label, rect.x + rect.w / 2, rect.y + (rect.h - labelHeight) / 2 - 1 + (pressed ? 2 : 0),
         rect.h >= 70 ? 25 : 22, labelColor,
         rect.w - (action.kind == UiActionKind::SelectArtistInitial ? 14 : 22), true, labelHeight);
    if (enabled) addHit(rect, action);
}

void UI::drawRetroStatus(const UiModel& model, std::uint64_t ticks) {
    const bool radio = model.currentTrack && model.currentTrack->mediaKind == MediaKind::Radio;
    const bool radioConnecting = !model.radioLoadingStatus.empty();
    constexpr std::uint64_t informationMs = 20000;
    constexpr std::uint64_t reminderMs = 5000;
    const std::string trackId = model.currentTrack ? model.currentTrack->id : "";
    if (!retroStatusCycleStart_ || trackId != retroStatusCycleTrack_ || ticks < retroStatusLastTick_) {
        retroStatusCycleStart_ = ticks;
        retroStatusCycleTrack_ = trackId;
    }
    const auto phase = (ticks - *retroStatusCycleStart_) % (informationMs + reminderMs);
    const bool coinPrompt = model.currentTrack && model.videoLoadingStatus.empty() &&
        !radioConnecting && phase >= informationMs;
    const bool waiting = !model.currentTrack;
    const std::string heading = radioConnecting ? "CONNECTING RADIO" : waiting || coinPrompt ? "PLEASE" :
        model.playback.state == PlaybackState::Paused ? "PAUSED" : "NOW PLAYING";
    const std::string message = coinPrompt ? "INSERT COIN" :
        model.currentTrack ? ellipsize(model.currentTrack->title, 31) :
        model.credits == 0 ? "INSERT COIN" : "SELECT A TRACK";
    if (retroStatusText_.empty() || coinPrompt != retroCoinPrompt_) {
        // The timed reminder switches cleanly; song changes retain their short slide.
        retroStatusText_ = message;
        retroPreviousStatusText_.clear();
        retroStatusElapsed_ = 180;
    } else if (retroStatusText_ != message) {
        retroPreviousStatusText_ = retroStatusText_;
        retroStatusText_ = message;
        retroStatusElapsed_ = 0;
        retroStatusLastTick_ = ticks;
    }
    retroCoinPrompt_ = coinPrompt;
    const auto delta = ticks >= retroStatusLastTick_ ? ticks - retroStatusLastTick_ : 0;
    retroStatusElapsed_ = std::min<std::uint64_t>(180, retroStatusElapsed_ + std::min<std::uint64_t>(delta, 50));
    retroStatusLastTick_ = ticks;
    phosphor({1440, 40, 436, 176});
    text(heading, 1460, 50, 15, dimGreen, 394, false, 19, true);
    SDL_Rect previousClip{};
    const bool clipped = SDL_RenderClipEnabled(renderer_);
    SDL_GetRenderClipRect(renderer_, &previousClip);
    SDL_Rect displayClip{1452, 72, 412, coinPrompt ? 91 : 39};
    if (clipped && !SDL_GetRectIntersection(&previousClip, &displayClip, &displayClip)) return;
    SDL_SetRenderClipRect(renderer_, &displayClip);
    const float progress = static_cast<float>(retroStatusElapsed_) / 180;
    const float eased = progress * progress * (3 - 2 * progress);
    if (progress < 1 && !retroPreviousStatusText_.empty()) {
        auto color = green;
        color.a = static_cast<Uint8>(255 * (1 - eased));
        text(retroPreviousStatusText_, 1658, 76 - 18 * eased, 26, color, 396, true, 30, true);
    }
    const bool visible = coinPrompt ? (phase - informationMs) % 1000 < 500
                                   : !waiting || progress < 1 || ticks % 1600 < 1050;
    if (visible) {
        auto color = green;
        color.a = static_cast<Uint8>(255 * eased);
        text(retroStatusText_, 1658, coinPrompt ? 98 : 76 + 18 * (1 - eased),
             coinPrompt ? 34 : 26, color, 396, true, coinPrompt ? 42.0F : 30.0F, true);
    }
    SDL_SetRenderClipRect(renderer_, clipped ? &previousClip : nullptr);

    if (model.currentTrack && !coinPrompt) {
        text(ellipsize(radio ? radioSubtitle(*model.currentTrack) : model.currentTrack->artist, 36),
             1658, 113, 21, green, 396, true, 24, true);
        std::string album = model.currentTrack->album.empty() ? "—" : model.currentTrack->album;
        album = ellipsize(album, 35);
        if (model.currentTrack->albumYear > 0) album += " · " + std::to_string(model.currentTrack->albumYear);
        if (!model.videoLoadingStatus.empty()) album = model.videoLoadingStatus;
        if (radio) album = "CANLI YAYIN · LIVE RADIO";
        if (radioConnecting) album = model.radioLoadingStatus;
        text(album, 1658, 144, 17, dimGreen, 396, true, 22, true);
    } else if (radioConnecting) {
        text(model.radioLoadingStatus, 1658, 144, 17, green, 396, true, 22, true);
    }
    const auto duration = std::max<std::int64_t>(0, model.playback.durationMs);
    const auto position = std::clamp<std::int64_t>(model.playback.positionMs, 0, duration);
    if (radio || radioConnecting) {
        text(radioConnecting ? "CONNECTING" : "CANLI · LIVE", 1456, 174, 15, green, 404, false, 18, true);
    } else {
        text(formatDuration(position), 1456, 174, 15, green, 90, false, 18, true);
        text(duration > 0 ? formatDuration(duration) : "--:--", 1810, 174, 15, dimGreen, 54, false, 18, true);
    }
    const float playbackProgress = !radio && !radioConnecting && model.currentTrack && duration > 0
        ? static_cast<float>(position) / static_cast<float>(duration) : 0;
    fill(renderer_, {1456, 198, 404, 8}, {7, 22, 11, 255});
    if (playbackProgress > 0) {
        const float width = 404 * playbackProgress;
        fill(renderer_, {1456, 197, width, 10}, {124, 224, 86, 45});
        gradient(renderer_, {1456, 198, width, 8}, {184, 246, 124, 255}, {70, 154, 54, 255});
        fill(renderer_, {1456, 198, width, 1}, {220, 255, 176, 230});
    }
}

void UI::drawRetroCards(const UiModel& model, bool enabled) {
    if (model.libraryFilter == LibraryFilter::Video) {
        drawRetroVideoCards(model, enabled);
        return;
    }
    const std::size_t count = model.filtered ? model.filtered->size() : 0;
    const auto capacity = themeDefinition(Theme::Retro).pageSize;
    const std::size_t pages = std::max<std::size_t>(1, (count + capacity - 1) / capacity);
    const std::size_t page = std::min(model.page, pages - 1);
    chrome({42, leftLeaf.y, 644, leftLeaf.h}, true);
    chrome({720, rightLeaf.y, 660, rightLeaf.h}, true);
    for (const bool right : {false, true}) {
        const float labelX = right ? 734.0F : 128.0F;
        const float labelWidth = right ? 560.0F : 544.0F;
        const float railX = right ? 1302.0F : 56.0F;
        gradient(renderer_, {railX, firstCardY, 64, leftLeaf.h - 38},
                 {189, 36, 25, 255}, {144, 22, 17, 255});
        for (int pair = 0; pair < 5; ++pair) {
            const float y = cardTop(static_cast<std::size_t>(pair) * 2);
            // Two printed title strips share one white insert in the grey metal leaf.
            fill(renderer_, {labelX + 2, y + 2, labelWidth, pairHeight}, {118, 126, 120, 255});
            fill(renderer_, {labelX, y, labelWidth, pairHeight}, {179, 184, 178, 255});
            fill(renderer_, {labelX + 2, y + 2, labelWidth - 4, pairHeight - 4}, {255, 255, 252, 255});
            line(renderer_, labelX + 4, y + 1, labelX + labelWidth - 4, y + 1, {247, 249, 243, 255});
            if (pair < 4) {
                const float jointY = y + pairHeight + pairGap * 0.5F;
                line(renderer_, railX, jointY, railX + 64, jointY, {251, 247, 233, 255});
                line(renderer_, railX, jointY + 1, railX + 64, jointY + 1, {251, 247, 233, 255});
                for (const float dotX : {labelX + 2, labelX + labelWidth * 0.5F, labelX + labelWidth - 2}) {
                    fill(renderer_, {dotX - 2, jointY - 2, 5, 5}, {216, 221, 214, 255});
                    fill(renderer_, {dotX - 1, jointY - 1, 3, 3}, {48, 55, 49, 255});
                }
            }
        }
    }
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        const bool right = slot >= 10;
        const std::size_t row = slot % 10;
        const float y = cardTop(row);
        const SDL_FRect card{right ? 734.0F : 56.0F, y, right ? 632.0F : 616.0F, cardHeight};
        const float codeX = right ? card.x + card.w - 64 : card.x;
        const float labelX = right ? 734.0F : 128.0F;
        const float labelWidth = right ? 560.0F : 544.0F;
        const float arrowX = labelX + 9;
        const float coverX = labelX + 54;
        const float contentX = coverX + 58;
        const float contentWidth = labelX + labelWidth - contentX - 12;
        const std::size_t item = page * capacity + slot;
        const Track* track = nullptr;
        std::size_t index = 0;
        if (model.library && item < count) {
            index = (*model.filtered)[item];
            if (index < model.library->tracks.size()) track = &model.library->tracks[index];
        }
        const bool selected = track && model.selectedTrack && model.selectedTrack->id == track->id;
        if (selected) {
            gradient(renderer_, {labelX + 2, y + 2, labelWidth - 4, cardHeight - 4},
                     {252, 224, 107, 255}, {226, 194, 76, 255});
            gradient(renderer_, {codeX, y, 64, cardHeight}, {219, 164, 45, 255}, {167, 110, 28, 255});
        }
        text(std::string(right ? "B" : "A") + std::to_string(row), codeX + 32, y + (cardHeight - 37) / 2,
             30, selected ? ink : paper, 56, true, 37);
        if (row % 2 == 0) {
            line(renderer_, labelX + 12, y + cardHeight, labelX + labelWidth - 12,
                 y + cardHeight, {157, 49, 41, 255});
            line(renderer_, labelX + 12, y + cardHeight + 1, labelX + labelWidth - 12,
                 y + cardHeight + 1, {197, 115, 104, 255});
        }
        if (track) {
            // The numbered arrow selects the same record as its title and artwork.
            const float arrowY = y + (cardHeight - 40) * 0.5F;
            selectorOutline(renderer_, arrowX, arrowY, outputScale_);
            text(std::to_string(slot + 1), arrowX + 16, arrowY + 7, 23, red, 27, true, 28);
            const SDL_FRect cardArtwork{coverX, y + (cardHeight - 48) / 2, 48, 48};
            drawCover(track, cardArtwork);
            const float labelTextY = y + (cardHeight - 54) / 2;
            const bool station = track->mediaKind == MediaKind::Radio;
            text(ellipsize(uppercaseForDisplay(station ? track->title : track->artist), 48), contentX + contentWidth / 2,
                 labelTextY, 26, ink, contentWidth, true, 28);
            text(ellipsize(uppercaseForDisplay(station ? "CANLI · " + radioSubtitle(*track) : track->title), 56), contentX + contentWidth / 2,
                 labelTextY + 28, station ? 21 : 24, station ? red : ink, contentWidth, true, 26, false, true);
            if (enabled) addHit(card, {UiActionKind::SelectTrack, index});
        } else {
            // Empty inserts retain their paper, divider and rail without fake selectors.
        }
    }
}

void UI::drawRetroVideoCards(const UiModel& model, bool enabled) {
    chrome({42, leftLeaf.y, 644, leftLeaf.h}, true);
    chrome({720, rightLeaf.y, 660, rightLeaf.h}, true);
    const auto count = model.filtered ? model.filtered->size() : 0;
    for (std::size_t slot = 0; slot < 2; ++slot) {
        const auto card = videoCardRect(slot);
        const float x = card.x, y = card.y;
        const auto item = model.page * 2 + slot;
        const auto index = model.filtered && item < count ? (*model.filtered)[item] : static_cast<std::size_t>(-1);
        const auto* track = model.library && index < model.library->tracks.size() ? &model.library->tracks[index] : nullptr;
        if (!track) {
            if (count) {
                fill(renderer_, {x + 16, y + 230, 484, 200}, {172, 180, 170, 255});
                text("END OF COLLECTION", x + 258, y + 305, 28, faded, 440, true);
            }
            continue;
        }
        const bool selected = model.selectedTrack && model.selectedTrack->id == track->id;
        const auto [labelArtist, labelTitle] = trackLabel(*track);
        // A printed VHS sleeve with the cassette partly exposed below it.
        fill(renderer_, {x + 9, y + 10, card.w, card.h}, {78, 84, 77, 255});
        fill(renderer_, card, selected ? SDL_Color{232, 190, 63, 255} : SDL_Color{40, 41, 37, 255});
        gradient(renderer_, {x + 4, y + 4, 508, 736}, {66, 67, 60, 255}, {15, 17, 17, 255});
        gradient(renderer_, {x + 6, y + 8, 23, 480}, {40, 42, 36, 255}, {106, 109, 97, 255}, true);
        fill(renderer_, {x + 30, y + 9, 476, 480}, {231, 223, 198, 255});
        for (float py = y + 13; py < y + 485; py += 5)
            line(renderer_, x + 33, py, x + 502, py, {255, 252, 225, 30});
        text(uppercaseForDisplay(labelArtist), x + 48, y + 22, 32, ink, 312, false, 38);
        text("VHS", x + 453, y + 22, 34, ink, 78, true, 38);
        const std::array<SDL_Color, 3> stripes{red, SDL_Color{206,102,40,255}, SDL_Color{199,161,67,255}};
        for (std::size_t band = 0; band < stripes.size(); ++band)
            fill(renderer_, {x + 30, y + 67 + static_cast<float>(band) * 6, 476, 6}, stripes[band]);

        const SDL_FRect picture{x + 48, y + 100, 440, 247.5F};
        fill(renderer_, {picture.x - 3, picture.y - 3, picture.w + 6, picture.h + 6}, {27, 29, 25, 255});
        if (auto* cover = videoArtwork_.get(renderer_, *track)) {
            float width{}, height{};
            if (SDL_GetTextureSize(cover, &width, &height) && width > 0 && height > 0) {
                const float fit = std::min(picture.w / width, picture.h / height);
                const SDL_FRect destination{picture.x + (picture.w - width * fit) / 2,
                    picture.y + (picture.h - height * fit) / 2, width * fit, height * fit};
                SDL_RenderTexture(renderer_, cover, nullptr, &destination);
            }
        }
        gradient(renderer_, {picture.x, picture.y + picture.h - 31, picture.w, 31},
                 {9,12,10,15}, {9,12,10,210});
        text("HI-FI STEREO", picture.x + 11, picture.y + picture.h - 24, 16, paper, 180);
        text(formatDuration(track->durationMs), picture.x + picture.w - 88,
             picture.y + picture.h - 25, 18, paper, 80);

        auto title = uppercaseForDisplay(labelTitle);
        std::string first = title, second;
        if (title.size() > 31) {
            auto split = title.rfind(' ', 31);
            if (split == std::string::npos || split < 10) split = title.find(' ', 31);
            if (split != std::string::npos) { first = title.substr(0, split); second = title.substr(split + 1); }
        }
        text(first, x + 268, y + (second.empty() ? 392 : 373), 33, ink, 442, true, 39);
        if (!second.empty()) text(ellipsize(second, 52), x + 268, y + 413, 29, ink, 442, true, 35);
        line(renderer_, x + 48, y + 477, x + 488, y + 477, {159,149,122,255});

        // Moulded black shell, horizontal ribs, transparent spool windows, paper label.
        const SDL_FRect cassette{x + 13, y + 492, 490, 238};
        gradient(renderer_, cassette, {42,44,43,255}, {14,16,16,255});
        line(renderer_, cassette.x + 3, cassette.y + 1, cassette.x + cassette.w - 3, cassette.y + 1, {99,102,96,255});
        for (float py = cassette.y + 11; py < cassette.y + 230; py += 6) {
            line(renderer_, cassette.x + 4, py, cassette.x + 26, py, {76,78,72,255});
            line(renderer_, cassette.x + 464, py, cassette.x + 486, py, {76,78,72,255});
        }
        text("VIDEO CASSETTE", x + 51, y + 502, 17, {196,184,131,255}, 240);
        text("SP", x + 453, y + 502, 17, {196,184,131,255}, 36);
        for (const float centerX : {x + 123, x + 393}) {
            fill(renderer_, {centerX - 67, y + 535, 134, 132}, {7,10,10,255});
            gradient(renderer_, {centerX - 64, y + 538, 128, 126}, {64,72,67,255}, {19,26,24,255});
            disc(renderer_, centerX, y + 601, 55, {10,12,11,255});
            for (int ring = 0; ring < 9; ++ring)
                disc(renderer_, centerX, y + 601, 53.0F - ring * 2.7F,
                     ring % 2 ? SDL_Color{30,29,24,255} : SDL_Color{20,21,17,255});
            disc(renderer_, centerX, y + 601, 27, {205,204,183,255});
            disc(renderer_, centerX, y + 601, 21, {159,165,151,255});
            for (int tooth = 0; tooth < 6; ++tooth) {
                const float angle = tooth * 6.2831853F / 6;
                disc(renderer_, centerX + std::cos(angle) * 15, y + 601 + std::sin(angle) * 15, 4, {53,58,51,255});
            }
            disc(renderer_, centerX, y + 601, 9, {15,19,16,255});
            line(renderer_, centerX - 60, y + 541, centerX + 58, y + 541, {174,185,167,120});
        }
        fill(renderer_, {x + 204, y + 539, 108, 125}, {214,207,179,255});
        text("VHS", x + 258, y + 548, 33, ink, 90, true, 40);
        line(renderer_, x + 212, y + 590, x + 304, y + 590, red);
        text(slot == 0 ? "A1" : "B2", x + 258, y + 600, 34, red, 90, true, 41);
        text("NO. " + std::to_string(item + 1), x + 258, y + 686, 17, {191,195,176,255}, 180, true);
        for (float sx : {x + 31, x + 485}) for (float sy : {y + 518, y + 712}) {
            disc(renderer_, sx, sy, 4, {91,95,86,255});
            line(renderer_, sx - 2, sy + 1, sx + 2, sy - 1, {7,9,8,255});
        }
        if (selected) {
            fill(renderer_, {x + 30, y + 9, 6, 480}, {255,215,85,255});
            fill(renderer_, {x + 13, y + 730, 490, 5}, {239,193,65,255});
        }
        if (enabled) addHit(card, {UiActionKind::SelectTrack, index});
    }
}

void UI::drawRetroBrowse(const UiModel& model, std::uint64_t ticks) {
    const bool browse = model.mode == UiMode::Browse;
    const bool unblocked = browse && !model.keyboardOpen && !model.playNowPrompt &&
                           !model.visualizerOpen && !model.genreMenuOpen;
    const bool enabled = unblocked && model.credits > 0;
    const std::size_t count = model.filtered ? model.filtered->size() : 0;
    const bool videos = model.libraryFilter == LibraryFilter::Video;
    const bool radio = model.libraryFilter == LibraryFilter::Radio;
    const bool currentRadio = model.currentTrack && model.currentTrack->mediaKind == MediaKind::Radio;
    const bool radioConnecting = !model.radioLoadingStatus.empty();
    const auto capacity = themePageSize(Theme::Retro, videos);
    const std::size_t pages = std::max<std::size_t>(1, (count + capacity - 1) / capacity);
    const std::size_t page = std::min(model.page, pages - 1);

    fill(renderer_, {0, 0, 1920, 1080}, {25, 34, 29, 255});
    chrome({20, 20, 1384, 1040}, true);
    chrome({1418, 20, 480, 1040}, true);

    retroButton({48, 40, 440, 62}, model.search.empty() ? (radio ? "SEARCH RADIO STATIONS" : "SEARCH TITLE / ARTIST / ALBUM") : ellipsize(model.search, 36),
                {UiActionKind::OpenKeyboard}, enabled);
    retroButton({504, 40, 384, 62}, model.selectedGenre.empty() ? "ALL GENRES  ▼" : ellipsize(model.selectedGenre, 26) + "  ▼",
                {UiActionKind::ToggleGenreMenu}, browse && model.credits > 0 &&
                !model.keyboardOpen && !model.playNowPrompt && !model.visualizerOpen,
                !model.selectedGenre.empty());
    retroButton({904, 40, 148, 62}, "MUSIC", {UiActionKind::ShowMusic}, enabled,
                model.libraryFilter == LibraryFilter::Music);
    retroButton({1064, 40, 148, 62}, "VIDEO", {UiActionKind::ShowVideo}, enabled,
                model.libraryFilter == LibraryFilter::Video);
    retroButton({1224, 40, 148, 62}, "RADYO", {UiActionKind::ShowRadio}, enabled, radio);

    // A separate artist index stays above both mechanical leaves.
    retroButton({48, 112, 56, 48}, "ALL", {UiActionKind::SelectArtistInitial}, enabled,
                model.selectedArtistInitial == '\0');
    retroButton({110, 112, 56, 48}, "0–9", {UiActionKind::SelectArtistInitial, 0, '#'}, enabled,
                model.selectedArtistInitial == '#');
    constexpr float letterPitch = 1206.0F / 26;
    for (int i = 0; i < 26; ++i) {
        const char initial = static_cast<char>('A' + i);
        retroButton({172 + i * letterPitch, 112, letterPitch - 6, 48}, std::string(1, initial),
                    {UiActionKind::SelectArtistInitial, 0, initial}, enabled,
                    model.selectedArtistInitial == initial);
    }

    drawRetroCatalogue(model, enabled, ticks);
    // Empty-state notices sit in front of the entire catalogue, including its hinge.
    if (count == 0) {
        const float noticeY = leftLeaf.y + (leftLeaf.h - 146) / 2;
        if (radio) drawRadioNotice(model, {254, noticeY, 920, 146});
        else {
            panel({254, noticeY, 920, 146}, paper, red);
            const bool building = model.buildingEmptyLibrary();
            text(building ? (videos ? "BUILDING YOUR VIDEO COLLECTION" : "BUILDING YOUR RECORD COLLECTION")
                 : (videos ? "NO MATCHING VIDEOS" : "NO MATCHING RECORDS"), 714, noticeY + 28, 36, ink, 830, true);
            text(building ? (videos ? "Your videos will appear here as they are found." : "Your music will appear here as it is found.")
                 : "Try ALL artists, a different search or another genre.", 714, noticeY + 84, 23, faded, 830, true);
        }
    }

    retroButton({48, 992, 240, 60}, "<  PREVIOUS", {UiActionKind::PagePrevious}, enabled && !retroTurnDirection_ && page > 0);
    text("PAGE " + std::to_string(page + 1) + " / " + std::to_string(pages) + "     •     " + std::to_string(count) + (radio ? " STATIONS" : videos ? " VIDEOS" : " RECORDS"),
         710, 988, 24, ink, 750, true, 29);
    const std::string instructions = radio
        ? (model.credits == 0 ? "INSERT COIN → TOUCH A STATION → DINLE" : "TOUCH A STATION → DINLE")
        : model.credits == 0 ? (videos ? "INSERT COIN → TOUCH A VIDEO → ADD TO QUEUE" : "INSERT COIN → TOUCH A RECORD → ADD TO QUEUE")
                            : (videos ? "TOUCH A VIDEO → ADD TO QUEUE" : "TOUCH A RECORD → ADD TO QUEUE");
    text(radio && model.radioFetching ? "Refreshing radio stations..." :
         radio && !model.radioStatus.empty() && count > 0 ? model.radioStatus : instructions,
         710, 1025, 18, red, 770, true, 23);
    retroButton({1132, 992, 240, 60}, "NEXT  >", {UiActionKind::PageNext}, enabled && !retroTurnDirection_ && page + 1 < pages);

    // Compact console: all displayed state comes from the live application model.
    const bool spinningDisc = !model.videoPlaying &&
        model.nowPlayingArtworkMode == NowPlayingArtworkMode::SpinningDisc;
    const auto media = nowPlayingMediaRect(model.theme, model.videoPlaying);
    if (!spinningDisc) chrome({media.x - 10, media.y - 10, media.w + 20, media.h + 20});
    if (model.videoPlaying) {
        fill(renderer_, media, {0, 0, 0, 255});
        if (unblocked) addHit(media, {UiActionKind::ToggleVideoFullscreen});
    } else if (spinningDisc) {
        drawSpinningDisc(model.currentTrack, media, false);
    } else {
        drawCover(model.currentTrack, media);
    }
    drawRetroStatus(model, ticks);

    phosphor({1440, 226, 128, 72});
    text("CREDITS", 1504, 234, 13, dimGreen, 100, true, 17, true);
    const std::string credits = (model.credits < 10 ? "0" : "") + std::to_string(model.credits);
    text(credits, 1504, 252, 39, green, 100, true, 42, true);
    phosphor({1582, 226, 294, 72});
    text(currentRadio || radioConnecting ? "RADYO" : "TIME LEFT", 1729, 234, 13, dimGreen, 264, true, 17, true);
    const auto remaining = radioConnecting ? "CONNECTING" : currentRadio ? "CANLI" : model.currentTrack && model.playback.durationMs > 0
        ? formatDuration(model.playback.durationMs - std::clamp<std::int64_t>(
            model.playback.positionMs, 0, model.playback.durationMs))
        : "--:--";
    text(remaining, 1729, 252, 39, green, 264, true, 42, true);

    const auto queued = model.queue ? model.queue->size() : 0;
    text("PLAYLIST  /  " + std::to_string(queued) + " REQUESTS", 1658, 812, 18, ink, 412, true, 23);
    phosphor({1440, 840, 436, 134});
    if (queued == 0) {
        text(radio || currentRadio ? "LIVE RADIO" : "THE NEXT RECORD IS YOURS", 1658, 868, 21, green, 388, true, 28, true);
        text(currentRadio ? "Live radio is playing" : radio ? "Select a station to listen live" : "Automatic shuffle is active",
             1658, 917, 15, dimGreen, 388, true, 21, true);
    } else {
        const auto shown = std::min<std::size_t>(3, queued);
        for (std::size_t i = 0; i < shown; ++i) {
            const auto* track = model.library ? LibraryScanner::find(*model.library, (*model.queue)[i].trackId) : nullptr;
            const float y = 848 + static_cast<float>(i) * 33;
            text(std::to_string(i + 1) + "  " + (track ? ellipsize(track->title, 29) : "Missing track"),
                 1456, y, 18, i == 0 ? green : dimGreen, 404, false, 21, true);
            if (track) text(ellipsize(track->artist, 32), 1490, y + 19, 12, dimGreen, 360, false, 14, true);
        }
        if (queued > shown) text("+ " + std::to_string(queued - shown) + " MORE IN QUEUE", 1658, 953, 12, dimGreen, 398, true, 14, true);
    }

    const SDL_FRect meter{1440, 666, 436, 134};
    drawVisualizer(meter, model.visualizerMode);
    if (unblocked) addHit(meter, {UiActionKind::OpenVisualizer});
    retroButton({1440, 984, 204, 66}, "INSERT COIN", {UiActionKind::InsertCoin}, unblocked);
    retroButton({1658, 984, 218, 66}, model.selectedTrack && model.selectedTrack->mediaKind == MediaKind::Radio ? "DINLE" : "ADD TO QUEUE",
                {UiActionKind::AddSelected}, enabled && model.selectedTrack, true);
    if (browse && model.adminReveal)
        retroButton({8, 8, 210, 76}, "ADMIN", {UiActionKind::OpenAdmin}, true);
}

}  // namespace neon
