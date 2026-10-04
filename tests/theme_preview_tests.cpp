#include "theme_preview.h"
#include "json_value.h"
#include "theme_workshop_tags.h"
#include "winui/theme_edit_state.h"
#include "theme_workshop.h"
#include "preview_png_writer.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

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
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--child-wait")
    {
        if (argc > 2) { std::ofstream marker{std::filesystem::path(argv[2])}; marker << "ready"; }
        Sleep(5000); return 0;
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"configuration")
    { std::cout << "{\"ok\":true,\"protocolVersion\":1,\"expectedAppId\":5080330,\"version\":\"test\",\"steamworksCompiled\":true,\"themeWorkflowProtocolVersion\":1,\"capabilities\":[\"workshop.theme.v1\",\"workshop.theme.tags.v1\",\"workshop.theme.gallery.v1\",\"workshop.theme.color-alpha.v1\"]}\n"; return 0; }
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
    check(preview::Parts(package, global.id, All, error).size() == 7, "full scope uses mapped-folder, bars and bound quick/popup/control-center production surfaces");
    auto bars = package; bars.at(global.id).scopes = Bars;
    const auto barsParts = preview::Parts(bars, global.id, Bars, error);
    check(barsParts.size() == 7 && barsParts.front().component == "folder-mapping" && bars.at(global.id).scopes == Bars,
        "full global mapped-folder appearance sample does not expand the applied bar scope");
    const auto popupParts = preview::Parts(package, popup.id, All, error);
    check(popupParts.size() == 2 && popupParts[0].component == "popup" && popupParts[1].component == "control-panel" && popupParts[1].themeId == popup.id,
        "popup themes also preview the actual control center with the same popup appearance");
    check(preview::Parts(package, global.id, Dock, error).size() == 1, "Dock entry respects target scope");
    auto partial = global; partial.scopes = Dock | StatusBar; partial.quickPanel.clear(); partial.popup.clear();
    Package partialPackage{{partial.id, partial}};
    const auto partialParts = preview::Parts(partialPackage, partial.id, partial.scopes, error);
    check(partialParts.size() == 2 && partialParts[0].component == "dock" && partialParts[1].component == "status-bar",
        "partial global previews require only their selected bars and no child bindings");
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
    std::atomic_bool inFlightCancel{false}, childReady{false};
    const auto marker = directory / L"child-ready";
    DWORD cancellationHandlesBefore = 0, cancellationHandlesAfter = 0;
    GetProcessHandleCount(GetCurrentProcess(), &cancellationHandlesBefore);
    const auto cancellationStarted = std::chrono::steady_clock::now();
    std::thread cancelRunning([&] {
        while (std::chrono::steady_clock::now() - cancellationStarted < std::chrono::seconds(5))
        {
            std::error_code ec;
            if (std::filesystem::exists(marker, ec))
            { childReady = true; inFlightCancel = true; return; }
            Sleep(1);
        }
    });
    const bool cancelledChild = !preview::Run(argv[0], {L"--child-wait", marker.wstring()}, output, 3000, error, &inFlightCancel);
    cancelRunning.join();
    GetProcessHandleCount(GetCurrentProcess(), &cancellationHandlesAfter);
    check(childReady && cancelledChild && error == "cancelled" &&
        std::chrono::steady_clock::now() - cancellationStarted < std::chrono::seconds(3) &&
        cancellationHandlesAfter <= cancellationHandlesBefore,
        "in-flight cancellation stops the ready private child and releases all process/file handles");
    std::filesystem::path cover = directory / L"noise.png";
    if (argc == 2)
    {
        const auto cli = directory / L"cli"; std::filesystem::create_directory(cli);
        const auto bridge = std::filesystem::path(argv[1]).parent_path() / L"SnowDesktopSteamBridge.exe";
        check(WritePackage(cli / L"package.snowtheme",package,error) &&
            preview::SaveCover(cli / L"cover.png",widget_preview::GenerateWallpaper(1024,1024,false),error), "CLI fixture package and cover");
        for (const auto& part : preview::Parts(package,global.id,global.scopes,error))
            std::filesystem::copy_file(cli / L"cover.png",cli / preview::GalleryFilename(steam_bridge::ThemeSha256(global.id),part.component));
        check(
            steam_bridge::WriteThemePreparation(cli,global.id,global.name,1700000000,error,tags::Applicable(global)), "CLI fixture preparation");
        check(preview::Run(bridge,{L"workshop",L"theme-plan",L"--prepared",cli.wstring(),L"--data-directory",directory.wstring()},output,3000,error) &&
            output.find("themeWorkflowProtocolVersion")!=std::string::npos, "actual bridge theme-plan validates offline prepared artifacts");
        check(!preview::Run(bridge,{L"workshop",L"theme-publish",L"--prepared",cli.wstring(),L"--data-directory",directory.wstring(),
            L"--package-sha256",L"changed",L"--cover-sha256",L"changed",L"--confirm-create"},output,3000,error) &&
            output.find("stalePreparation")!=std::string::npos, "actual publish CLI rejects changed hashes before any Steam initialization");
        DWORD handlesBefore = 0, handlesAfter = 0; GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        const auto productionUiDirectory = winui::theme_controls::TaskDirectory(directory, CreateId());
        const bool first = preview::Render(argv[1],package,global.id,All,productionUiDirectory,cover,error);
        check(first, ("actual immutable production render: " + error).c_str());
        const auto firstImage = first ? widget_preview::LoadWallpaperImage(cover) : widget_preview::Wallpaper{};
        const auto frozenHash = steam_bridge::ThemeFileSha256(productionUiDirectory / L"package.snowtheme");
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
        check(!preview::Render(argv[1],package,global.id,All,productionUiDirectory,cover,error) && cover.empty(), "duplicate directory never returns an old cover");
        cover = directory / L"noise.png";
        check(!preview::Render(argv[1],package,global.id,All,directory / L"cancelled",cover,error,&cancel) && cover.empty() &&
            !std::filesystem::exists(directory / L"cancelled"), "cancelled rendering discards package and cover together");
        check(!preview::Render(directory / L"missing.exe",package,global.id,All,directory / L"failed",cover,error) && cover.empty() &&
            !std::filesystem::exists(directory / L"failed"), "renderer failure cannot publish stale output");
        std::vector<preview::Image> gallery;
        const auto galleryDirectory = directory / L"gallery";
        const bool galleryOk = preview::RenderGallery(argv[1],package,global.id,All,galleryDirectory,gallery,cover,error);
        check(galleryOk && gallery.size() == 7 && gallery.front().component == "folder-mapping" &&
            gallery.back().component == "control-panel", "production gallery reuses actual mapped-folder and control-center renderers");
        if (galleryOk)
        {
            const auto builtinBackground = galleryDirectory / L"builtin-background.png";
            check(std::filesystem::is_regular_file(builtinBackground) &&
                steam_bridge::ThemeFileSha256(builtinBackground) ==
                    steam_bridge::ThemeFileSha256(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        L"assets" / L"preview" / L"theme-background.png"),
                "default wallpaper comes from the production executable's embedded approved asset");
            check(Compare(widget_preview::LoadWallpaperImage(galleryDirectory / L"background.png"),
                widget_preview::GenerateWallpaper(1024,1024,false)).material > 10000,
                "approved default replaces the procedural wallpaper in the real production stage");
            for (const auto& image : gallery)
                check(widget_preview::LoadWallpaperImage(image.path).pixels.size() == 1024*1024 &&
                    std::filesystem::file_size(image.path) < preview::kCoverMaximumBytes, "each independent gallery image is complete and fits Steam's limit");
            check(steam_bridge::ThemeFileSha256(cover) == steam_bridge::ThemeFileSha256(gallery.front().path), "default cover is one gallery image, never a composite");
            const auto folder = widget_preview::LoadWallpaperImage(gallery.front().path);
            const auto control = widget_preview::LoadWallpaperImage(gallery.back().path);
            check(Compare(folder,control).material > 10000, "component and popup control-center previews are distinct rendered images");
            const auto directDirectory = directory / L"direct-control-center";
            const auto directResult = directory / L"direct-control-center.json";
            const bool directOk = preview::Run(argv[1],{L"--native-component-preview",L"control-panel",directDirectory.wstring(),
                L"96",L"en-US",L"dark",(galleryDirectory / L"background.png").wstring(),L"1024",L"1024",L"32",L"0",L"0",
                directResult.wstring(),(galleryDirectory / L"package.snowtheme").wstring(),
                std::wstring(popup.id.begin(),popup.id.end())},output,60000,error);
            std::ifstream directFile(directResult,std::ios::binary);
            const std::string directText(std::istreambuf_iterator<char>(directFile),{});
            JsonValue directJson;
            bool sameStage = false;
            if (directOk && ParseJson(directText,directJson))
            {
                const auto* outputs = directJson.Find("outputs");
                if (outputs && outputs->IsArray() && !outputs->array.empty())
                    if (const auto* path = outputs->array.front().Find("path"); path && path->IsString())
                    {
                        const auto directCover = directory / L"direct-control-cover.png";
                        const auto direct = widget_preview::LoadWallpaperImage(
                            std::filesystem::path(std::u8string(path->string.begin(),path->string.end())));
                        const auto& metadata = outputs->array.front();
                        const auto value = [&](const char* key) {
                            const auto* number = metadata.Find(key);
                            return number && number->IsNumber() ? static_cast<int>(number->number) : 0;
                        };
                        const int width = value("componentWidth"), height = value("componentHeight");
                        const int side = std::max(width,height) + 48;
                        const int x = value("placementX") + width / 2 - side / 2;
                        const int y = value("placementY") + height / 2 - side / 2;
                        std::vector<std::uint32_t> scene;
                        if (side > 0 && x >= 0 && y >= 0 && x + side <= direct.width && y + side <= direct.height)
                            for (int row = y; row < y + side; ++row)
                                scene.insert(scene.end(), direct.pixels.begin() + row * direct.width + x,
                                    direct.pixels.begin() + row * direct.width + x + side);
                        const auto rawScene = directory / L"direct-control-camera.png";
                        if (!scene.empty() && preview_png::Save(rawScene,side,side,scene,error) &&
                            preview::NormalizeCover(rawScene,directCover,error))
                        {
                            const auto comparison = Compare(control,widget_preview::LoadWallpaperImage(directCover));
                            sameStage = comparison.alpha && comparison.maximum <= 2 && comparison.pixels <= 1024;
                        }
                    }
            }
            check(sameStage,"tight gallery camera preserves the complete production glass and background scene pixel for pixel");
            // The original offscreen control center ignored blur radius: both
            // artifacts were identical because it drew tint without a backdrop.
            auto patterned = widget_preview::GenerateWallpaper(1024,1024,false);
            for (int y = 0; y < 1024; ++y) for (int x = 0; x < 1024; ++x)
                patterned.pixels[y * 1024 + x] = ((x / 32 + y / 32) % 2) ? 0xffd0e0f0u : 0xff203050u;
            const auto glassBackground = directory / L"glass-background.png";
            check(preview::SaveCover(glassBackground,std::move(patterned),error), "known high-frequency glass backdrop");
            std::array<widget_preview::Wallpaper,2> glassImages;
            for (int sample = 0; sample < 2; ++sample)
            {
                auto glassPackage = package;
                auto& glass = glassPackage.at(popup.id).appearance;
                glass.glassEnabled = true; glass.acrylicEnabled = false; glass.widgetAlpha = .12f;
                glass.glassBlurRadius = sample ? 48.f : 4.f;
                const auto glassSnapshot = directory / (L"glass-" + std::to_wstring(sample) + L".snowtheme");
                const auto glassDirectory = directory / (L"glass-" + std::to_wstring(sample));
                const auto glassResult = directory / (L"glass-" + std::to_wstring(sample) + L".json");
                const bool rendered = WritePackage(glassSnapshot,glassPackage,error) && preview::Run(argv[1],
                    {L"--native-component-preview",L"control-panel",glassDirectory.wstring(),L"96",L"en-US",L"dark",
                        glassBackground.wstring(),L"1024",L"1024",L"32",L"0",L"0",glassResult.wstring(),glassSnapshot.wstring(),
                        std::wstring(popup.id.begin(),popup.id.end())},output,60000,error);
                check(rendered,"production control-center glass sample renders without opening settings");
                if (rendered) glassImages[sample] = widget_preview::LoadWallpaperImage(glassDirectory / L"control-panel-overview.png");
            }
            const auto blurred = Compare(glassImages[0],glassImages[1]);
            check(blurred.alpha && blurred.material > 10000 && blurred.maximum > 32,
                "control-center blur radius changes the actual frozen backdrop rather than just the tint");
            check(!glassImages[0].pixels.empty() && !glassImages[1].pixels.empty() &&
                glassImages[0].pixels.front() == glassImages[1].pixels.front() &&
                glassImages[0].pixels.back() == glassImages[1].pixels.back(),
                "control-center glass preserves the surrounding sharp wallpaper");
        }
        auto backdrop = widget_preview::GenerateWallpaper(1024,1024,false);
        std::fill(backdrop.pixels.begin(),backdrop.pixels.end(),0xff306090u);
        const auto importedBackground = directory / L"imported-background.png";
        check(preview::SaveCover(importedBackground,std::move(backdrop),error), "imported background fixture");
        const auto backgroundHash = steam_bridge::ThemeFileSha256(importedBackground);
        const auto importedDirectory = directory / L"gallery-imported-background";
        std::vector<preview::Image> importedGallery;
        const bool importedOk = preview::RenderGallery(argv[1],package,global.id,All,importedDirectory,
            importedGallery,cover,error,nullptr,importedBackground);
        check(importedOk && importedGallery.size() == 7 &&
            steam_bridge::ThemeFileSha256(importedBackground) == backgroundHash &&
            steam_bridge::ThemeFileSha256(importedDirectory / L"package.snowtheme") == frozenHash,
            "imported background renders all surfaces without writing the source image or changing theme scope/data");
        if (importedOk)
        {
            for (const auto& image : importedGallery)
            {
                const auto rendered = widget_preview::LoadWallpaperImage(image.path);
                check(rendered.pixels.size() == 1024*1024 && rendered.pixels.front() == 0xff306090u &&
                    rendered.pixels.back() == 0xff306090u && std::filesystem::file_size(image.path) < preview::kCoverMaximumBytes,
                    "every independent preview shares the selected imported backdrop and respects Steam's image limit");
            }
            check(Compare(widget_preview::LoadWallpaperImage(gallery.front().path),
                widget_preview::LoadWallpaperImage(importedGallery.front().path)).material > 10000,
                "imported background changes the actual mapped-folder production preview");
            check(steam_bridge::ThemeFileSha256(cover) == steam_bridge::ThemeFileSha256(importedGallery.front().path),
                "imported backdrop still uses a single gallery image as the main cover");
        }
        check(!preview::RenderGallery(argv[1],package,global.id,All,directory / L"gallery-invalid-background",
            importedGallery,cover,error,nullptr,directory / L"oversized.png") && importedGallery.empty() &&
            cover.empty() && !std::filesystem::exists(directory / L"gallery-invalid-background"),
            "invalid imported background fails atomically and cannot reuse an older preview");
        check(!preview::RenderGallery(argv[1],package,global.id,All,directory / L"gallery-cancel",gallery,cover,error,&cancel) &&
            gallery.empty() && cover.empty() && !std::filesystem::exists(directory / L"gallery-cancel"), "cancelled gallery releases every image and draft artifact");
        check(!preview::RenderGallery(directory / L"missing.exe",package,global.id,All,directory / L"gallery-missing-resource",
            gallery,cover,error) && gallery.empty() && cover.empty() &&
            !std::filesystem::exists(directory / L"gallery-missing-resource"),
            "missing embedded wallpaper fails atomically without a developer-file fallback");
    }
    else check(false,"host executable argument is required");
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (!failures) std::cout << "Theme preview and Workshop workflow tests passed\n";
    return failures ? 1 : 0;
}
