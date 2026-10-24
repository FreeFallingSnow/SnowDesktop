#pragma once
#include "icons/icon_image_codec.h"

namespace native_large_icon_completion_cases
{
using namespace native_drag_commit_cases;

struct Region
{
    HRGN value = CreateRectRgn(0, 0, 0, 0);
    explicit Region(HWND window)
    { Require(value && GetUpdateRgn(window, value, FALSE) != ERROR, "read real Win32 completion damage"); }
    ~Region() { DeleteObject(value); }
    bool Empty() const { RECT box{}; return GetRgnBox(value, &box) == NULLREGION; }
    bool Contains(int x, int y) const { return PtInRegion(value, x, y) != FALSE; }
    std::vector<RECT> Rectangles() const
    {
        const auto bytes = GetRegionData(value, 0, nullptr);
        std::vector<std::byte> storage(bytes);
        auto* data = reinterpret_cast<RGNDATA*>(storage.data());
        Require(bytes && GetRegionData(value, bytes, data) == bytes, "read completion damage rectangles");
        const auto* rects = reinterpret_cast<const RECT*>(data->Buffer);
        return {rects, rects + data->rdh.nCount};
    }
};

struct Surface
{
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Bitmap1> partial, full;
    Surface()
    {
        ComPtr<ID3D11Device> d3d;
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)), "create R04 WARP device");
        ComPtr<IDXGIDevice> dxgi; Require(SUCCEEDED(d3d.As(&dxgi)), "query R04 DXGI device");
        ComPtr<ID2D1Factory1> factory;
        Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))), "create R04 D2D factory");
        ComPtr<ID2D1Device> device;
        Require(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)), "create R04 D2D context");
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        Require(SUCCEEDED(context->CreateBitmap({512, 320}, nullptr, 0, &properties, &partial)) &&
            SUCCEEDED(context->CreateBitmap({512, 320}, nullptr, 0, &properties, &full)), "allocate full and incremental R04 surfaces");
    }
    std::vector<std::uint32_t> Read(ID2D1Bitmap1* bitmap)
    {
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            bitmap->GetPixelFormat(), 96, 96);
        ComPtr<ID2D1Bitmap1> cpu;
        Require(SUCCEEDED(context->CreateBitmap(bitmap->GetPixelSize(), nullptr, 0, &properties, &cpu)) &&
            SUCCEEDED(cpu->CopyFromBitmap(nullptr, bitmap, nullptr)), "copy R04 surface for pixel comparison");
        D2D1_MAPPED_RECT mapped{};
        Require(SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map R04 surface");
        std::vector<std::uint32_t> pixels(512 * 320);
        for (size_t y = 0; y < 320; ++y)
            std::memcpy(pixels.data() + y * 512, mapped.bits + y * mapped.pitch, 512 * sizeof(std::uint32_t));
        Require(SUCCEEDED(cpu->Unmap()), "unmap R04 surface");
        return pixels;
    }
    void Draw(DesktopApp& app, ID2D1Bitmap1* bitmap, const std::vector<RECT>& rectangles, bool complete)
    {
        context->SetTarget(bitmap); context->BeginDraw();
        if (complete)
        {
            context->Clear(D2D1::ColorF(0x182232));
            Access::DrawLargeCompletion(app, context.Get(), nullptr);
        }
        else for (const auto& rect : rectangles)
        {
            context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                static_cast<float>(rect.right), static_cast<float>(rect.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
            context->Clear(D2D1::ColorF(0x182232));
            Access::DrawLargeCompletion(app, context.Get(), &rect);
            context->PopAxisAlignedClip();
        }
        const auto result = context->EndDraw(); context->SetTarget(nullptr);
        Require(SUCCEEDED(result), "draw actual complete/incremental desktop background without a desktop host");
    }
    void Equivalent(DesktopApp& app, const Region& region)
    {
        Draw(app, partial.Get(), region.Rectangles(), false);
        Draw(app, full.Get(), {}, true);
        Require(Read(partial.Get()) == Read(full.Get()), "R04 incremental production draw must equal independent complete-surface reference pixels");
    }
};

