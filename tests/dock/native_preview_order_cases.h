#pragma once

// Refs #26: exercise the actual WinEvent -> host dispatcher -> preview collector.
// Only OS event input is supplied explicitly. Windows, identities, Z order and
// enumeration are real. No SnowDesktop desktop window is created or operated.
namespace native_preview_order_cases
{
using Access = NativeDragIntegrationAccess;
inline constexpr wchar_t kClass[] = L"SnowDesktopPreviewOrderFixture";
inline constexpr UINT kCreate = WM_APP + 21;
inline constexpr UINT kRaise = WM_APP + 22;
inline constexpr UINT kShow = WM_APP + 23;
inline constexpr UINT kDestroy = WM_APP + 24;
inline constexpr UINT kRename = WM_APP + 25;
inline HWND controller = nullptr;
inline std::vector<HWND> fixtureWindows;

inline void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

inline LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message)
    {
    case kCreate:
    {
        const bool proxy = lp != 0;
        const std::wstring title = L"Preview " + std::to_wstring(wp);
        const HWND created = CreateWindowExW(proxy
                ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_WINDOWEDGE
                : WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
            kClass, title.c_str(), proxy ? WS_POPUP | WS_CAPTION : WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            -16000, -16000, 300, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (created) fixtureWindows.push_back(created);
        return reinterpret_cast<LRESULT>(created);
    }
    case kRaise:
        return SetWindowPos(reinterpret_cast<HWND>(wp), HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    case kShow:
        ShowWindow(reinterpret_cast<HWND>(wp), static_cast<int>(lp));
        return TRUE;
    case kDestroy:
        return DestroyWindow(reinterpret_cast<HWND>(wp));
    case kRename:
        return SetWindowTextW(reinterpret_cast<HWND>(wp), L"Renamed preview");
    case WM_CLOSE:
        if (window == controller)
        {
            for (const HWND fixture : fixtureWindows)
                if (IsWindow(fixture)) DestroyWindow(fixture);
            DestroyWindow(window);
            PostQuitMessage(0);
            return 0;
        }
        break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

inline int RunFixture(const char* mappingText, const char* readyText)
{
    const HANDLE mapping = reinterpret_cast<HANDLE>(std::stoull(mappingText));
    const HANDLE ready = reinterpret_cast<HANDLE>(std::stoull(readyText));
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = FixtureProc;
    cls.lpszClassName = kClass;
    if (!RegisterClassW(&cls)) return 2;
    controller = CreateWindowExW(0, kClass, L"Preview order controller", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!controller) return 3;
    auto* shared = static_cast<HWND*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(HWND)));
    if (!shared) return 4;
    *shared = controller;
    UnmapViewOfFile(shared);
    SetEvent(ready);
    CloseHandle(mapping);
    CloseHandle(ready);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

struct Fixture
{
    HANDLE mapping = nullptr;
    HANDLE ready = nullptr;
    HANDLE process = nullptr;
    HWND target = nullptr;
    std::wstring executable;
    void Start(const wchar_t* desktop = nullptr)
    {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(HWND), nullptr);
        ready = CreateEventW(&attributes, TRUE, FALSE, nullptr);
        Require(mapping && ready, "create isolated preview fixture handshake");
        wchar_t path[32768]{};
        Require(GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path))) != 0,
            "resolve preview fixture executable");
        executable = path;
        std::wstring command = L"\"" + executable + L"\" --dock-preview-fixture " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping)) + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(ready));
        STARTUPINFOW startup{sizeof(startup)};
        startup.lpDesktop = const_cast<wchar_t*>(desktop);
        PROCESS_INFORMATION child{};
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE,
            "start owned preview fixture process");
        process = child.hProcess;
        CloseHandle(child.hThread);
        // Process identity uses the resolved image path, including when the
        // fixture was launched through a build-directory junction.
        DWORD imagePathSize = static_cast<DWORD>(std::size(path));
        Require(QueryFullProcessImageNameW(process, 0, path, &imagePathSize) != FALSE,
            "resolve the actual owned preview fixture image path");
        executable.assign(path, imagePathSize);
        const HANDLE waits[]{ready, process};
        Require(WaitForMultipleObjects(2, waits, FALSE, 10000) == WAIT_OBJECT_0,
            "owned preview fixture must signal readiness before exiting");
        const auto* shared = static_cast<const HWND*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(HWND)));
        Require(shared != nullptr, "read owned preview fixture controller");
        target = *shared;
        UnmapViewOfFile(shared);
        Require(IsWindow(target) != FALSE, "owned preview fixture controller exists");
    }
    ~Fixture()
    {
        if (target) PostMessageW(target, WM_CLOSE, 0, 0);
        if (process)
        {
            // Only the child launched above may be terminated on fixture failure.
            if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT)
            {
                TerminateProcess(process, 5);
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        }
        if (mapping) CloseHandle(mapping);
        if (ready) CloseHandle(ready);
    }
    DWORD_PTR Send(UINT message, WPARAM wp = 0, LPARAM lp = 0)
    {
        DWORD_PTR result = 0;
        Require(SendMessageTimeoutW(target, message, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                5000, &result) != 0, "owned preview fixture command completes");
        Require(result != 0, "owned preview fixture command succeeds");
        return result;
    }
    HWND Create(unsigned index, bool proxy)
    {
        const HWND window = reinterpret_cast<HWND>(Send(kCreate, index, proxy));
        Require(window != nullptr, "create a real off-screen application/proxy window");
        return window;
    }
};

