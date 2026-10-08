#include "../src/app/render/desktop_backdrop_compositor.h"
#include "../src/app/render/desktop_backdrop_update_rules.h"
#include "layout/popup_round_geometry.h"
#include "icons/large_icon_shape_geometry.h"

#include <roapi.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <iostream>
#include <thread>

namespace
{

constexpr UINT kCommitCompleted = WM_APP + 471;

struct PopupWindow
{
    HWND handle = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"backdrop-resize-test", WS_POPUP,
        80, 80, 320, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);

    ~PopupWindow()
    {
        if (handle) DestroyWindow(handle);
    }
};

// Substitute only widget rasterization/cache storage. The production
// reconciliation and Windows Composition target/region/commit remain real.
struct RetainedWidget
{
    RECT bounds{};
    bool visible = true;
    bool backdropRequested = true;
    bool backdropRegistered = false;
    int backdropCornerRadius = 18;
    int backdropBlurRadius = 24;
};

struct OwnedBackdropQuery
{
    const DesktopBackdropCompositor* compositor = nullptr;
    HWND helper = nullptr;
};

BOOL CALLBACK MatchOwnedBackdrop(HWND window, LPARAM parameter)
{
    auto& query = *reinterpret_cast<OwnedBackdropQuery*>(parameter);
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() &&
        process == GetCurrentProcessId() && query.compositor->IsBackdropWindow(window))
    {
        query.helper = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindOwnedBackdrop(const DesktopBackdropCompositor& compositor)
{
    OwnedBackdropQuery query{&compositor};
    EnumThreadWindows(GetCurrentThreadId(), MatchOwnedBackdrop,
        reinterpret_cast<LPARAM>(&query));
    return query.helper;
}

bool WaitForCommit(HWND window, WPARAM token)
{
    const ULONGLONG deadline = GetTickCount64() + 3000;
    for (;;)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.hwnd == window &&
                message.message == kCommitCompleted &&
                message.wParam == token)
            {
                // Windows.Foundation.AsyncStatus::Completed. A completed
                // transaction proves liveness, not which DWM frame displayed it.
                return message.lParam == 1;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        MsgWaitForMultipleObjectsEx(0, nullptr,
            static_cast<DWORD>(deadline - now),
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

int CheckHiddenPopupKeyboardFocus()
{
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
        return value;
    };
    const std::wstring desktopName = L"SnowDesktop.BackdropFocus." +
        std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr,
        0, GENERIC_ALL, nullptr);
    if (!check(desktop != nullptr, "create a private desktop for popup focus checks"))
        return failures;

    std::thread([&] {
        // Never SwitchDesktop or inject input. Showing/focusing these windows
        // cannot interact with the user's input desktop or desktop host.
        if (!check(SetThreadDesktop(desktop) != FALSE,
                "attach the focus test thread to its private desktop")) return;
        struct Apartment
        {
            HRESULT result = RoInitialize(RO_INIT_SINGLETHREADED);
            ~Apartment() { if (SUCCEEDED(result)) RoUninitialize(); }
        };
        // Construct before the production thread-local WinComp context, so
        // that context is released before this apartment at thread exit.
        static thread_local Apartment apartment;
        if (!check(SUCCEEDED(apartment.result),
                "initialize the private focus thread's Windows Runtime apartment")) return;
        struct Window
        {
            HWND handle = nullptr;
            ~Window() { if (handle) DestroyWindow(handle); }
        };
        Window outside{CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"outside",
            WS_POPUP, 480, 80, 240, 160, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr)};
        Window content{CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
            L"STATIC", L"popup", WS_POPUP, 80, 80, 320, 240, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr)};
        if (!check(content.handle && outside.handle,
                "create only test-owned popup focus windows")) return;
        DesktopBackdropCompositor glass;
        if (!check(glass.InitializePopup(content.handle, true, false),
                "initialize the real popup compositor for focus regression")) return;
        glass.BeginFrame(true);
        if (!check(glass.AddPanel({0, 0, 320, 240}, 12, 24, 1),
                "register the focus fixture's real backdrop panel")) return;
        glass.EndFrame();
        const HWND helper = FindOwnedBackdrop(glass);
        if (!check(glass.IsBackdropWindow(helper),
                "focus checks address only this compositor's helper")) return;
        check(GetPropW(helper, L"NonRudeHWND") == reinterpret_cast<HANDLE>(TRUE) &&
                !IsWindowVisible(helper),
            "a real popup backdrop receives its Shell exclusion before first show");

        enum class Case { Content, Child, DestroyedChild, Outside };
        for (const auto scenario : {Case::Content, Case::Child,
                 Case::DestroyedChild, Case::Outside})
        {
            Window edit{CreateWindowExW(0, L"EDIT", L"draft",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 12, 200, 30,
                content.handle, nullptr, GetModuleHandleW(nullptr), nullptr)};
            if (!check(edit.handle != nullptr, "create a real native EDIT child")) return;
            ShowWindow(outside.handle, SW_SHOWNOACTIVATE);
            glass.ShowPopupWindowPair(content.handle);
            SetActiveWindow(content.handle);
            const HWND initialFocus = scenario == Case::Content ? content.handle : edit.handle;
            SetFocus(initialFocus);
            if (!check(GetActiveWindow() == content.handle && GetFocus() == initialFocus,
                    "the popup must own keyboard focus before testing hide")) return;
            if (scenario == Case::DestroyedChild)
            {
                // SystemPanel clears native inputs before hiding its window.
                DestroyWindow(edit.handle);
                edit.handle = nullptr;
                if (!check(GetFocus() == content.handle,
                        "destroying the focused EDIT transfers focus to its popup")) return;
            }
            if (scenario == Case::Outside)
            {
                SetActiveWindow(outside.handle);
                SetFocus(outside.handle);
                if (!check(GetActiveWindow() == outside.handle && GetFocus() == outside.handle,
                        "another test window owns active/focus before popup hide")) return;
            }
            glass.HidePopupWindowPair(content.handle);
            check(!IsWindowVisible(content.handle) && !IsWindowVisible(helper),
                "the production pair hide removes content and backdrop together");
            if (scenario == Case::Outside)
            {
                check(GetFocus() == outside.handle && GetActiveWindow() == outside.handle,
                    "hiding a popup preserves another window's keyboard focus and activation");
                glass.HidePopupWindowPair(content.handle);
                check(GetFocus() == outside.handle && GetActiveWindow() == outside.handle,
                    "hiding an already-hidden popup still preserves another focus owner");
            }
            else
            {
                check(GetFocus() == nullptr,
                    "hidden popup and native input must not retain keyboard focus");
                glass.HidePopupWindowPair(content.handle);
                check(GetFocus() == nullptr,
                    "repeating pair hide must not restore hidden keyboard focus");
            }
        }
    }).join();
    // The thread and its WinComp context have ended before releasing desktop.
    check(CloseDesktop(desktop) != FALSE, "release the private popup focus desktop");
    return failures;
}

