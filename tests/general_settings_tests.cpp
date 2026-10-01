#include "general_settings.h"
#include "personalization.h"
#include "dock_gradient_storage.h"
#include "status_bar_appearance.h"
#include "item_title_layout.h"
#include "winui/font_picker_search.h"

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>

#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <thread>
#include <functional>

std::wstring GetDataFilePath(const wchar_t* filename)
{
    return filename ? std::wstring(filename) : std::wstring{};
}

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

// Startup draws the selected collection on another thread. Layout-only tests
// cannot expose released font loaders: glyph data is consumed during drawing.
HRESULT DrawSelectedFont(const std::function<void()>& beforeDraw = {})
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) return apartment;
    const auto draw = [&]() -> HRESULT {
        Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
        auto result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        result = wic->CreateBitmap(320, 80, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;
        result = d2d->CreateWicBitmapRenderTarget(bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE), &target);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        result = snowdesktop::app_fonts::CreateTextFormat(factory, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24, L"", &format);
        if (FAILED(result)) return result;
        if (beforeDraw) beforeDraw();
        // Real selected-family shaping must keep fitting labels intact.
        for (const auto* title : {L"PowerPoint", L"MATLAB", L"CAXA", L"Workbench"})
        {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> natural;
            result = factory->CreateTextLayout(title, static_cast<UINT32>(wcslen(title)), format.Get(), 10000, 10000, &natural);
            if (FAILED(result)) return result;
            DWRITE_TEXT_METRICS measure{};
            result = natural->GetMetrics(&measure);
            if (FAILED(result)) return result;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> fitting;
            result = factory->CreateTextLayout(title, static_cast<UINT32>(wcslen(title)), format.Get(), measure.widthIncludingTrailingWhitespace + 1, 1000, &fitting);
            if (FAILED(result)) return result;
            fitting->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 28, 20);
            result = snowdesktop::TrimItemTitle(factory.Get(), fitting.Get(), format.Get(), 1, 28);
            if (FAILED(result)) return result;
            Microsoft::WRL::ComPtr<IDWriteInlineObject> unexpectedSign;
            DWRITE_TRIMMING rule{};
            result = fitting->GetTrimming(&rule, &unexpectedSign);
            if (FAILED(result) || unexpectedSign || rule.granularity != DWRITE_TRIMMING_GRANULARITY_NONE) return E_FAIL;
        }
        constexpr wchar_t text[] = L"SnowDesktop 中文 520";
        Microsoft::WRL::ComPtr<IDWriteTextLayout> clipped;
        result = factory->CreateTextLayout(text, static_cast<UINT32>(std::size(text) - 1), format.Get(), 70, 1000, &clipped);
        if (FAILED(result)) return result;
        clipped->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 28, 20);
        result = snowdesktop::TrimItemTitle(factory.Get(), clipped.Get(), format.Get(), 1, 28);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
        DWRITE_TRIMMING trimming{};
        result = clipped->GetTrimming(&trimming, &sign);
        if (FAILED(result) || !sign) return E_FAIL;
        DWRITE_INLINE_OBJECT_METRICS metrics{};
        result = sign->GetMetrics(&metrics);
        if (FAILED(result) || metrics.width <= 0) return E_FAIL;
        // The tightened three periods must be narrower than Windows' normal
        // sign. CJK full-em and unadjusted native signs fail this regression.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> nativeFormat;
        result = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24, L"", &nativeFormat);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IDWriteInlineObject> nativeSign;
        result = factory->CreateEllipsisTrimmingSign(nativeFormat.Get(), &nativeSign);
        if (FAILED(result)) return result;
        DWRITE_INLINE_OBJECT_METRICS nativeMetrics{};
        result = nativeSign->GetMetrics(&nativeMetrics);
        if (FAILED(result) || metrics.width >= nativeMetrics.width || metrics.width > 24 * .6f) return E_FAIL;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        result = target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        if (FAILED(result)) return result;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        target->DrawText(text, static_cast<UINT32>(std::size(text) - 1), format.Get(), D2D1::RectF(0, 0, 320, 80), brush.Get());
        target->DrawTextLayout(D2D1::Point2F(0, 40), clipped.Get(), brush.Get());
        result = target->EndDraw();
        if (FAILED(result)) return result;
        std::vector<BYTE> pixels(320 * 80 * 4);
        result = bitmap->CopyPixels(nullptr, 320 * 4, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(result)) return result;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] || pixels[i + 1] || pixels[i + 2]) return S_OK;
        return E_FAIL;
    };
    const auto result = draw();
    CoUninitialize();
    return result;
}
}