inline void Run()
{
    Profile profile;
    auto& app = profile.Model(false, profile.root / L"completion-layout");
    const auto first = profile.root / L"First.url", second = profile.root / L"Second.txt";
    { std::ofstream(first) << "[InternetShortcut]\nURL=steam://rungameid/90001\n"; std::ofstream(second) << "second"; }
    Access::PopulateModel(app, {first, second}, 0);
    struct Window
    {
        HWND value = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"R04 isolated damage observer",
            WS_POPUP | WS_VISIBLE, -32000, -32000, 512, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (value) DestroyWindow(value); }
    } window;
    Require(window.value != nullptr, "create an offscreen nonactivating test-only update-region owner");
    auto notifications = std::make_shared<std::atomic<unsigned>>(0);
    Access::ConfigureLargeCompletion(app, window.value, notifications);
    const fs::path directory(GetDataSubdirectoryPath(L"large-icons"));
    fs::create_directories(directory);
    const auto write = [&](std::string_view name, std::uint32_t color) {
        snowdesktop::icon_image::Image image;
        image.width = name == "r04-blue.png" ? 34 : 70;
        image.height = name == "r04-blue.png" ? 70 : 34;
        image.pixels.assign(static_cast<size_t>(image.width) * image.height, color);
        // Include alpha and a transparent border in the independent source.
        for (int x = 0; x < image.width; ++x) image.pixels[static_cast<size_t>(x)] = 0;
        std::string bytes, error;
        Require(snowdesktop::icon_image::EncodePng(image, bytes, error), "encode R04 source image");
        std::ofstream out(directory / Utf8ToWide(std::string(name)), std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Require(static_cast<bool>(out), "write R04 source image");
    };
    write("r04-red.png", 0xffcc3311); write("r04-blue.png", 0xff1144dd); write("r04-green.png", 0xff229944);
    const auto steam = fs::path(GetDataDirectoryPath()) / L"r04-steam" / L"appcache" / L"librarycache";
    fs::create_directories(steam);
    fs::copy_file(directory / L"r04-red.png", steam / L"90001_library_header.png");
    fs::copy_file(directory / L"r04-blue.png", steam / L"90001_library_600x900.png");
    const auto await = [&](unsigned before) {
        const auto deadline = GetTickCount64() + 10000;
        while (notifications->load() == before && GetTickCount64() < deadline) Sleep(1);
        Require(notifications->load() > before, "actual large-icon worker publishes a bounded completion");
    };
    const auto deliver = [&](size_t index, bool refresh = false, int variant = 0) {
        const auto before = notifications->load();
        Access::QueueLargeCompletion(app, index, refresh, variant); await(before);
        Access::DrainLargeCompletion(app);
    };
    const auto clean = [&] { Access::ResetLargeCompletionDamage(app); };
    const auto quiet = [&](auto revision, const char* message) {
        Region damage(window.value);
        Require(damage.Empty() && Access::LargeCompletionSceneRevision(app) == revision &&
            !Access::LargeCompletionFullCollection(app), message);
    };

    clean(); deliver(0); deliver(1);
    Require(Access::LargeCompletionState(app).asset && Access::LargeCompletionState(app, 1).asset,
        "real requests install both initial desktop assets");
    Surface surface; surface.Draw(app, surface.partial.Get(), {}, true);
    clean();

    // Preview chooser completions are consumed by EditLargeIcon's existing
    // status reads. No new timer or desktop refresh drives their discovery.
    auto revision = Access::LargeCompletionSceneRevision(app);
    deliver(0, false, 1);
    Require(Access::LargeCompletionState(app).previewGenerations[0] == 0 &&
        Access::LargeCompletionState(app).previews[0] && !Access::LargeCompletionState(app).pending,
        "preview acceptance installs chooser pixels and clears its own loading state only");
    quiet(revision, "preview-only completion must not touch desktop damage or drag/backdrop state");
    const auto settings = Access::ReadLargeCompletionSettings(app);
    Require(settings.available && settings.error == "largeIcon.locked" &&
        !settings.landscapePath.empty() && !settings.landscapeSource.empty(),
        "actual settings snapshot exposes accepted preview while preserving the fixture's locked entitlement");
    // The existing locked-settings error intentionally invalidates its owner;
    // it is a separate caller action from the completion under observation.
    clean();
    // Run the exact old unconditional tail on the same actual preview route.
    Access::ReplayLegacyLargeCompletionTail(app);
    { Region old(window.value); Require(old.Contains(490, 300) &&
        Access::LargeCompletionSceneRevision(app) != revision && Access::LargeCompletionFullCollection(app),
        "negative control executes and detects the original full-desktop/backdrop submission fault"); }
    clean();

    auto previewBefore = notifications->load(); Access::QueueLargeCompletion(app, 0, false, 2);
    await(previewBefore); Access::ExpireLargeCompletion(app, 2); revision = Access::LargeCompletionSceneRevision(app);
    Access::DrainLargeCompletion(app);
    Require(!Access::LargeCompletionState(app).previews[1] && Access::LargeCompletionState(app).previewGenerations[1] != 0,
        "stale preview cannot settle a newer chooser generation");
    quiet(revision, "stale preview completion must not repaint");
    deliver(0, false, 2);
    Require(Access::LargeCompletionState(app).previews[1] && Access::LargeCompletionState(app).previewGenerations[1] == 0,
        "current portrait preview still becomes discoverable");
    quiet(revision, "current portrait preview only updates its consumer state");

    const auto previous = Access::LargeCompletionState(app).asset;
    auto before = notifications->load(); Access::QueueLargeCompletion(app, 0);
    await(before); Access::ExpireLargeCompletion(app); revision = Access::LargeCompletionSceneRevision(app);
    Access::DrainLargeCompletion(app);
    Require(Access::LargeCompletionState(app).asset == previous && Access::LargeCompletionState(app).pending,
        "stale host generation cannot replace source or settle a newer pending request");
    quiet(revision, "stale main completion must not repaint");
    clean(); revision = Access::LargeCompletionSceneRevision(app); deliver(0);
    Require(Access::LargeCompletionState(app).asset == previous && !Access::LargeCompletionState(app).pending,
        "same shared asset settles pending without replacing visible pixels");
    quiet(revision, "unchanged main asset must not repaint");

    // A failed same-source refresh keeps its previous pixels and retry/error
    // state. A failed different source discards those pixels, as before R04.
    fs::remove(directory / L"r04-red.png");
    clean(); revision = Access::LargeCompletionSceneRevision(app); deliver(0, true);
    Require(Access::LargeCompletionState(app).asset == previous && !Access::LargeCompletionState(app).error.empty() &&
        Access::LargeCompletionState(app).retryAt > 0, "same-source failure preserves last good image and retry state");
    quiet(revision, "failure retaining the previous asset must not repaint");
    Access::LargeCompletionConfig(app).foregroundImage = "r04-missing.png";
    clean(); deliver(0);
    Require(!Access::LargeCompletionState(app).asset, "different-source failure retains the original placeholder fallback");
    { Region damage(window.value); Require(damage.Contains(80, 80) && !damage.Contains(490, 300),
        "discarded old pixels repaint their own envelope only"); surface.Equivalent(app, damage); }

    // Distinct completions are delivered in one host drain; old and new source
    // pixels plus unrelated retained pixels must match a full redraw.
    Access::LargeCompletionConfig(app).foregroundImage = "r04-blue.png";
    Access::LargeCompletionConfig(app, 1).foregroundImage = "r04-green.png";
    clean(); revision = Access::LargeCompletionSceneRevision(app); before = notifications->load();
    Access::QueueLargeCompletion(app, 0); Access::QueueLargeCompletion(app, 1);
    const auto deadline = GetTickCount64() + 10000;
    while (notifications->load() < before + 2 && GetTickCount64() < deadline) Sleep(1);
    Require(notifications->load() >= before + 2, "both distinct batch assets finish before the single production drain");
    Access::DrainLargeCompletion(app);
    { Region damage(window.value); Require(damage.Contains(80, 80) && damage.Contains(320, 80) && !damage.Contains(490, 300),
        "batched main completion covers both changed frames without unrelated background"); surface.Equivalent(app, damage); }
    Require(Access::LargeCompletionSceneRevision(app) == revision + 1 && !Access::LargeCompletionFullCollection(app),
        "one accepted batch invalidates drag pixels once without changing glass topology");

    Access::HideLargeCompletion(app, true);
    Access::LargeCompletionConfig(app).foregroundImage = "r04-green.png";
    clean(); revision = Access::LargeCompletionSceneRevision(app); deliver(0);
    { Region damage(window.value); Require(damage.Empty() && Access::LargeCompletionState(app).asset &&
        Access::LargeCompletionSceneRevision(app) == revision + 1, "hidden main source updates cache but does not submit invisible desktop pixels"); }
    Access::LargeCompletionConfig(app).keepWhenDesktopHidden = true;
    Access::LargeCompletionConfig(app).foregroundImage = "r04-blue.png";
    clean(); deliver(0);
    { Region damage(window.value); Require(damage.Contains(80, 80) && !damage.Contains(490, 300),
        "retained icon still refreshes while ordinary desktop icons are hidden"); }
    Access::HideLargeCompletion(app, false);
    Access::LargeCompletionConfig(app).showOnHoverOnly = true;
    Access::LargeCompletionConfig(app).foregroundImage = "r04-green.png";
    clean(); deliver(0);
    { Region damage(window.value); Require(damage.Empty(), "unhovered and unselected hover-only icon does not submit pixels"); }
    Access::SelectLargeCompletion(app, true);
    Access::LargeCompletionConfig(app).foregroundImage = "r04-blue.png";
    clean(); deliver(0);
    { Region damage(window.value); Require(damage.Contains(80, 80) && !damage.Contains(490, 300),
        "selection retains the hover-only icon and its asset refresh"); }
    Access::LargeCompletionConfig(app).showOnHoverOnly = false;
    Access::LargeCompletionConfig(app).effect = 1;
    Access::LargeCompletionConfig(app).foregroundImage = "r04-green.png";
    clean(); deliver(0);
    { Region damage(window.value); Require(damage.Contains(490, 300), "3D geometry retains reliable complete-surface fallback"); }

    Access::StopLargeCompletion(app); Access::StopModel(app);
    std::cout << "R04 actual completion routing, legacy submission negative control and incremental/full pixels passed\n";
}
}
