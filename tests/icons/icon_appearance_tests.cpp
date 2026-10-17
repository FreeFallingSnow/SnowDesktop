#include "icons/icon_appearance_rules.h"
#include "icons/icon_control_source.h"
#include <iostream>

int RunIconAppearanceTests()
{
    using namespace snowdesktop;
    namespace rules = icon_appearance;
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    const auto global = icon_beautify::MakePreset(IconBeautifyPreset::DefaultBeautify);
    IconAppearanceRecord own; own.target = L"control:start";
    own.settings = icon_beautify::MakePreset(IconBeautifyPreset::FrostedGlassDark); own.settings.mode = 1;
    auto disabledGlobal = global; disabledGlobal.enabled = false;
    check(rules::Resolve(disabledGlobal, &own, false, false).enabled,
        "independent Start appearance remains enabled when global appearance is off");
    auto off = own; off.settings.enabled = false;
    check(!rules::Resolve(global, &off, false, false).enabled,
        "explicit per-icon None suppresses enabled global appearance");
    check(icon_beautify::Equal(rules::Resolve(global, nullptr, false, false), global),
        "removing an override restores the current global style");
    check(!rules::Resolve(global, nullptr, true, false).enabled && rules::Resolve(global, &own, true, false).enabled,
        "media thumbnails keep legacy global exemption but allow an explicit independent style");
    check(!rules::Resolve(global, &own, false, true).enabled,
        "capturing a native Dock source never bakes in a prior beautification plate");
    rules::StyleIdentities identities;
    const auto a = identities.Get(global), b = identities.Get(own.settings);
    check(a != b && identities.Get(global) == a, "shared original icon has isolated reusable style identities");
    auto changed = own.settings; changed.mode = 0;
    check(identities.Get(changed) != b, "processing mode is part of the bitmap cache identity");
    changed = own.settings; changed.glassBlurRadius += 1;
    check(identities.Get(changed) != b, "glass settings are part of the bitmap cache identity");
    check(identities.Get(off.settings) != b, "enabled and disabled versions cannot share a rendered bitmap");
    IconAppearanceRequest edit; edit.target = L"control:search"; edit.session = 2; edit.revision = 7;
    check(rules::CurrentEdit(edit, L"control:search", 2, 7), "current Search edit is accepted");
    check(!rules::CurrentEdit(edit, L"control:start", 2, 7) && !rules::CurrentEdit(edit, L"control:search", 3, 7) &&
        !rules::CurrentEdit(edit, L"control:search", 2, 8), "switched target, reopened editor and stale layout revision all reject delayed edits");
    for (const auto& style : {own.settings, off.settings})
    {
        JsonValue json; IconBeautifySettings decoded;
        check(ParseJson(rules::EncodeStyle(style), json) && rules::DecodeStyle(json, decoded) && icon_beautify::Equal(style, decoded),
            "local appearance roundtrip retains processing strategy, theme values and explicit disablement");
    }
    JsonValue broken; IconBeautifySettings decoded;
    auto encoded = rules::EncodeStyle(own.settings); encoded.replace(encoded.find("\"mode\":1"), 8, "\"mode\":9");
    check(ParseJson(encoded, broken) && !rules::DecodeStyle(broken, decoded), "corrupt processing mode is rejected rather than silently changing the icon");
    check(rules::ValidTarget(L"control:start") && rules::ValidTarget(L"control:search") &&
        rules::ValidTarget(L"recycle-empty:BIN") && rules::ValidTarget(L"recycle-full:BIN") &&
        !rules::ValidTarget(L"widget:") && !rules::ValidTarget(L"control:unknown"), "target identities distinguish controls and recycle states");
    for (const auto target : {L"control:start", L"control:search"})
    {
        check(icon_control::UsesBitmap(target, false, global),
            "idle native Start/Search inheriting smart beautification still enter the shared bitmap pipeline");
        check(!icon_control::UsesBitmap(target, false, disabledGlobal) && icon_control::UsesBitmap(target, true, off.settings),
            "native controls preserve global-off legacy behavior and explicit per-icon None without adding a plate");
        for (const bool light : {false, true})
        {
            const auto source = icon_control::MakeSource(target, light);
            check(!icon_beautify::DetectPlateFill(source.pixels, source.width, source.height),
                "native control source never contains a pre-beautified Dock frame");
            const auto rendered = icon_beautify::Render(source.pixels, source.width, source.height, global);
            const auto plain = icon_beautify::Render(source.pixels, source.width, source.height, disabledGlobal);
            const auto side = static_cast<size_t>(64) * 128 + 10;
            check(source.pixels[side] == 0 && (rendered[side] >> 24) > 0 && plain == source.pixels,
                "smart beautification adds the selected theme to idle native glyphs while None preserves transparency");
        }
    }
    const auto magnified = icon_control::MakeSource(L"control:start", false, 256);
    check(magnified.width == 256 && magnified.height == 256 && magnified.pixels.size() == 256 * 256,
        "magnified native control sources retain target bucket resolution instead of upscaling a fixed preview");
    auto noUpscale = global; noUpscale.mode = 2; noUpscale.backgroundOpacity = 0;
    noUpscale.shape = IconBeautifyShape::ContinuousRounded; noUpscale.outlineEnabled = false; noUpscale.shadowStrength = 0;
    noUpscale.textureHighlightStrength = noUpscale.textureShadeStrength = noUpscale.textureEdgeHighlight = 0;
    noUpscale.edgeHighlightEnabled = false; noUpscale.filterEnabled = false;
    std::vector<std::uint32_t> sparse(64 * 64, 0);
    for (int y = 28; y < 36; ++y) for (int x = 28; x < 36; ++x) sparse[y * 64 + x] = 0xffd02030;
    const auto countVisible = [](const auto& pixels) { return std::count_if(pixels.begin(), pixels.end(), [](auto pixel) { return (pixel >> 24) > 0; }); };
    const auto retained = icon_beautify::Render(sparse, 64, 64, noUpscale);
    auto smart = noUpscale; smart.mode = 0;
    check(countVisible(retained) == 64 && countVisible(icon_beautify::Render(sparse, 64, 64, smart)) > 64,
        "no-enlargement retains sparse glyph dimensions while existing smart mode still normalizes them");
    std::vector<std::uint32_t> full(64 * 64, 0xffd02030);
    check(countVisible(icon_beautify::Render(full, 64, 64, noUpscale)) < countVisible(full),
        "no-enlargement can still shrink oversized glyphs to the configured content bounds");
    JsonValue local; IconBeautifySettings roundtrip;
    check(ParseJson(rules::EncodeStyle(noUpscale), local) && rules::DecodeStyle(local, roundtrip) && roundtrip.mode == 2,
        "no-enlargement survives layout persistence rather than becoming smart or shrink-all on restart");
    for (const auto target : {L"control:start", L"control:search"})
    {
        check(icon_control::ReplacementSourcePixels(target, 48) == 256 && icon_control::ReplacementSourcePixels(target, 96) == 256,
            "replaced Start/Search use the same classification source at rest and under magnification");
    }
    // Regression: ordinary beautification enlarged both built-in control glyphs
    // to 68% even though their source canvas deliberately reserves more space.
    const auto extent = [](const auto& pixels, int edge) {
        int left = edge, right = -1;
        for (int y = 0; y < edge; ++y) for (int x = 0; x < edge; ++x)
            if ((pixels[static_cast<size_t>(y) * edge + x] >> 24) > 16) { left = std::min(left, x); right = std::max(right, x); }
        return std::max(0, right - left + 1);
    };
    for (const auto target : {L"control:start", L"control:search"}) for (const int edge : {128, 256})
    {
        const auto source = icon_control::MakeSource(target, false, edge);
        const int original = extent(source.pixels, edge);
        check(original <= static_cast<int>(edge * (std::wstring_view(target) == L"control:start" ? .60 : .50)),
            "native Start/Search sources retain their intended optical margins");
        for (int mode : {0, 1, 2})
        {
            auto requested = noUpscale; requested.mode = mode;
            const auto style = icon_control::NativeGlyphStyle(requested);
            const auto rendered = icon_beautify::Render(source.pixels, edge, edge, style);
            check(extent(rendered, edge) <= original + 1,
                "themed default Start/Search never expand to ordinary application-icon bounds");
            check(requested.mode == mode, "native rendering leaves the saved processing preference unchanged");
        }
        auto ordinary = noUpscale; ordinary.mode = 0;
        check(extent(icon_beautify::Render(source.pixels, edge, edge, ordinary), edge) > original + 2,
            "negative control reproduces default glyph enlargement through the ordinary image path");
    }
    return failures;
}
