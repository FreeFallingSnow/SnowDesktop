#pragma once
#include "theme/theme_library_settings.h"
#include "data/layout_storage.h"
#include "native_icon_cold_pixels_cases.h"

namespace native_icon_customization_cases
{
using namespace native_drag_commit_cases;

template<class Predicate> void Wait(DesktopApp& app, Predicate predicate, const char* message)
{
    const auto deadline = GetTickCount64() + 10000;
    do
    {
        Access::DrainIconCustomizations(app);
        if (predicate()) return;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    Require(false, message);
}

inline void Run()
{
    Profile profile;
    auto& app = profile.Model(false, profile.root / L"icon-layout");
    const auto directory = profile.root / L"icon-sources";
    fs::create_directories(directory);
    const auto file = directory / L"original.txt", unloaded = directory / L"unopened.txt";
    { std::ofstream(file) << "original"; std::ofstream(unloaded) << "unopened"; }
    Access::PopulateModel(app, {file}, 0);
    const auto target = L"item:" + ToUpperInvariant(file.wstring());
    const auto unloadedTarget = L"item:" + ToUpperInvariant(unloaded.wstring());
    snowdesktop::icon_image::Image image;
    image.width = 96; image.height = 40;
    image.pixels.assign(96 * 40, 0xffff0000);
    std::string png, error;
    Require(snowdesktop::icon_image::EncodePng(image, png, error), "encode a real rectangular library source");
    const auto source = directory / L"thumbnail.png";
    { std::ofstream output(source, std::ios::binary); output.write(png.data(), static_cast<std::streamsize>(png.size())); }
    const fs::path libraryRoot(GetDataSubdirectoryPath(L"icon-library"));
    snowdesktop::IconLibrary library(libraryRoot);
    snowdesktop::IconLibraryRequest import; import.action = "import"; import.paths = {source.wstring()};
    const auto imported = library.Invoke(import);
    Require(imported.succeeded && imported.imported.size() == 1, "import the actual icon-library asset before binding it");
    const auto asset = imported.imported.front();
    Access::SeedIconCustomization(app, target, asset);
    Access::SeedIconCustomization(app, unloadedTarget, {});
    Require(Access::Save(app), "save actual active icon customizations");
    const auto active = Access::ReadIconAppearances(app);
    Require(active.succeeded && active.entries.size() == 2 && std::any_of(active.entries.begin(), active.entries.end(),
        [&](const auto& record) { return record.target == target && record.available; }),
        "the real appearance read preserves existing unloaded-folder records until deletion is confirmed");
    snowdesktop::IconLibrarySnapshot snapshot;
    Wait(app, [&] { snapshot = Access::ReadIconLibrary(app); return snapshot.succeeded && !snapshot.busy && !snapshot.entries.empty(); },
        "load actual icon-library read response");
    Require(snapshot.entries[0].uses.size() == 1, "the active target appears as an applied library use");

    fs::remove(file);
    Access::ProbeIconCustomizations(app);
    Wait(app, [&] { return Access::MissingIconTargets(app).contains(target); }, "deliver confirmed deletion through the real background host service");
    const auto since = Access::MissingIconTargets(app).at(target);
    Require(!Access::ApplyRetainedIcon(app, target, asset) && !Access::BeginRetainedIconAppearance(app, target) &&
        Access::MissingIconTargets(app).at(target) == since,
        "an old picker or stale model cannot edit a confirmed-missing target or extend its recovery deadline");
    Require(!Access::MissingIconTargets(app).contains(unloadedTarget), "a real existing file outside loaded folders never starts expiry");
    const auto pendingStyles = Access::ReadIconAppearances(app);
    Require(pendingStyles.entries.size() == 1 && pendingStyles.entries[0].target == unloadedTarget,
        "pending targets disappear from the actual independent-style response while existing unloaded entries remain");
    snapshot = Access::ReadIconLibrary(app);
    Require(snapshot.entries[0].uses.empty(), "pending targets no longer mark the actual library asset as applied");
    snowdesktop::layout_storage::Document saved;
    Require(snowdesktop::layout_storage::LoadDocument(Access::LayoutPath(app), saved).status ==
        snowdesktop::layout_storage::LoadStatus::LoadedPrimary && saved.missingIconTargets.size() == 1 &&
        saved.missingIconTargets[0].missingSince == since, "the host persisted its original deletion deadline");
    Access::ReloadIconCustomizationLayout(app);
    Require(Access::MissingIconTargets(app).at(target) == since && Access::ReadIconAppearances(app).entries.size() == 1,
        "actual layout reload preserves the deadline and hides the retained style");
    { std::ofstream(file) << "returned"; }
    Access::ProbeIconCustomizations(app);
    Wait(app, [&] { return !Access::MissingIconTargets(app).contains(target); }, "recover the returning original identity");
    Require(Access::HasIconReplacement(app, target) && Access::HasIconAppearance(app, target) &&
        Access::ReadIconAppearances(app).entries.size() == 2, "the real host restores both configurations before expiry");

    fs::remove(file);
    Access::ProbeIconCustomizations(app);
    Wait(app, [&] { return Access::MissingIconTargets(app).contains(target); }, "observe the second deletion");
    fs::remove(snowdesktop::IconAssetPath(libraryRoot, asset));
    { std::ofstream(file) << "returned without replacement"; }
    Access::ProbeIconCustomizations(app);
    Wait(app, [&] { return !Access::MissingIconTargets(app).contains(target); }, "recover with a physically removed replacement source");
    Require(!Access::HasIconReplacement(app, target) && Access::HasIconAppearance(app, target),
        "the actual recovery transaction drops the missing image binding and keeps the independent style");
    Access::SeedIconCustomization(app, target, asset,
        snowdesktop::icon_customization::NowSeconds() - snowdesktop::icon_customization::kRetentionSeconds);
    Require(Access::Save(app), "run actual save-boundary expiry without waiting 24 wall-clock hours");
    Access::ReloadIconCustomizationLayout(app);
    Require(!Access::HasIconReplacement(app, target) && !Access::HasIconAppearance(app, target) &&
        !Access::MissingIconTargets(app).contains(target), "expired configurations remain deleted after actual layout reload");
    for (const bool enabled : {true, false})
        Require(Access::ReloadThumbnailPreference(app, enabled) == enabled,
            "actual settings commits and layout reload retain both file thumbnail preference values");

    ComPtr<ID3D11Device> d3d;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)), "create isolated WARP rendering without a desktop host");
    ComPtr<IDXGIDevice> dxgi; Require(SUCCEEDED(d3d.As(&dxgi)), "query the isolated DXGI device");
    ComPtr<ID2D1Factory1> factory;
    Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))), "create isolated D2D factory");
    ComPtr<ID2D1Device> device; Require(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)), "create isolated D2D device");
    ComPtr<ID2D1DeviceContext> renderContext;
    Require(SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &renderContext)), "create isolated D2D context");
    const auto bitmap = snowdesktop::icon_image::CreateBitmap(image);
    Require(bitmap != nullptr, "create real non-square HBITMAP");
    const auto beautified = Access::UploadIconCustomization(app, renderContext.Get(), bitmap, true);
    const auto original = Access::UploadIconCustomization(app, renderContext.Get(), bitmap, false);
    const auto thumbnailOff = Access::UploadFileThumbnail(app, renderContext.Get(), bitmap, false, true);
    const auto thumbnailSmart = Access::UploadFileThumbnail(app, renderContext.Get(), bitmap, true, true);
    const auto ordinarySmart = Access::UploadFileThumbnail(app, renderContext.Get(), bitmap, true, false);
    const auto thumbnailNoResize = Access::UploadFileThumbnail(app, renderContext.Get(), bitmap, true, true, 2);
    Require(thumbnailOff.first.width == 96 && thumbnailOff.first.height == 40 && !thumbnailOff.second.enabled,
        "the real host preserves a file thumbnail when its global opt-in is off");
    Require(thumbnailSmart.first.width == 96 && thumbnailSmart.first.height == 96 &&
            thumbnailSmart.second.enabled && thumbnailSmart.second.mode == 1,
        "smart file thumbnail upload always shrinks the content and adds the theme background");
    Require(ordinarySmart.second.mode == 0 && thumbnailNoResize.second.mode == 2,
        "thumbnail smart handling preserves ordinary icon recognition and explicit no-resize styling");
    DeleteObject(bitmap);
    snowdesktop::icon_image::Image tinyImage;
    tinyImage.width = 6; tinyImage.height = 1;
    tinyImage.pixels.assign(6, 0xfff9f9f9);
    const auto tinyBitmap = snowdesktop::icon_image::CreateBitmap(tinyImage);
    Require(tinyBitmap != nullptr, "create the reported six-by-one opaque PNG equivalent");
    const auto tinyStyled = Access::UploadFileThumbnail(app, renderContext.Get(), tinyBitmap, true, true);
    const auto tinyOriginal = Access::UploadFileThumbnail(app, renderContext.Get(), tinyBitmap, false, true);
    DeleteObject(tinyBitmap);
    Require(tinyStyled.first.width == 64 && tinyStyled.first.height == 64 &&
            tinyOriginal.first.width == 6 && tinyOriginal.first.height == 1,
        "actual host upload renders the reported tiny thumbnail theme at full resolution while opt-out preserves its source size");
    Require(beautified.width == 96 && beautified.height == 96 && original.width == 96 && original.height == 40,
        "the actual host upload uses a square selected shape only while beautification is enabled");
    native_icon_cold_pixels_cases::Run(app, renderContext.Get(), profile.root);
    Require(Access::NoPresentationWindows(app), "icon lifecycle and upload regression never starts a desktop presentation host");
    Access::StopModel(app);
    std::cout << "Native icon customization lifecycle, storage, usage and non-square upload passed\n";
}
}
