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
HWND fixture = nullptr;
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
    if (!RegisterClassW(&type)) return nullptr;
    return CreateWindowExW(WS_EX_TOOLWINDOW, name, name, WS_OVERLAPPEDWINDOW,
        -20000, -20000, 240, 160, nullptr, nullptr, type.hInstance, nullptr);
}
}

int wmain(int argc, wchar_t** argv)
{
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
        SetPropW(fixture, kTargetProperty, ULongToHandle(123));
        Check(monitor.Start(receiver, dll.wstring(), {}, {}), "monitor can restart after shutdown");
        monitor.UpdateTargets(targets);
        Check(!monitor.OwnsTarget(fixture), "another owner's target property is not adopted");
        monitor.Stop();
        Check(GetPropW(fixture, kTargetProperty) == ULongToHandle(123),
            "cleanup preserves another owner's target property");
        RemovePropW(fixture, kTargetProperty);
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
