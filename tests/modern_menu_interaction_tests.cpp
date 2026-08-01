#include "modern_menu.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{

constexpr wchar_t kOwnerClass[] =
    L"SnowDesktop.ModernMenuInteractionTestOwner";
constexpr UINT_PTR kDriveTimer = 1;
constexpr UINT_PTR kWatchdogTimer = 2;
enum class DriveMode { Cascade, Simple, Persistent, Nested };
DriveMode gDriveMode = DriveMode::Cascade;
int gDrivePhase = 0;
bool gInputPosted = false;
bool gCaptureRootRect = false;
RECT gObservedRootRect{};
bool gCaptureTopmost = false;
bool gObservedTopmost = false;
bool gNestedMenuCompleted = false;
UINT gNestedMenuCommand = 0;
bool gWatchdogFired = false;

struct MenuWindows
{
    HWND root = nullptr;
    HWND child = nullptr;
};

BOOL CALLBACK FindMenuWindows(HWND hwnd, LPARAM parameter)
{
    wchar_t className[96]{};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    if (wcscmp(className, L"SnowDesktop.ModernMenuPopup") == 0)
    {
        auto& windows = *reinterpret_cast<MenuWindows*>(parameter);
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((style & WS_EX_NOACTIVATE) != 0)
            windows.child = hwnd;
        else if (IsWindowVisible(hwnd))
            windows.root = hwnd;
    }
    return TRUE;
}

