#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

namespace neon {

enum class Theme { Neon, Retro };

struct ThemeDefinition {
    Theme theme;
    std::string_view id;
    std::string_view label;
    std::size_t pageSize;
};

inline constexpr std::array themeDefinitions{
    ThemeDefinition{Theme::Neon, "neon", "MODERN", 9},
    ThemeDefinition{Theme::Retro, "retro", "RETRO", 20}
};

constexpr const ThemeDefinition& themeDefinition(Theme theme) {
    for (const auto& definition : themeDefinitions)
        if (definition.theme == theme) return definition;
    return themeDefinitions.front();
}

constexpr Theme themeFromId(std::string_view id) {
    for (const auto& definition : themeDefinitions)
        if (definition.id == id) return definition.theme;
    return Theme::Neon;
}

constexpr std::size_t themePageSize(Theme theme, bool videoOnly = false) {
    return theme == Theme::Retro && videoOnly ? 2 : themeDefinition(theme).pageSize;
}

constexpr std::size_t pageAfterThemeChange(Theme previous, Theme next, std::size_t page,
                                         std::size_t count,
                                         std::optional<std::size_t> selectedOffset = {}, bool videoOnly = false) {
    if (count == 0) return 0;
    const auto previousSize = themePageSize(previous, videoOnly);
    const auto first = std::min(page, (count - 1) / previousSize) * previousSize;
    const auto anchor = selectedOffset && *selectedOffset >= first &&
        *selectedOffset - first < previousSize && *selectedOffset < count
        ? *selectedOffset : first;
    return anchor / themePageSize(next, videoOnly);
}

}  // namespace neon
