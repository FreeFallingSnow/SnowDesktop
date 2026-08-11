#include "icon_beautify.h"

#include <array>
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
    using snowdesktop::IconBeautifyOutlineMode;
    using snowdesktop::IconBeautifyPreset;
    using snowdesktop::IconBeautifySettings;
    using snowdesktop::IconBeautifyShape;
    using snowdesktop::IconBeautifyUpdateKind;

    Check(static_cast<int>(IconBeautifyUpdateKind::Preview) == 0 &&
        static_cast<int>(IconBeautifyUpdateKind::Commit) == 1,
        "preview and commit update kinds have stable values");
    Check(static_cast<int>(IconBeautifyShape::LegacyRounded) == 0 &&
        static_cast<int>(IconBeautifyShape::Pebble) == 10 &&
        static_cast<int>(IconBeautifyFinish::Flat) == 0 &&
        static_cast<int>(IconBeautifyFinish::Sticker) == 3 &&
        static_cast<int>(IconBeautifyOutlineMode::None) == 0 &&
        static_cast<int>(IconBeautifyOutlineMode::Custom) == 2 &&
        static_cast<int>(IconBeautifyPreset::None) == 0 &&
        static_cast<int>(IconBeautifyPreset::Custom) == 5,
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

    constexpr std::array<IconBeautifyPreset, 5> builtInPresets{
        IconBeautifyPreset::None,
        IconBeautifyPreset::ClassicRounded,
        IconBeautifyPreset::AppleGlass,
        IconBeautifyPreset::CircleSticker,
        IconBeautifyPreset::PebbleGloss,
    };
    for (IconBeautifyPreset preset : builtInPresets)
    {
        const auto presetSettings = beautify::MakePreset(preset);
        Check(beautify::IdentifyPreset(presetSettings) == preset,
            "built-in icon beautify presets round-trip through identification");
    }
    auto customPreset = beautify::MakePreset(IconBeautifyPreset::ClassicRounded);
    customPreset.contentScale = 0.71f;
    Check(beautify::IdentifyPreset(customPreset) == IconBeautifyPreset::Custom,
        "edited preset settings are identified as custom");

    constexpr std::array<IconBeautifyShape, 11> shapes{
        IconBeautifyShape::LegacyRounded,
        IconBeautifyShape::Apple,
        IconBeautifyShape::Circle,
        IconBeautifyShape::Samsung,
        IconBeautifyShape::RoundedSquare,
        IconBeautifyShape::Teardrop,
        IconBeautifyShape::Bookmark,
        IconBeautifyShape::Lemon,
        IconBeautifyShape::Diamond,
        IconBeautifyShape::Flower,
        IconBeautifyShape::Pebble,
    };

    std::set<std::uint64_t> maskHashes;
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
    }
    Check(maskHashes.size() == shapes.size(),
        "all eleven shape masks must be geometrically distinct");
    Check(beautify::ShapeMaskAlpha(IconBeautifyShape::Circle, 0, 0, 64, 64) == 0,
        "circle excludes square corners");
    Check(beautify::ShapeMaskAlpha(IconBeautifyShape::Diamond, 32, 1, 64, 64) > 0,
        "diamond keeps its top vertex");
    Check(beautify::ShapeMaskAlpha(IconBeautifyShape::Bookmark, 32, 60, 64, 64) == 0,
        "bookmark keeps its bottom notch");

    IconBeautifySettings defaults;
    Check(defaults.preset == IconBeautifyPreset::None &&
        defaults.shape == IconBeautifyShape::LegacyRounded,
        "legacy rounded is the compatibility shape default");
    Check(defaults.contentScale == 0.68f &&
        defaults.finish == IconBeautifyFinish::Flat &&
        defaults.outlineMode == IconBeautifyOutlineMode::Automatic &&
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
    invalid.finish = static_cast<IconBeautifyFinish>(99);
    invalid.outlineMode = static_cast<IconBeautifyOutlineMode>(99);
    const IconBeautifySettings normalized = beautify::Normalize(invalid);
    Check(normalized.preset == IconBeautifyPreset::Custom &&
        normalized.mode == 1 && normalized.contentScale == 0.50f &&
        normalized.outlineWidth == 4.0f && normalized.outlineOpacity == 0.0f &&
        normalized.shadowStrength == 1.0f &&
        normalized.shape == IconBeautifyShape::Pebble &&
        normalized.finish == IconBeautifyFinish::Sticker &&
        normalized.outlineMode == IconBeautifyOutlineMode::Custom,
        "settings normalization clamps persisted values to stable ranges");

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
    settings.outlineMode = IconBeautifyOutlineMode::None;
    settings.finish = IconBeautifyFinish::Flat;
    const auto flat = beautify::Render(source, 64, 64, settings);
    settings.finish = IconBeautifyFinish::Gloss;
    const auto gloss = beautify::Render(source, 64, 64, settings);
    settings.finish = IconBeautifyFinish::Glass;
    const auto glass = beautify::Render(source, 64, 64, settings);
    settings.finish = IconBeautifyFinish::Sticker;
    const auto sticker = beautify::Render(source, 64, 64, settings);
    Check(HashPixels(flat) != HashPixels(gloss) &&
        HashPixels(flat) != HashPixels(glass) &&
        HashPixels(flat) != HashPixels(sticker) &&
        HashPixels(gloss) != HashPixels(glass),
        "flat, gloss, glass, and sticker finishes are semantically distinct");

    settings.finish = IconBeautifyFinish::Flat;
    settings.outlineMode = IconBeautifyOutlineMode::None;
    const auto noOutline = beautify::Render(source, 64, 64, settings);
    settings.outlineMode = IconBeautifyOutlineMode::Custom;
    settings.outlineWidth = 3.0f;
    settings.outlineOpacity = 1.0f;
    settings.outlineR = 0.0f;
    settings.outlineG = 1.0f;
    settings.outlineB = 0.0f;
    const auto customOutline = beautify::Render(source, 64, 64, settings);
    Check(HashPixels(noOutline) != HashPixels(customOutline),
        "custom outline changes edge pixels");

    settings.outlineMode = IconBeautifyOutlineMode::None;
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
    settings.shape = IconBeautifyShape::Apple;
    settings.contentScale = 0.50f;
    const auto smartCompact = beautify::Render(source, 64, 64, settings,
        beautify::EdgeColor{240, 240, 240});
    settings.contentScale = 0.90f;
    const auto smartLarge = beautify::Render(source, 64, 64, settings,
        beautify::EdgeColor{240, 240, 240});
    Check(smartCompact == smartLarge,
        "smart recognition clips the original icon without content scaling");

    if (failures != 0)
    {
        std::cerr << failures << " icon beautify test(s) failed\n";
        return 1;
    }
    std::cout << "Icon beautify tests passed\n";
    return 0;
}
