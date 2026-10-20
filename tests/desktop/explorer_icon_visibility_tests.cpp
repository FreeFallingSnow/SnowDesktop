#include "desktop/explorer_icon_visibility.h"

#include <commctrl.h>
#include <iostream>
#include <string>
#include <thread>

namespace
{
// Exercise the production visibility controller with real ListView HWNDs on a
// private desktop. No Explorer windows or user desktop preferences are changed.
struct Fixture
{
    HWND parent = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"icon-layer-fixture",
        WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND list = CreateList();

    HWND CreateList() const
    {
        return parent ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 100, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    }

    ~Fixture() { if (parent) DestroyWindow(parent); }
};

LRESULT CALLBACK BlockShow(HWND window, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR data)
{
    if (message == WM_WINDOWPOSCHANGING && *reinterpret_cast<bool*>(data))
        reinterpret_cast<WINDOWPOS*>(lp)->flags &= ~SWP_SHOWWINDOW;
    return DefSubclassProc(window, message, wp, lp);
}
}

int RunExplorerIconVisibilityTests()
{
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    std::thread([&] {
        const HDESK original = GetThreadDesktop(GetCurrentThreadId());
        const auto name = L"SnowDesktop.IconRestore." + std::to_wstring(GetCurrentProcessId());
        const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        const bool attached = desktop && SetThreadDesktop(desktop);
        check(attached, "icon restoration fixture uses a private desktop");
        if (!attached) { if (desktop) CloseDesktop(desktop); return; }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        check(InitCommonControlsEx(&controls) != FALSE, "initialize actual icon ListView class");
        {
            Fixture fixture;
            check(fixture.parent && fixture.list, "create native icon-layer windows");
            if (fixture.parent && fixture.list)
            {
                snowdesktop::ExplorerIconVisibility visibility;
                visibility.Hide(fixture.list);
                visibility.Hide(fixture.list);
                check(!IsWindowVisible(fixture.list) && GetPropW(fixture.list, kHiddenBySnowDesktopProp),
                    "repeated hiding retains recovery ownership");
                visibility.RequestRestore();
                check(visibility.TryRestore(fixture.list) && IsWindowVisible(fixture.list) &&
                        !visibility.RestorePending() && !GetPropW(fixture.list, kHiddenBySnowDesktopProp),
                    "ordinary restoration shows icons and retires recovery evidence");

                // A completed transition must not override a later user hide.
                ShowWindow(fixture.list, SW_HIDE);
                check(visibility.TryRestore(fixture.list) && !IsWindowVisible(fixture.list),
                    "completed recovery does not keep forcing icons visible");
                visibility.RequestRestore(true);
                check(visibility.TryRestore(fixture.list) && IsWindowVisible(fixture.list),
                    "explicit native desktop selection shows an unmarked hidden icon layer");

                visibility.Hide(fixture.list);
                DestroyWindow(fixture.list);
                fixture.list = nullptr;
                visibility.RequestRestore();
                check(!visibility.TryRestore(nullptr) && visibility.RestorePending(),
                    "missing Explorer ListView retains the restoration request");
                fixture.list = fixture.CreateList();
                check(fixture.list != nullptr, "recreate Explorer icon layer without its old marker");
                if (fixture.list)
                {
                    ShowWindow(fixture.list, SW_HIDE);
                    check(visibility.TryRestore(fixture.list) && IsWindowVisible(fixture.list),
                        "restoration survives replacement of the originally hidden HWND");

                    bool blockShow = true;
                    check(SetWindowSubclass(fixture.list, BlockShow, 1, reinterpret_cast<DWORD_PTR>(&blockShow)) != FALSE,
                        "install deterministic failed-show fixture");
                    visibility.Hide(fixture.list);
                    visibility.RequestRestore();
                    check(!visibility.TryRestore(fixture.list) && visibility.RestorePending() &&
                            GetPropW(fixture.list, kHiddenBySnowDesktopProp),
                        "a show request without visible icons must retain its marker and retry");
                    blockShow = false;
                    check(visibility.TryRestore(fixture.list) && IsWindowVisible(fixture.list),
                        "the next host poll completes a previously unsuccessful show");
                    RemoveWindowSubclass(fixture.list, BlockShow, 1);

                    visibility.Hide(fixture.list);
                    ShowWindow(fixture.parent, SW_HIDE);
                    visibility.RequestRestore();
                    check(!visibility.TryRestore(fixture.list) && visibility.RestorePending() &&
                            GetPropW(fixture.list, kHiddenBySnowDesktopProp),
                        "an invisible Explorer ancestor cannot acknowledge successful restoration");
                    ShowWindow(fixture.parent, SW_SHOWNOACTIVATE);
                    check(visibility.TryRestore(fixture.list) && IsWindowVisible(fixture.list),
                        "restoration completes after the Explorer host becomes visible");

                    visibility.Hide(fixture.list);
                    visibility.RequestRestore();
                    visibility.Hide(fixture.list);
                    check(!visibility.RestorePending() && !IsWindowVisible(fixture.list) &&
                            visibility.TryRestore(nullptr),
                        "re-enabling the custom desktop cancels the pending native restore");
                    snowdesktop::ExplorerIconVisibility recovered;
                    check(recovered.TryRestore(fixture.list) && IsWindowVisible(fixture.list),
                        "a surviving ownership marker supports recovery without cached state");

                    ShowWindow(fixture.list, SW_HIDE);
                    snowdesktop::ExplorerIconVisibility untouched;
                    untouched.Hide(fixture.list);
                    untouched.RequestRestore();
                    check(untouched.TryRestore(fixture.list) && !IsWindowVisible(fixture.list),
                        "ordinary cleanup preserves icons that SnowDesktop never hid");
                    untouched.RequestRestore(true);
                    check(untouched.TryRestore(fixture.list) && IsWindowVisible(fixture.list),
                        "explicit selection restores icons even without earlier hide ownership");

                    untouched.Hide(fixture.list);
                    DestroyWindow(fixture.list);
                    fixture.list = nullptr;
                    unsigned discoveries = 0;
                    check(untouched.RestoreWhenReady([&] {
                            if (++discoveries == 2)
                            {
                                fixture.list = fixture.CreateList();
                                if (fixture.list) ShowWindow(fixture.list, SW_HIDE);
                            }
                            return fixture.list;
                        }, 500) && discoveries == 2 && IsWindowVisible(fixture.list),
                        "exit recovery rediscovers an icon layer created after the initial attempt");
                    if (fixture.list)
                    {
                        untouched.Hide(fixture.list);
                        const auto start = GetTickCount64();
                        check(!untouched.RestoreWhenReady([] { return HWND{}; }, 20) &&
                                untouched.RestorePending() && GetTickCount64() - start < 500,
                            "exit recovery stops within a finite budget without discarding the request");
                        check(untouched.RestoreWhenReady([&] { return fixture.list; }) &&
                                IsWindowVisible(fixture.list),
                            "final cleanup can finish an earlier bounded recovery attempt");
                    }
                }
            }
        }
        // A busy Explorer input queue must not make the exit retry block in
        // User32, and posting SHOW alone must not be recorded as success.
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE pump = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(ready && pump && stop, "create independent Shell-thread barriers");
        if (ready && pump && stop)
        {
            HWND remoteList = nullptr;
            std::thread shell([&] {
                if (SetThreadDesktop(desktop))
                {
                    Fixture fixture;
                    remoteList = fixture.list;
                    if (remoteList) ShowWindow(remoteList, SW_HIDE);
                    SetEvent(ready);
                    HANDLE barriers[]{stop, pump};
                    if (WaitForMultipleObjects(2, barriers, FALSE, 2000) == WAIT_OBJECT_0 + 1)
                    {
                        while (MsgWaitForMultipleObjects(1, &stop, FALSE, 2000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
                        {
                            MSG message{};
                            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                                DispatchMessageW(&message);
                        }
                    }
                }
                else SetEvent(ready);
            });
            const bool shellReady = WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0;
            check(shellReady && remoteList, "independent Shell queue creates its icon layer");
            if (shellReady && remoteList)
            {
                snowdesktop::ExplorerIconVisibility visibility;
                check(SetPropW(remoteList, kHiddenBySnowDesktopProp, reinterpret_cast<HANDLE>(1)) != FALSE,
                    "preserve recovery evidence on the independent icon layer");
                visibility.RequestRestore(true);
                const auto start = GetTickCount64();
                check(!visibility.TryRestore(remoteList) && visibility.RestorePending() &&
                        GetTickCount64() - start < 500 && GetPropW(remoteList, kHiddenBySnowDesktopProp),
                    "an unresponsive Shell queue returns promptly without false restoration success");
                SetEvent(pump);
                check(visibility.RestoreWhenReady([&] { return remoteList; }, 1000) &&
                        IsWindowVisible(remoteList) && !GetPropW(remoteList, kHiddenBySnowDesktopProp),
                    "exit recovery observes actual display after the Shell queue resumes");
            }
            SetEvent(stop);
            shell.join();
        }
        if (ready) CloseHandle(ready);
        if (pump) CloseHandle(pump);
        if (stop) CloseHandle(stop);
        check(SetThreadDesktop(original) != FALSE, "restore original fixture thread desktop");
        check(CloseDesktop(desktop) != FALSE, "release private icon restoration desktop");
    }).join();
    return failures;
}