int CheckPopupRoundedEdgeCoverage()
{
    using Microsoft::WRL::ComPtr;
    namespace rounded = snowdesktop::popup_round_geometry;
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
        return value;
    };
    ComPtr<ID3D11Device> device;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> drawing;
    ComPtr<ID2D1DeviceContext> context;
    if (!check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, nullptr)) && SUCCEEDED(device.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
            SUCCEEDED(factory->CreateDevice(dxgi.Get(), &drawing)) &&
            SUCCEEDED(drawing->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)),
            "rounded-edge regression creates an offscreen D2D WARP target")) return failures;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (!check(SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 1.f), &brush)),
            "rounded-edge regression creates its opaque coverage brush")) return failures;

    std::size_t fractionalCoverage = 0, oldMaskLoss = 0;
    for (const float scale : {1.f, 1.25f, 1.5f, 2.f, 3.f})
    {
        const UINT width = static_cast<UINT>(std::ceil(200 * scale));
        const UINT height = static_cast<UINT>(std::ceil(160 * scale));
        const RECT frame{static_cast<LONG>(std::lround(12 * scale)), static_cast<LONG>(std::lround(16 * scale)),
            static_cast<LONG>(std::lround(164 * scale)), static_cast<LONG>(std::lround(120 * scale))};
        // Fractional radius and pose cover both DPI conversion and an animation
        // between physical rows. Neither may be snapped before alpha rasterizing.
        const float radius = 12.25f * scale, pose = .375f;
        const auto shape = rounded::Resolve(frame, radius, pose);
        check(shape.radiusX == radius && shape.radiusY == radius,
            "popup radius was rounded to integer physical pixels before rendering");
        const auto clamped = rounded::Resolve(frame, 10000.f);
        check(clamped.radiusX == static_cast<float>((std::min)(frame.right-frame.left, frame.bottom-frame.top)) / 2.f,
            "content and backdrop do not clamp oversized corner radii to the same short edge");
        const auto fence = rounded::WindowFence(frame, pose);
        HRGN region = rounded::CreateWindowFence(frame, radius, pose);
        const int diameter = static_cast<int>(std::lround(radius * 2));
        HRGN oldRegion = CreateRoundRectRgn(frame.left,
            frame.top + static_cast<int>(std::floor(pose)), frame.right + 1,
            frame.bottom + static_cast<int>(std::ceil(pose)) + 1, diameter, diameter);
        if (!check(region && oldRegion, "rounded-edge regression creates the old and current window fences"))
        { if (region) DeleteObject(region); if (oldRegion) DeleteObject(oldRegion); return failures; }
        check(!PtInRegion(region, frame.left, frame.top) &&
                PtInRegion(region, (frame.left + frame.right) / 2, frame.top),
            "conservative rounded HWND fence must exclude the transparent corner while retaining the top edge");

        ComPtr<ID2D1Bitmap1> target, readable;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        const auto readProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        bool rendered = SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &target)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, readProperties, &readable));
        if (rendered)
        {
            context->SetTarget(target.Get()); context->SetDpi(96, 96); context->BeginDraw();
            context->Clear(D2D1::ColorF(0, 0.f));
            context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            context->FillRoundedRectangle(shape, brush.Get());
            context->DrawRoundedRectangle(shape, brush.Get(), 1.f);
            rendered = SUCCEEDED(context->EndDraw()); context->SetTarget(nullptr);
            rendered = rendered && SUCCEEDED(readable->CopyFromBitmap(nullptr, target.Get(), nullptr));
        }
        D2D1_MAPPED_RECT mapped{};
        rendered = rendered && SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        if (check(rendered, "rounded-edge regression reads real antialiased D2D pixels"))
        {
            std::size_t lost = 0;
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                {
                    const auto alpha = mapped.bits[static_cast<std::size_t>(y) * mapped.pitch + x * 4 + 3];
                    if (!alpha) continue;
                    if (!PtInRegion(region, static_cast<int>(x), static_cast<int>(y))) ++lost;
                    if (alpha < 255)
                    {
                        ++fractionalCoverage;
                        if (!PtInRegion(oldRegion, static_cast<int>(x), static_cast<int>(y))) ++oldMaskLoss;
                    }
                }
            readable->Unmap();
            if(lost)std::cerr<<"rounded coverage scale="<<scale<<" lostPixels="<<lost<<'\n';
            check(lost == 0, "the current HWND fence still cuts antialiased rounded-edge pixels");
        }
        check(!rounded::Contains(shape, {shape.rect.left, shape.rect.top}) &&
                rounded::Contains(shape, {(shape.rect.left+shape.rect.right)/2, (shape.rect.top+shape.rect.bottom)/2}),
            "conservative visibility fence changed the popup's transparent-corner hit geometry");
        const RECT media{frame.left, frame.bottom + 8, frame.right, frame.bottom + 32};
        const auto mediaFence = rounded::WindowFence(media, pose);
        check(fence.bottom < mediaFence.top,
            "AA coverage fences joined the separate control and media cards across their transparent gap");
        DeleteObject(region); DeleteObject(oldRegion);
    }
    check(fractionalCoverage > 100 && oldMaskLoss > 0,
        "rounded-edge oracle cannot distinguish the old binary GDI mask from antialiased content");
    return failures;
}

} // namespace

