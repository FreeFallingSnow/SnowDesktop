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
    return failures;
}
