#include "dock/dock_fullscreen_policy.h"
#include "dock/dock_fullscreen_storage.h"
#include "dock/dock_running_animation.h"
#include "platform/foreground_fullscreen.h"

#include <iostream>
#include <map>
#include <thread>

namespace
{
// The production picker collector consumes Dock caches; replace only the OS
// window/path reader so fixed entries need no live desktop or installed game.
void CheckApplicationPicker(const auto& check)
{
    namespace policy = snowdesktop::dock_fullscreen;
    struct Running
    {
        snowdesktop::dock_running_animation::Presence presence;
        HWND window;
        std::wstring title, executablePath;
    };
    struct Pinned
    {
        HWND window;
        bool running;
        std::vector<HWND> trackedWindows;
    };
    const auto window = [](UINT_PTR id) { return reinterpret_cast<HWND>(id); };
    Running player{{}, window(1), L"Player", L"C:\\Apps\\Player.exe"};
    player.presence.SetVisible(true, 0, false);
    Running exiting{{}, window(7), L"Exiting", L"C:\\Apps\\Exiting.exe"};
    exiting.presence.SetVisible(true, 0, false);
    exiting.presence.SetVisible(false, 0, true);
    const std::vector<Running> running{player, exiting};
    const std::map<std::wstring, Pinned> pinned{
        {L"game", {window(2), true, {window(2), window(3)}}},
        {L"helper", {window(4), true, {window(4), window(5)}}},
        {L"not-running", {window(6), false, {window(6)}}},
        {L"steam-process-only", {nullptr, true, {}}}};
    const std::map<HWND, DockFullscreenApplication> live{
        {window(1), {L"Player", L"C:/Apps/PLAYER.exe"}},
        {window(2), {L"Game", L"D:\\Games\\game.exe"}},
        {window(3), {L"Second game window", L"d:/games/GAME.EXE"}},
        {window(4), {L"Player pinned too", L"C:\\Apps\\Player.exe"}},
        {window(5), {L"Game child", L"D:\\Games\\bin\\client.exe"}},
        {window(6), {L"Stopped", L"C:\\Apps\\Stopped.exe"}},
        {window(7), {L"Exiting", L"C:\\Apps\\Exiting.exe"}}};
    const auto readWindow = [&](HWND handle, const std::wstring&, const std::wstring&)
        -> std::optional<DockFullscreenApplication> {
        const auto found = live.find(handle);
        return found == live.end() ? std::nullopt : std::optional{found->second};
    };
    const auto applications = policy::CollectApplications(running, pinned, readWindow);
    const std::vector<DockFullscreenApplication> expected{
        {L"Player", L"C:\\Apps\\PLAYER.exe"},
        {L"Game", L"D:\\Games\\game.exe"},
        {L"Game child", L"D:\\Games\\bin\\client.exe"}};
    check(applications == expected,
        "picker includes running fixed apps and real child executables, deduplicates windows/paths and excludes stopped/exiting entries");
    const std::vector<Running> emptyRunning;
    const std::map<std::wstring, Pinned> onlyPinned{
        {L"game", {window(2), true, {window(2)}}}};
    check(policy::CollectApplications(emptyRunning, onlyPinned, readWindow) ==
        std::vector<DockFullscreenApplication>{{L"Game", L"D:\\Games\\game.exe"}},
        "pinning the only running app cannot empty the exception picker");
    auto stalePinned = onlyPinned;
    stalePinned.begin()->second.window = window(99);
    stalePinned.begin()->second.trackedWindows = {window(99)};
    check(policy::CollectApplications(emptyRunning, stalePinned, readWindow).empty(),
        "closed windows remaining in a Dock cache cannot become exceptions");
}
}

