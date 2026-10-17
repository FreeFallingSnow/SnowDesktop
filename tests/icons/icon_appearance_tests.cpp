#include "icons/icon_appearance_rules.h"
#include "icons/icon_control_source.h"
#include "icons/icon_beautify_preview.h"
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
    for (const auto target : {L"control:start", L"control:search", L"widget:group-1", L"recycle-empty:BIN", L"recycle-full:BIN", L"item:FILE"})
    {
        rules::EditPreview preview;
        IconAppearanceRequest live; live.target = target; live.session = 7; live.revision = 3;
        auto candidate = own; candidate.target = target; candidate.settings.mode = 2;
        check(preview.Update(live, target, 7, 3, candidate) &&
                preview.Effective(target, &off)->settings.mode == 2 &&
                preview.Effective(L"widget:other", &off) == &off && !off.settings.enabled,
            "live style replaces only its stable target while leaving saved appearance untouched");
        auto stale = live; stale.session = 6;
        check(!preview.Update(stale, target, 7, 3, std::nullopt) &&
                preview.Effective(target, &off)->settings.mode == 2,
            "a delayed previous editor cannot replace the current live preview");
        stale = live; stale.revision = 2;
        check(!preview.Update(stale, target, 7, 3, std::nullopt),
            "a changed layout revision rejects the old editor preview");
        check(preview.Update(live, target, 7, 3, std::nullopt) && !preview.Effective(target, &off) &&
                icon_beautify::Equal(rules::Resolve(global, preview.Effective(target, &off), false, false), global),
            "previewing global inheritance temporarily hides an existing saved override");
        check(preview.Clear() && preview.Effective(target, &off) == &off && !preview.Clear(),
            "cancel or window teardown removes the transient overlay and restores the saved style");
    }
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
    // Distinguish explicit theme compositing from automatic plate recognition.
    constexpr std::uint32_t green = 0xff208050, red = 0xffe04030, blue = 0xff0000ff;
    std::vector<std::uint32_t> padded(64 * 64, 0);
    for (int y = 20; y < 44; ++y) for (int x = 24; x < 40; ++x) padded[y * 64 + x] = green;
    for (int y = 25; y < 29; ++y) for (int x = 27; x < 31; ++x) padded[y * 64 + x] = red;
    auto themeOnly = noUpscale; themeOnly.backgroundOpacity = 1;
    themeOnly.backgroundStartR = themeOnly.backgroundEndR = 0;
    themeOnly.backgroundStartG = themeOnly.backgroundEndG = 0;
    themeOnly.backgroundStartB = themeOnly.backgroundEndB = 1;
    const auto unscaled = icon_beautify::Render(padded, 64, 64, themeOnly, icon_beautify::EdgeColor{32,128,80});
    auto compactFill = themeOnly; compactFill.contentScale = .5f;
    auto largeFill = themeOnly; largeFill.contentScale = .9f;
    check(unscaled[10 * 64 + 32] == blue && unscaled[25 * 64 + 27] == red &&
        std::count(unscaled.begin(), unscaled.end(), red) == 16 &&
        unscaled == icon_beautify::Render(padded, 64, 64, compactFill) && unscaled == icon_beautify::Render(padded, 64, 64, largeFill),
        "add-theme-background retains source canvas, glyph size and offset while ignoring automatic plate colors");
    auto shrink = themeOnly; shrink.mode = 1;
    const auto shrunk = icon_beautify::Render(padded, 64, 64, shrink, icon_beautify::EdgeColor{32,128,80});
    const auto shrunkMark = std::count(shrunk.begin(), shrunk.end(), red);
    check(shrunk[10 * 64 + 32] == blue && shrunkMark > 0 && shrunkMark < 16,
        "add-theme-and-shrink scales the original canvas instead of enlarging a sparse visible glyph");
    auto automatic = themeOnly; automatic.mode = 0;
    check(icon_beautify::Render(padded, 64, 64, automatic, icon_beautify::EdgeColor{32,128,80})[10 * 64 + 32] == green,
        "smart mode still uses a recognized original background rather than the selected theme color");
    check(unscaled == icon_beautify::RenderPreview(padded, 64, 64, themeOnly, {}, 2),
        "supersampled settings previews share unscaled theme compositing");
    auto disabled = themeOnly; disabled.enabled = false;
    check(icon_beautify::Render(padded, 64, 64, disabled) == padded, "explicit None preserves the original source");
    check(icon_beautify::NormalizeGlobal(themeOnly).mode == 0 && icon_beautify::Normalize(themeOnly).mode == 2,
        "legacy third global mode falls back to smart while per-icon add-background remains available");
    auto inherited = own; inherited.followGlobalTheme = true; inherited.settings.mode = 2;
    const auto inheritedStyle = rules::Resolve(global, &inherited, false, false);
    auto globalChanged = icon_beautify::MakePreset(IconBeautifyPreset::FrostedGlassLight); globalChanged.mode = 1;
    const auto refreshed = rules::Resolve(globalChanged, &inherited, false, false);
    check(inheritedStyle.mode == 2 && refreshed.mode == 2 && refreshed.preset == globalChanged.preset &&
        refreshed.backgroundStartR == globalChanged.backgroundStartR &&
        !rules::Resolve(disabledGlobal, &inherited, false, false).enabled,
        "independent processing follows live global theme and enablement without following its processing mode");
    JsonValue local; IconBeautifySettings roundtrip;
    check(ParseJson(rules::EncodeStyle(noUpscale), local) && rules::DecodeStyle(local, roundtrip) && roundtrip.mode == 2,
        "no-enlargement survives layout persistence rather than becoming smart or shrink-all on restart");
    for (const auto target : {L"control:start", L"control:search", L"widget:collection", L"widget:group", L"widget:folder",
        L"item:C:/fixture/icon.lnk", L"recycle-empty:BIN", L"recycle-full:BIN"})
    {
        check(icon_control::ReplacementSourcePixels(target, 48) == 256 && icon_control::ReplacementSourcePixels(target, 96) == 256,
            "all replaced desktop, recycle and Dock icons use one source at rest and under magnification");
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
            const auto rendered = icon_beautify::Render(source.pixels, edge, edge, requested, {},
                icon_beautify::SourceKind::NativeControl);
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
