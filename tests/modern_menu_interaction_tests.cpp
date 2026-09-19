#include "modern_menu.h"
#include "modern_menu_appearance_rules.h"
#include "shell_extension_menu_presentation.h"
#include "menu_label.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace
{

constexpr wchar_t kOwnerClass[] =
    L"SnowDesktop.ModernMenuInteractionTestOwner";
constexpr UINT_PTR kDriveTimer = 1;
constexpr UINT_PTR kWatchdogTimer = 2;
constexpr UINT kReorderMenuMessage = WM_APP + 90;
constexpr UINT kInspectMenuMessage = WM_APP + 91;
enum class DriveMode
{
    Cascade,
    Simple,
    Persistent,
    PersistentSubmenu,
    RebuiltRootSubmenu,
    TextInput,
    Nested,
    Script,
};
std::function<void(HWND)> gMenuScript;
DriveMode gDriveMode = DriveMode::Cascade;
int gDrivePhase = 0;
bool gInputPosted = false;
bool gCaptureRootRect = false;
RECT gObservedRootRect{};
bool gCaptureTopmost = false;
bool gObservedTopmost = false;
bool gCaptureRootOwner = false;
HWND gObservedRootOwner = nullptr;
bool gCaptureAboveZOrderOwner = false;
bool gObservedAboveZOrderOwner = false;
HWND gZOrderOwnerProbe = nullptr;
bool gDismissOnDrive = false;
bool gObservedDismissHidden = false;
bool gSelectEnd = false;
bool gNestedMenuCompleted = false;
UINT gNestedMenuCommand = 0;
bool gWatchdogFired = false;
HWND gPersistentSubmenuWindow = nullptr;
bool gPersistentSubmenuStayedOpen = false;
bool gRepeatSubmenuValue = false;
bool gRebuiltRootSubmenuClosed = false;
bool gMessageReorderObserved = false;
bool gMessageOrderRestored = false;
bool gPresentationSawWrongOrder = false;
bool gReorderCascade = false;

struct MenuWindows
{
    HWND root = nullptr;
    HWND child = nullptr;
};

bool IsWindowAbove(HWND upper, HWND lower)
{
    if (!upper || !lower)
        return false;
    for (HWND current = upper; current;
         current = GetWindow(current, GW_HWNDNEXT))
    {
        if (current == lower)
            return true;
    }
    return false;
}

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
    if (message == kReorderMenuMessage)
    {
        const HWND root = snowdesktop::modern_menu::ActiveRootWindow();
        // Inject the ordering captured in the user's log. Merely raising an
        // owned window's owner does not invert the pair on every Windows build.
        if (root && gZOrderOwnerProbe)
        {
            HWND reorderedMenu = root;
            HWND lowerWindow = gZOrderOwnerProbe;
            if (gReorderCascade)
            {
                SendMessageW(root, WM_KEYDOWN, VK_HOME, 0);
                SendMessageW(root, WM_KEYDOWN, VK_RIGHT, 0);
                MenuWindows menus;
                EnumThreadWindows(GetCurrentThreadId(), FindMenuWindows,
                    reinterpret_cast<LPARAM>(&menus));
                reorderedMenu = menus.child;
                lowerWindow = root;
            }
            SetWindowPos(reorderedMenu, lowerWindow, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                    SWP_NOOWNERZORDER);
            gMessageReorderObserved = reorderedMenu &&
                !IsWindowAbove(reorderedMenu, lowerWindow);
        }
        PostMessageW(hwnd, kInspectMenuMessage, 0, 0);
        return 0;
    }
    if (message == kInspectMenuMessage)
    {
        const HWND root = snowdesktop::modern_menu::ActiveRootWindow();
        gMessageOrderRestored = IsWindowAbove(root, gZOrderOwnerProbe);
        HWND selectedMenu = root;
        if (gReorderCascade)
        {
            MenuWindows menus;
            EnumThreadWindows(GetCurrentThreadId(), FindMenuWindows,
                reinterpret_cast<LPARAM>(&menus));
            selectedMenu = menus.child;
            gMessageOrderRestored = gMessageOrderRestored &&
                IsWindowAbove(selectedMenu, root);
        }
        if (selectedMenu)
        {
            SendMessageW(selectedMenu, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(selectedMenu, WM_KEYDOWN, VK_RETURN, 0);
        }
        return 0;
    }
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
        if (gDriveMode == DriveMode::Script && menus.root)
        {
            gInputPosted = true;
            KillTimer(hwnd, kDriveTimer);
            gMenuScript(menus.root);
        }
        else if (gDriveMode == DriveMode::Cascade && menus.root &&
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
            if (gCaptureRootOwner)
                gObservedRootOwner =
                    GetWindow(menus.root, GW_OWNER);
            if (gCaptureAboveZOrderOwner)
            {
                gObservedAboveZOrderOwner =
                    IsWindowAbove(menus.root, gZOrderOwnerProbe);
            }
            if (gDismissOnDrive)
            {
                snowdesktop::modern_menu::DismissActive();
                gObservedDismissHidden =
                    IsWindowVisible(menus.root) == FALSE;
                gInputPosted = true;
                KillTimer(hwnd, kDriveTimer);
                return 0;
            }
            // Dispatch synchronously: CI runners can briefly transfer the
            // foreground window after popup creation, so queued keystrokes
            // may otherwise arrive only after the menu has deactivated.
            SendMessageW(menus.root, WM_KEYDOWN,
                gSelectEnd ? VK_END : VK_HOME, 0);
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
        else if (gDriveMode == DriveMode::PersistentSubmenu && menus.root &&
            gDrivePhase == 0)
        {
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_RIGHT, 0);
            gDrivePhase = 1;
        }
        else if (gDriveMode == DriveMode::PersistentSubmenu && menus.child &&
            gDrivePhase == 1)
        {
            gPersistentSubmenuWindow = menus.child;
            SendMessageW(menus.child, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.child, WM_KEYDOWN, VK_RETURN, 0);
            gDrivePhase = 2;
        }
        else if (gDriveMode == DriveMode::PersistentSubmenu && menus.child &&
            gDrivePhase == 2)
        {
            gPersistentSubmenuStayedOpen =
                menus.child == gPersistentSubmenuWindow;
            if (gRepeatSubmenuValue)
            {
                // Repeated Enter must apply the same value command without
                // having to reselect it or reopen its submenu.
                SendMessageW(menus.child, WM_KEYDOWN, VK_RETURN, 0);
                SendMessageW(menus.child, WM_KEYDOWN, VK_END, 0);
            }
            else SendMessageW(menus.child, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.child, WM_KEYDOWN, VK_RETURN, 0);
            gInputPosted = true;
            KillTimer(hwnd, kDriveTimer);
        }
        else if (gDriveMode == DriveMode::RebuiltRootSubmenu && menus.root &&
            gDrivePhase == 0)
        {
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_RIGHT, 0);
            gDrivePhase = 1;
        }
        else if (gDriveMode == DriveMode::RebuiltRootSubmenu && menus.child &&
            gDrivePhase == 1)
        {
            SendMessageW(menus.child, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.child, WM_KEYDOWN, VK_RETURN, 0);
            gDrivePhase = 2;
        }
        else if (gDriveMode == DriveMode::RebuiltRootSubmenu && menus.root &&
            gDrivePhase == 2)
        {
            MenuWindows afterCommand;
            EnumThreadWindows(GetCurrentThreadId(), FindMenuWindows,
                reinterpret_cast<LPARAM>(&afterCommand));
            gRebuiltRootSubmenuClosed = afterCommand.child == nullptr;
            if (!gRebuiltRootSubmenuClosed)
            {
                gInputPosted = true;
                KillTimer(hwnd, kDriveTimer);
                return 0;
            }
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_RIGHT, 0);
            gDrivePhase = 3;
        }
        else if (gDriveMode == DriveMode::RebuiltRootSubmenu && menus.child &&
            gDrivePhase == 3)
        {
            SendMessageW(menus.child, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.child, WM_KEYDOWN, VK_RETURN, 0);
            gInputPosted = true;
            KillTimer(hwnd, kDriveTimer);
        }
        else if (gDriveMode == DriveMode::TextInput && menus.root &&
            gDrivePhase == 0)
        {
            SendMessageW(menus.root, WM_CHAR, L'x', 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_BACK, 0);
            SendMessageW(menus.root, WM_CHAR, L's', 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_HOME, 0);
            SendMessageW(menus.root, WM_CHAR, L'a', 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_DELETE, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_END, 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_SPACE, 0);
            SendMessageW(menus.root, WM_CHAR, L' ', 0);
            SendMessageW(menus.root, WM_KEYDOWN, VK_BACK, 0);
            // Text refresh is synchronous. Keep result navigation in this
            // drive step, just like Simple mode: a later timer can run after
            // an unrelated foreground transfer has already cancelled the menu.
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

struct IsolatedMenuDesktop
{
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK isolated = nullptr;

    IsolatedMenuDesktop()
    {
        const std::wstring name = L"SnowDesktop.MenuTests." + std::to_wstring(GetCurrentProcessId());
        isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        Expect(isolated != nullptr, "an isolated desktop is created for menu tests");
        Expect(SetThreadDesktop(isolated) != FALSE, "menu tests attach to their isolated desktop");
    }

    ~IsolatedMenuDesktop()
    {
        SetThreadDesktop(original);
        if (isolated) CloseDesktop(isolated);
    }
};

} // namespace