LRESULT CALLBACK OwnerWindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_TIMER && wParam == kWatchdogTimer)
    {
        KillTimer(hwnd, kWatchdogTimer);
        gWatchdogFired = true;
        MenuWindows menus;
        EnumThreadWindows(GetCurrentThreadId(),
            FindMenuWindows, reinterpret_cast<LPARAM>(&menus));
        if (menus.root)
        {
            PostMessageW(menus.root, WM_KEYDOWN, VK_ESCAPE, 0);
            PostMessageW(menus.root, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        else
        {
            PostQuitMessage(1);
        }
        return 0;
    }
    if (message == WM_TIMER && wParam == kDriveTimer && !gInputPosted)
    {
        MenuWindows menus;
        EnumThreadWindows(GetCurrentThreadId(),
            FindMenuWindows, reinterpret_cast<LPARAM>(&menus));
        if (gDriveMode == DriveMode::Cascade && menus.root &&
            gDrivePhase == 0)
        {
            // Select the cascade row, open it, then activate its first item.
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            for (int i = 0; i < 4; ++i)
                SendMessageW(menus.root, WM_KEYDOWN, VK_DOWN, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_RIGHT, 0);
            gDrivePhase = 1;
        }
        else if (gDriveMode == DriveMode::Cascade && menus.root &&
            menus.child && gDrivePhase == 1)
        {
            // A child popup becoming the mouse target must not cancel the root.
            SendMessageW(menus.root, WM_ACTIVATE,
                MAKEWPARAM(WA_INACTIVE, FALSE),
                reinterpret_cast<LPARAM>(menus.child));
            SendMessageW(menus.child, WM_LBUTTONUP, 0,
                MAKELPARAM(30, 30));
            gInputPosted = true;
            KillTimer(hwnd, kDriveTimer);
        }
        else if (gDriveMode == DriveMode::Simple && menus.root)
        {
            if (gCaptureRootRect)
                GetWindowRect(menus.root, &gObservedRootRect);
            if (gCaptureTopmost)
            {
                gObservedTopmost =
                    (GetWindowLongPtrW(menus.root, GWL_EXSTYLE) &
                        WS_EX_TOPMOST) != 0;
            }
            // Dispatch synchronously: CI runners can briefly transfer the
            // foreground window after popup creation, so queued keystrokes
            // may otherwise arrive only after the menu has deactivated.
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_RETURN, 0);
            gInputPosted = true;
            KillTimer(hwnd, kDriveTimer);
        }
        else if (gDriveMode == DriveMode::Persistent && menus.root &&
            gDrivePhase == 0)
        {
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_RETURN, 0);
            gDrivePhase = 1;
        }
        else if (gDriveMode == DriveMode::Persistent && menus.root &&
            gDrivePhase == 1)
        {
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_DOWN, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_RETURN, 0);
            gInputPosted = true;
            KillTimer(hwnd, kDriveTimer);
        }
        else if (gDriveMode == DriveMode::Nested && menus.root)
        {
            KillTimer(hwnd, kDriveTimer);
            const std::vector<snowdesktop::modern_menu::Item> replacement{
                { 31, L"Replacement command", L"R", true },
            };
            snowdesktop::modern_menu::Options replacementOptions;
            replacementOptions.owner = hwnd;
            replacementOptions.anchor = { 120, 120 };
            replacementOptions.dpi = USER_DEFAULT_SCREEN_DPI;

            gDriveMode = DriveMode::Simple;
            gInputPosted = false;
            SetTimer(hwnd, kDriveTimer, 10, nullptr);
            gNestedMenuCommand = snowdesktop::modern_menu::
                Show(replacement, replacementOptions).command;
            gNestedMenuCompleted = true;
            gInputPosted = true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int wmain()
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = OwnerWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kOwnerClass;
    Expect(RegisterClassExW(&windowClass) != 0,
        "interaction-test owner class is registered");

    HWND owner = CreateWindowExW(0, kOwnerClass, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Expect(owner != nullptr, "interaction-test owner window is created");
    // Give the popup a real active owner.  A hidden owner lets CTest's console
    // reclaim activation while the menu fade-in is running, which can dismiss
    // the popup before the driver timer sees it.
    ShowWindow(owner, SW_SHOW);
    SetForegroundWindow(owner);
    SetFocus(owner);

    using snowdesktop::modern_menu::Item;
    const std::vector<Item> items{
        { 1, L"Details", L"D", true },
        { 2, L"Add", L"A", true },
        { 3, L"Disabled edit", L"E", false },
        { 4, L"Disabled delete", L"X", false },
        { 0, L"", L"", false, false, true },
        { 5, L"Today", L"T", true },
        { 6, L"Previous", L"P", true },
        { 0, L"Next", L"N", true, false, false,
            {
                { 7, L"Tomorrow", L"T", true },
                { 8, L"Next week", L"W", true },
            } },
    };

    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    snowdesktop::modern_menu::Options options;
    options.owner = owner;
    options.anchor = { 80, 80 };
    options.dpi = USER_DEFAULT_SCREEN_DPI;
    const auto result = snowdesktop::modern_menu::Show(items, options);
    KillTimer(owner, kWatchdogTimer);

    Expect(!gWatchdogFired, "cascaded popup did not time out");
    Expect(gInputPosted, "test input reached the cascaded popup");
    Expect(result.command == 7,
        "a command selected from a cascaded submenu is returned");

    const std::vector<Item> adjustmentItems{
        { 0, L"Current: 8 x 6", L"", false },
        { 0, L"", L"", false, false, true },
        { 21, L"Add row", L"+", true },
        { 22, L"Remove row", L"-", true },
    };
    gDriveMode = DriveMode::Simple;
    gDrivePhase = 0;
    gInputPosted = false;
    gCaptureRootRect = true;
    gObservedRootRect = {};
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    GetMonitorInfoW(MonitorFromPoint({ 80, 80 },
        MONITOR_DEFAULTTONEAREST), &monitorInfo);
    options.anchor = {
        monitorInfo.rcWork.left + 120,
        monitorInfo.rcWork.bottom - 60,
    };
    options.rootPlacement = snowdesktop::modern_menu::
        RootPlacement::AboveAnchorRect;
    options.anchorRect = {
        monitorInfo.rcWork.left + 80,
        monitorInfo.rcWork.bottom - 80,
        monitorInfo.rcWork.right - 80,
        monitorInfo.rcWork.bottom - 40,
    };
    const auto adjustmentResult =
        snowdesktop::modern_menu::Show(adjustmentItems, options);
    KillTimer(owner, kWatchdogTimer);

    Expect(!gWatchdogFired, "Dock-placed popup did not time out");
    Expect(gInputPosted, "test input reached the follow-up popup");
    Expect(adjustmentResult.command == 21,
        "the follow-up grid adjustment popup returns its parameter command");
    Expect(gObservedRootRect.bottom - 12 <= options.anchorRect.top,
        "an above-Dock menu keeps its panel outside the Dock rectangle");

    const auto captureMenuWindowRect =
        [&](auto appearance, bool topmost) {
        gDriveMode = DriveMode::Simple;
        gDrivePhase = 0;
        gInputPosted = false;
        gCaptureRootRect = true;
        gObservedRootRect = {};
        gCaptureTopmost = topmost;
        gObservedTopmost = false;
        gWatchdogFired = false;
        options.anchor = { 220, 220 };
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::Default;
        options.appearance = appearance;
        options.topmost = topmost;
        SetTimer(owner, kDriveTimer, 10, nullptr);
        SetTimer(owner, kWatchdogTimer, 3000, nullptr);
        const auto menuResult =
            snowdesktop::modern_menu::Show(adjustmentItems, options);
        KillTimer(owner, kWatchdogTimer);
        Expect(!gWatchdogFired, "menu bounds capture did not time out");
        Expect(menuResult.command == 21,
            "menu bounds capture returns its parameter command");
        return gObservedRootRect;
    };
    const RECT followSystemMenuRect = captureMenuWindowRect(
        snowdesktop::modern_menu::Appearance::FollowSystem, false);
    const RECT blurMenuRect = captureMenuWindowRect(
        snowdesktop::modern_menu::Appearance::SystemLightBlur, true);
    Expect((followSystemMenuRect.right - followSystemMenuRect.left) ==
            (blurMenuRect.right - blurMenuRect.left),
        "follow-system menu uses the blur menu's shadow-free HWND width");
    Expect((followSystemMenuRect.bottom - followSystemMenuRect.top) ==
            (blurMenuRect.bottom - blurMenuRect.top),
        "follow-system menu uses the blur menu's shadow-free HWND height");
    Expect(gObservedTopmost,
        "a topmost modern menu is created above taskbar windows");

    auto quickAdjustmentItems = adjustmentItems;
    quickAdjustmentItems[2].label =
        L"Remove Dock Mapping With An Intentionally Long Label";
    quickAdjustmentItems[2].quickAction = true;
    quickAdjustmentItems[3].quickAction = true;
    gDriveMode = DriveMode::Simple;
    gDrivePhase = 0;
    gInputPosted = false;
    gCaptureRootRect = true;
    gObservedRootRect = {};
    gCaptureTopmost = false;
    options.topmost = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto quickResult = snowdesktop::modern_menu::Show(
        quickAdjustmentItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired, "quick-action popup did not time out");
    Expect(quickResult.command == 21,
        "keyboard navigation starts in the top quick-action strip");
    const int regularHeight = followSystemMenuRect.bottom -
        followSystemMenuRect.top;
    const int quickHeight = gObservedRootRect.bottom -
        gObservedRootRect.top;
    Expect(quickHeight < regularHeight,
        "quick actions reduce the vertical menu height");
    const int quickItemWidth = quickResult.itemScreenRect.right -
        quickResult.itemScreenRect.left;
    const int quickMenuWidth = gObservedRootRect.right -
        gObservedRootRect.left;
    Expect(quickItemWidth < quickMenuWidth / 2,
        "a short quick-action strip remains left-aligned at fixed width");
    Expect(quickItemWidth <= 64,
        "a long quick-action label cannot widen every top button");
    options.appearance = snowdesktop::modern_menu::Appearance::FollowSystem;
    options.topmost = false;
    gCaptureTopmost = false;

    const std::vector<Item> persistentItems{
        { 41, L"Adjust once", L"+", true },
        { 42, L"Finish", L"F", true },
    };
    int persistentCommandCount = 0;
    options.rootPlacement = snowdesktop::modern_menu::RootPlacement::Default;
    options.onCommand = [&](UINT command, auto& currentItems) {
        if (command != 41)
            return false;
        ++persistentCommandCount;
        currentItems.front().label = L"Adjusted";
        return true;
    };
    gCaptureRootRect = false;
    gDriveMode = DriveMode::Persistent;
    gDrivePhase = 0;
    gInputPosted = false;
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto persistentResult =
        snowdesktop::modern_menu::Show(persistentItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired, "persistent popup did not time out");
    Expect(persistentCommandCount == 1,
        "persistent command callback runs without closing the popup");
    Expect(persistentResult.command == 42,
        "persistent popup remains interactive until Finish is selected");

    gCaptureRootRect = false;
    gDriveMode = DriveMode::Nested;
    gDrivePhase = 0;
    gInputPosted = false;
    gNestedMenuCompleted = false;
    gNestedMenuCommand = 0;
    gWatchdogFired = false;
    options.rootPlacement = snowdesktop::modern_menu::RootPlacement::Default;
    options.onCommand = {};
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto replacedResult =
        snowdesktop::modern_menu::Show(adjustmentItems, options);
    KillTimer(owner, kWatchdogTimer);

    DestroyWindow(owner);
    UnregisterClassW(kOwnerClass, GetModuleHandleW(nullptr));
    Expect(!gWatchdogFired, "replacement popup did not time out");
    Expect(gNestedMenuCompleted,
        "a replacement menu completed inside the first modal loop");
    Expect(gNestedMenuCommand == 31,
        "the replacement menu remains interactive");
    Expect(replacedResult.command == 0,
        "opening a replacement dismisses the previous menu session");
    std::cout << "modern menu interaction tests passed\n";
    return 0;
}
