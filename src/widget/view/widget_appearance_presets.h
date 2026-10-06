#pragma once

#include "theme/personalization.h"
#include "theme/edge_light_codec.h"
#include <array>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
// Host theme IDs are persisted independently of the resolved material snapshot.
inline PersonalizationSettings ResolveWidgetHostAppearancePreset(
    PersonalizationSettings saved, std::string_view selection,
    const std::unordered_map<std::string, std::string>* authored = nullptr)
{
    constexpr std::array<std::pair<std::string_view, int>, 7> builtIns{{
        {"__global_dark", kAppearancePresetDark},
        {"__global_light", kAppearancePresetLight},
        {"__global_glass_dark", kAppearancePresetGlassDark},
        {"__global_glass_light", kAppearancePresetGlassLight},
        {"__global_glass_transparent", kAppearancePresetGlassTransparent},
        {"__global_acrylic_dark", kAppearancePresetAcrylicDark},
        {"__global_acrylic_light", kAppearancePresetAcrylicLight},
    }};
    for (const auto& [id, preset] : builtIns)
        if (selection == id)
        {
            ApplyAppearancePreset(saved, preset);
            return saved;
        }
    if (selection.empty() || selection == "__custom" || !authored) return saved;

    // Only host material values follow an authored theme. Ordinary widget
    // settings and their persistent storage remain independent.
    saved.backgroundPreset = kAppearancePresetCustom;
    saved.glassEnabled = saved.acrylicEnabled = false;
    saved.edgeLight = {};
    saved.panelGradient.enabled = false;
    saved.widgetBorderWidth = 1.f;
    saved.widgetEdgeHighlightWidth = kDefaultEdgeHighlightWidth;
    saved.widgetEdgeHighlightStrength = kDefaultEdgeHighlightStrength;
    const auto number = [&](const char* key, float& target, float minimum, float maximum) {
        const auto found = authored->find(key);
        if (found == authored->end()) return;
        char* end = nullptr;
        const float value = std::strtof(found->second.c_str(), &end);
        if (end != found->second.c_str() && end && *end == '\0' && std::isfinite(value))
            target = std::clamp(value, minimum, maximum);
    };
    const auto color = [&](const char* key, float& red, float& green, float& blue) {
        const auto found = authored->find(key);
        if (found == authored->end()) return;
        char* end = nullptr;
        const long value = std::strtol(found->second.c_str(), &end, 0);
        if (end == found->second.c_str() || !end || *end != '\0' || value < 0 || value > 0xffffff) return;
        red = static_cast<float>((value >> 16) & 255) / 255.f;
        green = static_cast<float>((value >> 8) & 255) / 255.f;
        blue = static_cast<float>(value & 255) / 255.f;
    };
    const auto flag = [&](const char* key, bool& target) {
        if (const auto found = authored->find(key); found != authored->end())
            target = found->second == "1" || found->second == "true";
    };
    color("bg", saved.widgetBgR, saved.widgetBgG, saved.widgetBgB);
    color("border", saved.widgetBorderR, saved.widgetBorderG, saved.widgetBorderB);
    number("alpha", saved.widgetAlpha, 0.f, 1.f);
    number("borderAlpha", saved.widgetBorderAlpha, 0.f, 1.f);
    number("gradientEndA", saved.gradientEndA, 0.f, 1.f);
    number("borderWidth", saved.widgetBorderWidth, kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    number("edgeHighlightWidth", saved.widgetEdgeHighlightWidth, kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    number("edgeHighlightStrength", saved.widgetEdgeHighlightStrength, 0.f, 1.f);
    flag("glassEnabled", saved.glassEnabled);
    flag("acrylicEnabled", saved.acrylicEnabled);
    saved.widgetEdgeHighlightEnabled = saved.glassEnabled;
    if (authored->contains("borderStyle")) flag("borderStyle", saved.widgetEdgeHighlightEnabled);
    flag("edgeHighlightEnabled", saved.widgetEdgeHighlightEnabled);
    if (saved.glassEnabled && !authored->contains("borderAlpha")) saved.widgetBorderAlpha = 0.f;
    if (const auto found = authored->find("__contentTheme"); found != authored->end())
        if (found->second == "0" || found->second == "1") saved.contentTheme = found->second == "1" ? 1 : 0;
    if (const auto found = authored->find("__edgeLight"); found != authored->end())
    {
        JsonValue json;
        if (ParseJson(found->second, json)) (void)DecodeEdgeLight(json, saved.edgeLight);
    }
    return saved;
}
}