int main(int argc, char** argv)
{
    using namespace snowdesktop;
    {
        // The picker must find a middle-of-name query (native ComboBox type
        // search only finds prefixes), including a family's English alias.
        const std::vector<app_fonts::Choice> fonts{
            {{}, L"System", L"Segoe UI Variable, Segoe UI"},
            {{"installed", "Microsoft YaHei"}, L"\u5fae\u8f6f\u96c5\u9ed1", L"Microsoft YaHei"},
            {{"MiSans", "MiSans"}, L"MiSans", L""},
            {{"custom-package", "MiSans"}, L"MiSans", L""},
            {{"HarmonyOS-Sans", "HarmonyOS Sans SC"}, L"HarmonyOS Sans SC", L""},
        };
        const auto search = [&](std::wstring_view query) {
            return winui::font_picker::Filter(fonts, query, L"\u7cfb\u7edf\u9ed8\u8ba4");
        };
        Check(search(L"  yAhEi\t") == std::vector<std::size_t>{1},
            "font search matches a canonical English substring case-insensitively after trimming spaces");
        Check(search(L"\u96c5\u9ed1") == std::vector<std::size_t>{1},
            "font search also matches the localized family name");
        Check(search(L"sAnS") == std::vector<std::size_t>{2, 3, 4},
            "font search preserves source indices for built-in and imported families with identical display names");
        Check(search(L"\u9ed8\u8ba4") == std::vector<std::size_t>{0},
            "font search matches the localized system default option");
        Check(search(L"missing font").empty() && search(L" \t\u3000") == std::vector<std::size_t>{0, 1, 2, 3, 4},
            "unmatched font queries return no candidates and clearing the query restores the complete list");
    }
    {
        const auto path = std::filesystem::temp_directory_path() / (L"SnowDesktopFontSettings-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        GeneralSettings saved, loaded;
        saved.font = {"missing-package", "字体 \"name\"\\line\n"};
        Check(SaveGeneralSettings(path.c_str(), saved) && LoadGeneralSettings(path.c_str(), loaded) && loaded.font == saved.font,
            "font selection preserves Unicode, quotes, control characters and unavailable packages across restarts");
        { std::ofstream old(path); old << "{}"; }
        GeneralSettings legacy;
        Check(LoadGeneralSettings(path.c_str(), legacy) && legacy.font.package == "system" && legacy.font.family.empty(),
            "older profiles default to system fonts without migration writes");
        std::error_code ec; std::filesystem::remove(path, ec);
    }
    if (argc > 1)
    {
        const auto assets = std::filesystem::path(argv[1]) / "assets";
        const auto data = std::filesystem::temp_directory_path() / (L"SnowDesktopFontPackageTests-" + std::to_wstring(GetCurrentProcessId()));
        const auto choices = app_fonts::List(assets, data);
        Check(std::count_if(choices.begin(), choices.end(), [](const auto& choice) { return choice.selection.package != "installed"; }) == 3,
            "system, MiSans and HarmonyOS appear once per family, alongside installed fonts without duplicate static weight choices");
        const auto local = std::find_if(choices.begin(), choices.end(), [](const auto& choice) {
            return choice.selection == app_fonts::Selection{"installed", "Segoe UI"};
        });
        Check(local != choices.end(), "Windows' installed Segoe UI family is directly selectable without importing a font package");
        if (local != choices.end())
        {
            Check(local->xamlSource == L"Segoe UI" && app_fonts::Select(local->selection, assets, data),
                "installed font selection uses its canonical family for WinUI and DirectWrite");
            Check(app_fonts::GdiFamily() == L"Segoe UI" && app_fonts::XamlFamily() == L"Segoe UI" && !std::filesystem::exists(data / "fonts"),
                "installed selection uses the local family without creating or copying a managed font package");
            const auto active = app_fonts::current.load();
            Check(!app_fonts::Select({"installed", "SnowDesktop missing font 26E6C3A8"}, assets, data) && app_fonts::current.load() == active,
                "an unavailable installed family preserves the active choice rather than silently selecting a substitute");
            HRESULT rendered = E_FAIL;
            std::thread renderer([&] { rendered = DrawSelectedFont(); });
            renderer.join();
            Check(SUCCEEDED(rendered), "a selected installed family rasterizes on the startup thread after enumeration returns");
            const auto path = data.parent_path() / (L"SnowDesktopInstalledFontSettings-" + std::to_wstring(GetCurrentProcessId()) + L".json");
            GeneralSettings saved, restored;
            saved.font = local->selection;
            Check(SaveGeneralSettings(path.c_str(), saved) && LoadGeneralSettings(path.c_str(), restored) && restored.font == local->selection,
                "installed font choice survives saving and restarting without altering package or family identity");
            std::error_code error; std::filesystem::remove(path, error);
        }
        const auto mi = std::find_if(choices.begin(), choices.end(), [](const auto& c) { return c.selection.package == "MiSans"; });
        const auto harmony = std::find_if(choices.begin(), choices.end(), [](const auto& c) { return c.selection.package == "HarmonyOS-Sans"; });
        Check(mi != choices.end() && harmony != choices.end(), "both bundled font packages load via DirectWrite");
        for (const auto* id : {"MiSans", "HarmonyOS-Sans"})
        {
            const auto choice = std::find_if(choices.begin(), choices.end(), [id](const auto& c) { return c.selection.package == id; });
            if (choice == choices.end()) continue;
            Check(app_fonts::Select(choice->selection, assets, data), "select bundled font before startup rendering");
            HRESULT rendered = E_FAIL;
            std::thread renderer([&] {
                rendered = DrawSelectedFont([&] {
                    Check(app_fonts::Select(choice->selection, assets, data),
                        "reloading the active choice while formats exist retains usable font resources");
                });
            });
            renderer.join();
            Check(SUCCEEDED(rendered), "selected original font rasterizes on the startup rendering thread after package enumeration returns");
        }
        if (mi != choices.end())
        {
            Check(app_fonts::Select(mi->selection, assets, data), "select original MiSans Regular font");
            Microsoft::WRL::ComPtr<IDWriteFactory> factory;
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
            Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
            Check(SUCCEEDED(app_fonts::CreateTextFormat(factory.Get(), L"Segoe UI", static_cast<DWRITE_FONT_WEIGHT>(520),
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"", &format)), "selected font produces text formats");
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            factory->CreateTextLayout(L"Title", 5, format.Get(), 100, 40, &layout);
            app_fonts::SetWeight(layout.Get(), static_cast<DWRITE_FONT_WEIGHT>(416), {0, 5});
            Microsoft::WRL::ComPtr<IDWriteTextLayout4> variable;
            layout.As(&variable);
            DWRITE_FONT_AXIS_VALUE axis{};
            DWRITE_FONT_WEIGHT requestedWeight{};
            Check(SUCCEEDED(layout->GetFontWeight(0, &requestedWeight)) &&
                requestedWeight == static_cast<DWRITE_FONT_WEIGHT>(416) && variable &&
                SUCCEEDED(variable->GetFontAxisValues(0, &axis, 1, nullptr)) && axis.axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT && axis.value == 416,
                "rendering requests the corrected weight in both classic and axis formats even when only Regular is bundled");
            Check(!app_fonts::Select({"../escape", "fake"}, assets, data) && app_fonts::current.load()->selection == mi->selection,
                "invalid or unavailable selection does not mutate the active font");
        }
        if (harmony != choices.end()) Check(harmony->xamlSource.find(L"Regular.ttf") != std::wstring::npos,
            "settings font URI selects the regular face rather than the alphabetically first bold file");
        std::vector<app_fonts::Choice> imported;
        std::string error;
        Check(app_fonts::Import(assets / "fonts" / "HarmonyOS-Sans", data, imported, error) && imported.size() == 1,
            "a font folder imports as a selectable family");
        if (!imported.empty())
        {
            const auto all = app_fonts::List(assets, data);
            Check(std::any_of(all.begin(), all.end(), [&](const auto& c) { return c.selection == imported.front().selection; }),
                "imported family remains available on the next package scan");
            const auto package = std::filesystem::path(imported.front().selection.package);
            const auto file = std::filesystem::directory_iterator(data / "fonts" / package)->path().filename();
            const auto uri = L"Files/SnowDesktopUserFonts/" + package.wstring() + L"/" + file.wstring();
            Check(!app_fonts::ResolveXamlResource(uri, data).empty(), "managed custom font resolves for unpackaged and MSIX settings");
        }
        Check(app_fonts::ResolveXamlResource(L"Files/SnowDesktopUserFonts/../secret.ttf", data).empty() &&
            app_fonts::ResolveXamlResource(L"Files/SnowDesktopUserFonts/id/../../secret.ttf", data).empty(),
            "custom XAML resource lookup rejects traversal and unrelated resources");
        Check(!app_fonts::Import(assets / "fonts" / "README.md", data, imported, error), "non-font packages are rejected");
        app_fonts::Select({}, assets, data);
        std::error_code ec; std::filesystem::remove_all(data, ec);
        Check(!ec, "only the test-owned imported package directory is removed");
    }
    {
        GeneralSettings value;
        Check(!value.statusBar.cpu && !value.statusBar.memory && !value.statusBar.gpu && !value.statusBar.traffic,
            "resource information starts disabled for a new profile");
        StatusBarSettings firstEnable;
        JsonValue minimal; ParseJson("{\"enabled\":true}", minimal);
        Check(DecodeStatusBarSettings(minimal, firstEnable) && firstEnable.enabled &&
            !firstEnable.cpu && !firstEnable.memory && !firstEnable.gpu && !firstEnable.traffic,
            "enabling a bar with no information preferences must not turn information on");
        Check(!value.statusBar.enabled && value.statusBar.position == DockPosition::Top &&
                value.statusBar.monitorScope == DockMonitorScope::First && value.statusBar.theme.mode == -1,
            "new and migrated settings must keep the bar off and follow the global theme");
        Check(!firstEnable.noWindow.enabled && !firstEnable.maximizedWindow.enabled,
            "legacy status bar settings without scene rules must retain the default appearance in every scene");
        Check(firstEnable.taskView && !firstEnable.clockSystemPanel && !firstEnable.controlCenterSystemPanel,
            "task view starts visible while existing clock and controls retain the SnowDesktop panels");
        Check(firstEnable.inputMethod, "existing profiles gain the input indicator by default");
        {
            firstEnable.inputMethod = false;
            JsonValue stored; StatusBarSettings restored;
            Check(ParseJson(EncodeStatusBarSettings(firstEnable), stored) && DecodeStatusBarSettings(stored, restored) &&
                !restored.inputMethod, "hiding the input indicator persists across settings reloads");
        }
        JsonValue legacyJson; StatusBarSettings legacy;
        ParseJson("{\"theme\":{\"mode\":4,\"customized\":true,\"appearance\":{\"backgroundR\":0.125,\"opacity\":0.42}},"
            "\"pinnedTrayItems\":[\"guid:legacy\"]}", legacyJson);
        Check(DecodeStatusBarSettings(legacyJson, legacy) && legacy.theme.mode == 4 && legacy.theme.customized &&
            legacy.pinnedTrayItems == std::vector<std::string>{"guid:legacy"} &&
            ResolveStatusBarAppearance(legacy, PersonalizationSettings{}, {true, true}).widgetAlpha == .42f &&
            legacy.theme.appearance.widgetBgR == .125f,
            "old custom themes and tray identities survive migration even with conflicting scene observations");
        for (const auto* version : {"", "\"sceneRulesVersion\":1,"})
        {
            JsonValue oldScenes; StatusBarSettings migrated, reloaded;
            Check(ParseJson(std::string("{") + version +
                    "\"theme\":{\"mode\":1,\"appearance\":{}},\"shellUi\":{\"enabled\":true,\"theme\":{\"mode\":5,\"appearance\":{}}},"
                    "\"visibleWindow\":{\"enabled\":true,\"theme\":{\"mode\":3,\"appearance\":{}}},"
                    "\"maximizedWindow\":{\"enabled\":true,\"theme\":{\"mode\":0,\"appearance\":{}}}}", oldScenes) &&
                DecodeStatusBarSettings(oldScenes, migrated), "old scene schemas can be migrated explicitly");
            Check(!migrated.noWindow.enabled && migrated.legacyShellUi.enabled && migrated.legacyVisibleWindow.enabled &&
                migrated.legacyShellUi.theme.mode == 5 && migrated.legacyVisibleWindow.theme.mode == 3 &&
                ResolveStatusBarAppearance(migrated, PersonalizationSettings{}, {true, false}) == MakeAppearancePreset(kAppearancePresetLight) &&
                ResolveStatusBarAppearance(migrated, PersonalizationSettings{}, {}) == MakeAppearancePreset(kAppearancePresetLight) &&
                ResolveStatusBarAppearance(migrated, PersonalizationSettings{}, {false, true}) == MakeAppearancePreset(kAppearancePresetDark),
                "retired scenes remain inert and must never become an enabled no-window rule; maximized keeps its meaning");
            JsonValue saved;
            Check(ParseJson(EncodeStatusBarSettings(migrated), saved) && saved.Find("sceneRulesVersion") &&
                saved.Find("sceneRulesVersion")->number == 2 && DecodeStatusBarSettings(saved, reloaded) && reloaded == migrated,
                "migration writes the new schema while preserving retired rule values for compatibility");
        }
        value.statusBar.enabled = true;
        value.statusBar.position = DockPosition::Bottom;
        value.statusBar.monitorScope = DockMonitorScope::All;
        value.statusBar.scale = 1.5f;
        value.statusBar.menu = false;
        value.statusBar.taskView = false;
        value.statusBar.clockSystemPanel = value.statusBar.controlCenterSystemPanel = true;
        value.statusBar.leftOrder = {"quickSearch", "menu", "taskView"};
        std::reverse(value.statusBar.rightOrder.begin(), value.statusBar.rightOrder.end());
        value.statusBar.pinnedTrayItems = {"C:\\测试\\app.exe|42", "guid:\"test\""};
        value.statusBar.trayOrder = {"guid:\"test\"", "C:\\测试\\app.exe|42"};
        value.statusBar.theme.mode = 4;
        value.statusBar.theme.customized = true;
        value.statusBar.theme.appearance.backgroundPreset = kAppearancePresetCustom;
        value.statusBar.theme.appearance.widgetBgR = .35f;
        value.statusBar.noWindow.enabled = true;
        value.statusBar.noWindow.theme.mode = kStatusBarThemeTransparentDarkText;
        value.statusBar.legacyShellUi.enabled = true;
        value.statusBar.legacyShellUi.theme.mode = 6;
        value.statusBar.maximizedWindow.enabled = true;
        value.statusBar.maximizedWindow.theme.mode = -1;
        value.statusBar.legacyVisibleWindow.theme.mode = 4;
        value.statusBar.legacyVisibleWindow.theme.customized = true;
        value.statusBar.legacyVisibleWindow.theme.appearance.widgetBgB = .73f;
        const auto path = std::filesystem::temp_directory_path() / (L"SnowDesktopStatusBar-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        GeneralSettings restored;
        Check(SaveGeneralSettings(path.c_str(), value) && LoadGeneralSettings(path.c_str(), restored) &&
                restored.statusBar == value.statusBar,
            "status bar geometry, default and active or disabled scene themes, and tray identities must survive persistence");
        std::error_code error;
        std::filesystem::remove(path, error);
        const std::array<int, 7> expectedPresets{-1, kAppearancePresetDark, kAppearancePresetLight,
            kAppearancePresetGlassDark, kAppearancePresetGlassLight, kAppearancePresetAcrylicDark,
            kAppearancePresetAcrylicLight};
        for (std::size_t i = 0; i < StatusBarThemeModes.size(); ++i)
        {
            StatusBarSettings preset; preset.theme.mode = StatusBarThemeModes[i];
            preset.theme.appearance.widgetBgR = .125f;
            JsonValue encoded; StatusBarSettings decoded;
            Check(ParseJson(EncodeStatusBarSettings(preset), encoded) && DecodeStatusBarSettings(encoded, decoded) &&
                decoded.theme.mode == preset.theme.mode && decoded.theme.customized == preset.theme.customized &&
                decoded.theme.appearance.widgetBgR == .125f && StatusBarThemeSelection(decoded.theme.mode) == static_cast<int>(i),
                "all status bar presets, global and custom survive persistence with stable legacy modes");
            if (i > 0 && i < 7)
                Check(ResolveStatusBarAppearance(decoded.theme, MakeAppearancePreset(kAppearancePresetLight)) == MakeAppearancePreset(expectedPresets[i]),
                    "bar preset matches the corresponding global material");
            if (preset.theme.mode == 7 || preset.theme.mode == 8)
            {
                const auto transparent = ResolveStatusBarAppearance(decoded.theme, PersonalizationSettings{});
                Check(transparent.backgroundPreset == kAppearancePresetTaskbarTransparent && transparent.widgetAlpha == 0 &&
                    transparent.widgetBorderAlpha == 0 && transparent.gradientEndA == 0 &&
                    !transparent.glassEnabled && !transparent.acrylicEnabled && !transparent.widgetEdgeHighlightEnabled &&
                    !transparent.panelGradient.enabled && transparent.contentTheme == (preset.theme.mode == 7 ? 1 : 0),
                    "transparent presets clear all background effects and retain independent dark or light text");
            }
        }
        for (const int mode : {5, 6, 7, 8})
        {
            JsonValue barOnly; SurfaceTheme popup;
            ParseJson("{\"mode\":" + std::to_string(mode) + "}", barOnly);
            Check(!DecodeSurfaceTheme(barOnly, popup), "status bar presets must not broaden the popup theme contract");
        }
        for (const auto* json : {"{\"shellUi\":true}", "{\"maximizedWindow\":{\"enabled\":1}}",
            "{\"visibleWindow\":{\"theme\":{\"mode\":9}}}", "{\"noWindow\":{\"enabled\":1}}",
            "{\"sceneRulesVersion\":3}", "{\"sceneRulesVersion\":\"2\"}"})
        {
            JsonValue invalid; ParseJson(json, invalid);
            auto unchanged = value.statusBar;
            Check(!DecodeStatusBarSettings(invalid, unchanged) && unchanged == value.statusBar,
                "invalid scene settings must fail atomically without replacing valid preferences");
        }
        // Empty and maximized are mutually exclusive on a real display. An
        // inconsistent observation must never apply the empty-display rule.
        StatusBarSettings scenes;
        scenes.theme.mode = 1;
        scenes.noWindow.enabled = scenes.maximizedWindow.enabled = true;
        scenes.noWindow.theme.mode = 5;
        scenes.maximizedWindow.theme.mode = 0;
        const auto global = MakeAppearancePreset(kAppearancePresetGlassLight);
        const auto savedScenes = scenes;
        Check(ResolveStatusBarAppearance(scenes, global, {true, false}) == MakeAppearancePreset(kAppearancePresetGlassDark),
            "an observed empty display selects its own full global preset");
        Check(ResolveStatusBarAppearance(scenes, global, {false, true}) == MakeAppearancePreset(kAppearancePresetDark) &&
            ResolveStatusBarAppearance(scenes, global, {true, true}) == MakeAppearancePreset(kAppearancePresetDark),
            "a maximized application selects its rule even if the empty observation is inconsistent");
        Check(ResolveStatusBarAppearance(scenes, global, {}) == MakeAppearancePreset(kAppearancePresetLight) && scenes == savedScenes,
            "ordinary windows or an unknown observation use the saved default without rewriting preferences");
        scenes.maximizedWindow.enabled = false;
        Check(ResolveStatusBarAppearance(scenes, global, {true, true}) == MakeAppearancePreset(kAppearancePresetLight),
            "a disabled maximized rule cannot accidentally activate the no-window rule");
        scenes.noWindow.enabled = false;
        Check(ResolveStatusBarAppearance(scenes, global, {true, false}) == MakeAppearancePreset(kAppearancePresetLight),
            "disabled no-window override returns to the independent bar default");
        scenes.maximizedWindow.enabled = true;
        scenes.maximizedWindow.theme.mode = -1;
        Check(ResolveStatusBarAppearance(scenes, global, {false, true}) == global,
            "a scene following global must use global appearance rather than the independent bar default");
        scenes.noWindow.enabled = true;
        scenes.noWindow.theme.mode = 4;
        scenes.noWindow.theme.appearance.widgetAlpha = .43f;
        Check(ResolveStatusBarAppearance(scenes, global, {true, false}) == scenes.noWindow.theme.appearance,
            "a custom scene keeps its independent material values");
        value.statusBar.position = DockPosition::Right;
        NormalizeStatusBarSettings(value.statusBar);
        Check(value.statusBar.position == DockPosition::Top && value.statusBar.pinnedTrayItems.size() == 2,
            "retired side positions migrate to the top without losing tray preferences");
        value.statusBar.leftOrder = {"quickSearch", "unknown", "quickSearch"};
        value.statusBar.rightOrder = {"volume", "clock", "menu", "volume"};
        NormalizeStatusBarSettings(value.statusBar);
        Check(value.statusBar.leftOrder == std::vector<std::string>{"quickSearch", "menu", "taskView"} &&
                value.statusBar.rightOrder.front() == "volume" && value.statusBar.rightOrder.size() == 9 &&
                std::find(value.statusBar.rightOrder.begin(), value.statusBar.rightOrder.end(), "clock") == value.statusBar.rightOrder.end(),
            "saved bar order excludes invalid or duplicate items and restores supported missing entries");
    }
    {
        using namespace snowdesktop::shell_extensions;
        const auto path = std::filesystem::temp_directory_path() / (L"SnowDesktopMenuPreferences-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        GeneralSettings saved, loaded;
        saved.shellExtensions = {true, {{"handler:{provider}", "", "压缩软件", Placement::Submenu},
            {"handler:{provider}", "compress", "压缩 \"文件\"", Placement::Root},
            {"handler:{provider}", "extract", "解压", Placement::Hidden}}};
        saved.shellExtensions.hidden = {{"verb:sevenzip", Context::File}, {"verb:sevenzip", Context::Desktop},
            {"menu:特殊\"项目", Context::FolderBackground}};
        saved.shellExtensions.shown = {{"verb:sevenzip", Context::Folder},
                                       {"menu:特殊\"项目", Context::Desktop}};
        Check(SaveGeneralSettings(path.c_str(), saved) && LoadGeneralSettings(path.c_str(), loaded) &&
                  loaded.shellExtensions == saved.shellExtensions,
              "explicit visibility and legacy preferences survive restart including escaped Unicode "
              "identities");
        saved.shellExtensions.enabled = false;
        Check(SaveGeneralSettings(path.c_str(), saved) && LoadGeneralSettings(path.c_str(), loaded) && loaded.shellExtensions == saved.shellExtensions,
            "legacy selector data remains preserved alongside the active scoped exclusions");
        { std::ofstream legacy(path); legacy << "{}"; }
        Check(LoadGeneralSettings(path.c_str(), loaded) && !loaded.shellExtensions.enabled && loaded.shellExtensions.selections.empty(),
            "old configuration has no local exclusions while retaining legacy fields for compatibility");
        Check(loaded.shellExtensions.shown.empty() &&
                  IsHidden(loaded.shellExtensions, "verb:sevenzip", Context::File),
              "missing preferences hide every third-party menu item by default");
        JsonValue legacy;
        Check(ParseJson(R"({"enabled":true,"hidden":[{"id":"verb:editor","context":0}]})", legacy) &&
                  ReadPreferences(&legacy).shown.empty(),
              "exclusion-only settings never opt other extensions in");
        std::error_code error; std::filesystem::remove(path, error);
    }

    {
        const auto path = std::filesystem::temp_directory_path() / (L"SnowDesktopCalendarPreferences-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        GeneralSettings settings;
        settings.calendarDisplay = {true, "hebrew", false, ""};
        Check(SaveGeneralSettings(path.c_str(), settings), "save host calendar preferences");
        GeneralSettings loaded;
        Check(LoadGeneralSettings(path.c_str(), loaded) && loaded.calendarDisplay == settings.calendarDisplay, "calendar preferences survive restart");
        settings.calendarDisplay.enabled = false; settings.calendarDisplay.holidaysEnabled = false;
        Check(SaveGeneralSettings(path.c_str(), settings) && LoadGeneralSettings(path.c_str(), loaded) && loaded.calendarDisplay == settings.calendarDisplay, "off switch preserves the selected calendar");
        settings.calendarDisplay = {true, "invalid", true, "invalid"};
        Check(SaveGeneralSettings(path.c_str(), settings) && LoadGeneralSettings(path.c_str(), loaded) && !loaded.calendarDisplay.enabled && !loaded.calendarDisplay.holidaysEnabled, "unknown preferences safely disable annotations");
        { std::ofstream old(path); old << R"({"calendarEnabled":true,"calendarType":"hebrew","holidaysEnabled":true,"holidayRegion":"CN"})"; }
        Check(LoadGeneralSettings(path.c_str(), loaded) && loaded.calendarDisplay.enabled && loaded.calendarDisplay.calendar == "hebrew" && !loaded.calendarDisplay.holidaysEnabled && loaded.calendarDisplay.region.empty(), "old holiday settings are ignored without losing the extra calendar");
        std::error_code error; std::filesystem::remove(path, error);
    }
    {
        // A gradient must survive restart independently in every taskbar rule,
        // including its inactive colors after switching back to solid fill.
        DockSettings saved;
        auto gradients = snowdesktop::TaskbarGradients(saved);
        for (size_t i = 0; i < gradients.size(); ++i)
        {
            gradients[i]->enabled = i != 2;
            gradients[i]->angle = 37.5 + i * 45;
            gradients[i]->start = .125;
            gradients[i]->end = .875;
            gradients[i]->stops = {{0, 0xff2233, .15}, {.4, 0x3355ff, .8}, {1, 0x11aa22, .45}};
        }
        std::ostringstream serialized;
        Check(snowdesktop::WriteTaskbarGradients(serialized, saved), "serialize all taskbar gradients");
        JsonValue document;
        Check(ParseJson("{" + serialized.str() + "\"schema\":1}", document), "taskbar gradient fields form valid JSON");
        DockSettings restored;
        Check(snowdesktop::ReadTaskbarGradients(document, restored), "read taskbar gradient fields");
        auto loaded = snowdesktop::TaskbarGradients(restored);
        for (size_t i = 0; i < gradients.size(); ++i)
            Check(*loaded[i] == *gradients[i], "taskbar gradients preserve colors, opacity, direction, range and enabled state per rule");
        document.object[snowdesktop::kTaskbarGradientKeys[3]].object["angle"].number = 900;
        Check(!snowdesktop::ReadTaskbarGradients(document, restored), "reject a damaged taskbar gradient");
        for (size_t i = 0; i < gradients.size(); ++i)
            Check(*loaded[i] == *gradients[i], "invalid gradient load never partially overwrites other scenarios");
        Check(ParseJson("{}", document) && snowdesktop::ReadTaskbarGradients(document, restored), "legacy taskbar settings remain readable");
        for (const auto* value : loaded) Check(!value->enabled, "legacy taskbar settings default to no gradient");
        gradients[2]->stops[1].position = 1;
        std::ostringstream invalid;
        Check(!snowdesktop::WriteTaskbarGradients(invalid, saved) && invalid.str().empty(), "invalid taskbar gradients are rejected before writing any fields");
    }
    std::error_code error;
    const auto path = std::filesystem::temp_directory_path(error) /
        (L"SnowDesktopGeneralSettingsTests-" +
            std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(path, error);

    GeneralSettings saved;
    saved.animationMode = 1;
    saved.popupAnimationEffect = 1;
    saved.animationSpeed = 2;
    saved.animationFrameLimit = 30;
    saved.animationEnergySaver = false;
    saved.animationOnBattery = true;
    saved.demoModeEnabled = true;
    saved.widgetDeveloperToolsEnabled = true;
    saved.quickNavTheme = kFourThemeAcrylicDark;
    saved.collectionPopupTheme = kFourThemeAcrylicLight;
    saved.quickNavigationAppearance.mode = 4;
    saved.quickNavigationAppearance.customized = true;
    saved.quickNavigationAppearance.appearance.backgroundPreset = kAppearancePresetCustom;
    saved.quickNavigationAppearance.appearance.panelGradient.enabled = true;
    saved.quickNavigationAppearance.appearance.panelGradient.angle = 213;
    saved.collectionPopupAppearance.mode = 2;
    saved.pageNavigationKeyboardEnabled = false;
    saved.pageNavigationPreviousModifiers = MOD_CONTROL;
    saved.pageNavigationPreviousVirtualKey = VK_HOME;
    saved.pageNavigationNextModifiers = MOD_ALT;
    saved.pageNavigationNextVirtualKey = VK_END;
    strcpy_s(saved.language, "zh-CN");
    Check(SaveGeneralSettings(path.c_str(), saved),
        "general settings save succeeds");

    GeneralSettings loaded;
    Check(LoadGeneralSettings(path.c_str(), loaded),
        "general settings load succeeds");
    Check(loaded.quickNavigationAppearance == saved.quickNavigationAppearance &&
        loaded.collectionPopupAppearance.mode == 2,
        "independent surface custom appearance and fixed preset survive save and reload");
    {
        using namespace snowdesktop;
        SurfaceTheme theme;
        auto global = PersonalizationSettings::LightPreset();
        Check(ResolveSurfaceTheme(theme, global, 0, true).contentTheme == 1,
            "new surface theme follows the global preset by default");
        theme.mode = 0;
        Check(ResolveSurfaceTheme(theme, global, 1, true).contentTheme == 0,
            "fixed surface preset applies even when the global theme is not custom");
        theme.mode = -2;
        Check(ResolveSurfaceTheme(theme, global, 0, false).contentTheme == 1,
            "legacy surface follows a non-custom global theme");
        global.backgroundPreset = kAppearancePresetCustom;
        Check(ResolveSurfaceTheme(theme, global, 0, false).contentTheme == 0,
            "legacy surface keeps its old override for a custom global theme");
        theme.mode = -1;
        auto bar = SurfaceTheme{};
        bar.customized = true; bar.appearance.widgetAlpha = .13f;
        for (const int preset : {kAppearancePresetDark, kAppearancePresetLight, kAppearancePresetGlassDark, kAppearancePresetGlassLight,
                kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight})
        {
            const auto actualGlobal = MakeAppearancePreset(preset);
            Check(ResolveStatusBarAppearance(bar, actualGlobal) == actualGlobal,
                "bar follow mode preserves the complete global preset instead of substituting a popup preset");
        }
        global.panelGradient.enabled = true; global.panelGradient.angle = 42;
        global.widgetAlpha = .37f;
        Check(ResolveStatusBarAppearance(bar, global) == global,
            "bar follows custom global changes even when an old independent appearance is retained");
        bar.mode = 4;
        Check(ResolveStatusBarAppearance(bar, global) == bar.appearance,
            "explicit custom bar appearance stays independent");
        global.panelGradient.enabled = true;
        global.panelGradient.angle = 123;
        Check(IsCustomSurfaceTheme(theme, global) && ResolveSurfaceTheme(theme, global, 0, true) == global,
            "following a custom global theme exposes the actual custom fill and material");
        theme.customized = true; theme.appearance.widgetAlpha = .23f;
        Check(ResolveSurfaceTheme(theme, global, 0, false).widgetAlpha == .23f,
            "editing a following custom popup applies its independent appearance");
        auto light = PersonalizationSettings::LightPreset();
        Check(!IsCustomSurfaceTheme(theme, light) && ResolveSurfaceTheme(theme, light, 0, false).contentTheme == 1,
            "switching global back to a preset resumes inheritance without deleting custom values");
        {
            EdgeLightSettings limits;
            VisitEdgeLightFields([&](auto, auto field, float, float maximum) { limits.*field = maximum; });
            JsonValue json; EdgeLightSettings decoded;
            Check(ParseJson(EncodeEdgeLight(limits), json) && DecodeEdgeLight(json, decoded) && decoded == limits,
                "all edge-light upper bounds survive JSON round trips");
            VisitEdgeLightFields([&](auto, auto field, float minimum, float) { limits.*field = minimum; });
            Check(ParseJson(EncodeEdgeLight(limits), json) && DecodeEdgeLight(json, decoded) && decoded == limits,
                "all edge-light lower bounds survive JSON round trips");
            Check(ParseJson("{\"direction\":-1}", json) && !DecodeEdgeLight(json, decoded),
                "invalid edge-light parameters are rejected atomically");
            Check(ParseJson("{}", json) && DecodeEdgeLight(json, decoded) && decoded == EdgeLightSettings{},
                "older appearance objects inherit the shared material defaults");
        }
        const auto transparentGlass = MakeAppearancePreset(kAppearancePresetGlassTransparent);
        Check(transparentGlass.glassEnabled && !transparentGlass.acrylicEnabled &&
            transparentGlass.widgetAlpha < .1f && transparentGlass.contentTheme == 0 &&
            transparentGlass.widgetEdgeHighlightEnabled,
            "transparent glass retains light text, blur and edge highlights with faint fill");
        Check(NormalizeAppearancePresetId(transparentGlass.backgroundPreset) == kAppearancePresetGlassTransparent,
            "transparent glass retains its independent persisted preset identity");
        SurfaceTheme following;
        Check(ResolveSurfaceTheme(following, transparentGlass, 0, true).backgroundPreset == kAppearancePresetAcrylicDark &&
            ResolveSurfaceTheme(following, transparentGlass, 0, false).backgroundPreset == kAppearancePresetAcrylicDark,
            "popup and quick navigation follow transparent global glass with dark acrylic");
        DockSettings dock;
        dock.followComponentAppearance = false;
        for (int preset : {0, 1, 6, 7, 10, 11, kAppearancePresetGlassTransparent})
        {
            dock.appearancePreset = preset;
            auto expected = MakeAppearancePreset(preset); expected.cornerRadius = global.cornerRadius;
            Check(ResolveDockAppearance(dock, global) == expected, "Dock resolves each global appearance preset independently");
        }
        dock.appearancePreset = kAppearancePresetCustom; dock.customAppearance.widgetAlpha = .39f;
        Check(ResolveDockAppearance(dock, global).widgetAlpha == .39f, "legacy Dock custom appearance remains effective");
        dock.followComponentAppearance = true;
        Check(ResolveDockAppearance(dock, global) == global, "Dock follow mode uses the complete component appearance");
        dock.appearancePreset = 123; NormalizeDockSettings(dock);
        Check(dock.appearancePreset == kAppearancePresetCustom, "unsupported Dock preset falls back to retained custom settings");
        theme = saved.quickNavigationAppearance;
        Check(ResolveSurfaceTheme(theme, global, 0, true) == theme.appearance,
            "custom surface preserves gradient, opacity, material and foreground independently");
        JsonValue encoded;
        Check(ParseJson(EncodeSurfaceTheme(theme), encoded), "custom surface JSON is readable");
        encoded.object["appearance"].object["opacity"].number = -1;
        SurfaceTheme before = theme;
        Check(!DecodeSurfaceTheme(encoded, theme) && theme == before,
            "invalid custom opacity rejects the complete replacement without changing stored appearance");
    }
    Check(loaded.animationMode == 1 && loaded.popupAnimationEffect == 1 &&
        loaded.animationSpeed == 2 && loaded.animationFrameLimit == 30 &&
        !loaded.animationEnergySaver && loaded.animationOnBattery,
        "animation preferences survive a settings save and reload");
    Check(loaded.demoModeEnabled &&
        loaded.widgetDeveloperToolsEnabled &&
        loaded.quickNavTheme == kFourThemeAcrylicDark &&
        loaded.collectionPopupTheme == kFourThemeAcrylicLight &&
        !loaded.pageNavigationKeyboardEnabled &&
        loaded.pageNavigationPreviousModifiers == MOD_CONTROL &&
        loaded.pageNavigationPreviousVirtualKey == VK_HOME &&
        loaded.pageNavigationNextModifiers == MOD_ALT &&
        loaded.pageNavigationNextVirtualKey == VK_END &&
        std::strcmp(loaded.language, "zh-CN") == 0,
        "general flags, page keys, and four-theme selections persist");

    {
        std::ifstream persisted(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(persisted)),
            std::istreambuf_iterator<char>());
        Check(text.find("agentSkillTargetMask") == std::string::npos,
            "detected Agent Skill installations are not persisted as settings");
    }

    {
        std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
        invalid << "{\n"
                   "  \"animationMode\": 99,\n"
                   "  \"popupAnimationEffect\": -1,\n"
                   "  \"animationSpeed\": 99,\n"
                   "  \"animationFrameLimit\": 999,\n"
                   "  \"collectionPopupTheme\": 99,\n"
                   "  \"pageNavigationPreviousVirtualKey\": 999,\n"
                   "  \"pageNavigationNextVirtualKey\": -1,\n"
                   "  \"language\": \"system\"\n"
                   "}\n";
    }
    GeneralSettings clamped;
    Check(LoadGeneralSettings(path.c_str(), clamped) &&
            clamped.collectionPopupTheme == kFourThemeAcrylicLight &&
            clamped.pageNavigationPreviousVirtualKey == VK_PRIOR &&
            clamped.pageNavigationNextVirtualKey == VK_NEXT,
        "general settings reject invalid persisted themes and page keys");
    Check(clamped.animationMode == 0 && clamped.popupAnimationEffect == 2 &&
        clamped.animationSpeed == 1 && clamped.animationFrameLimit == 0,
        "invalid animation choices fall back without disabling the interface");

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "{\n"
                  "  \"agentSkillTargetMask\": 21,\n"
                  "  \"language\": \"system\"\n"
                  "}\n";
    }
    GeneralSettings migrated;
    Check(LoadGeneralSettings(path.c_str(), migrated),
        "legacy general settings still load");
    Check(migrated.animationMode == 0 && migrated.popupAnimationEffect == 2 &&
        migrated.animationSpeed == 1 && migrated.animationFrameLimit == 0 &&
        migrated.animationEnergySaver && !migrated.animationOnBattery,
        "legacy animation defaults preserve current effects and automatic cadence");
    Check(!migrated.demoModeEnabled &&
        !migrated.widgetDeveloperToolsEnabled &&
        migrated.collectionPopupTheme == kFourThemeDark &&
        migrated.pageNavigationKeyboardEnabled &&
        migrated.pageNavigationPreviousModifiers == 0 &&
        migrated.pageNavigationPreviousVirtualKey == VK_PRIOR &&
        migrated.pageNavigationNextModifiers == 0 &&
        migrated.pageNavigationNextVirtualKey == VK_NEXT,
        "legacy settings ignore the obsolete Agent Skill mask and preserve defaults");

    Check(FourThemeSelectionFromAppearancePreset(
            kAppearancePresetDark) == kFourThemeDark &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetLight) == kFourThemeLight &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetGlassDark) == kFourThemeAcrylicDark &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetAcrylicDark) == kFourThemeAcrylicDark &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetGlassLight) == kFourThemeAcrylicLight &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetAcrylicLight) == kFourThemeAcrylicLight,
        "six global themes map to the four overlay themes");
    Check(AppearancePresetFromFourThemeSelection(kFourThemeDark) ==
            kAppearancePresetDark &&
        AppearancePresetFromFourThemeSelection(kFourThemeLight) ==
            kAppearancePresetLight &&
        AppearancePresetFromFourThemeSelection(kFourThemeAcrylicDark) ==
            kAppearancePresetAcrylicDark &&
        AppearancePresetFromFourThemeSelection(kFourThemeAcrylicLight) ==
            kAppearancePresetAcrylicLight,
        "four overlay themes map back to stable appearance preset IDs");

    const auto darkPreset = PersonalizationSettings::DarkPreset();
    const auto lightPreset = PersonalizationSettings::LightPreset();
    const auto glassDarkPreset =
        PersonalizationSettings::GlassDarkPreset();
    const auto glassLightPreset =
        PersonalizationSettings::GlassLightPreset();
    const auto acrylicDarkPreset =
        PersonalizationSettings::AcrylicDarkPreset();
    const auto acrylicLightPreset =
        PersonalizationSettings::AcrylicLightPreset();
    Check(!darkPreset.widgetEdgeHighlightEnabled &&
            !lightPreset.widgetEdgeHighlightEnabled &&
            darkPreset.widgetBorderWidth == 1.0f &&
            lightPreset.widgetBorderWidth == 1.0f,
        "ordinary appearance presets keep a one-pixel border without edge highlight");
    Check(glassDarkPreset.widgetEdgeHighlightEnabled &&
            glassLightPreset.widgetEdgeHighlightEnabled &&
            acrylicDarkPreset.widgetEdgeHighlightEnabled &&
            acrylicLightPreset.widgetEdgeHighlightEnabled &&
            glassDarkPreset.widgetEdgeHighlightWidth ==
                1.25f &&
            acrylicLightPreset.widgetEdgeHighlightWidth ==
                1.2f &&
            glassDarkPreset.widgetBorderAlpha == 0.0f &&
            acrylicLightPreset.widgetBorderAlpha == 0.0f &&
            glassLightPreset.widgetEdgeHighlightStrength ==
                .38f &&
            acrylicDarkPreset.widgetEdgeHighlightStrength ==
                .30f,
        "glass and acrylic presets disable the border and load the recommended edge highlight");

    const auto quickNavigationAcrylic =
        MakeQuickNavigationAppearancePreset(
            kAppearancePresetAcrylicDark);
    const auto collectionPopupAcrylic =
        MakeCollectionPopupAppearancePreset(
            kAppearancePresetAcrylicDark);
    const auto collectionPopupDark =
        MakeCollectionPopupAppearancePreset(
            kAppearancePresetDark);
    Check(quickNavigationAcrylic.widgetBorderAlpha > 0.0f &&
            quickNavigationAcrylic.widgetEdgeHighlightEnabled &&
            collectionPopupAcrylic.widgetBorderAlpha == 0.0f &&
            collectionPopupAcrylic.widgetEdgeHighlightEnabled &&
            collectionPopupAcrylic.widgetEdgeHighlightWidth ==
                acrylicDarkPreset.widgetEdgeHighlightWidth &&
            collectionPopupAcrylic.widgetEdgeHighlightStrength ==
                acrylicDarkPreset.widgetEdgeHighlightStrength &&
            collectionPopupDark.widgetBorderAlpha > 0.0f &&
            !collectionPopupDark.widgetEdgeHighlightEnabled,
        "collection acrylic keeps the popup palette but replaces the Quick Navigation outline with an independent edge highlight");

    const auto personalizationPath =
        std::filesystem::temp_directory_path(error) /
        (L"SnowDesktopPersonalizationTests-" +
            std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(personalizationPath, error);
    PersonalizationSettings savedAppearance =
        PersonalizationSettings::DarkPreset();
    savedAppearance.backgroundPreset = kAppearancePresetCustom;
    savedAppearance.glassEnabled = false;
    savedAppearance.acrylicEnabled = false;
    savedAppearance.widgetBorderWidth = 3.5f;
    savedAppearance.widgetEdgeHighlightEnabled = true;
    savedAppearance.widgetEdgeHighlightWidth = 2.5f;
    savedAppearance.widgetEdgeHighlightStrength = 0.42f;
    savedAppearance.luaWidgetContentRowHeight = 34.0f;
    Check(!savedAppearance.showGroupTabCounts,
        "group tab file counts are opt-in for a new profile");
    savedAppearance.showGroupTabCounts = true;
    Check(!savedAppearance.scrollableTitleBarOnTop,
        "existing profiles default to a bottom title bar");
    savedAppearance.scrollableTitleBarOnTop = true;
    Check(!savedAppearance.popupHoverOpen,
        "popup hover opening defaults off for new profiles");
    Check(savedAppearance.popupHoverDelayMs == 600.0f,
        "new profiles retain the original 600 ms hover delay");
    savedAppearance.popupHoverOpen = true;
    savedAppearance.popupHoverDelayMs = 1200.0f;
    savedAppearance.showCategoryTabCounts = false;
    savedAppearance.edgeLight.direction = 127.f;
    savedAppearance.edgeLight.outerGlow = .4f;
    savedAppearance.edgeLight.glowThreshold = .99f;
    savedAppearance.panelGradient.enabled = true;
    savedAppearance.panelGradient.angle = 45;
    savedAppearance.panelGradient.start = .1;
    savedAppearance.panelGradient.end = .9;
    savedAppearance.panelGradient.stops.insert(savedAppearance.panelGradient.stops.begin() + 1, {.4, 0x102030, .2});
    Check(SavePersonalization(
            personalizationPath.c_str(), savedAppearance),
        "personalization save succeeds");
    PersonalizationSettings loadedAppearance;
    Check(LoadPersonalization(
            personalizationPath.c_str(), loadedAppearance) &&
            loadedAppearance.edgeLight == savedAppearance.edgeLight &&
            loadedAppearance.widgetBorderWidth == 3.5f &&
            loadedAppearance.widgetEdgeHighlightEnabled &&
            loadedAppearance.widgetEdgeHighlightWidth == 2.5f &&
            std::abs(loadedAppearance.widgetEdgeHighlightStrength -
                0.42f) < 0.0001f &&
            loadedAppearance.luaWidgetContentRowHeight == 34.0f &&
            loadedAppearance.showGroupTabCounts &&
            loadedAppearance.scrollableTitleBarOnTop &&
            loadedAppearance.popupHoverOpen &&
            loadedAppearance.popupHoverDelayMs == 1200.0f &&
            !loadedAppearance.showCategoryTabCounts &&
            !loadedAppearance.glassEnabled && loadedAppearance.panelGradient == savedAppearance.panelGradient &&
            loadedAppearance.gradientEndA == savedAppearance.gradientEndA,
        "appearance and Lua widget row height round trip independently");
    // Group counts must survive the acrylic preset refresh on load and must
    // remain independent from category counts, including an explicit off.
    for (const int preset : {kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight,
            kAppearancePresetGlassTransparent})
    {
        for (const bool enabled : {true, false})
        {
            auto appearance = MakeAppearancePreset(preset);
            appearance.showGroupTabCounts = enabled;
            appearance.scrollableTitleBarOnTop = enabled;
            appearance.popupHoverOpen = enabled;
            appearance.popupHoverDelayMs = 1400.0f;
            appearance.showCategoryTabCounts = !enabled;
            loadedAppearance.showGroupTabCounts = !enabled;
            Check(SavePersonalization(personalizationPath.c_str(), appearance) &&
                    LoadPersonalization(personalizationPath.c_str(), loadedAppearance) &&
                    loadedAppearance.showGroupTabCounts == enabled &&
                    loadedAppearance.scrollableTitleBarOnTop == enabled &&
                    loadedAppearance.popupHoverOpen == enabled &&
                    loadedAppearance.popupHoverDelayMs == 1400.0f &&
                    loadedAppearance.showCategoryTabCounts == !enabled,
                "group count preference survives material preset refresh independently from category counts");
        }
    }
    // Once the shared material object is saved, a preset ID is only a label;
    // restarting must preserve the user's actual fill, blur and reflection.
    for (const bool editedEdge : {false, true})
    {
        auto previousGlass = PersonalizationSettings::GlassTransparentPreset();
        previousGlass.widgetAlpha = 0.06f;
        previousGlass.glassBlurRadius = 24.0f;
        previousGlass.widgetEdgeHighlightWidth = editedEdge ? 3.0f : 1.25f;
        previousGlass.widgetEdgeHighlightStrength = editedEdge ? 0.60f : 0.45f;
        previousGlass.widgetEdgeHighlightEnabled = !editedEdge;
        previousGlass.cornerRadius = 32.0f;
        previousGlass.barHeight = 37.0f;
        previousGlass.luaWidgetContentRowHeight = 34.0f;
        previousGlass.contextMenuStyle = 6;

        Check(SavePersonalization(personalizationPath.c_str(), previousGlass) &&
                LoadPersonalization(personalizationPath.c_str(), loadedAppearance) &&
                loadedAppearance.widgetAlpha == previousGlass.widgetAlpha &&
                loadedAppearance.glassBlurRadius == previousGlass.glassBlurRadius &&
                loadedAppearance.contentTheme == 0 &&
                loadedAppearance.backgroundPreset == kAppearancePresetGlassTransparent &&
                loadedAppearance.widgetEdgeHighlightWidth == previousGlass.widgetEdgeHighlightWidth &&
                loadedAppearance.widgetEdgeHighlightStrength == previousGlass.widgetEdgeHighlightStrength &&
                loadedAppearance.widgetEdgeHighlightEnabled == !editedEdge &&
                loadedAppearance.cornerRadius == previousGlass.cornerRadius &&
                loadedAppearance.barHeight == previousGlass.barHeight &&
                loadedAppearance.luaWidgetContentRowHeight == previousGlass.luaWidgetContentRowHeight &&
                loadedAppearance.contextMenuStyle == 6,
            "saved material parameters survive restart without preset-ID overrides");
    }
    // Persist through a non-custom theme as well: applying a preset must not
    // discard the independent context-menu selection.
    for (int style = 0; style <= 6; ++style)
    {
        auto menuAppearance = PersonalizationSettings::LightPreset();
        menuAppearance.contextMenuStyle = style;
        Check(SavePersonalization(personalizationPath.c_str(), menuAppearance) &&
                LoadPersonalization(personalizationPath.c_str(), loadedAppearance) &&
                loadedAppearance.contextMenuStyle == style,
            "all existing and Win10 menu styles survive save/load with a theme preset");
    }
    Check(SavePersonalization(personalizationPath.c_str(), savedAppearance),
        "restore the valid gradient fixture before testing rejected writes");
    auto invalidAppearance = savedAppearance;
    invalidAppearance.panelGradient.stops[1].position = 1;
    Check(!SavePersonalization(personalizationPath.c_str(), invalidAppearance) &&
        LoadPersonalization(personalizationPath.c_str(), loadedAppearance) &&
        loadedAppearance.panelGradient == savedAppearance.panelGradient,
        "invalid gradient stops are rejected before truncating the last valid appearance file");
    snowdesktop::PanelGradient direction;
    direction.angle = 0; direction.start = .25; direction.end = .75;
    const auto horizontal = snowdesktop::ResolvePanelGradientLine(direction, 200, 100);
    direction.angle = 90;
    const auto vertical = snowdesktop::ResolvePanelGradientLine(direction, 200, 100);
    Check(std::abs(horizontal.x1 - 50) < .0001 && std::abs(horizontal.x2 - 150) < .0001 &&
        std::abs(horizontal.y1 - 50) < .0001 && std::abs(vertical.x1 - 100) < .0001 &&
        std::abs(vertical.y1 - 25) < .0001 && std::abs(vertical.y2 - 75) < .0001,
        "gradient direction and percentage range use the full panel axes after resizing");

    {
        std::ofstream legacyGlass(
            personalizationPath, std::ios::binary | std::ios::trunc);
        legacyGlass << "{\n"
                       "  \"backgroundPreset\": 9,\n"
                       "  \"glassEnabled\": true\n"
                       "}\n";
    }
    PersonalizationSettings migratedGlass;
    migratedGlass.showGroupTabCounts = true;
    migratedGlass.scrollableTitleBarOnTop = true;
    migratedGlass.popupHoverOpen = true;
    migratedGlass.popupHoverDelayMs = 2200.0f;
    migratedGlass.panelGradient = savedAppearance.panelGradient;
    Check(LoadPersonalization(personalizationPath.c_str(), migratedGlass) &&
            !migratedGlass.showGroupTabCounts &&
            !migratedGlass.scrollableTitleBarOnTop &&
            !migratedGlass.popupHoverOpen &&
            migratedGlass.popupHoverDelayMs == 600.0f &&
            migratedGlass.widgetEdgeHighlightEnabled &&
            migratedGlass.widgetEdgeHighlightWidth ==
                kDefaultEdgeHighlightWidth &&
            migratedGlass.widgetEdgeHighlightStrength ==
                kDefaultEdgeHighlightStrength &&
            migratedGlass.widgetBorderWidth == 1.0f &&
            migratedGlass.widgetBorderAlpha == 0.0f && !migratedGlass.panelGradient.enabled,
        "legacy glass appearance migrates to an independent edge highlight");
    // Loading old, hand-edited or out-of-range profiles must never turn the
    // delay into an immediate popup or a practically infinite wait.
    for (const auto& [serialized, expected] : {
            std::pair{"-100", 100.0f}, std::pair{"99999", 3000.0f},
            std::pair{"1e300", 3000.0f}, std::pair{"\"invalid\"", 600.0f},
            std::pair{"450.4", 450.0f}})
    {
        {
            std::ofstream fixture(personalizationPath, std::ios::binary | std::ios::trunc);
            fixture << "{\"popupHoverDelayMs\":" << serialized << "}";
        }
        Check(LoadPersonalization(personalizationPath.c_str(), loadedAppearance) &&
                loadedAppearance.popupHoverDelayMs == expected,
            "persisted hover delay is bounded and invalid values use the default");
    }
    for (const auto& [requested, expected] : {
            std::pair{-1.0f, 100.0f}, std::pair{9999.0f, 3000.0f}})
    {
        auto appearance = savedAppearance;
        appearance.popupHoverDelayMs = requested;
        Check(SavePersonalization(personalizationPath.c_str(), appearance) &&
                LoadPersonalization(personalizationPath.c_str(), loadedAppearance) &&
                loadedAppearance.popupHoverDelayMs == expected,
            "saved hover delay respects the supported range");
    }
    {
        std::ofstream legacyOpaque(
            personalizationPath, std::ios::binary | std::ios::trunc);
        legacyOpaque << "{\n"
                        "  \"backgroundPreset\": 9,\n"
                        "  \"glassEnabled\": false\n"
                        "}\n";
    }
    PersonalizationSettings migratedOpaque;
    Check(LoadPersonalization(personalizationPath.c_str(), migratedOpaque) &&
            !migratedOpaque.widgetEdgeHighlightEnabled &&
            migratedOpaque.widgetBorderWidth == 1.0f,
        "legacy non-glass appearance keeps the ordinary border only");
    {
        std::ofstream explicitAcrylic(
            personalizationPath, std::ios::binary | std::ios::trunc);
        explicitAcrylic << "{\n"
                           "  \"backgroundPreset\": 10,\n"
                           "  \"glassEnabled\": true,\n"
                           "  \"acrylicEnabled\": true,\n"
                           "  \"widgetBorderStyle\": 1,\n"
                           "  \"widgetBorderWidth\": 99,\n"
                           "  \"widgetEdgeHighlightEnabled\": false,\n"
                           "  \"widgetEdgeHighlightWidth\": 99,\n"
                           "  \"widgetEdgeHighlightStrength\": -1,\n"
                           "  \"luaWidgetTitleAreaHeight\": 999\n"
                           "}\n";
    }
    PersonalizationSettings explicitAppearance;
    Check(LoadPersonalization(
            personalizationPath.c_str(), explicitAppearance) &&
            !explicitAppearance.widgetEdgeHighlightEnabled &&
            explicitAppearance.widgetBorderWidth ==
                kMaximumWidgetBorderWidth &&
            explicitAppearance.widgetEdgeHighlightWidth ==
                kMaximumWidgetBorderWidth &&
            explicitAppearance.widgetEdgeHighlightStrength == 0.0f &&
            explicitAppearance.luaWidgetContentRowHeight == 48.0f,
        "legacy Lua title-area height migrates to the clamped content row height");
    std::filesystem::remove(personalizationPath, error);

    std::filesystem::remove(path, error);
    if (failures == 0)
        std::cout << "All general settings tests passed.\n";
    return failures == 0 ? 0 : 1;
}
