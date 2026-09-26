#include "app/desktop_backdrop_compositor.h"
#include "app/desktop_backdrop_update_rules.h"

#include <roapi.h>
#include <d2d1_1helper.h>

#include <array>
#include <iostream>

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
        cardsGlass.BeginFrame(true);
        check(cardsGlass.AddPanel({16, 20, 304, 132}, 12, 24, 31001) &&
                cardsGlass.AddPanel({16, 148, 304, 212}, 12, 24, 31002),
            "two independent cards register before animation starts");
        cardsGlass.EndFrame(false);
        const HWND cardsHelper = GetWindow(cardsContent.handle, GW_HWNDNEXT);
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
        const HWND helper = GetWindow(startupContent.handle, GW_HWNDNEXT);
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

        HWND helper = GetWindow(content.handle, GW_HWNDNEXT);
        RECT actual{};
        check(glass.IsBackdropWindow(helper) &&
                GetWindowRect(helper, &actual) &&
                EqualRect(&actual, &placement),
            "the hidden glass HWND follows popup growth, shrinkage and movement");
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        check(region && GetWindowRgn(helper, region) != ERROR &&
                PtInRegion(region, width - 1, height - 1) &&
                !PtInRegion(region, width + 2, height + 2),
            "glass clipping includes the new corner and excludes stale outer bounds");
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
    const HWND guideHelper = GetWindow(content.handle, GW_HWNDNEXT);
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
