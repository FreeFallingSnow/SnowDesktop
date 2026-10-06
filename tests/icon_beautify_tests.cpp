#include "icon_beautify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

namespace beautify = snowdesktop::icon_beautify;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

std::uint32_t Premultiplied(int r, int g, int b, int a)
{
    return (static_cast<std::uint32_t>(a) << 24) |
        (static_cast<std::uint32_t>((r * a + 127) / 255) << 16) |
        (static_cast<std::uint32_t>((g * a + 127) / 255) << 8) |
        static_cast<std::uint32_t>((b * a + 127) / 255);
}

std::uint64_t HashPixels(const std::vector<std::uint32_t>& pixels)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint32_t pixel : pixels)
    {
        hash ^= pixel;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::uint32_t> TestIcon(int size)
{
    std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * size, 0);
    const int center = size / 2;
    for (int y = size / 5; y < size - size / 5; ++y)
        for (int x = size / 5; x < size - size / 5; ++x)
            if ((x >= center - size / 10 && x <= center + size / 10) ||
                (y >= center - size / 10 && y <= center + size / 10))
                pixels[static_cast<size_t>(y) * size + x] =
                    Premultiplied(232, 72, 62, 255);
    return pixels;
}

std::vector<std::uint32_t> TwoColorIcon(int size)
{
    std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * size, 0);
    for (int y = size / 4; y < size - size / 4; ++y)
        for (int x = size / 4; x < size - size / 4; ++x)
            pixels[static_cast<size_t>(y) * size + x] = x < size / 2
                ? Premultiplied(255, 0, 0, 255)
                : Premultiplied(0, 130, 0, 255);
    return pixels;
}

int CountPartiallyCovered(snowdesktop::IconBeautifyShape shape)
{
    int count = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const int alpha = beautify::ShapeMaskAlpha(shape, x, y, 64, 64);
            if (alpha > 0 && alpha < 255) ++count;
        }
    return count;
}
}