int RunFullscreenFocusTests()
{
    namespace policy = snowdesktop::dock_fullscreen;
    namespace fullscreen = snowdesktop::fullscreen;
    int failures = 0;
    const auto check = [&](bool value, const char* description) {
        if (!value) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    CheckApplicationPicker(check);
    using P = DockFullscreenPolicy;
    using S = DockRevealSource;
    check(policy::LoadPolicy({}, {}) == P::FullProtection,
        "fresh preferences protect both exclusive and borderless fullscreen");
    check(policy::LoadPolicy({}, true) == P::BlockGestures && policy::LoadPolicy({}, false) == P::Allow,
        "legacy toggle migration retains the user's deliberate shortcut availability");
    check(policy::LoadPolicy(0, true) == P::Allow && policy::LoadPolicy(2, false) == P::FullProtection,
        "saved new policies take precedence over the downgrade compatibility field");
    check(policy::LoadPolicy(99, false) == P::FullProtection,
        "corrupt policy cannot silently disable fullscreen protection");
    check(policy::BlocksReveal(P::FullProtection, S::Hotkey, true, false) &&
        policy::BlocksReveal(P::FullProtection, S::AssociatedSurface, true, false) &&
        policy::BlocksReveal(P::FullProtection, S::Gesture, true, false),
        "full protection blocks every reveal source even with the cursor on another monitor");
    check(!policy::BlocksReveal(P::BlockGestures, S::Hotkey, true, true) &&
        policy::BlocksReveal(P::BlockGestures, S::Gesture, true, true) &&
        !policy::BlocksReveal(P::BlockGestures, S::Gesture, true, false),
        "gesture-only protection preserves explicit hotkeys and the other monitor's passive Dock");
    check(!policy::BlocksReveal(P::Allow, S::Gesture, true, true) &&
        !policy::BlocksReveal(P::FullProtection, S::Hotkey, false, true),
        "opt-outs and ordinary windows preserve invocation");
    check(!policy::StartsKeyboardSession(S::Gesture) && policy::StartsKeyboardSession(S::Hotkey),
        "gesture reveals cannot create the keyboard proxy that minimizes exclusive games");

    std::vector<DockFullscreenException> exceptions{
        {L"C:\\Apps\\Player.exe", P::Allow}, {L"C:/Apps/PLAYER.exe", P::BlockGestures},
        {L"player.exe", P::Allow}, {L"D:\\Games\\game.exe", P::FullProtection}};
    policy::NormalizeExceptions(exceptions);
    check(exceptions.size() == 2 && policy::SelectPolicy(P::FullProtection, exceptions,
        L"\\\\?\\C:\\Apps\\player.exe") == P::BlockGestures,
        "full executable paths normalize separators, extended prefixes and case; duplicates update instead of expanding scope");
    check(policy::SelectPolicy(P::FullProtection, exceptions, L"D:\\Other\\Player.exe") == P::FullProtection,
        "same filename in another directory cannot inherit a player exception");
    check(policy::NormalizeExecutable(std::wstring(L"C:\\Player.exe\0Other.exe", 23)).empty(),
        "embedded null cannot shorten the executable exception to a different file");
    JsonValue document;
    check(ParseJson(R"({"floatingEdgeSwipeBlockFullscreen":true})", document) &&
        policy::DecodePolicy(document) == P::BlockGestures,
        "the production JSON loader migrates the legacy true toggle to gestures only");
    check(ParseJson(R"({"fullscreenPolicy":"invalid","floatingEdgeSwipeBlockFullscreen":false})", document) &&
        policy::DecodePolicy(document) == P::FullProtection,
        "an invalid new field cannot silently inherit an old opt-out");
    check(ParseJson(R"({"fullscreenExceptions":[{"executable":"C:\\Apps\\播放器.exe","policy":0}]})", document) &&
        policy::DecodeExceptions(document, exceptions) && exceptions.size() == 1 && exceptions[0].executable == L"C:\\Apps\\播放器.exe",
        "persisted exceptions retain Unicode executable paths");
    const auto previous = exceptions;
    check(ParseJson(R"({"fullscreenExceptions":[{"executable":"player.exe","policy":0}]})", document) &&
        !policy::DecodeExceptions(document, exceptions) && exceptions == previous,
        "invalid exception cannot broaden matching or partially replace prior preferences");

    int activationCalls = 0;
    bool fullscreenActive = true;
    const auto observe = [&]() { return fullscreenActive; };
    const auto activate = [&]() { ++activationCalls; };
    check(!fullscreen::GuardFocusRequest(false, observe, activate),
        "automatic completion must never steal foreground focus, independent of shortcut settings");
    check(activationCalls == 0, "fullscreen gate prevents activation side effects, not just invocation labels");
    fullscreenActive = false;
    check(fullscreen::GuardFocusRequest(false, observe, activate), "ordinary desktop interaction can request input focus");
    fullscreenActive = true;
    check(!fullscreen::GuardFocusRequest(false, observe, activate) && activationCalls == 1,
        "a game entering fullscreen before an attached-input retry prevents the second activation");
    check(fullscreen::GuardFocusRequest(true, observe, activate) && activationCalls == 2,
        "an explicitly allowed hotkey can intentionally leave fullscreen");

    // Real geometry/style observation on a private desktop. Never switch the
    // user's input desktop, activate the production host or drive a game.
    const auto name = L"SnowFullscreenFocus_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
    check(desktop != nullptr, "create isolated fullscreen fixture desktop");
    if (!desktop) return failures;
    std::thread fixture([&]() {
        if (!SetThreadDesktop(desktop)) { check(false, "attach isolated fixture thread"); return; }
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = instance;
        cls.lpszClassName = L"SnowFullscreenFixture";
        if (!RegisterClassW(&cls)) { check(false, "register fixture window"); return; }
        MONITORINFO monitor{sizeof(monitor)};
        const auto handle = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(handle, &monitor)) { check(false, "read fixture monitor"); UnregisterClassW(cls.lpszClassName, instance); return; }
        const RECT rect = monitor.rcMonitor;
        HWND window = CreateWindowExW(0, cls.lpszClassName, L"Fullscreen fixture", WS_POPUP,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
        check(window != nullptr, "create fullscreen-sized fixture");
        if (window)
        {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            check(fullscreen::ObserveWindow(window, 0).has_value(), "native borderless client is detected without a D3D-exclusive hint");
            check(!fullscreen::ObserveWindow(window).has_value(), "own-process desktop surfaces do not trigger external fullscreen protection");
            int nativeActivationCalls = 0;
            check(!fullscreen::GuardFocusRequest(false, [&]() { return fullscreen::ObserveWindow(window, 0); },
                [&]() { ++nativeActivationCalls; SetFocus(window); }) && nativeActivationCalls == 0,
                "native fullscreen observation blocks the production focus boundary before SetFocus");
            SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_NOACTIVATE);
            check(!fullscreen::ObserveWindow(window, 0), "fullscreen-sized passive overlays are excluded");
            SetWindowLongPtrW(window, GWL_EXSTYLE, 0);
            SetPropW(window, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
            check(!fullscreen::ObserveWindow(window, 0), "Shell overlay exclusion is respected");
            RemovePropW(window, L"NonRudeHWND");
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top - 80, SWP_NOACTIVATE | SWP_NOZORDER);
            check(!fullscreen::ObserveWindow(window, 0), "windowed or taskbar-sized clients are not fullscreen");
            ShowWindow(window, SW_HIDE);
            check(!fullscreen::ObserveWindow(window, 0), "hidden background fullscreen clients cannot block invocation");
            DestroyWindow(window);
        }
        UnregisterClassW(cls.lpszClassName, instance);
    });
    fixture.join();
    CloseDesktop(desktop);
    return failures;
}
