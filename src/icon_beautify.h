#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace snowdesktop
{
enum class IconBeautifyShape : int
{
    LegacyRounded = 0,
    Apple = 1,
    Circle = 2,
    Samsung = 3,
    RoundedSquare = 4,
    Teardrop = 5,
    Bookmark = 6,
    Lemon = 7,
    Diamond = 8,
    Flower = 9,
    Pebble = 10,
};

enum class IconBeautifyFinish : int
{
    Flat = 0,
    Gloss = 1,
    Glass = 2,
    Sticker = 3,
};

enum class IconBeautifyOutlineMode : int
{
    None = 0,
    Automatic = 1,
    Custom = 2,
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
    AppleGlass = 2,
    CircleSticker = 3,
    PebbleGloss = 4,
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
    IconBeautifyFinish finish = IconBeautifyFinish::Flat;
    IconBeautifyOutlineMode outlineMode = IconBeautifyOutlineMode::Automatic;
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