int main()
{
    using snowdesktop::IconBeautifyFinish;
    using snowdesktop::IconBeautifyPreset;
    using snowdesktop::IconBeautifySettings;
    using snowdesktop::IconBeautifyShape;
    using snowdesktop::IconBeautifyUpdateKind;

    Check(static_cast<int>(IconBeautifyUpdateKind::Preview) == 0 &&
        static_cast<int>(IconBeautifyUpdateKind::Commit) == 1,
        "preview and commit update kinds have stable values");
    Check(static_cast<int>(IconBeautifyShape::LegacyRounded) == 0 &&
        static_cast<int>(IconBeautifyShape::ContinuousRounded) == 1 &&
        static_cast<int>(IconBeautifyShape::SoftRounded) == 3 &&
        static_cast<int>(IconBeautifyShape::Pebble) == 10 &&
        static_cast<int>(IconBeautifyFinish::Flat) == 0 &&
        static_cast<int>(IconBeautifyFinish::Sticker) == 3 &&
        static_cast<int>(IconBeautifyPreset::None) == 0 &&
        static_cast<int>(IconBeautifyPreset::DefaultBeautify) == 1 &&
        static_cast<int>(IconBeautifyPreset::Custom) == 5 &&
        static_cast<int>(IconBeautifyPreset::FrostedGlass) == 6 &&
        static_cast<int>(IconBeautifyPreset::FrostedGlassDark) == 7 &&
        static_cast<int>(IconBeautifyPreset::FrostedGlassLight) == 8,
        "persisted beautification enums have stable values");

    beautify::ContinuousPreviewState continuousState;
    Check(beautify::AdvanceContinuousPreview(
            continuousState, true, false, 1000) ==
            beautify::InteractionAction::Preview &&
        beautify::AdvanceContinuousPreview(
            continuousState, true, false, 1099) ==
            beautify::InteractionAction::None &&
        beautify::AdvanceContinuousPreview(
            continuousState, true, false, 1100) ==
            beautify::InteractionAction::Preview &&
        beautify::AdvanceContinuousPreview(
            continuousState, false, true, 1101) ==
            beautify::InteractionAction::Commit,
        "continuous controls throttle previews and commit only on release");

    constexpr std::array<IconBeautifyPreset, 6> builtInPresets{
        IconBeautifyPreset::None,
        IconBeautifyPreset::DefaultBeautify,
        IconBeautifyPreset::Custom,
        IconBeautifyPreset::FrostedGlass,
        IconBeautifyPreset::FrostedGlassDark,
        IconBeautifyPreset::FrostedGlassLight,
    };
    for (IconBeautifyPreset preset : builtInPresets)
    {
        const auto presetSettings = beautify::MakePreset(preset);
        Check(beautify::IdentifyPreset(presetSettings) == preset,
            "built-in icon beautify presets round-trip through identification");
    }
    // The loader must refresh explicit recipes, but preserve custom/legacy
    // material and geometry values. This protects updates, not visual taste.
    for (const auto preset : builtInPresets)
    {
        auto previous = beautify::MakePreset(preset);
        previous.edgeLight.direction = 127.f;
        previous.edgeLight.spread = 117.f;
        previous.backgroundOpacity = .47f;
        previous.mode = 1;
        previous.contentScale = .79f;
        const auto loaded = beautify::ResolvePersistedSettings(previous, true);
        Check(beautify::Equal(loaded, preset == IconBeautifyPreset::Custom
                ? previous : beautify::MakePreset(preset)),
            "explicit built-in icon presets refresh while custom parameters survive loading");
        if (preset != IconBeautifyPreset::None && preset != IconBeautifyPreset::Custom)
        {
            previous.enabled = false;
            auto expected = beautify::MakePreset(preset); expected.enabled = false;
            Check(beautify::Equal(beautify::ResolvePersistedSettings(previous, true), expected),
                "refreshing an icon recipe does not re-enable disabled beautification");
        }
    }
    {
        auto legacy = beautify::MakePreset(IconBeautifyPreset::FrostedGlass);
        legacy.mode = 1; legacy.contentScale = .79f;
        auto expected = legacy; expected.preset = IconBeautifyPreset::Custom;
        Check(beautify::Equal(beautify::ResolvePersistedSettings(legacy, false), expected),
            "layouts without an explicit icon preset preserve saved scaling and appearance");
        legacy.preset = static_cast<IconBeautifyPreset>(2);
        Check(beautify::Equal(beautify::ResolvePersistedSettings(legacy, true), expected),
            "retired icon preset identities retain their material as custom");
    }
    {
        auto plate = beautify::MakePreset(IconBeautifyPreset::FrostedGlass);
        const auto baseline = beautify::RenderEdgeReflection(104, 104, plate);
        auto plain = plate; plain.glassEnabled = false; plain.preset = IconBeautifyPreset::Custom;
        Check(beautify::RenderEdgeReflection(104, 104, plain) == baseline,
            "edge pixels depend on material parameters rather than glass or preset identity");
        snowdesktop::VisitEdgeLightFields([&](auto name, auto field, float minimum, float maximum) {
            auto changed = plate;
            changed.edgeLight.*field = plate.edgeLight.*field < (minimum + maximum) * .5f ? maximum : minimum;
            const bool changedPixels = beautify::RenderEdgeReflection(104, 104, changed) != baseline;
            if (!changedPixels) std::cerr << "Unresponsive edge parameter: " << name << '\n';
            Check(changedPixels,
                "each shared material parameter changes actual icon-edge pixels");
            Check(beautify::IdentifyPreset(changed) == IconBeautifyPreset::Custom,
                "editing a reflection parameter turns a preset into custom values");
        });
    }
    auto customPreset = beautify::MakePreset(IconBeautifyPreset::DefaultBeautify);
    customPreset.contentScale = 0.71f;
    Check(beautify::IdentifyPreset(customPreset) == IconBeautifyPreset::Custom,
        "edited preset settings are identified as custom");

    constexpr std::array<IconBeautifyShape, 5> shapes{
        IconBeautifyShape::LegacyRounded,
        IconBeautifyShape::ContinuousRounded,
        IconBeautifyShape::Circle,
        IconBeautifyShape::SoftRounded,
        IconBeautifyShape::Pebble,
    };

    std::set<std::uint64_t> maskHashes;
    std::set<std::uint64_t> outlineHashes;
    for (IconBeautifyShape shape : shapes)
    {
        std::vector<std::uint32_t> mask;
        mask.reserve(64 * 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                mask.push_back(beautify::ShapeMaskAlpha(shape, x, y, 64, 64));
        maskHashes.insert(HashPixels(mask));
        Check(beautify::ShapeMaskAlpha(shape, 32, 32, 64, 64) == 255,
            "every shape must contain its center");
        Check(CountPartiallyCovered(shape) > 0,
            "every shape must expose anti-aliased boundary coverage");

        const auto& outline = beautify::ShapeOutline(shape);
        std::vector<std::uint32_t> outlineCoordinates;
        outlineCoordinates.reserve(outline.size());
        bool normalizedOutline = !outline.empty();
        for (const auto& point : outline)
        {
            normalizedOutline = normalizedOutline &&
                point.x >= 0.0f && point.x <= 1.0f &&
                point.y >= 0.0f && point.y <= 1.0f;
            const auto x = static_cast<std::uint32_t>(
                std::round(point.x * 65535.0f));
            const auto y = static_cast<std::uint32_t>(
                std::round(point.y * 65535.0f));
            outlineCoordinates.push_back((x << 16) | y);
        }
        Check(normalizedOutline,
            "every vector plate outline stays in normalized bounds");
        outlineHashes.insert(HashPixels(outlineCoordinates));
    }
    Check(maskHashes.size() == shapes.size(),
        "all five exposed shape masks must be geometrically distinct");
    Check(outlineHashes.size() == shapes.size(),
        "all five vector plate outlines must be geometrically distinct");
    Check(beautify::ShapeMaskAlpha(IconBeautifyShape::Circle, 0, 0, 64, 64) == 0,
        "circle excludes square corners");

    IconBeautifySettings defaults;
    Check(defaults.preset == IconBeautifyPreset::None &&
        defaults.shape == IconBeautifyShape::LegacyRounded,
        "legacy rounded is the compatibility shape default");
    Check(defaults.contentScale == 0.68f &&
        defaults.textureHighlightStrength == 0.0f &&
        defaults.textureShadeStrength == 0.0f &&
        defaults.textureEdgeHighlight == 0.0f && !defaults.filterEnabled &&
        defaults.filterStrength == 1.0f &&
        !defaults.outlineEnabled &&
        defaults.outlineWidth == 1.0f && defaults.shadowStrength == 0.35f,
        "new settings retain the locked compatibility defaults");
    Check(beautify::UsesLegacyGeometryDefaults(defaults),
        "locked defaults select the legacy pixel path");

    IconBeautifySettings invalid = defaults;
    invalid.mode = 9;
    invalid.preset = static_cast<IconBeautifyPreset>(99);
    invalid.contentScale = -2.0f;
    invalid.outlineWidth = 99.0f;
    invalid.outlineOpacity = -1.0f;
    invalid.shadowStrength = 2.0f;
    invalid.shape = static_cast<IconBeautifyShape>(99);
    invalid.textureHighlightStrength = 2.0f;
    invalid.textureHighlightSize = 0.0f;
    invalid.textureHighlightAngle = 2.0f;
    invalid.textureShadeStrength = -1.0f;
    invalid.textureEdgeHighlight = 2.0f;
    invalid.filterStrength = 2.0f;
    const IconBeautifySettings normalized = beautify::Normalize(invalid);
    Check(normalized.preset == IconBeautifyPreset::Custom &&
        normalized.mode == 1 && normalized.contentScale == 0.50f &&
        normalized.outlineWidth == 4.0f && normalized.outlineOpacity == 0.0f &&
        normalized.shadowStrength == 1.0f &&
        normalized.shape == IconBeautifyShape::LegacyRounded &&
        normalized.textureHighlightStrength == 1.0f &&
        normalized.textureHighlightSize == 0.10f &&
        normalized.textureHighlightAngle == 1.0f &&
        normalized.textureShadeStrength == 0.0f &&
        normalized.textureEdgeHighlight == 1.0f &&
        normalized.filterStrength == 1.0f,
        "settings normalization clamps persisted values to stable ranges");
    IconBeautifySettings removedShape = defaults;
    removedShape.shape = static_cast<IconBeautifyShape>(4);
    Check(beautify::Normalize(removedShape).shape ==
        IconBeautifyShape::LegacyRounded,
        "removed shape values migrate to an exposed rounded shape");
    IconBeautifySettings removedPreset = defaults;
    removedPreset.preset = static_cast<IconBeautifyPreset>(2);
    removedPreset.shape = IconBeautifyShape::ContinuousRounded;
    removedPreset.outlineEnabled = true;
    const IconBeautifySettings migratedPreset = beautify::Normalize(removedPreset);
    Check(migratedPreset.preset == IconBeautifyPreset::Custom &&
        migratedPreset.shape == IconBeautifyShape::ContinuousRounded &&
        migratedPreset.outlineEnabled,
        "removed presets migrate to custom without losing their parameters");

    const auto source = TestIcon(64);
    Check(beautify::Render(source, 64, 64, defaults) == source,
        "disabled beautification returns source pixels unchanged");

    IconBeautifySettings settings = defaults;
    settings.enabled = true;
    std::set<std::uint64_t> renderHashes;
    for (IconBeautifyShape shape : shapes)
    {
        settings.shape = shape;
        renderHashes.insert(HashPixels(beautify::Render(source, 64, 64, settings)));
    }
    Check(renderHashes.size() == shapes.size(),
        "each shape changes the composite output");

    settings.shape = IconBeautifyShape::Pebble;
    settings.outlineEnabled = false;
    const auto flat = beautify::Render(source, 64, 64, settings);
    settings.textureHighlightStrength = 0.45f;
    const auto highlighted = beautify::Render(source, 64, 64, settings);
    settings.textureHighlightAngle = 0.8f;
    const auto angledHighlight = beautify::Render(source, 64, 64, settings);
    settings.textureHighlightStrength = 0.0f;
    settings.textureShadeStrength = 0.45f;
    const auto shaded = beautify::Render(source, 64, 64, settings);
    settings.textureShadeStrength = 0.0f;
    settings.textureEdgeHighlight = 0.65f;
    const auto edgeHighlighted = beautify::Render(source, 64, 64, settings);
    Check(HashPixels(flat) != HashPixels(highlighted) &&
        HashPixels(highlighted) != HashPixels(angledHighlight) &&
        HashPixels(flat) != HashPixels(shaded) &&
        HashPixels(flat) != HashPixels(edgeHighlighted),
        "numeric texture controls independently change the composite output");
    IconBeautifySettings legacyGlass = defaults;
    beautify::ApplyLegacyFinish(legacyGlass, IconBeautifyFinish::Glass);
    Check(legacyGlass.textureHighlightStrength > 0.0f &&
        legacyGlass.textureShadeStrength > 0.0f &&
        legacyGlass.textureEdgeHighlight > 0.0f,
        "legacy finish presets migrate into numeric texture controls");

    settings.textureEdgeHighlight = 0.0f;
    settings.filterStrength = 0.0f;
    settings.filterTintR = 0.0f;
    settings.filterTintG = 0.2f;
    settings.filterTintB = 1.0f;
    settings.filterEnabled = true;
    const auto zeroStrengthFilter = beautify::Render(source, 64, 64, settings);
    settings.filterStrength = 1.0f;
    settings.shape = IconBeautifyShape::LegacyRounded;
    const auto twoColorSource = TwoColorIcon(64);
    const auto unifiedFilter = beautify::Render(twoColorSource, 64, 64, settings,
        beautify::EdgeColor{0, 0, 0});
    const auto unifiedWhiteFill = beautify::Render(
        twoColorSource, 64, 64, settings,
        beautify::EdgeColor{255, 255, 255});
    const std::uint32_t leftFiltered = unifiedFilter[32 * 64 + 20];
    const std::uint32_t rightFiltered = unifiedFilter[32 * 64 + 44];
    const std::uint32_t filteredFill = unifiedWhiteFill[32 * 64 + 10];
    auto channel = [](std::uint32_t pixel, int shift) {
        return static_cast<int>((pixel >> shift) & 0xff);
    };
    Check(zeroStrengthFilter == flat &&
        HashPixels(unifiedFilter) != HashPixels(twoColorSource) &&
        channel(leftFiltered, 0) > channel(leftFiltered, 8) &&
        channel(leftFiltered, 0) > channel(leftFiltered, 16) &&
        channel(rightFiltered, 0) > channel(rightFiltered, 8) &&
        channel(rightFiltered, 0) > channel(rightFiltered, 16) &&
        leftFiltered == rightFiltered && filteredFill != leftFiltered,
        "equal-luminance source colors become the same grayscale-derived tint");
    Check(channel(filteredFill, 0) > channel(filteredFill, 8) &&
        channel(filteredFill, 0) > channel(filteredFill, 16) &&
        channel(filteredFill, 16) < 32,
        "smart recognition applies unified coloring to detected solid fills");
    settings.filterEnabled = false;

    settings.outlineEnabled = false;
    const auto noOutline = beautify::Render(source, 64, 64, settings);
    settings.outlineEnabled = true;
    settings.outlineWidth = 3.0f;
    settings.outlineOpacity = 1.0f;
    settings.outlineR = 0.0f;
    settings.outlineG = 1.0f;
    settings.outlineB = 0.0f;
    const auto customOutline = beautify::Render(source, 64, 64, settings);
    Check(HashPixels(noOutline) != HashPixels(customOutline),
        "custom outline changes edge pixels");

    settings.outlineEnabled = false;
    settings.shadowStrength = 0.0f;
    const auto noShadow = beautify::Render(source, 64, 64, settings);
    settings.shadowStrength = 1.0f;
    const auto fullShadow = beautify::Render(source, 64, 64, settings);
    Check(HashPixels(noShadow) != HashPixels(fullShadow),
        "shadow strength changes the composite output");

    settings.shadowStrength = 0.0f;
    settings.contentScale = 0.50f;
    const auto compact = beautify::Render(source, 64, 64, settings);
    settings.contentScale = 0.90f;
    const auto large = beautify::Render(source, 64, 64, settings);
    Check(HashPixels(compact) != HashPixels(large),
        "content scale changes the foreground composition");

    for (IconBeautifyShape shape : shapes)
    {
        settings.shape = shape;
        const auto smart = beautify::Render(source, 64, 64, settings,
            beautify::EdgeColor{240, 240, 240});
        Check(smart.size() == source.size(),
            "smart recognition uses the shared compositor for every shape");
    }
    settings.shape = IconBeautifyShape::ContinuousRounded;
    settings.contentScale = 0.50f;
    const auto smartCompact = beautify::Render(source, 64, 64, settings,
        beautify::EdgeColor{240, 240, 240});
    settings.contentScale = 0.90f;
    const auto smartLarge = beautify::Render(source, 64, 64, settings,
        beautify::EdgeColor{240, 240, 240});
    Check(smartCompact == smartLarge,
        "smart recognition clips the original icon without content scaling");

    const auto glass = beautify::MakePreset(IconBeautifyPreset::FrostedGlass);
    const auto darkGlass = beautify::MakePreset(IconBeautifyPreset::FrostedGlassDark);
    const auto lightGlass = beautify::MakePreset(IconBeautifyPreset::FrostedGlassLight);
    // A wholly empty source is intentionally returned unchanged. Use a real
    // glyph and inspect its uncovered plate, isolating fill from rim/shadow.
    const auto fillOnly = [](auto style) {
        style.edgeHighlightEnabled = false; style.outlineEnabled = false; style.shadowStrength = 0;
        return beautify::Render(TestIcon(52), 52, 52, style);
    };
    const auto darkPlate = fillOnly(darkGlass);
    const auto lightPlate = fillOnly(lightGlass);
    const auto transparentPlate = fillOnly(glass);
    Check(darkGlass.glassEnabled && lightGlass.glassEnabled &&
        darkPlate != lightPlate && lightPlate != transparentPlate &&
        darkPlate != transparentPlate,
        "glass presets enable the material and produce distinct fills");
    Check(glass.glassEnabled && glass.edgeHighlightEnabled,
        "transparent glass enables both material and edge highlight");
    auto changedGlass = glass;
    changedGlass.glassBlurRadius = 28.0f;
    Check(!beautify::Equal(glass, changedGlass) &&
        beautify::IdentifyPreset(changedGlass) == IconBeautifyPreset::Custom,
        "editing blur creates a custom preset rather than losing the change");
    for (IconBeautifyShape shape : shapes)
    {
        auto style = glass; style.shape = shape;
        const auto reflection = beautify::RenderEdgeReflection(104, 104, style);
        const auto noReflection = [&] { style.edgeHighlightEnabled = false;
            return beautify::RenderEdgeReflection(104, 104, style); }();
        bool masked = true;
        for (int y = 0; y < 104; ++y) for (int x = 0; x < 104; ++x)
        {
            const auto alpha = reflection[static_cast<size_t>(y) * 104 + x] >> 24;
            if (beautify::ShapeMaskAlpha(shape, x, y, 104, 104) == 0 && alpha) masked = false;
        }
        Check(masked && reflection[52 * 104 + 52] == 0,
            "reflection follows each contour and leaves the center and exterior clear");
        Check(HashPixels(reflection) != HashPixels(noReflection),
            "disabling reflection removes its pixels for every shape");
    }
    const std::vector<std::uint32_t> nativePlate(104 * 104, Premultiplied(18, 110, 62, 255));
    const auto preservedPlate = beautify::Render(nativePlate, 104, 104, glass,
        beautify::DetectEdgeFill(nativePlate, 104, 104));
    auto unlitGlass = glass;
    unlitGlass.edgeHighlightEnabled = false;
    const auto unlitPlate = beautify::Render(nativePlate, 104, 104, unlitGlass,
        beautify::DetectEdgeFill(nativePlate, 104, 104));
    // A narrow rim need not reach a fixed inset pixel. Compare the actual
    // reflected plate with its unlit counterpart while protecting native color.
    Check(preservedPlate[52 * 104 + 52] == nativePlate[52 * 104 + 52] &&
        unlitPlate[52 * 104 + 52] == nativePlate[52 * 104 + 52] &&
        preservedPlate != unlitPlate,
        "smart glass preserves the opaque native plate color while adding its edge reflection");

    if (failures != 0)
    {
        std::cerr << failures << " icon beautify test(s) failed\n";
        return 1;
    }
    std::cout << "Icon beautify tests passed\n";
    return 0;
}
