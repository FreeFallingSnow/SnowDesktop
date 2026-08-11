#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace snowdesktop
{
enum class IconBeautifyShape : int
{
    LegacyRounded = 0,
    ContinuousRounded = 1,
    Circle = 2,
    SoftRounded = 3,
    Pebble = 10,
};

// Retained only to migrate layouts written by the former finish presets.
enum class IconBeautifyFinish : int
{
    Flat = 0,
    Gloss = 1,
    Glass = 2,
    Sticker = 3,
};

enum class IconBeautifyUpdateKind : int
{
    Preview = 0,
    Commit = 1,
};

enum class IconBeautifyPreset : int
{
    None = 0,
    ClassicRounded = 1,
    Custom = 5,
};

namespace icon_beautify
{
enum class InteractionAction : int
{
    None = 0,
    Preview,
    Commit,
};

struct ContinuousPreviewState
{
    std::uint32_t lastPreviewTick = 0;
};

InteractionAction AdvanceContinuousPreview(ContinuousPreviewState& state,
    bool changed, bool deactivatedAfterEdit, std::uint32_t now);
}

struct IconBeautifySettings
{
    bool enabled = false;
    IconBeautifyPreset preset = IconBeautifyPreset::None;
    int mode = 0;
    float backgroundOpacity = 0.65f;
    bool gradientEnabled = false;
    int gradientDirection = 0;
    float backgroundStartR = 232.0f / 255.0f;
    float backgroundStartG = 236.0f / 255.0f;
    float backgroundStartB = 244.0f / 255.0f;
    float backgroundEndR = 222.0f / 255.0f;
    float backgroundEndG = 228.0f / 255.0f;
    float backgroundEndB = 240.0f / 255.0f;

    IconBeautifyShape shape = IconBeautifyShape::LegacyRounded;
    float contentScale = 0.68f;
    float textureHighlightStrength = 0.0f;
    float textureHighlightSize = 0.55f;
    float textureHighlightAngle = 0.0f;
    float textureShadeStrength = 0.0f;
    float textureEdgeHighlight = 0.0f;
    bool filterEnabled = false;
    float filterHue = 0.0f;
    float filterSaturation = 1.0f;
    float filterBrightness = 0.0f;
    float filterContrast = 1.0f;
    float filterTintStrength = 0.25f;
    float filterTintR = 90.0f / 255.0f;
    float filterTintG = 140.0f / 255.0f;
    float filterTintB = 1.0f;
    bool outlineEnabled = false;
    float outlineWidth = 1.0f;
    float outlineOpacity = 1.0f;
    float outlineR = 190.0f / 255.0f;
    float outlineG = 199.0f / 255.0f;
    float outlineB = 214.0f / 255.0f;
    float shadowStrength = 0.35f;
};

namespace icon_beautify
{
struct EdgeColor
{
    int r = 0;
    int g = 0;
    int b = 0;
};

IconBeautifySettings Normalize(IconBeautifySettings settings);
bool Equal(const IconBeautifySettings& lhs, const IconBeautifySettings& rhs);
bool UsesLegacyGeometryDefaults(const IconBeautifySettings& settings);
IconBeautifySettings MakePreset(IconBeautifyPreset preset);
IconBeautifyPreset IdentifyPreset(const IconBeautifySettings& settings);
void ApplyLegacyFinish(IconBeautifySettings& settings,
    IconBeautifyFinish finish);

std::uint8_t ShapeMaskAlpha(IconBeautifyShape shape, int x, int y,
    int width, int height, float inset = 0.0f);

std::vector<std::uint32_t> Render(
    const std::vector<std::uint32_t>& source,
    int width,
    int height,
    const IconBeautifySettings& settings,
    std::optional<EdgeColor> detectedEdgeFill = std::nullopt);
}
}
