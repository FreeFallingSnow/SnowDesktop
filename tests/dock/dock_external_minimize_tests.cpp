#include "dock/dock_external_minimize.h"
#include "dock/dock_minimize_protocol.h"
#include "dock/dock_window_source_cloak.h"
#include "dock/dock_window_transition.h"
#include "settings/animation_settings.h"
#include <d3d11.h>
#include <dxgi.h>

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
DockWindowTransition* transitionForRequest = nullptr;
bool nativeGenieStarted = false;

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
    if (observationReceiver && message == kCrashHost)
    {
        bool acquired = false;
        const HRESULT hr = snowdesktop::dock_source_cloak::Acquire(fixture, window, acquired);
        ExitProcess(SUCCEEDED(hr) && acquired ? 0 : 4);
    }
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
        if (transitionForRequest)
        {
            const RECT dock{-19800, -19800, -19736, -19736};
            nativeGenieStarted = transitionForRequest->StartExternalMinimize(
                reinterpret_cast<HWND>(wParam), dock, static_cast<DWORD>(lParam));
            return nativeGenieStarted ? 1 : 0;
        }
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

bool AppCloaked(HWND window)
{
    DWORD flags = 0;
    Check(SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags))),
        "cross-process source cloak state is observable");
    return (flags & DWM_CLOAKED_APP) != 0;
}

void CheckCrossProcessGenie()
{
    // Same-process image fixtures cannot detect DWMWA_CLOAK's access check.
    // Exercise the real engine against the child HWND, including the reverse
    // sent message while its CBT hook is waiting for this receiver.
    using Microsoft::WRL::ComPtr;
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComPtr<ID3D11Device> graphics;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2d;
    if (SUCCEEDED(hr)) hr = graphics.As(&dxgi);
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice3(dxgi.Get(), IID_PPV_ARGS(&composition));
    if (SUCCEEDED(hr)) hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDevice(dxgi.Get(), &d2d);
    snowdesktop::UiAnimationScheduler scheduler;
    Check(SUCCEEDED(hr) && scheduler.Initialize(), "cross-process Genie device and scheduler initialize");
    if (SUCCEEDED(hr) && scheduler.WaitHandle())
    {
        snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn, 2, 0, 60, false, false, 3);
        DockWindowTransition transition;
        Check(transition.Initialize(GetModuleHandleW(nullptr), &scheduler, d2d.Get(), composition.Get()),
            "cross-process Genie engine initializes");
        std::wstring presentation;
        transition.SetDiagnosticCallback([&](const wchar_t* message) {
            if (std::wstring(message).find(L"Dock transition:") == 0) presentation = message;
            if (std::wstring(message).find(L"Dock animation aborted:") == 0) std::wcout << message << L'\n';
        });
        const auto drain = [&] {
            Check(PumpUntil([&] { scheduler.DispatchDue(); return !transition.IsActive(); }),
                "cross-process Genie finishes within its bounded handoff");
        };
        PostMessageW(fixture, kAction, 2, 0);
        Check(PumpUntil([] { return IsIconic(fixture) != FALSE; }), "child is minimized before its first custom restore");
        int restores = 0;
        const RECT dock{-19800, -19800, -19736, -19736};
        const bool restoring = transition.StartRestore(fixture, dock,
            [&](HWND target, DockWindowRestoreTransitionPhase phase) {
                if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                {
                    ++restores;
                    PostMessageW(target, kAction, 1, 0);
                }
            });
        std::wcout << L"Cross-process first restore: " << presentation << L'\n';
        Check(restoring && presentation.find(L"effective=3") != std::wstring::npos &&
            presentation.find(L"snapshot=dwm-shared-window") != std::wstring::npos && AppCloaked(fixture),
            "already-minimized child starts Genie with no previous minimize snapshot or native fallback");
        if (restoring) drain();
        Check(restores == 1 && !IsIconic(fixture) && !AppCloaked(fixture),
            "first child restore commits once and releases its source cloak");
        transitionForRequest = &transition;
        nativeGenieStarted = false;
        const ULONGLONG start = GetTickCount64();
        PostMessageW(fixture, kAction, 0, 0);
        Check(PumpUntil([] { return IsIconic(fixture) != FALSE; }) && nativeGenieStarted && AppCloaked(fixture),
            "child minimize button starts Genie and services the source-cloak command during its CBT wait");
        std::cout << "Cross-process native preparation: " << GetTickCount64() - start << " ms\n";
        transitionForRequest = nullptr;
        if (nativeGenieStarted) drain();
        Check(IsIconic(fixture) && !AppCloaked(fixture), "native Genie finishes with the child minimized and uncloaked");
        PostMessageW(fixture, kAction, 1, 0);
        Check(PumpUntil([] { return !IsIconic(fixture); }), "child is restored after the cross-process regression");
        requests = 0;
        cancellations = 0;
    }
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
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
        Check(!AppCloaked(fixture), "replacement host recovers an existing cross-process source cloak");
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
    {
        Check(crash ? GetPropW(fixture, snowdesktop::dock_minimize::kTargetProperty) == hostReceiver :
                GetPropW(fixture, snowdesktop::dock_minimize::kTargetProperty) == nullptr,
            crash ? "abrupt host exit leaves a stale marker for recovery" : "normal host exit removes its marker");
        if (crash) Check(AppCloaked(fixture) &&
            snowdesktop::dock_source_cloak::TaskWindowCloakFlags(fixture, DWM_CLOAKED_APP) == 0,
            "an interrupted child-process cloak stays discoverable for a replacement host");
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 7 && std::wstring(argv[1]) == L"--host")
    {
        observationReceiver = WindowArgument(argv[2]);
        const HWND target = WindowArgument(argv[3]);
        fixture = target;
        const HWND receiver = CreateFixtureWindow(L"SnowDesktopMinimizeHostReceiver", ReceiverProc);
        if (receiver) snowdesktop::dock_source_cloak::RegisterOwner(receiver);
        DockExternalMinimize monitor;
        if (!receiver || !monitor.Start(receiver, argv[4], argv[5], argv[6])) return 2;
        monitor.UpdateTargets(std::array<HWND, 1>{target});
        if (!monitor.OwnsTarget(target) || (*argv[5] &&
            !PumpUntil([&] { return GetPropW(target, snowdesktop::dock_minimize::kReadyProperty) == receiver; })))
            return 3;
        monitor.UpdateTargets(std::array<HWND, 1>{target});
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

        CheckCrossProcessGenie();

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
