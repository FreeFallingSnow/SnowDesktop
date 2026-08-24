#include "settings_ui_theme.h"

#include <iostream>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}

int main()
{
    using namespace snowdesktop::settings_ui;

    Check(ResolveTheme(SettingsWindowTheme::System, true, false) ==
            EffectiveTheme::Light &&
        ResolveTheme(SettingsWindowTheme::System, false, false) ==
            EffectiveTheme::Dark,
        "system preference follows the Windows app theme");
    Check(ResolveTheme(SettingsWindowTheme::Light, false, false) ==
            EffectiveTheme::Light &&
        ResolveTheme(SettingsWindowTheme::Dark, true, false) ==
            EffectiveTheme::Dark,
        "explicit preferences override the Windows app theme");
    Check(ResolveTheme(SettingsWindowTheme::Dark, false, true) ==
            EffectiveTheme::HighContrast,
        "high contrast overrides the regular theme preference");

    Check(ShouldUseMica(true, true, false) &&
            !ShouldUseMica(false, true, false) &&
            !ShouldUseMica(true, false, false) &&
            !ShouldUseMica(true, true, true),
        "Mica requires support, transparency, and regular contrast");
    Check(ShouldAnimate(true, false) &&
            !ShouldAnimate(false, false) &&
            !ShouldAnimate(true, true),
        "motion follows system animation and contrast policy");
    Check(ResolveNavigationMode(719.9f) == NavigationMode::Compact &&
            ResolveNavigationMode(720.0f) == NavigationMode::Expanded,
        "navigation switches at the 720 DIP breakpoint");
    Check(ResolvePreferredCjkFont("zh-CN") ==
            CjkFontFamily::MicrosoftYaHei &&
            ResolvePreferredCjkFont("zh-Hans-SG") ==
            CjkFontFamily::MicrosoftYaHei,
        "simplified Chinese uses Microsoft YaHei");
    Check(ResolvePreferredCjkFont("zh-TW") ==
            CjkFontFamily::MicrosoftJhengHei &&
            ResolvePreferredCjkFont("zh-Hant-HK") ==
            CjkFontFamily::MicrosoftJhengHei,
        "traditional Chinese uses Microsoft JhengHei");
    Check(ResolvePreferredCjkFont("ja-JP") ==
            CjkFontFamily::YuGothic &&
            ResolvePreferredCjkFont("ko-KR") ==
            CjkFontFamily::MalgunGothic,
        "Japanese and Korean use their matching system fonts");

    const auto lightAccent = MakeAccentPalette(0xf7e8a0, true);
    const auto darkAccent = MakeAccentPalette(0x183a75, false);
    Check(lightAccent.foreground == 0x000000 &&
            darkAccent.foreground == 0xffffff,
        "accent foreground selects readable black or white");
    Check(ContrastRatio(lightAccent.base, lightAccent.foreground) >= 4.5 &&
            ContrastRatio(darkAccent.base, darkAccent.foreground) >= 4.5,
        "accent foreground meets the minimum text contrast ratio");
    Check(BlendRgb(0x000000, 0xffffff, 0.5f) == 0x808080,
        "RGB blending is deterministic");

    if (failures == 0)
        std::cout << "All settings UI theme tests passed.\n";
    return failures == 0 ? 0 : 1;
}
