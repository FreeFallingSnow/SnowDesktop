#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <vector>
#include "theme/edge_light_settings.h"
#include "theme/panel_gradient.h"

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
    DefaultBeautify = 1,
    Custom = 5,
    FrostedGlass = 6,
    FrostedGlassDark = 7,
    FrostedGlassLight = 8,
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
    // 0 smart; 1 add theme background and shrink; 2 add theme background without resizing (per icon only).
    int mode = 0;
    float backgroundOpacity = 0.65f;
    bool glassEnabled = false;
    float glassBlurRadius = 16.0f;
    bool edgeHighlightEnabled = false;
    float edgeHighlightWidth = 1.0f;
    float edgeHighlightStrength = 0.40f;
    EdgeLightSettings edgeLight;
    bool gradientEnabled = false;
    int gradientDirection = 0;
    float backgroundStartR = 232.0f / 255.0f;
    float backgroundStartG = 236.0f / 255.0f;
    float backgroundStartB = 244.0f / 255.0f;
    float backgroundEndR = 222.0f / 255.0f;
    float backgroundEndG = 228.0f / 255.0f;
    float backgroundEndB = 240.0f / 255.0f;
    // 0: custom, 1: original icon plate, 2: icon accent. Resolved per icon.
    int backgroundSource = 0;
    PanelGradient backgroundGradient{false, 90, 0, 1, {{0, 0xe8ecf4, 1}, {1, 0xdee4f0, 1}}};

    IconBeautifyShape shape = IconBeautifyShape::LegacyRounded;
    float contentScale = 0.68f;
    float textureHighlightStrength = 0.0f;
    float textureHighlightSize = 0.55f;
    float textureHighlightAngle = 0.0f;
    float textureShadeStrength = 0.0f;
    float textureEdgeHighlight = 0.0f;
    bool filterEnabled = false;
    // 0 retains the legacy multiply filter; 1 preserves luminosity; 2 maps luminosity to two tones.
    int filterMode = 0;
    float filterStrength = 1.0f;
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

// RGB remains useful for color pickers; automatic compositing also needs the
// spatial model. Derivation keeps existing explicit solid-color calls valid.
struct PlateFill : EdgeColor
{
    PlateFill() = default;
    PlateFill(EdgeColor color) : EdgeColor(color) {}
    bool gradient = false;
    // Change across one source canvas, measured from its center.
    std::array<float, 3> dx{}, dy{};
};

enum class BackplateKind { Foreground, Solid, Gradient, Artwork, Framed, Contour };
struct BackplateAnalysis
{
    BackplateKind kind = BackplateKind::Foreground;
    std::optional<PlateFill> fill;
};

// A complete silhouette alone does not authorize inventing background pixels.
BackplateAnalysis AnalyzeBackplate(const std::vector<std::uint32_t>& pixels,
    int width, int height);

struct ShapePoint
{
    float x = 0.0f;
    float y = 0.0f;
};

IconBeautifySettings Normalize(IconBeautifySettings settings);
IconBeautifySettings NormalizeGlobal(IconBeautifySettings settings);
PanelGradient BackgroundGradient(const IconBeautifySettings& settings);
bool Equal(const IconBeautifySettings& lhs, const IconBeautifySettings& rhs);
bool UsesLegacyGeometryDefaults(const IconBeautifySettings& settings);
IconBeautifySettings MakePreset(IconBeautifyPreset preset);
IconBeautifyPreset IdentifyPreset(const IconBeautifySettings& settings);
IconBeautifySettings ResolvePersistedSettings(IconBeautifySettings settings,
    bool hasExplicitPreset);
void ApplyLegacyFinish(IconBeautifySettings& settings,
    IconBeautifyFinish finish);

std::uint8_t ShapeMaskAlpha(IconBeautifyShape shape, int x, int y,
    int width, int height, float inset = 0.0f);

/** Normalized closed outline shared by vector UI plates and bitmap masks. */
const std::vector<ShapePoint>& ShapeOutline(IconBeautifyShape shape);

// Native controls carry intentional optical margins independently of the
// user-selected processing mode. This is render metadata, never persisted.
enum class SourceKind { Image, NativeControl };

// Beautification shapes use a square canvas. Center rectangular sources with
// transparent padding so the chosen outline never inherits a thumbnail's ratio.
bool PrepareCanvas(std::vector<std::uint32_t>& pixels, int& width, int& height, bool enabled);

std::vector<std::uint32_t> Render(
    const std::vector<std::uint32_t>& source,
    int width,
    int height,
    const IconBeautifySettings& settings,
    std::optional<PlateFill> detectedEdgeFill = std::nullopt,
    SourceKind sourceKind = SourceKind::Image);

/** Directional reflection without a fill or glyph, for vector control plates. */
std::vector<std::uint32_t> RenderEdgeReflection(int width, int height,
    const IconBeautifySettings& settings);

/** Shared host plate detector: follows visible contours through transparent margins. */
std::optional<PlateFill> DetectPlateFill(
    const std::vector<std::uint32_t>& pixels, int width, int height);

/** Detect a solid application-icon plate that smart beautification should clip. */
std::optional<PlateFill> DetectEdgeFill(
    const std::vector<std::uint32_t>& pixels, int width, int height);
}
}
