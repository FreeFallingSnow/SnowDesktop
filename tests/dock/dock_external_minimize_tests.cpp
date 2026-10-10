#include "dock/dock_external_minimize.h"
#include "dock/dock_minimize_protocol.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <functional>
#include <string>
#include <cstdlib>

namespace
{
constexpr UINT kReady = WM_APP + 1;
constexpr UINT kAction = WM_APP + 2;
constexpr UINT kHostReady = WM_APP + 3;
constexpr UINT kCrashHost = WM_APP + 4;
HWND fixture = nullptr;
HWND hostReceiver = nullptr;
HWND observationReceiver = nullptr;
int requests = 0;
int cancellations = 0;
bool observedBeforeMinimize = false;
bool slowReceiver = false;
bool acceptReceiver = false;
int failures = 0;

void Check(bool condition, const char* name)
{
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

bool PumpUntil(const std::function<bool()>& predicate, DWORD timeout = 3000)
{
    const ULONGLONG end = GetTickCount64() + timeout;
    while (!predicate() && GetTickCount64() < end)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    return predicate();
}

LRESULT CALLBACK ReceiverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kReady) { fixture = reinterpret_cast<HWND>(wParam); return 0; }
    if (message == kHostReady) { hostReceiver = reinterpret_cast<HWND>(wParam); return 0; }
    if (observationReceiver && message == kCrashHost) ExitProcess(0);
    if (observationReceiver && message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (observationReceiver && message >= 0xC000 &&
        (message == snowdesktop::dock_minimize::RequestMessage() ||
         message == snowdesktop::dock_minimize::CancelMessage()))
    {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(observationReceiver, message, wParam, lParam,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, snowdesktop::dock_minimize::kRequestTimeoutMs, &result);
        return static_cast<LRESULT>(result);
    }
    if (message >= 0xC000 && message == snowdesktop::dock_minimize::RequestMessage())
    {
        ++requests;
        observedBeforeMinimize = !IsIconic(reinterpret_cast<HWND>(wParam)) &&
            snowdesktop::dock_minimize::RequestIsCurrent(static_cast<DWORD>(lParam), GetTickCount());
        if (slowReceiver) Sleep(snowdesktop::dock_minimize::kRequestTimeoutMs + 80);
        return acceptReceiver ? 1 : 0;
    }
    if (message >= 0xC000 && message == snowdesktop::dock_minimize::CancelMessage())
    {
        ++cancellations;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kAction)
    {
        if (wParam == 0) return DefWindowProcW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        if (wParam == 1) ShowWindow(window, SW_SHOWNOACTIVATE);
        if (wParam == 2) ShowWindow(window, SW_MINIMIZE);
        if (wParam == 3) ShowWindow(window, SW_MAXIMIZE);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateFixtureWindow(const wchar_t* name, WNDPROC procedure)
{
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = name;
    type.lpfnWndProc = procedure;
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowExW(WS_EX_TOOLWINDOW, name, name, WS_OVERLAPPEDWINDOW,
        -20000, -20000, 240, 160, nullptr, nullptr, type.hInstance, nullptr);
}

HWND WindowArgument(const wchar_t* argument)
{
    return reinterpret_cast<HWND>(ULongToHandle(wcstoul(argument, nullptr, 10)));
}

void CheckHostRestart(HWND receiver, const std::wstring& executable,
    const std::filesystem::path& dll, const std::wstring& helper, const std::wstring& helperDll,
    const std::filesystem::path& copyDirectory, bool crash)
{
    std::error_code error;
    std::filesystem::create_directories(copyDirectory, error);
    Check(!error, "private restart runtime directory is created");
    const auto copy = [&](const std::filesystem::path& source) {
        if (source.empty()) return std::wstring{};
        const auto target = copyDirectory / source.filename();
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
        Check(!error, "restart uses a fresh private runtime copy");
        return target.wstring();
    };
    const auto privateDll = copy(dll);
    const auto privateHelper = copy(helper);
    const auto privateHelperDll = copy(helperDll);
    if (error) return;
    hostReceiver = nullptr;
    std::wstring command = L"\"" + executable + L"\" --host " +
        std::to_wstring(HandleToULong(receiver)) + L" " +
        std::to_wstring(HandleToULong(fixture)) + L" \"" + privateDll +
        L"\" \"" + privateHelper + L"\" \"" + privateHelperDll + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
    Check(started, "replacement hook host starts in a separate process");
    if (!started) return;
    CloseHandle(child.hThread);
    const bool ready = PumpUntil([&] { return hostReceiver || WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0; });
    Check(ready && hostReceiver, "replacement host attaches the already running application");
    if (hostReceiver)
    {
        const int before = requests;
        PostMessageW(fixture, kAction, 1, 0);
        Check(PumpUntil([] { return !IsIconic(fixture); }), "existing fixture is restored before restart regression");
        PostMessageW(fixture, kAction, 0, 0);
        Check(PumpUntil([] { return IsIconic(fixture) != FALSE; }), "existing application minimizes after host replacement");
        Check(requests == before + 1 && observedBeforeMinimize,
            "fresh private hook reports existing application's minimize exactly once after restart");
        PostMessageW(hostReceiver, crash ? kCrashHost : WM_CLOSE, 0, 0);
    }
    if (!PumpUntil([&] { return WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0; }))
    {
        Check(false, "owned replacement host exits");
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 1000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(child.hProcess, &exitCode);
    Check(exitCode == 0, "replacement host reports successful setup and shutdown");
    CloseHandle(child.hProcess);
    if (hostReceiver)
        Check(crash ? GetPropW(fixture, snowdesktop::dock_minimize::kTargetProperty) == hostReceiver :
                GetPropW(fixture, snowdesktop::dock_minimize::kTargetProperty) == nullptr,
            crash ? "abrupt host exit leaves a stale marker for recovery" : "normal host exit removes its marker");
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 7 && std::wstring(argv[1]) == L"--host")
    {
        observationReceiver = WindowArgument(argv[2]);
        const HWND target = WindowArgument(argv[3]);
        const HWND receiver = CreateFixtureWindow(L"SnowDesktopMinimizeHostReceiver", ReceiverProc);
        DockExternalMinimize monitor;
        if (!receiver || !monitor.Start(receiver, argv[4], argv[5], argv[6])) return 2;
        monitor.UpdateTargets(std::array<HWND, 1>{target});
        if (!monitor.OwnsTarget(target) || (*argv[5] &&
            !PumpUntil([&] { return GetPropW(target, snowdesktop::dock_minimize::kReadyProperty) == receiver; })))
            return 3;
        PostMessageW(observationReceiver, kHostReady, reinterpret_cast<WPARAM>(receiver), 0);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        monitor.Stop();
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--fixture")
    {
        const HWND receiver = reinterpret_cast<HWND>(ULongToHandle(wcstoul(argv[2], nullptr, 10)));
        const HWND window = CreateFixtureWindow(L"SnowDesktopMinimizeFixture", FixtureProc);
        if (!window) return 1;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        PostMessageW(receiver, kReady, reinterpret_cast<WPARAM>(window), 0);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }
    if (argc != 2 && argc != 5) return 1;
    using namespace snowdesktop::dock_minimize;
    Check(RequestIsCurrent(100, 0), "fresh request is current");
    Check(!RequestIsCurrent(100, 100), "expired request is rejected");
    Check(!RequestIsCurrent(100, 101), "late request is rejected");
    Check(RequestIsCurrent(50, MAXDWORD - 100), "request remains valid across tick wrap");
    Check(!RequestIsCurrent(1000, 0), "future request outside budget is rejected");
    Check(IsMinimizeCommand(SW_MINIMIZE) && IsMinimizeCommand(SW_SHOWMINIMIZED) &&
        IsMinimizeCommand(SW_SHOWMINNOACTIVE) && IsMinimizeCommand(SW_FORCEMINIMIZE),
        "all native minimize forms are recognized");
    Check(!IsMinimizeCommand(SW_RESTORE) && !IsMinimizeCommand(SW_MAXIMIZE) &&
        !IsMinimizeCommand(SW_HIDE), "restore, maximize and tray hiding remain independent");

    const HWND receiver = CreateFixtureWindow(L"SnowDesktopMinimizeReceiver", ReceiverProc);
    Check(receiver != nullptr, "receiver is created");
    if (!receiver) return 1;
    std::array<wchar_t, 32768> executable{};
    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    const std::wstring fixtureExecutable = argc == 5 ? argv[2] : executable.data();
    std::wstring command = L"\"" + fixtureExecutable + (argc == 5 ? L"\" " : L"\" --fixture ") +
        std::to_wstring(HandleToULong(receiver));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(fixtureExecutable.c_str(), command.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
    Check(started, "isolated application fixture starts");
    if (!started) { DestroyWindow(receiver); return 1; }
    CloseHandle(child.hThread);
    const bool ready = PumpUntil([] { return fixture && IsWindow(fixture); });
    Check(ready, "fixture announces its own window");
    if (ready)
    {
        std::filesystem::path dll = argv[1];
        if (!std::filesystem::is_regular_file(dll))
            dll = dll.parent_path() / L"SnowDesktop.Runtime" / dll.filename();
        DockExternalMinimize monitor;
        const auto runtimePath = [](const wchar_t* path) {
            std::filesystem::path result = path;
            if (!std::filesystem::is_regular_file(result))
                result = result.parent_path() / L"SnowDesktop.Runtime" / result.filename();
            return result.wstring();
        };
        Check(monitor.Start(receiver, dll.wstring(), argc == 5 ? runtimePath(argv[3]) : L"",
            argc == 5 ? runtimePath(argv[4]) : L""), "native hook loads");
        const std::array<HWND, 1> targets{fixture};
        monitor.UpdateTargets(targets);
        if (argc == 5)
            Check(PumpUntil([&] { return GetPropW(fixture, kReadyProperty) == receiver; }),
                "32-bit helper installs the matching in-process hook");
        Check(monitor.OwnsTarget(fixture), "tracked window is marked with validated process identity");

        PostMessageW(fixture, kAction, 0, 0);
        Check(PumpUntil([] { return IsIconic(fixture) != FALSE; }), "system-menu minimize completes after declined animation");
        Check(requests == 1 && observedBeforeMinimize, "hook reports exactly once before minimize");
        PostMessageW(fixture, kAction, 1, 0);
        Check(PumpUntil([] { return !IsIconic(fixture); }), "restore remains operational");
        Check(requests == 1, "restore does not request minimize animation");

        acceptReceiver = true;
        PostMessageW(fixture, kAction, 0, 0);
        Check(PumpUntil([] { return IsIconic(fixture) != FALSE; }), "accepted animation preparation preserves original minimize");
        Check(requests == 2 && observedBeforeMinimize, "accepted request is reported before the native state changes");
        acceptReceiver = false;
        PostMessageW(fixture, kAction, 1, 0);
        Check(PumpUntil([] { return !IsIconic(fixture); }), "accepted minimize can be restored");

        slowReceiver = true;
        PostMessageW(fixture, kAction, 2, 0);
        Check(PumpUntil([] { return IsIconic(fixture) != FALSE && cancellations > 0; }),
            "timed-out preparation sends cancellation and preserves minimize");
        Check(requests == 3, "ShowWindow minimize is intercepted once");
        slowReceiver = false;
        PostMessageW(fixture, kAction, 1, 0);
        Check(PumpUntil([] { return !IsIconic(fixture); }), "fixture restores after timeout");

        const int beforeMaximize = requests;
        PostMessageW(fixture, kAction, 3, 0);
        Check(PumpUntil([] { return IsZoomed(fixture) != FALSE; }), "maximize is not swallowed");
        Check(requests == beforeMaximize, "maximize does not trigger minimize animation");
        monitor.UpdateTargets({});
        Check(!monitor.OwnsTarget(fixture) && !GetPropW(fixture, kTargetProperty),
            "removing target releases its property and hook");
        PostMessageW(fixture, kAction, 0, 0);
        Check(PumpUntil([] { return IsIconic(fixture) != FALSE; }), "untracked window still minimizes");
        Check(requests == beforeMaximize, "untracked minimize is not animated");
        monitor.UpdateTargets(targets);
        monitor.Stop();
        Check(!GetPropW(fixture, kTargetProperty) && !GetPropW(receiver, kOwnerProperty),
            "host shutdown removes only its installed properties");
        const HWND otherReceiver = CreateFixtureWindow(L"SnowDesktopMinimizeOtherReceiver", ReceiverProc);
        SetPropW(otherReceiver, kOwnerProperty, ULongToHandle(GetCurrentProcessId()));
        SetPropW(otherReceiver, kRevisionProperty, ULongToHandle(1));
        SetPropW(fixture, kTargetProperty, otherReceiver);
        Check(HasLiveOwner(otherReceiver), "matching live owner protocol is recognized");
        Check(monitor.Start(receiver, dll.wstring(), {}, {}), "monitor can restart after shutdown");
        monitor.UpdateTargets(targets);
        Check(!monitor.OwnsTarget(fixture), "another live owner's target property is not adopted");
        monitor.Stop();
        Check(GetPropW(fixture, kTargetProperty) == otherReceiver,
            "cleanup preserves another live owner's target property");
        // Same HWND is still valid, but no longer belongs to the protocol.
        RemovePropW(otherReceiver, kOwnerProperty);
        Check(!HasLiveOwner(otherReceiver), "recycled or detached receiver is not treated as a live owner");
        SetPropW(fixture, kReadyProperty, otherReceiver);
        Check(monitor.Start(receiver, dll.wstring(), {}, {}), "new host starts while stale target remains");
        monitor.UpdateTargets(targets);
        Check(monitor.OwnsTarget(fixture) && !GetPropW(fixture, kReadyProperty),
            "stale owner and readiness markers are reclaimed for an existing window");
        monitor.Stop();
        DestroyWindow(otherReceiver);

        const auto copies = std::filesystem::temp_directory_path() /
            (L"SnowDesktopDockMinimizeRestart-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64()));
        const std::wstring helper = argc == 5 ? runtimePath(argv[3]) : L"";
        const std::wstring helperDll = argc == 5 ? runtimePath(argv[4]) : L"";
        CheckHostRestart(receiver, executable.data(), dll, helper, helperDll, copies / L"first", false);
        CheckHostRestart(receiver, executable.data(), dll, helper, helperDll, copies / L"second", true);
        CheckHostRestart(receiver, executable.data(), dll, helper, helperDll, copies / L"third", false);
        std::error_code cleanup;
        std::filesystem::remove_all(copies, cleanup);
    }
    if (fixture) PostMessageW(fixture, WM_CLOSE, 0, 0);
    if (!PumpUntil([&] { return WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0; }))
    {
        Check(false, "owned fixture exits normally");
        TerminateProcess(child.hProcess, 1);
    }
    CloseHandle(child.hProcess);
    DestroyWindow(receiver);
    if (!failures) std::cout << "Dock external minimize hook checks passed\n";
    return failures ? 1 : 0;
}
