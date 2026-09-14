#include "app/desktop_backdrop_compositor.h"

#include <roapi.h>

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