int RunDesktopBackdropCompositorTests()
{
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    };

    // Like the host, retain the apartment until process exit: the production
    // compositor/DispatcherQueue context is thread-local and outlives fixtures.
    if (!check(SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED)),
            "backdrop integration initializes its Windows Runtime apartment"))
        return failures;

    PopupWindow content;
    PopupWindow otherContent;
    if (!check(content.handle && otherContent.handle,
            "backdrop integration creates only its own hidden popup windows"))
        return failures;

    failures += CheckPopupRoundedEdgeCoverage();
    failures += CheckHiddenPopupKeyboardFocus();

    {
        // Status-bar control and media cards move without another AddPanel /
        // EndFrame during each animation frame. Exercise those production
        // commits directly, with only test-owned windows kept hidden.
        PopupWindow cardsContent;
        DesktopBackdropCompositor cardsGlass;
        if (!check(cardsContent.handle &&
                cardsGlass.InitializePopup(cardsContent.handle, false, false),
                "card-animation fixture creates its own hidden backdrop target"))
            return failures;
        // The real native compositor must accept every card silhouette and
        // reuse one visual through shape changes, including returning to the
        // original rounded panel. The shared D2D mask is pixel-tested separately.
        for (int shape : {0, 1, 2, 3, 4, 5, 2, 0})
        {
            cardsGlass.BeginFrame(true);
            check(cardsGlass.AddLargeIconPanel({20, 20, 220, 220}, shape, 24, 24, 31003),
                "native backdrop accepts all large-icon silhouettes and shape transitions");
            cardsGlass.EndFrame(false);
            check(cardsGlass.PanelCount() == 1,
                "large-icon shape changes retain one native backdrop visual");
            check(cardsGlass.HasPanelContaining({30, 30, 40, 40}) == (shape <= 1),
                "arbitrary large-icon silhouettes do not claim rectangular glass coverage");
        }
        for (int direction : {0, 1, 2, 3, 0})
        {
            cardsGlass.BeginFrame(true);
            check(cardsGlass.AddLargeIconPanel({20, 20, 220, 220}, 3, 0, 24, 31003, direction),
                "native flag backdrop accepts direction changes without a shape or size change");
            cardsGlass.EndFrame(false);
            check(cardsGlass.PanelCount() == 1, "flag direction changes reuse the native owner visual");
        }
        for (const bool regular : {false, true, false})
        {
            snowdesktop::LargeIconConfig hexagon; hexagon.shape = 5; hexagon.regularHexagon = regular;
            const auto frame = snowdesktop::large_icon_shape::Frame(hexagon, RECT{20, 20, 320, 220});
            cardsGlass.BeginFrame(true);
            check(cardsGlass.AddLargeIconPanel(frame, 5, 0, 24, 31003),
                "native hexagon backdrop accepts equal-edge frame changes");
            cardsGlass.EndFrame(false);
            check(cardsGlass.PanelCount() == 1, "regular-hexagon toggles retain one native owner visual");
        }
        cardsGlass.BeginFrame(true);
        check(cardsGlass.AddPanel({16, 20, 304, 132}, 12, 24, 31001) &&
                cardsGlass.AddPanel({16, 148, 304, 212}, 12, 24, 31002),
            "two independent cards register before animation starts");
        cardsGlass.EndFrame(false);
        // A Z-order neighbor need not be this compositor's helper. Keep a
        // different test-owned window nearby while checking exact identity.
        PopupWindow unrelatedHidden;
        if (!check(unrelatedHidden.handle && SetWindowPos(unrelatedHidden.handle,
                cardsContent.handle, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
                "identity fixture inserts only its own hidden neighboring popup"))
            return failures;
        const HWND cardsHelper = FindOwnedBackdrop(cardsGlass);
        if (!check(cardsGlass.IsBackdropWindow(cardsHelper),
                "card-animation assertions address only this fixture's helper HWND"))
            return failures;
        const auto regionMatches = [&](POINT first, POINT second, POINT gap,
            POINT retired) {
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            const bool matches = region && GetWindowRgn(cardsHelper, region) != ERROR &&
                PtInRegion(region, first.x, first.y) &&
                PtInRegion(region, second.x, second.y) &&
                !PtInRegion(region, gap.x, gap.y) &&
                !PtInRegion(region, retired.x, retired.y) &&
                !PtInRegion(region, 8, 80);
            if (region) DeleteObject(region);
            return matches;
        };
        check(regionMatches({40, 24}, {40, 180}, {40, 140}, {40, 8}),
            "initial cards have separate regions and a transparent gap");
        check(cardsGlass.SetPanelTransform(31001,
                D2D1::Matrix4x4F::Translation(0, -18, 0), {16, 2, 304, 114}) &&
                cardsGlass.SetPanelTransform(31002,
                    D2D1::Matrix4x4F::Translation(0, -18, 0), {16, 130, 304, 194}),
            "animation updates both card transforms without collecting a new frame");
        cardsGlass.CommitVisualChanges();
        check(regionMatches({40, 8}, {40, 140}, {40, 122}, {40, 204}),
            "ordinary visual commit moves both glass regions, preserves their gap and removes old pixels");
        check(cardsGlass.SetPanelTransform(31001,
                D2D1::Matrix4x4F::Translation(0, 12, 0), {16, 32, 304, 144}) &&
                cardsGlass.SetPanelTransform(31002,
                    D2D1::Matrix4x4F::Translation(0, 12, 0), {16, 160, 304, 224}),
            "a later pose dirties the retained cards again");
        check(cardsGlass.CommitVisualChangesAndNotify(
                cardsContent.handle, kCommitCompleted, 200) &&
                WaitForCommit(cardsContent.handle, 200),
            "notified card-animation commit completes through the real dispatcher");
        check(regionMatches({40, 40}, {40, 218}, {40, 152}, {40, 8}),
            "notified visual commit also moves the region and retains the inter-card gap");
        check(!IsWindowVisible(cardsContent.handle) && !IsWindowVisible(cardsHelper) &&
                cardsGlass.PanelCount() == 2 && cardsGlass.BlurFactoryCount() == 1,
            "card-animation checks never show windows or accumulate retained panels and blur factories");
    }

    {
        PopupWindow startupContent;
        DesktopBackdropCompositor startupGlass;
        if (!check(startupContent.handle && SetWindowPos(
                startupContent.handle, nullptr, 0, 0, 3840, 3240,
                SWP_NOACTIVATE | SWP_NOZORDER),
                "startup fixture covers the reported two-monitor desktop"))
            return failures;
        // Primary: 3840x2160. Secondary: (960,2160)-(2880,3240).
        // Both surfaces already exist; neither is redrawn in this regression.
        RetainedWidget primary{{100, 100, 400, 300}};
        RetainedWidget secondary{{1100, 2300, 1400, 2500}};
        RetainedWidget hidden{{1500, 2300, 1800, 2500}, false};
        RetainedWidget opaque{{1900, 2300, 2200, 2500}, true, false};
        const auto reconcile = [&] {
            for (auto* widget : {&primary, &secondary, &hidden, &opaque})
            {
                widget->backdropRegistered =
                    snowdesktop::desktop_backdrop_update_rules::
                        KeepOrRestoreWidgetPanel(startupGlass, *widget);
            }
        };
        reconcile();
        check(!primary.backdropRegistered && !secondary.backdropRegistered &&
                startupGlass.PanelCount() == 0,
            "prepaint without a target retains glass requests without claiming registration");
        if (!check(startupGlass.InitializePopup(startupContent.handle, false, false),
                "the startup backdrop target is created after the cached widget surfaces"))
            return failures;
        startupGlass.BeginFrame(false);
        reconcile();
        startupGlass.EndFrame();
        check(primary.backdropRegistered && secondary.backdropRegistered &&
                !hidden.backdropRegistered && !opaque.backdropRegistered &&
                startupGlass.PanelCount() == 2,
            "partial paint must restore both monitors' cached glass without repainting widgets");
        const HWND helper = FindOwnedBackdrop(startupGlass);
        if (!check(startupGlass.IsBackdropWindow(helper),
                "region fixture locates its exact owned helper before inspecting pixels"))
            return failures;
        check(GetPropW(helper, L"NonRudeHWND") == reinterpret_cast<HANDLE>(TRUE),
            "the real startup backdrop is excluded from Shell fullscreen detection");
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        check(startupGlass.IsBackdropWindow(helper) && region &&
                GetWindowRgn(helper, region) != ERROR &&
                PtInRegion(region, 200, 200) && PtInRegion(region, 1200, 2400) &&
                !PtInRegion(region, 1600, 2400) && !PtInRegion(region, 2000, 2400),
            "startup region includes primary and secondary glass but excludes hidden and opaque widgets");
        if (region) DeleteObject(region);
        check(!IsWindowVisible(helper) &&
                startupGlass.CommitVisualChangesAndNotify(
                    startupContent.handle, kCommitCompleted, 100) &&
                WaitForCommit(startupContent.handle, 100),
            "the complete glass transaction submits while the startup windows remain hidden");

        startupGlass.BeginFrame(true);
        reconcile();
        startupGlass.EndFrame();
        check(startupGlass.PanelCount() == 2 && startupGlass.BlurFactoryCount() == 1,
            "a full collection keeps cached glass without duplicating panels or factories");
        // Cached registration flags are deliberately left true across reset.
        check(startupGlass.InitializePopup(startupContent.handle, false, false),
            "the backdrop target can be replaced while widget surfaces survive");
        startupGlass.BeginFrame(false);
        reconcile();
        startupGlass.EndFrame();
        check(primary.backdropRegistered && secondary.backdropRegistered &&
                startupGlass.PanelCount() == 2,
            "stale registered flags cannot strand widgets after target replacement");
        secondary.visible = false;
        primary.backdropRequested = false;
        startupGlass.BeginFrame(true);
        reconcile();
        startupGlass.EndFrame();
        check(!primary.backdropRegistered && !secondary.backdropRegistered &&
                startupGlass.PanelCount() == 0 && startupGlass.BlurFactoryCount() == 0,
            "hiding widgets and disabling glass retire restored panels and factories");
    }

    DesktopBackdropCompositor glass;
    DesktopBackdropCompositor otherGlass;
    if (!check(glass.InitializePopup(content.handle, false, false) &&
            otherGlass.InitializePopup(otherContent.handle, false, false),
            "popup backdrops initialize with a shared composition controller"))
        return failures;

    otherGlass.BeginFrame(true);
    check(otherGlass.AddPanel({0, 0, 320, 240}, 18, 24, 2),
        "the second popup has its own backdrop panel");
    otherGlass.EndFrame();
    check(otherGlass.CommitVisualChangesAndNotify(
            otherContent.handle, kCommitCompleted, 1) &&
            WaitForCommit(otherContent.handle, 1),
        "the shared controller completes an initial backdrop transaction");

    {
        // Real hidden WinComp targets and native regions. Only content raster
        // success/failure is substituted; the shared commit remains production.
        const RECT oldPanel{12, 12, 92, 92}, retiredPanel{120, 12, 180, 72};
        const RECT candidate{200, 100, 300, 200}, exclusion{230, 130, 250, 150};
        glass.BeginFrame(true);
        check(glass.AddPanel(oldPanel, 8, 24, 101) && glass.AddPanel(retiredPanel, 8, 24, 102),
            "staging fixture installs two live glass panels");
        glass.EndFrame();
        glass.BeginFrame(false);
        check(!glass.BeginStagedFrame() && glass.PanelCount() == 2,
            "candidate collection rejects a busy live collector before content BeginDraw");
        glass.EndFrame();
        const HWND helper = FindOwnedBackdrop(glass);
        HRGN previous = CreateRectRgn(0, 0, 0, 0), observed = CreateRectRgn(0, 0, 0, 0);
        if (!check(helper && previous && observed && GetWindowRgn(helper, previous) != ERROR,
                "staging fixture reads only its own hidden helper region"))
        {
            if (previous) DeleteObject(previous);
            if (observed) DeleteObject(observed);
            return failures;
        }
        check(glass.BeginStagedFrame() && glass.AddPanel(candidate, 12, 18, 101),
            "Dock candidate collects new geometry under the stable owner");
        glass.SetOcclusionRect(exclusion);
        check(glass.PanelCount() == 2 && glass.HasPanelContaining(candidate) && !glass.HasPanelContaining(oldPanel),
            "icon inheritance sees candidate geometry while the two live panels remain retained");
        // CommitNeeded and explicit commits share this compositor. Either can
        // run while candidate pixels are unfinished (or EndDraw has failed).
        check(otherGlass.CommitVisualChangesAndNotify(otherContent.handle, kCommitCompleted, 110) &&
                WaitForCommit(otherContent.handle, 110),
            "another target really commits while Dock candidate preparation is incomplete");
        check(GetWindowRgn(helper, observed) != ERROR && EqualRgn(previous, observed) && glass.PanelCount() == 2,
            "the shared commit cannot publish candidate geometry, exclusion, or panel retirement");
        glass.DiscardStagedFrame(); // Production EndDraw/SetContent failure exit.
        check(glass.HasPanelContaining(oldPanel) && glass.HasPanelContaining(retiredPanel) &&
                !glass.HasPanelContaining(candidate) && glass.BlurFactoryCount() == 1,
            "discarding failed content preserves the complete old glass and its resources");

        check(glass.BeginStagedFrame() && glass.AddPanel(candidate, 12, 18, 101),
            "a later complete content frame can prepare the same owner again");
        glass.SetOcclusionRect(exclusion);
        check(glass.ApplyStagedFrame() && glass.PanelCount() == 1 && glass.BlurFactoryCount() == 1,
            "successful application updates the owner and retires omitted panels and factories together");
        check(GetWindowRgn(helper, observed) != ERROR && PtInRegion(observed, 210, 110) &&
                !PtInRegion(observed, 40, 40) && !PtInRegion(observed, 240, 140),
            "successful publication applies candidate geometry and exclusion to the native region");
        check(glass.BeginStagedFrame() && glass.AddIconPanel({10, 110, 70, 170},
                snowdesktop::IconBeautifyShape::ContinuousRounded, 24, 103),
            "a glass-disabled Dock can stage its independent icon glass");
        check(glass.SetPanelOpacity({10, 110, 70, 170}, 0.4f),
            "icon opacity is part of the candidate data");
        glass.DiscardStagedFrame();
        check(glass.PanelCount() == 1 && glass.HasPanelContaining(candidate),
            "aborted icon collection does not remove the completed parent glass");
        check(glass.BeginStagedFrame() && glass.AddIconPanel({10, 110, 70, 170},
                snowdesktop::IconBeautifyShape::Circle, 24, 103) &&
                glass.SetPanelOpacity({10, 110, 70, 170}, 0.4f) &&
                glass.SetPanelTransform(103, D2D1::Matrix4x4F::Translation(100, 0, 0), {110, 110, 170, 170}) &&
                glass.ApplyStagedFrame(),
            "successful icon publication replays shape, opacity and projected region");
        check(glass.PanelCount() == 1 && GetWindowRgn(helper, observed) != ERROR &&
                PtInRegion(observed, 140, 140) && !PtInRegion(observed, 40, 140),
            "staged transformed icon fences its new footprint and retires the old panel");
        check(glass.BeginStagedFrame() && glass.ApplyStagedFrame() && glass.PanelCount() == 0,
            "a successful empty frame retires glass when material is disabled");
        check(glass.BeginStagedFrame() && glass.AddLargeIconPanel(candidate, 1, 12, 18, 104) &&
                glass.HasPanelContaining(candidate),
            "a rounded large card supplies enclosing glass while still staged");
        snowdesktop::desktop_backdrop_update_rules::ReconcileIconPanel(glass,
            candidate, snowdesktop::IconBeautifyShape::Circle, 24, 1, 105, false);
        check(glass.ApplyStagedFrame() && glass.PanelCount() == 1,
            "staged card inheritance prevents a second overlapping icon blur");
        glass.BeginFrame(true);
        check(glass.AddPanel(oldPanel, 8, 24, 101), "negative control restores the old panel");
        glass.EndFrame();
        GetWindowRgn(helper, previous);
        glass.BeginFrame(true);
        check(glass.AddPanel(candidate, 12, 18, 101), "negative control prepares through the old live collection");
        glass.EndFrame(false);
        check(otherGlass.CommitVisualChangesAndNotify(otherContent.handle, kCommitCompleted, 111) &&
                WaitForCommit(otherContent.handle, 111),
            "negative control really flushes the shared controller through another target");
        check(GetWindowRgn(helper, observed) != ERROR && !EqualRgn(previous, observed) &&
                !glass.HasPanelContaining(oldPanel) && glass.HasPanelContaining(candidate),
            "negative control: delaying explicit Commit alone still leaks live candidate geometry");
        glass.BeginFrame(true);
        glass.EndFrame();
        DeleteObject(previous);
        DeleteObject(observed);
    }

    // Exercise the production popup sequence after file-count/layout changes:
    // move the content HWND, reattach, collect geometry, then commit its pose.
    // These checks protect sizing, retained identity and controller liveness;
    // the visible content/glass timing still requires desktop acceptance.
    const std::array<RECT, 4> placements{{
        {80, 80, 400, 320},
        {60, 40, 620, 460},
        {120, 100, 360, 280},
        {120, 100, 360, 280},
    }};
    WPARAM token = 10;
    for (const RECT& placement : placements)
    {
        const LONG width = placement.right - placement.left;
        const LONG height = placement.bottom - placement.top;
        check(SetWindowPos(content.handle, nullptr,
                placement.left, placement.top, width, height,
                SWP_NOACTIVATE | SWP_NOZORDER) != FALSE,
            "popup content accepts the new layout bounds");
        glass.Reattach(content.handle);
        glass.BeginFrame(true);
        check(glass.AddPanel({0, 0, width, height}, 18, 24, 1),
            "popup backdrop accepts expanded or contracted panel geometry");
        glass.EndFrame(false);
        glass.SetVisualTransform(1, 1, width / 2.0f, height / 2.0f);
        check(glass.IsAvailable(),
            "popup geometry and pose can share one transaction");
        glass.CommitVisualChanges();

        HWND helper = FindOwnedBackdrop(glass);
        RECT actual{};
        check(glass.IsBackdropWindow(helper) &&
                GetWindowRect(helper, &actual) &&
                EqualRect(&actual, &placement),
            "the hidden glass HWND follows popup growth, shrinkage and movement");
        check(GetPropW(helper, L"NonRudeHWND") == reinterpret_cast<HANDLE>(TRUE),
            "popup layout and target reattachment retain the Shell exclusion");
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        check(region && GetWindowRgn(helper, region) != ERROR &&
                PtInRegion(region, width - 1, height / 2) &&
                PtInRegion(region, width / 2, height - 1) &&
                !PtInRegion(region, width - 1, height - 1) &&
                !PtInRegion(region, width + 2, height + 2),
            "glass clipping includes resized edges while excluding transparent corners and stale outer bounds");
        if (region) DeleteObject(region);
        check(glass.PanelCount() == 1 && glass.BlurFactoryCount() == 1,
            "resizing a retained popup does not accumulate panels or blur factories");
        check(glass.CommitVisualChangesAndNotify(
                content.handle, kCommitCompleted, token) &&
                WaitForCommit(content.handle, token),
            "each popup resize transaction completes through the Windows dispatcher");
        ++token;
    }

    // A guide above an independent Dock must remove the glass HWND's pixels
    // in the overlap, then restore them when the guide moves or closes.
    const HWND guideHelper = FindOwnedBackdrop(glass);
    if (!check(glass.IsBackdropWindow(guideHelper),
            "guide overlap checks locate only their exact owned helper"))
        return failures;
    const auto contains = [&](int x, int y) {
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        const bool result = region && GetWindowRgn(guideHelper, region) != ERROR &&
            PtInRegion(region, x, y);
        if (region) DeleteObject(region);
        return result;
    };
    check(contains(40, 40), "unoccluded Dock glass covers the future guide overlap");
    glass.SetOcclusionRect({20, 20, 80, 80});
    check(!contains(40, 40) && contains(120, 120),
        "guide overlap excludes glass without removing unrelated Dock pixels");
    glass.BeginFrame(true);
    glass.AddPanel({0, 0, 240, 180}, 18, 24, 1);
    glass.EndFrame();
    check(!contains(40, 40), "a Dock repaint preserves guide occlusion");
    glass.SetOcclusionRect({100, 100, 150, 150});
    check(contains(40, 40) && !contains(120, 120),
        "moving the guide restores its old overlap and masks its new overlap");
    glass.SetOcclusionRect({});
    check(contains(40, 40) && contains(120, 120),
        "closing the guide restores the complete Dock glass region");

    glass.BeginFrame(true);
    check(glass.AddIconPanel({20, 20, 84, 84}, snowdesktop::IconBeautifyShape::Circle, 16, 101),
        "standalone icon backdrop accepts a shared icon contour");
    glass.EndFrame();
    for (int i = 0; i < 12; ++i)
    {
        glass.BeginFrame(false);
        check(glass.AddIconPanel({20 + i, 20, 84 + i, 84}, snowdesktop::IconBeautifyShape::ContinuousRounded, 16, 101),
            "moving icons retain their visual while updating shape and bounds");
        glass.EndFrame();
    }
    check(glass.PanelCount() == 1 && glass.BlurFactoryCount() == 1 &&
        !glass.HasPanelContaining({40, 40, 60, 60}),
        "moving icon panels do not accumulate or masquerade as containing widget panels");
    glass.BeginFrame(false);
    check(glass.AddPanel({0, 0, 240, 180}, 18, 24, 200) &&
        glass.HasPanelContaining({40, 40, 60, 60}) &&
        glass.RemoveIconPanel({40, 40, 104, 104}, 101),
        "entering a glass widget retires the independent icon panel by stable owner");
    glass.EndFrame();
    check(glass.PanelCount() == 1 && glass.BlurFactoryCount() == 1,
        "inherited glass retires the extra blur factory after the transaction");
    glass.BeginFrame(true); glass.EndFrame();
    check(glass.PanelCount() == 0 && glass.BlurFactoryCount() == 0,
        "hiding icons and panels retires their native blur resources");

    // Reproduce the merged-Dock ownership gap with two real native targets.
    // The strip is external, so the Dock's own panel list cannot find it.
    using snowdesktop::desktop_backdrop_update_rules::ReconcileIconPanel;
    constexpr RECT mergedIcon{20, 20, 84, 84};
    glass.BeginFrame(false);
    ReconcileIconPanel(glass, mergedIcon, snowdesktop::IconBeautifyShape::ContinuousRounded,
        16.f, 1.f, 101, false);
    glass.EndFrame();
    check(otherGlass.HasPanelContaining(mergedIcon) && !glass.HasPanelContaining(mergedIcon) &&
            glass.PanelCount() == 1 && glass.BlurFactoryCount() == 1,
        "missing external inheritance reproduces an unnecessary second native blur");
    for (int i = 0; i < 6; ++i)
    {
        const RECT moved{20 + i, 20, 84 + i, 84};
        glass.BeginFrame(false);
        ReconcileIconPanel(glass, moved, snowdesktop::IconBeautifyShape::ContinuousRounded,
            16.f, 1.f, 101, otherGlass.IsAvailable() && otherGlass.HasPanelContaining(moved));
        glass.EndFrame();
        check(glass.PanelCount() == 0 && glass.BlurFactoryCount() == 0 && otherGlass.PanelCount() == 1,
            "merged icon repaint retires the old blur and retains only its external parent material");
    }
    glass.BeginFrame(false);
    ReconcileIconPanel(glass, mergedIcon, snowdesktop::IconBeautifyShape::ContinuousRounded,
        16.f, .4f, 101, false);
    glass.EndFrame();
    check(glass.PanelCount() == 1 && glass.BlurFactoryCount() == 1,
        "icons restore their own glass when the enclosing material is unavailable or disabled");
    glass.BeginFrame(false);
    ReconcileIconPanel(glass, mergedIcon, snowdesktop::IconBeautifyShape::ContinuousRounded,
        16.f, 0.f, 101, false);
    glass.EndFrame();
    check(glass.PanelCount() == 0 && glass.BlurFactoryCount() == 0,
        "zero-opacity icons retire their retained blur even in a partial frame");

    glass.Reset();
    check(otherGlass.SetVisualOpacity(0.5f),
        "closing one popup preserves another popup's shared controller");
    otherGlass.CommitVisualChanges();
    check(otherGlass.IsAvailable() && otherGlass.PanelCount() == 1 &&
            otherGlass.CommitVisualChangesAndNotify(
                otherContent.handle, kCommitCompleted, token) &&
            WaitForCommit(otherContent.handle, token),
        "a surviving popup still commits after the other target is destroyed");
    return failures;
}