#include "native_running_refresh_cases.h"
#include "native_preview_close_refresh_cases.h"

inline void CheckClosePreview(int legacyRoute)
{
    native_drag_commit_cases::Profile profile;
    auto& app = profile.Model(false, profile.root / L"close-preview");
    Access::PreparePreviewOrder(app);
    Fixture fixture;
    fixture.Start();
    const HWND first = fixture.Create(10, false);
    const HWND middle = fixture.Create(11, false);
    const HWND last = fixture.Create(12, false);
    auto& preview = Access::PrepareClosePreview(app,
        {{first, L"first"}, {middle, L"middle"}, {last, L"last"}}, legacyRoute);
    const auto popups = preview.GetWindows();
    Require(popups.size() == 3, "three owned windows have preview cards before closing");
    RECT firstBounds{}, lastBounds{};
    GetWindowRect(popups[0], &firstBounds);
    GetWindowRect(popups[2], &lastBounds);
    const auto clickClose = [&](HWND popup) {
        RECT client{};
        GetClientRect(popup, &client);
        const RECT close = CalculateDockWindowPreviewCloseButtonRect(client, GetDpiForWindow(popup));
        SendMessageW(popup, WM_LBUTTONUP, 0, MAKELPARAM(
            (close.left + close.right) / 2, (close.top + close.bottom) / 2));
    };
    clickClose(popups[1]);
    constexpr const char* retentionFailure = "closing one window must keep the remaining preview cards open";
    Require(preview.IsVisible() && preview.IsShowingWindow(first) && preview.IsShowingWindow(last) &&
            !preview.IsShowingWindow(middle) && preview.GetWindows().size() == 2 &&
            !Access::ClosePreviewSuppressed(app), retentionFailure);
    Require(preview.GetWindows() == std::vector<HWND>{popups[0], popups[2]},
        "closing the middle source retires only its native card");
    RECT afterFirst{}, afterLast{};
    GetWindowRect(popups[0], &afterFirst);
    GetWindowRect(popups[2], &afterLast);
    if (legacyRoute == 3)
    {
        // Restore the original no-relayout geometry on the same native cards.
        for (const auto& [popup, rect] : {std::pair{popups[0], firstBounds}, std::pair{popups[2], lastBounds}})
            SetWindowPos(popup, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        GetWindowRect(popups[0], &afterFirst);
        GetWindowRect(popups[2], &afterLast);
    }
    const LONG originalCenter = (firstBounds.left + lastBounds.right) / 2;
    Require(afterLast.right - afterFirst.left < lastBounds.right - firstBounds.left &&
            std::abs((afterFirst.left + afterLast.right) / 2 - originalCenter) <= 1,
        "remaining cards must compact and recenter around the same Dock anchor");
    Require(Access::PreviewItems(app, fixture.executable).size() == 2,
        "accepted close is excluded from the real host preview collector");
    SendMessageW(popups[0], WM_MBUTTONUP, 0, MAKELPARAM(10, 10));
    Require(preview.IsVisible() && preview.GetWindows() == std::vector<HWND>{popups[2]} &&
            preview.IsShowingWindow(last) && !preview.IsShowingWindow(first),
        "middle-clicking the primary card retains the final card and transfers timer ownership");
    RECT finalBounds{};
    GetWindowRect(popups[2], &finalBounds);
    Require(std::abs((finalBounds.left + finalBounds.right) / 2 - originalCenter) <= 1,
        "the final surviving card must recenter without another Dock hover");
    clickClose(popups[2]);
    Require(preview.IsCleared() && Access::ClosePreviewSuppressed(app),
        "closing the final window dismisses the preview until pointer leave");
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while ((IsWindow(first) || IsWindow(middle) || IsWindow(last)) && GetTickCount64() < deadline)
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    Require(!IsWindow(first) && !IsWindow(middle) && !IsWindow(last),
        "production tracked close requests actually destroy only the three owned source windows");
}

inline void RunClosePreview()
{
    for (const int legacyRoute : {1, 2, 3})
    {
        bool rejected = false;
        try { CheckClosePreview(legacyRoute); }
        catch (const std::runtime_error& error)
        {
            const std::string_view expected = legacyRoute == 3
                ? "remaining cards must compact and recenter around the same Dock anchor"
                : "closing one window must keep the remaining preview cards open";
            if (std::string_view(error.what()) != expected) throw;
            rejected = true;
        }
        Require(rejected, "old dismissal and no-relayout routes must fail their corresponding business assertions");
        std::cout << "old close route " << legacyRoute << " rejected by its preview business assertion\n";
    }
    CheckClosePreview(0);
}

inline void RunLivePreviewRefresh()
{
    native_drag_commit_cases::Profile profile;
    auto& app = profile.Model(false, profile.root / L"live-preview");
    Access::PreparePreviewOrder(app);
    Fixture fixture;
    fixture.Start();
    const HWND first = fixture.Create(30, false);
    Access::NotifyPreviewWindow(app, first, EVENT_OBJECT_CREATE);
    auto& preview = Access::PrepareClosePreview(app, Access::PreviewItems(app, fixture.executable), 0);
    Access::TrackLivePreview(app, fixture.executable);
    const HWND initialCard = preview.GetWindow();
    Require(preview.GetWindows().size() == 1, "live preview begins with one source window");
    const HWND second = fixture.Create(31, false);
    Access::NotifyPreviewWindow(app, second, EVENT_OBJECT_SHOW);
    Require(preview.IsVisible() && preview.GetWindows().size() == 2 && preview.IsShowingWindow(second) &&
            preview.GetWindow() == initialCard && !Access::ClosePreviewSuppressed(app),
        "native create/show event must add a new source to the open preview without another Dock hover");
    const HWND third = fixture.Create(32, false);
    Access::NotifyPreviewWindow(app, third, EVENT_OBJECT_CREATE);
    Require(preview.GetWindows().size() == 3 && preview.IsShowingWindow(third),
        "new source membership continues updating after the first live refresh");
    fixture.Send(kDestroy, reinterpret_cast<WPARAM>(second));
    Access::NotifyPreviewWindow(app, second, EVENT_OBJECT_DESTROY);
    Require(preview.IsVisible() && preview.GetWindows().size() == 2 && preview.IsShowingWindow(first) &&
            preview.IsShowingWindow(third) && !preview.IsShowingWindow(second),
        "source destruction must refresh only current membership while retaining the open view");
    preview.Hide();
}

inline void Run(int onlyVariant = -1)
{
    native_drag_commit_cases::Profile profile;
    for (const bool proxy : {false, true})
    {
        if (onlyVariant != -1 && onlyVariant != static_cast<int>(proxy)) continue;
        auto& app = profile.Model(false, profile.root / (proxy ? L"proxy-order" : L"window-order"));
        Access::PreparePreviewOrder(app);
        Fixture fixture;
        fixture.Start();
        std::vector<HWND> owned;
        for (unsigned index = 0; index < 3; ++index)
        {
            owned.push_back(fixture.Create(index, proxy));
            Access::NotifyPreviewWindow(app, owned.back(), EVENT_OBJECT_CREATE);
        }
        const auto expect = [&](std::initializer_list<HWND> expected, const char* message) {
            std::vector<HWND> actual;
            for (const auto& item : Access::PreviewItems(app, fixture.executable))
                if (std::find(owned.begin(), owned.end(), item.window) != owned.end()) actual.push_back(item.window);
            Require(actual == std::vector<HWND>(expected), message);
        };
        fixture.Send(kRaise, reinterpret_cast<WPARAM>(owned[0]));
        fixture.Send(kRaise, reinterpret_cast<WPARAM>(owned[1]));
        // The literal expected order comes from creation calls, never EnumWindows.
        expect({owned[0], owned[1], owned[2]}, proxy
            ? "document proxies must retain opening order before the first preview"
            : "regular windows must retain opening order before the first preview");
        fixture.Send(kRaise, reinterpret_cast<WPARAM>(owned[2]));
        Access::NotifyPreviewWindow(app, owned[2], EVENT_SYSTEM_FOREGROUND);
        expect({owned[0], owned[1], owned[2]}, "foreground/Z-order changes must not reorder preview cards");
        Require(fixture.Send(kRename, reinterpret_cast<WPARAM>(owned[1])) != 0, "rename fixture window");
        const auto renamed = Access::PreviewItems(app, fixture.executable);
        Require(std::any_of(renamed.begin(), renamed.end(), [&](const auto& item) {
            return item.window == owned[1] && item.title == L"Renamed preview";
        }), "stable preview order must still refresh window titles");
        if (!proxy)
        {
            fixture.Send(kShow, reinterpret_cast<WPARAM>(owned[1]), SW_HIDE);
            Access::NotifyPreviewWindow(app, owned[1], EVENT_OBJECT_HIDE);
            expect({owned[0], owned[2]}, "hiding a window removes only its current preview");
            fixture.Send(kShow, reinterpret_cast<WPARAM>(owned[1]), SW_SHOWNOACTIVATE);
            Access::NotifyPreviewWindow(app, owned[1], EVENT_OBJECT_SHOW);
            expect({owned[0], owned[1], owned[2]}, "showing a surviving HWND restores its original position");
            fixture.Send(kShow, reinterpret_cast<WPARAM>(owned[1]), SW_SHOWMINNOACTIVE);
            Access::NotifyPreviewWindow(app, owned[1], EVENT_SYSTEM_MINIMIZESTART);
            expect({owned[0], owned[1], owned[2]}, "minimizing must retain the opening order");
            fixture.Send(kShow, reinterpret_cast<WPARAM>(owned[1]), SW_SHOWNOACTIVATE);
            Access::NotifyPreviewWindow(app, owned[1], EVENT_SYSTEM_MINIMIZEEND);
            expect({owned[0], owned[1], owned[2]}, "restoring must retain the opening order");
        }
        Require(fixture.Send(kDestroy, reinterpret_cast<WPARAM>(owned[1])) != 0, "destroy fixture window");
        Access::NotifyPreviewWindow(app, owned[1], EVENT_OBJECT_DESTROY);
        owned.push_back(fixture.Create(3, proxy));
        Access::NotifyPreviewWindow(app, owned.back(), EVENT_OBJECT_CREATE);
        expect({owned[0], owned[2], owned[3]}, "a newly opened window appends after survivors, including recycled HWNDs");
    }
    std::cout << "actual host preview opening-order cases passed\n";
    if (onlyVariant != 1)
    {
        RunRunningRefresh();
        RunClosePreview();
        RunCloseRefresh();
        RunLivePreviewRefresh();
    }
}
}
