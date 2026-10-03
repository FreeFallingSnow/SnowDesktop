#include "theme_preview.h"
#include "theme_workshop.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>

namespace
{
struct Difference { unsigned maximum = 0; std::size_t pixels = 0, material = 0; bool alpha = true; };
Difference Compare(const snowdesktop::widget_preview::Wallpaper& a, const snowdesktop::widget_preview::Wallpaper& b)
{
    Difference result;
    if (a.width != 1024 || a.height != 1024 || b.width != a.width || b.height != a.height ||
        a.pixels.size() != 1024 * 1024 || b.pixels.size() != a.pixels.size())
    { result.alpha = false; result.maximum = 255; return result; }
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
    {
        unsigned maximum = 0;
        for (unsigned shift : {0u, 8u, 16u})
        {
            const auto x = (a.pixels[i] >> shift) & 255u, y = (b.pixels[i] >> shift) & 255u;
            maximum = std::max(maximum, x > y ? x - y : y - x);
        }
        result.maximum = std::max(result.maximum, maximum);
        if (maximum) ++result.pixels;
        if (maximum > 2) ++result.material;
        result.alpha &= (a.pixels[i] >> 24) == (b.pixels[i] >> 24);
    }
    return result;
}
}

int RunThemeWorkshopTests(const std::filesystem::path&);
int wmain(int argc, wchar_t** argv)
{
    using namespace snowdesktop;
    using namespace themes;
    // One-shot mock children exercise the actual bounded process runner.
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--child-wait") { Sleep(5000); return 0; }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"configuration")
    { std::cout << "{\"ok\":true,\"protocolVersion\":1,\"expectedAppId\":5080330,\"version\":\"test\",\"steamworksCompiled\":true,\"themeWorkflowProtocolVersion\":1,\"capabilities\":[\"workshop.theme.v1\"]}\n"; return 0; }
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << "FAIL preview: " << message << '\n'; } };
    const auto directory = std::filesystem::temp_directory_path() / (L"SnowDesktop-theme-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(directory);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{directory};
    failures += RunThemeWorkshopTests(directory);
    Theme quick = Capture(Kind::QuickPanel, MakeQuickNavigationAppearancePreset(kAppearancePresetAcrylicDark));
    quick.id = "theme/quick"; quick.name = "Quick";
    Theme popup = Capture(Kind::Popup, MakeCollectionPopupAppearancePreset(kAppearancePresetLight));
    popup.id = "theme/popup"; popup.name = "Popup";
    Theme global = Capture(Kind::Global, MakeAppearancePreset(kAppearancePresetGlassLight));
    global.id = "theme/global"; global.name = "Global"; global.quickPanel = quick.id; global.popup = popup.id;
    Package package{{global.id,global},{quick.id,quick},{popup.id,popup}};
    std::string error;
    check(preview::Parts(package, global.id, All, error).size() == 6, "full scope uses all production surfaces and bound themes");
    check(preview::Parts(package, global.id, Dock, error).size() == 1, "Dock entry respects target scope");
    auto missing = package; missing.erase(quick.id);
    check(preview::Parts(missing, global.id, All, error).empty(), "missing dependency rejects complete preview");
    auto noise = widget_preview::GenerateWallpaper(1024,1024,false);
    std::uint32_t seed = 19;
    for (auto& pixel : noise.pixels) { seed = seed * 1664525u + 1013904223u; pixel = 0xff000000u | (seed & 0xffffffu); }
    check(preview::SaveCover(directory / L"noise.png", std::move(noise), error) &&
        std::filesystem::file_size(directory / L"noise.png") < preview::kCoverMaximumBytes, "worst-case custom cover is encoded below the strict bridge limit");
    check(preview::NormalizeCover(directory / L"noise.png",directory / L"normalized.png",error) &&
        widget_preview::LoadWallpaperImage(directory / L"normalized.png").pixels.size()==1024*1024,
        "custom cover normalizes through a bounded 1024-square decoder");
    { std::ofstream oversized(directory / L"oversized.png",std::ios::binary); oversized << "invalid"; }
    std::filesystem::resize_file(directory / L"oversized.png",32 * 1024 * 1024 + 1);
    check(!preview::NormalizeCover(directory / L"oversized.png",directory / L"invalid-cover.png",error) &&
        !std::filesystem::exists(directory / L"invalid-cover.png"), "oversized or invalid custom source cannot allocate an unbounded image or produce a cover");
    std::string output;
    check(preview::Run(argv[0], {L"configuration"}, output, 3000, error) && workshop::Capabilities(output,"test"), "mock child exercises actual bounded runner and capability decoder");
    check(!preview::Run(argv[0], {L"--child-wait"}, output, 30, error) && error == "processTimeout", "request timeout terminates only its child");
    std::atomic_bool cancel{true};
    check(!preview::Run(argv[0], {L"--child-wait"}, output, 3000, error, &cancel) && error == "cancelled", "pre-cancel never starts a child");
    std::filesystem::path cover = directory / L"noise.png";
    if (argc == 2)
    {
        const auto cli = directory / L"cli"; std::filesystem::create_directory(cli);
        const auto bridge = std::filesystem::path(argv[1]).parent_path() / L"SnowDesktopSteamBridge.exe";
        check(WritePackage(cli / L"package.snowtheme",package,error) &&
            preview::SaveCover(cli / L"cover.png",widget_preview::GenerateWallpaper(1024,1024,false),error) &&
            steam_bridge::WriteThemePreparation(cli,global.id,global.name,1700000000,error), "CLI fixture preparation");
        check(preview::Run(bridge,{L"workshop",L"theme-plan",L"--prepared",cli.wstring(),L"--data-directory",directory.wstring()},output,3000,error) &&
            output.find("themeWorkflowProtocolVersion")!=std::string::npos, "actual bridge theme-plan validates offline prepared artifacts");
        check(!preview::Run(bridge,{L"workshop",L"theme-publish",L"--prepared",cli.wstring(),L"--data-directory",directory.wstring(),
            L"--package-sha256",L"changed",L"--cover-sha256",L"changed",L"--confirm-create"},output,3000,error) &&
            output.find("stalePreparation")!=std::string::npos, "actual publish CLI rejects changed hashes before any Steam initialization");
        DWORD handlesBefore = 0, handlesAfter = 0; GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        const bool first = preview::Render(argv[1],package,global.id,All,directory / L"first",cover,error);
        check(first, ("actual immutable production render: " + error).c_str());
        const auto firstImage = first ? widget_preview::LoadWallpaperImage(cover) : widget_preview::Wallpaper{};
        const auto frozenHash = steam_bridge::ThemeFileSha256(directory / L"first" / L"package.snowtheme");
        const bool second = preview::Render(argv[1],package,global.id,All,directory / L"second",cover,error);
        const auto repeated = Compare(firstImage, second ? widget_preview::LoadWallpaperImage(cover) : widget_preview::Wallpaper{});
        std::cout << "Repeated production cover: differing pixels=" << repeated.pixels << ", maximum channel difference=" << repeated.maximum << '\n';
        // Independent production D2D runs can differ by sparse 8-bit edge
        // rounding. This narrow bound still rejects changed data or materials.
        check(second && repeated.alpha && repeated.maximum <= 2 && repeated.pixels <= 1024 &&
            steam_bridge::ThemeFileSha256(directory / L"second" / L"package.snowtheme") == frozenHash,
            "same frozen package repeats within sparse two-channel-value edge rounding");
        GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
        check(handlesAfter <= handlesBefore + 4, "completed requests release child and file handles without a retained bitmap cache");
        auto changed = package;
        changed.at(quick.id).layout.expandedWidth += 160; changed.at(quick.id).colors["searchText"] = "#ff0000";
        const bool changedOk = preview::Render(argv[1],changed,global.id,All,directory / L"changed",cover,error);
        const auto changedImage = Compare(firstImage, changedOk ? widget_preview::LoadWallpaperImage(cover) : widget_preview::Wallpaper{});
        check(changedOk && changedImage.material > 10000 && changedImage.maximum > 128,
            "bound child layout and color cause substantial changes in the real renderer");
        const bool taskbarOk = preview::Render(argv[1],package,global.id,Taskbar,directory / L"taskbar",cover,error);
        const auto taskbarImage = taskbarOk ? widget_preview::LoadWallpaperImage(cover) : widget_preview::Wallpaper{};
        auto material = package;
        auto& surface = material.at(global.id).appearance;
        surface.glassEnabled = surface.acrylicEnabled = surface.panelGradient.enabled = false;
        surface.widgetAlpha = 1; surface.widgetBgR = .9f; surface.widgetBgG = .15f; surface.widgetBgB = .2f;
        surface.widgetBorderAlpha = 1; surface.widgetBorderWidth = 3;
        surface.widgetBorderR = .1f; surface.widgetBorderG = surface.widgetBorderB = 1;
        const bool materialOk = preview::Render(argv[1],material,global.id,Taskbar,directory / L"taskbar-material",cover,error);
        const auto materialImage = Compare(taskbarImage, materialOk ? widget_preview::LoadWallpaperImage(cover) : widget_preview::Wallpaper{});
        check(taskbarOk && materialOk && materialImage.material > 10000 && materialImage.maximum > 128,
            "direct taskbar preview uses global material and physical-edge production rendering");
        check(!preview::Render(argv[1],package,global.id,All,directory / L"first",cover,error) && cover.empty(), "duplicate directory never returns an old cover");
        cover = directory / L"noise.png";
        check(!preview::Render(argv[1],package,global.id,All,directory / L"cancelled",cover,error,&cancel) && cover.empty() &&
            !std::filesystem::exists(directory / L"cancelled"), "cancelled rendering discards package and cover together");
        check(!preview::Render(directory / L"missing.exe",package,global.id,All,directory / L"failed",cover,error) && cover.empty() &&
            !std::filesystem::exists(directory / L"failed"), "renderer failure cannot publish stale output");
    }
    else check(false,"host executable argument is required");
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (!failures) std::cout << "Theme preview and Workshop workflow tests passed\n";
    return failures ? 1 : 0;
}