int wmain()
{
    // Regression: asynchronous Shell entries belong immediately above More,
    // preserving its separator and any host commands that follow that group.
    {
        using snowdesktop::modern_menu::Item;
        Item open; open.command = 1;
        Item separator; separator.separator = true;
        Item more; more.command = 2;
        Item after; after.command = 3;
        Item archive; archive.label = L"7-Zip";
        Item compress; compress.command = 4;
        archive.children = {compress};
        std::vector<Item> items{open, separator, more, separator, after};
        snowdesktop::shell_extensions::InsertBeforeMore(items, {archive}, 2);
        Expect(items.size() == 6 && items[1].separator && items[2].label == L"7-Zip" &&
                   items[2].children.front().command == 4 && items[3].command == 2 &&
                   items[4].separator && items[5].command == 3,
               "Shell entries appear above More in the same group, without a synthetic wrapper");
    }
    // Do not switch the user's input desktop. Test windows need real activation
    // and Z-order, but unrelated applications must not cancel their menu loops.
    IsolatedMenuDesktop isolatedDesktop;
    using snowdesktop::modern_menu::Appearance;
    using snowdesktop::modern_menu::appearance_rules::ResolveForWindows;
    Expect(ResolveForWindows(
            Appearance::FollowSystem, true, 10, 19045) ==
            Appearance::OpaqueLight,
        "Windows 10 follows the system light theme with an opaque menu");
    Expect(ResolveForWindows(
            Appearance::FollowSystem, false, 10, 19045) ==
            Appearance::OpaqueDark,
        "Windows 10 follows the system dark theme with an opaque menu");
    Expect(ResolveForWindows(
            Appearance::FollowSystem, true, 10, 22621) ==
            Appearance::FollowSystem,
        "Windows 11 keeps the system backdrop for follow-system menus");
    Expect(ResolveForWindows(
            Appearance::SystemLightBlur, true, 10, 19045) ==
            Appearance::SystemLightBlur,
        "an explicitly selected blur theme remains available on Windows 10");
    Expect(ResolveForWindows(
            Appearance::OpaqueDark, false, 10, 22621) ==
            Appearance::OpaqueDark,
        "an explicitly selected opaque theme remains available on Windows 11");
    for (const auto appearance : {Appearance::Win10Light, Appearance::Win10Dark})
    {
        for (const unsigned long build : {19045UL, 22621UL})
            Expect(ResolveForWindows(appearance, true, 10, build) == appearance,
                "Win10 styles are available only by explicit selection on either OS");
        for (const bool systemLight : {false, true})
            Expect(snowdesktop::modern_menu::appearance_rules::IsLightTheme(
                    appearance, systemLight) == (appearance == Appearance::Win10Light),
                "explicit Win10 colors are independent of the system theme");
        Expect(!snowdesktop::modern_menu::appearance_rules::UsesSystemBlur(appearance),
            "Win10 styles always render opaque panels");
    }

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
    HANDLE scheduledWork = CreateEventW(
        nullptr, FALSE, FALSE, nullptr);
    int scheduledWorkCalls = 0;
    int presentationFlushes = 0;
    options.eventPump.scheduledWorkHandle = scheduledWork;
    options.eventPump.dispatchScheduledWork = [&]() {
        ++scheduledWorkCalls;
    };
    options.eventPump.flushPresentation = [&]() {
        ++presentationFlushes;
    };
    SetEvent(scheduledWork);
    const auto result = snowdesktop::modern_menu::Show(items, options);
    options.eventPump = {};
    CloseHandle(scheduledWork);
    KillTimer(owner, kWatchdogTimer);

    Expect(!gWatchdogFired, "cascaded popup did not time out");
    Expect(gInputPosted, "test input reached the cascaded popup");
    Expect(result.command == 7,
        "a command selected from a cascaded submenu is returned");
    Expect(scheduledWorkCalls == 1 &&
            presentationFlushes > 0,
        "the synchronous menu loop pumps scheduled animation work and presentation flushes");

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
    const RECT opaqueMenuRect = captureMenuWindowRect(
        snowdesktop::modern_menu::Appearance::OpaqueLight, false);
    const RECT blurMenuRect = captureMenuWindowRect(
        snowdesktop::modern_menu::Appearance::SystemLightBlur, true);
    Expect((opaqueMenuRect.right - opaqueMenuRect.left) >
            (blurMenuRect.right - blurMenuRect.left),
        "opaque menus reserve an HWND margin for the analytic shadow");
    Expect((opaqueMenuRect.bottom - opaqueMenuRect.top) >
            (blurMenuRect.bottom - blurMenuRect.top),
        "opaque menus reserve vertical space for the analytic shadow");
    Expect(gObservedTopmost,
        "a topmost modern menu is created above taskbar windows");

    HWND zOrderOwner = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kOwnerClass, L"", WS_POPUP,
        2, 2, 1, 1, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Expect(zOrderOwner != nullptr,
        "the independent Z-order owner window is created");
    ShowWindow(zOrderOwner, SW_SHOWNOACTIVATE);
    gDriveMode = DriveMode::Simple;
    gDrivePhase = 0;
    gInputPosted = false;
    gCaptureRootRect = false;
    gCaptureTopmost = true;
    gObservedTopmost = false;
    gCaptureRootOwner = true;
    gObservedRootOwner = nullptr;
    gCaptureAboveZOrderOwner = true;
    gObservedAboveZOrderOwner = false;
    gZOrderOwnerProbe = zOrderOwner;
    gWatchdogFired = false;
    options.anchor = { 220, 220 };
    options.topmost = true;
    options.zOrderOwner = zOrderOwner;
    HANDLE zOrderRefresh = CreateEventW(
        nullptr, FALSE, FALSE, nullptr);
    Expect(zOrderRefresh != nullptr,
        "the Z-order refresh event is created");
    options.eventPump.scheduledWorkHandle = zOrderRefresh;
    std::vector<std::wstring> zOrderDiagnostics;
    bool observedActiveRootWindow = false;
    options.eventPump.traceDiagnostic =
        [&](const std::wstring& message) {
            zOrderDiagnostics.push_back(message);
            observedActiveRootWindow = observedActiveRootWindow ||
                snowdesktop::modern_menu::ActiveRootWindow() != nullptr;
        };
    options.eventPump.dispatchScheduledWork = [&]() {
        SetWindowPos(
            zOrderOwner, HWND_TOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                SWP_NOOWNERZORDER);
    };
    SetEvent(zOrderRefresh);
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto ownedMenuResult =
        snowdesktop::modern_menu::Show(
            adjustmentItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired,
        "owned topmost menu did not time out");
    Expect(ownedMenuResult.command == 21,
        "owned topmost menu returns its command");
    Expect(gObservedTopmost &&
            gObservedRootOwner == zOrderOwner,
        "a floating-host menu is topmost and owned by its Z-order host");
    Expect(gObservedAboveZOrderOwner,
        "a floating-host menu recovers after scheduled work raises its owner");
    const auto hasZOrderDiagnostic =
        [&](const wchar_t* stage) {
            return std::any_of(
                zOrderDiagnostics.begin(), zOrderDiagnostics.end(),
                [&](const std::wstring& message) {
                    return message.find(stage) != std::wstring::npos;
                });
        };
    Expect(observedActiveRootWindow &&
            snowdesktop::modern_menu::ActiveRootWindow() == nullptr,
        "the active root menu diagnostic is scoped to the menu session");
    Expect(hasZOrderDiagnostic(L"stage=session-start") &&
            hasZOrderDiagnostic(L"stage=session-end"),
        "Z-order diagnostics record the menu session boundaries");
    options.eventPump = {};
    CloseHandle(zOrderRefresh);

    // No scheduler handle and no timer-driven recovery: the ordinary message
    // must restore menu order before presentation or the next queued input.
    gMessageReorderObserved = false;
    gMessageOrderRestored = false;
    gPresentationSawWrongOrder = false;
    gWatchdogFired = false;
    options.eventPump.flushPresentation = [&]() {
        const HWND root = snowdesktop::modern_menu::ActiveRootWindow();
        if (root && gMessageReorderObserved &&
            !IsWindowAbove(root, zOrderOwner))
        {
            gPresentationSawWrongOrder = true;
        }
    };
    PostMessageW(owner, kReorderMenuMessage, 0, 0);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto messageReorderedResult =
        snowdesktop::modern_menu::Show(adjustmentItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired && messageReorderedResult.command == 21,
        "the message-only Z-order regression completes without animation work");
    Expect(gMessageReorderObserved,
        "the regression actually puts the menu behind its owner");
    Expect(gMessageOrderRestored && !gPresentationSawWrongOrder,
        "ordinary messages restore menu order before presentation and queued input");
    options.eventPump = {};

    gReorderCascade = true;
    gMessageReorderObserved = false;
    gMessageOrderRestored = false;
    gWatchdogFired = false;
    const std::vector<Item> cascadeZOrderItems{
        { 0, L"Parent", L"", true, false, false,
            { { 23, L"Child", L"", true } } },
    };
    PostMessageW(owner, kReorderMenuMessage, 0, 0);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto cascadeReorderedResult =
        snowdesktop::modern_menu::Show(cascadeZOrderItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired && cascadeReorderedResult.command == 23 &&
            gMessageReorderObserved && gMessageOrderRestored,
        "a displaced cascade recovers even when its root remains above the host");
    gReorderCascade = false;
    options.zOrderOwner = nullptr;
    gCaptureRootOwner = false;
    gCaptureAboveZOrderOwner = false;
    gZOrderOwnerProbe = nullptr;
    DestroyWindow(zOrderOwner);

    gDriveMode = DriveMode::Simple;
    gDrivePhase = 0;
    gInputPosted = false;
    gCaptureTopmost = false;
    gDismissOnDrive = true;
    gObservedDismissHidden = false;
    gWatchdogFired = false;
    options.topmost = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto dismissedMenuResult =
        snowdesktop::modern_menu::Show(
            adjustmentItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired,
        "programmatically dismissed menu did not time out");
    Expect(dismissedMenuResult.command == 0 &&
            gObservedDismissHidden,
        "popup transitions hide the active menu before its loop unwinds");
    gDismissOnDrive = false;

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

    std::vector<Item> inlinePagingItems(3);
    inlinePagingItems[0].command = 61;
    inlinePagingItems[0].glyph = L"<";
    inlinePagingItems[0].inlineAction = true;
    inlinePagingItems[1].command = 62;
    inlinePagingItems[1].label = L"Page 1 / 3";
    inlinePagingItems[1].enabled = false;
    inlinePagingItems[1].inlineAction = true;
    inlinePagingItems[2].command = 63;
    inlinePagingItems[2].glyph = L">";
    inlinePagingItems[2].inlineAction = true;
    gDriveMode = DriveMode::Simple;
    gDrivePhase = 0;
    gInputPosted = false;
    gCaptureRootRect = true;
    gObservedRootRect = {};
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto inlinePagingResult =
        snowdesktop::modern_menu::Show(inlinePagingItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired, "inline paging popup did not time out");
    Expect(inlinePagingResult.command == 61,
        "inline paging group remains keyboard accessible");
    const int inlinePagingHeight = gObservedRootRect.bottom -
        gObservedRootRect.top;
    Expect(inlinePagingHeight < regularHeight,
        "three paging actions share one menu row");
    Expect(inlinePagingResult.itemScreenRect.right -
            inlinePagingResult.itemScreenRect.left <=
            32,
        "paging arrow uses a compact square cell");

    std::vector<Item> horizontalTagItems(4);
    for (size_t i = 0; i < horizontalTagItems.size(); ++i)
    {
        horizontalTagItems[i].command = 64 + static_cast<UINT>(i);
        horizontalTagItems[i].label =
            L"Intentionally wide source label " + std::to_wstring(i + 1);
        horizontalTagItems[i].inlineAction = true;
        horizontalTagItems[i].inlineGroup = 1;
        horizontalTagItems[i].horizontalScrollAction = true;
    }
    gDriveMode = DriveMode::Simple;
    gDrivePhase = 0;
    gInputPosted = false;
    gCaptureRootRect = true;
    gObservedRootRect = {};
    gSelectEnd = true;
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto horizontalTagResult =
        snowdesktop::modern_menu::Show(horizontalTagItems, options);
    KillTimer(owner, kWatchdogTimer);
    gSelectEnd = false;
    Expect(!gWatchdogFired, "horizontal tag popup did not time out");
    Expect(horizontalTagResult.command == 67,
        "keyboard navigation reaches the final horizontally scrolled tag");
    const int horizontalTagMenuWidth =
        gObservedRootRect.right - gObservedRootRect.left;
    Expect(horizontalTagMenuWidth <= 280,
        "wide source tags do not expand the menu panel");
    Expect(horizontalTagResult.itemScreenRect.left >= gObservedRootRect.left &&
            horizontalTagResult.itemScreenRect.right <= gObservedRootRect.right,
        "the selected tag is scrolled fully into the visible tag bar");

    std::vector<Item> previewRows(4);
    previewRows[0].command = 71;
    previewRows[0].label = L"Collection";
    previewRows[0].glyph = L"C";
    previewRows[0].inlineAction = true;
    previewRows[0].inlineGroup = 1;
    previewRows[1].command = 72;
    previewRows[1].label = L"Preview";
    previewRows[1].inlineAction = true;
    previewRows[1].inlineGroup = 1;
    previewRows[1].compactInlineAction = true;
    previewRows[2].command = 73;
    previewRows[2].label = L"Desktop Files";
    previewRows[2].glyph = L"D";
    previewRows[2].inlineAction = true;
    previewRows[2].inlineGroup = 2;
    previewRows[3].command = 74;
    previewRows[3].label = L"Preview";
    previewRows[3].inlineAction = true;
    previewRows[3].inlineGroup = 2;
    previewRows[3].compactInlineAction = true;
    gDriveMode = DriveMode::Simple;
    gDrivePhase = 0;
    gInputPosted = false;
    gCaptureRootRect = true;
    gObservedRootRect = {};
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto previewRowsResult =
        snowdesktop::modern_menu::Show(previewRows, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired,
        "grouped add/preview rows did not time out");
    Expect(previewRowsResult.command == 71,
        "the component-name side remains the primary command");
    const int previewRowsHeight =
        gObservedRootRect.bottom - gObservedRootRect.top;
    Expect(previewRowsHeight > inlinePagingHeight,
        "each component add/preview pair occupies its own menu row");
    Expect(previewRowsResult.itemScreenRect.right -
            previewRowsResult.itemScreenRect.left > 64,
        "the component name receives more space than the preview action");
    gCaptureRootRect = false;

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

    const std::vector<Item> persistentSubmenuItems{
        { 0, L"Widgets", L"W", true, false, false,
            {
                { 51, L"Next page", L">", true },
                { 52, L"Widget A", L"A", true },
            } },
    };
    int persistentSubmenuCommandCount = 0;
    options.onCommand = [&](UINT command, auto& currentItems) {
        if (command != 51)
            return false;
        ++persistentSubmenuCommandCount;
        currentItems.front().children = {
            { 53, L"Widget B", L"B", true },
        };
        return true;
    };
    gDriveMode = DriveMode::PersistentSubmenu;
    gDrivePhase = 0;
    gInputPosted = false;
    gPersistentSubmenuWindow = nullptr;
    gPersistentSubmenuStayedOpen = false;
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto persistentSubmenuResult =
        snowdesktop::modern_menu::Show(persistentSubmenuItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired, "persistent submenu did not time out");
    Expect(persistentSubmenuCommandCount == 1,
        "submenu page command runs without closing the menu");
    Expect(gPersistentSubmenuStayedOpen,
        "submenu page update keeps the existing popup window visible");
    Expect(persistentSubmenuResult.command == 53,
        "updated submenu remains interactive after changing page");

    persistentSubmenuCommandCount = 0;
    gRepeatSubmenuValue = true;
    options.onCommand = [&](UINT command, auto& currentItems) {
        if (command != 51) return false;
        ++persistentSubmenuCommandCount;
        auto replacement = currentItems.front().children;
        replacement.front().checked = !replacement.front().checked;
        replacement.back().enabled = true;
        const auto* children = currentItems.front().children.data();
        const auto childCount = currentItems.front().children.size();
        snowdesktop::modern_menu::UpdateItemStates(currentItems.front().children, replacement);
        Expect(children == currentItems.front().children.data() &&
                childCount == currentItems.front().children.size(),
            "value updates preserve submenu element storage and count");
        currentItems.front().label = L"Values updated";
        return true;
    };
    gDriveMode = DriveMode::PersistentSubmenu; gDrivePhase = 0; gInputPosted = false;
    gWatchdogFired = false; gPersistentSubmenuStayedOpen = false;
    SetTimer(owner, kDriveTimer, 10, nullptr); SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto valueResult = snowdesktop::modern_menu::Show(persistentSubmenuItems, options);
    KillTimer(owner, kWatchdogTimer); gRepeatSubmenuValue = false;
    Expect(!gWatchdogFired && persistentSubmenuCommandCount == 2,
        "a retained submenu selection accepts repeated value changes");
    Expect(gPersistentSubmenuStayedOpen && valueResult.command == 52,
        "value changes preserve the same popup and subsequent closing commands");

    const std::vector<Item> rebuiltRootSubmenuItems{
        { 0, L"Widgets", L"W", true, false, false,
            {
                { 91, L"Rebuild root", L">", true },
            } },
    };
    int rebuiltRootSubmenuCommandCount = 0;
    options.onCommand = [&](UINT command, auto& currentItems) {
        if (command != 91)
            return false;
        ++rebuiltRootSubmenuCommandCount;
        std::vector<Item> replacement{
            { 0, L"Widgets", L"W", true, false, false,
                {
                    { 93, L"Updated widget", L"U", true },
                } },
        };
        currentItems = std::move(replacement);
        return true;
    };
    options.onTextChanged = {};
    gDriveMode = DriveMode::RebuiltRootSubmenu;
    gDrivePhase = 0;
    gInputPosted = false;
    gRebuiltRootSubmenuClosed = false;
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto rebuiltRootSubmenuResult =
        snowdesktop::modern_menu::Show(rebuiltRootSubmenuItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired, "rebuilt-root submenu did not time out");
    Expect(rebuiltRootSubmenuCommandCount == 1,
        "root rebuild command runs from a submenu");
    Expect(gRebuiltRootSubmenuClosed,
        "root rebuild closes the stale submenu popup");
    Expect(rebuiltRootSubmenuResult.command == 93,
        "rebuilt root submenu remains interactive after refresh");

    std::vector<Item> textInputItems(2);
    textInputItems[0].command = 81;
    textInputItems[0].label = L"Search components";
    textInputItems[0].glyph = L"S";
    textInputItems[0].textInput = true;
    textInputItems[1].command = 82;
    textInputItems[1].label = L"Initial result";
    std::wstring observedSearch;
    int textChangeCount = 0;
    options.onCommand = {};
    options.onTextChanged = [&](UINT command, const std::wstring& text,
                                auto& currentItems) {
        Expect(command == 81,
            "text callback receives the search row command");
        observedSearch = text;
        ++textChangeCount;
        currentItems[1].label = L"Filtered result";
    };
    gDriveMode = DriveMode::TextInput;
    gDrivePhase = 0;
    gInputPosted = false;
    gWatchdogFired = false;
    SetTimer(owner, kDriveTimer, 10, nullptr);
    SetTimer(owner, kWatchdogTimer, 3000, nullptr);
    const auto textInputResult =
        snowdesktop::modern_menu::Show(textInputItems, options);
    KillTimer(owner, kWatchdogTimer);
    Expect(!gWatchdogFired, "text-input popup did not time out");
    Expect(textChangeCount == 7 && observedSearch == L"a",
        "caret insertion, delete, spaces, and backspace update search in place");
    Expect(textInputResult.command == 82,
        "search input stays outside keyboard result navigation");

    // Drive the real popup on the isolated desktop. Commands and returned row
    // bounds catch accidental promotion of quick actions and stale hit geometry.
    const auto runScript = [&](const std::vector<Item>& scriptItems,
                               snowdesktop::modern_menu::Options scriptOptions,
                               std::function<void(HWND)> script) {
        gDriveMode = DriveMode::Script;
        gMenuScript = std::move(script);
        gInputPosted = false;
        gWatchdogFired = false;
        SetTimer(owner, kDriveTimer, 10, nullptr);
        SetTimer(owner, kWatchdogTimer, 3000, nullptr);
        const auto selected = snowdesktop::modern_menu::Show(scriptItems, scriptOptions);
        KillTimer(owner, kWatchdogTimer);
        gMenuScript = {};
        Expect(gInputPosted && !gWatchdogFired,
            "compact-menu input reaches the real popup without timing out");
        return selected;
    };
    {
        auto nativeLabel = snowdesktop::DecodeMenuLabel(L"打开(&O)\tCtrl+O");
        Item open; open.command = 9101; open.label = nativeLabel.text; open.accessKey = nativeLabel.accessKey;
        auto keyOptions = options;
        keyOptions.onTextChanged = {}; keyOptions.onHover = {}; keyOptions.onCommand = {};
        const auto selected = runScript({open}, keyOptions, [](HWND root) {
            SendMessageW(root, WM_CHAR, L'o', 0);
            // Deterministic negative control: absent key handling returns Cancel,
            // rather than failing only because the test's watchdog times out.
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
        });
        Expect(selected.command == 9101, "the displayed native access key executes its unique matching command");

        Item disabled = open; disabled.command = 9102; disabled.enabled = false;
        const auto alt = runScript({disabled, open}, keyOptions, [](HWND root) {
            SendMessageW(root, WM_SYSCHAR, L'O', 0);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
        });
        Expect(alt.command == 9101, "Alt access keys are case insensitive and ignore disabled matches");

        Item duplicate = open; duplicate.command = 9103;
        const auto cycled = runScript({disabled, open, duplicate}, keyOptions, [](HWND root) {
            SendMessageW(root, WM_CHAR, L'o', 0);
            SendMessageW(root, WM_CHAR, L'O', 0);
            SendMessageW(root, WM_KEYDOWN, VK_RETURN, 0);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
        });
        Expect(cycled.command == 9103, "duplicate access keys cycle without prematurely executing the first command");
        const auto posted = runScript({disabled, open, duplicate}, keyOptions, [](HWND root) {
            PostMessageW(root, WM_KEYDOWN, 'O', 1);
            PostMessageW(root, WM_KEYDOWN, 'O', 1);
            PostMessageW(root, WM_KEYDOWN, VK_RETURN, 1);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 1);
        });
        Expect(posted.command == 9103, "the real message loop handles letter keys before translation without duplicate character actions");

        Item cascade; cascade.label = L"归档(Z)"; cascade.accessKey = L'z'; cascade.children = {open};
        const auto childKey = runScript({cascade}, keyOptions, [](HWND root) {
            SendMessageW(root, WM_CHAR, L'z', 0);
            SendMessageW(root, WM_CHAR, L'o', 0);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
        });
        Expect(childKey.command == 9101, "access keys open cascades and route the next key to the active child");

        Item literal; literal.command = 9104; literal.label = L"name(O) & data";
        const auto unmatched = runScript({disabled, literal}, keyOptions, [](HWND root) {
            SendMessageW(root, WM_CHAR, L'o', 0);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
        });
        Expect(unmatched.command == 0, "disabled keys and unmarked parentheses cannot execute a command");

        Item input; input.command = 9105; input.textInput = true; input.label = L"搜索";
        std::wstring entered;
        keyOptions.onTextChanged = [&](UINT, const std::wstring& value, auto&) { entered = value; };
        const auto searched = runScript({input, open}, keyOptions, [](HWND root) {
            SendMessageW(root, WM_CHAR, L'o', 0);
            SendMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
            SendMessageW(root, WM_KEYDOWN, VK_RETURN, 0);
        });
        Expect(searched.command == 9101 && entered == L"o", "focused search input takes precedence over bare access keys");
    }
    for (const auto appearance : {Appearance::Win10Light, Appearance::Win10Dark})
    {
        for (const UINT dpi : {96U, 120U, 144U, 192U})
        {
            snowdesktop::modern_menu::Options compact;
            compact.owner = owner;
            compact.anchor = {80, 80};
            compact.appearance = appearance;
            compact.dpi = dpi;
            std::vector<Item> compactItems{
                {101, L"普通命令", L"", true},
                {102, L"复制一个较长名称的项目\tCtrl+C", L"C", true},
                {103, L"不可用的剪切", L"X", false},
                {104, L"已选中", L"", true, true},
                {0, L"子菜单", L"", true, false, false,
                    {{106, L"子菜单命令", L"", true}}},
            };
            compactItems[1].quickAction = true;
            compactItems[2].quickAction = true;
            const int rowHeight = MulDiv(24, dpi, 96);
            const auto first = runScript(compactItems, compact, [](HWND root) {
                SendMessageW(root, WM_KEYDOWN, VK_HOME, 0);
                SendMessageW(root, WM_KEYDOWN, VK_RETURN, 0);
            });
            Expect(first.command == 101,
                "Win10 keyboard navigation preserves source order instead of promoting quick actions");

            snowdesktop::modern_menu::HoverInfo hover;
            compact.onHover = [&](const auto& info) { hover = info; };
            RECT rootBounds{};
            const auto pointer = runScript(compactItems, compact, [&](HWND root) {
                GetWindowRect(root, &rootBounds);
                SendMessageW(root, WM_KEYDOWN, VK_HOME, 0);
                SendMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
                Expect(hover.command == 102, "compact quick actions remain selectable rows");
                const RECT copyBounds = hover.itemScreenRect;
                SendMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
                Expect(hover.command == 104,
                    "compact keyboard navigation skips disabled quick actions");
                POINT point{copyBounds.left + 4, copyBounds.bottom + rowHeight / 2};
                ScreenToClient(root, &point);
                SendMessageW(root, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
                Expect(snowdesktop::modern_menu::IsActive(),
                    "clicking a disabled compact row cannot activate a command");
                point = {(copyBounds.left + copyBounds.right) / 2,
                    (copyBounds.top + copyBounds.bottom) / 2};
                ScreenToClient(root, &point);
                SendMessageW(root, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
            });
            Expect(pointer.command == 102 &&
                    pointer.itemScreenRect.bottom - pointer.itemScreenRect.top == rowHeight &&
                    pointer.itemScreenRect.right - pointer.itemScreenRect.left ==
                        rootBounds.right - rootBounds.left - 2 * MulDiv(6, dpi, 96),
                "Win10 quick actions use full-width compact hit targets at every DPI");
            const auto child = runScript(compactItems, compact, [&](HWND root) {
                SendMessageW(root, WM_KEYDOWN, VK_END, 0);
                const RECT parentRow = hover.itemScreenRect;
                SendMessageW(root, WM_KEYDOWN, VK_RIGHT, 0);
                MenuWindows cascade;
                EnumThreadWindows(GetCurrentThreadId(), FindMenuWindows,
                    reinterpret_cast<LPARAM>(&cascade));
                Expect(cascade.child != nullptr, "compact submenu opens from the keyboard");
                RECT childBounds{};
                GetWindowRect(cascade.child, &childBounds);
                Expect(childBounds.top + MulDiv(6, dpi, 96) ==
                        parentRow.top - MulDiv(3, dpi, 96),
                    "compact submenus align using the same panel padding as their parent");
                SendMessageW(cascade.child, WM_KEYDOWN, VK_HOME, 0);
                SendMessageW(cascade.child, WM_KEYDOWN, VK_RETURN, 0);
            });
            Expect(child.command == 106 &&
                    child.itemScreenRect.bottom - child.itemScreenRect.top == rowHeight,
                "child popups inherit compact metrics and dispatch their command");

            std::vector<Item> longMenu;
            for (UINT i = 0; i < 100; ++i)
                longMenu.push_back({200 + i, L"滚动菜单项", L"", true});
            compact.anchor = {monitorInfo.rcWork.right - 2, monitorInfo.rcWork.bottom - 2};
            const auto scrolled = runScript(longMenu, compact, [](HWND root) {
                SendMessageW(root, WM_KEYDOWN, VK_END, 0);
                SendMessageW(root, WM_KEYDOWN, VK_RETURN, 0);
            });
            Expect(scrolled.command == 299 &&
                    scrolled.itemScreenRect.bottom <= monitorInfo.rcWork.bottom &&
                    scrolled.itemScreenRect.right <= monitorInfo.rcWork.right,
                "compact menus scroll to the last command within the monitor work area");

            compact.anchor = {80, 80};
            compact.onTextChanged = options.onTextChanged;
            textChangeCount = 0;
            observedSearch.clear();
            const auto search = runScript(textInputItems, compact, [](HWND root) {
                SendMessageW(root, WM_CHAR, L'a', 0);
                SendMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
                SendMessageW(root, WM_KEYDOWN, VK_RETURN, 0);
            });
            Expect(search.command == 82 && observedSearch == L"a" && textChangeCount == 1,
                "compact search rows accept input and retain keyboard result navigation");
        }
    }

    gCaptureRootRect = false;
    // An asynchronous Shell reply can grow a tiny popup at a screen edge.
    // Exercise actual placement and keyboard scrolling, not layout arithmetic.
    for (UINT dpi : {96u, 120u, 144u, 192u})
    {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
        options.anchor = {monitor.rcWork.right - 8, monitor.rcWork.bottom - 8};
        options.rootPlacement = snowdesktop::modern_menu::RootPlacement::Default;
        options.dpi = dpi;
        options.appearance = snowdesktop::modern_menu::Appearance::Win10Light;
        options.onCommand = {}; options.onTextChanged = {}; options.onHover = {};
        bool populated = false; RECT expanded{};
        const auto readyAt = GetTickCount64() + 30;
        options.pollItems = [&](const auto&, bool canApply) -> std::optional<std::vector<snowdesktop::modern_menu::Item>> {
            if (!canApply || populated || GetTickCount64() < readyAt) return {};
            populated = true;
            std::vector<snowdesktop::modern_menu::Item> expandedItems(100);
            for (UINT i=0;i<100;++i)
            {expandedItems[i].command=8100+i;expandedItems[i].label=L"扩展命令 / long extension command " + std::to_wstring(i);}
            expandedItems[1].enabled=false;expandedItems[2].checked=true;
            return expandedItems;
        };
        gDriveMode = DriveMode::Script; gInputPosted = false; gWatchdogFired = false;
        gMenuScript = [&](HWND menu) {
            GetWindowRect(menu,&expanded);
            SendMessageW(menu,WM_KEYDOWN,VK_END,0);
            SendMessageW(menu,WM_KEYDOWN,VK_RETURN,0);
        };
        SetTimer(owner,kDriveTimer,120,nullptr);SetTimer(owner,kWatchdogTimer,3000,nullptr);
        snowdesktop::modern_menu::Item loading;loading.label=L"Loading";loading.enabled=false;
        const auto asyncResult=snowdesktop::modern_menu::Show({loading},options);
        KillTimer(owner,kWatchdogTimer);
        Expect(populated&&!gWatchdogFired&&asyncResult.command==8199,"asynchronous extension commands support keyboard scrolling at every DPI");
        Expect(expanded.bottom<=monitor.rcWork.bottom+16&&expanded.right<=monitor.rcWork.right+16,
            "asynchronous menu growth stays within the monitor work area at every DPI");
    }
    // Polling must continue while a cascade is open so an extension deadline
    // cannot be postponed indefinitely by keyboard navigation.
    {
        bool observedCascade = false, refreshed = false;
        snowdesktop::modern_menu::Item group; group.command=8200;group.label=L"Group";
        snowdesktop::modern_menu::Item leaf;leaf.command=8201;leaf.label=L"Child";group.children={leaf};
        gDriveMode=DriveMode::Script;gInputPosted=false;gWatchdogFired=false;
        gMenuScript=[](HWND root){SendMessageW(root,WM_KEYDOWN,VK_HOME,0);SendMessageW(root,WM_KEYDOWN,VK_RIGHT,0);};
        options.pollItems=[&](const auto& current,bool canApply)->std::optional<std::vector<snowdesktop::modern_menu::Item>> {
            if(!canApply)
            {
                observedCascade=true;MenuWindows windows;EnumThreadWindows(GetCurrentThreadId(),FindMenuWindows,reinterpret_cast<LPARAM>(&windows));
                if(windows.child)PostMessageW(windows.child,WM_KEYDOWN,VK_ESCAPE,0);
                return {};
            }
            if(!observedCascade||refreshed)return {};
            refreshed=true;auto updated=current;snowdesktop::modern_menu::Item command;command.command=8202;command.label=L"Loaded";updated.push_back(command);
            const auto root=snowdesktop::modern_menu::ActiveRootWindow();PostMessageW(root,WM_KEYDOWN,VK_END,0);PostMessageW(root,WM_KEYDOWN,VK_RETURN,0);
            return updated;
        };
        SetTimer(owner,kDriveTimer,10,nullptr);SetTimer(owner,kWatchdogTimer,3000,nullptr);
        const auto cascadeResult=snowdesktop::modern_menu::Show({group},options);KillTimer(owner,kWatchdogTimer);
        if (!observedCascade || !refreshed || gWatchdogFired || cascadeResult.command != 8202)
            std::cerr << "cascade: observed=" << observedCascade << " refreshed=" << refreshed
                      << " watchdog=" << gWatchdogFired << " command=" << cascadeResult.command
                      << " input=" << gInputPosted << '\n';
        Expect(observedCascade&&refreshed&&!gWatchdogFired&&cascadeResult.command==8202,
            "extension deadlines are serviced during an open cascade and additions wait for its closure");
    }
    options.pollItems = {}; gMenuScript = {};

    gDriveMode = DriveMode::Nested;
    gDrivePhase = 0;
    gInputPosted = false;
    gNestedMenuCompleted = false;
    gNestedMenuCommand = 0;
    gWatchdogFired = false;
    options.rootPlacement = snowdesktop::modern_menu::RootPlacement::Default;
    options.onCommand = {};
    options.onTextChanged = {};
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
