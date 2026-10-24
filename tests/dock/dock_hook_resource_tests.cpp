#include "dock/dock_minimize_hook_threads.h"
#include "dock/dock_external_minimize.h"
#include "dock/dock_minimize_protocol.h"
#include "dock/dock_window_source_cloak.h"
#include <atomic>
#include <array>
#include <iostream>
#include <string>
#include <functional>
#include <filesystem>
#include <thread>

extern "C" LRESULT CALLBACK SnowDesktopDockSourceCloakHook(int, WPARAM, LPARAM);

using namespace snowdesktop::dock_minimize;
namespace
{
std::atomic<unsigned> cbt{0}, cloak{0}, removed{0};
int failures = 0;
constexpr wchar_t kRaceFixture[] = L"SnowDesktop.Dock.LeaseRaceFixture";
HANDLE raceEntered = nullptr, raceRelease = nullptr;
std::atomic<bool> pauseRaceInstall{false};
std::function<LRESULT(HWND, DWORD)> minimizeRequest;
unsigned minimizeCancels = 0;
HHOOK WINAPI RaceInstall(int, HOOKPROC, HINSTANCE, DWORD)
{
    if (pauseRaceInstall.exchange(false))
    {
        SetEvent(raceEntered);
        WaitForSingleObject(raceRelease, 1000);
    }
    return reinterpret_cast<HHOOK>(static_cast<UINT_PTR>(1));
}
BOOL WINAPI RaceRemove(HHOOK) { return TRUE; }
void Check(bool value, const char* message)
{
    if (!value) { ++failures; std::cerr << "FAIL " << message << '\n'; }
}
HHOOK WINAPI Install(int type, HOOKPROC callback, HINSTANCE module, DWORD thread)
{
    if (type == WH_CBT) ++cbt; else if (type == WH_CALLWNDPROC) ++cloak;
    return SetWindowsHookExW(type, callback, module, thread);
}
BOOL WINAPI Remove(HHOOK hook) { ++removed; return UnhookWindowsHookEx(hook); }
HHOOK WINAPI Deny(int, HOOKPROC, HINSTANCE, DWORD) { SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
struct Fixture { HWND window = nullptr; };
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == RequestMessage() && minimizeRequest)
        return minimizeRequest(reinterpret_cast<HWND>(wp), static_cast<DWORD>(lp));
    if (message == CancelMessage()) { ++minimizeCancels; return 0; }
    if (message == WM_COPYDATA && GetPropW(window, kRaceFixture))
    {
        CWPSTRUCT payload{};
        payload.hwnd = window; payload.message = message; payload.wParam = wp; payload.lParam = lp;
        SnowDesktopDockSourceCloakHook(HC_ACTION, 0, reinterpret_cast<LPARAM>(&payload));
        return TRUE;
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
HWND CreateWindowFixture()
{
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = WindowProc;
    type.lpszClassName = L"SnowDesktop.Dock.HookResource.Fixture";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        -20000, -20000, 200, 100, nullptr, nullptr, type.hInstance, nullptr);
}
bool WaitFor(const std::function<bool()>& predicate, DWORD timeout = 4000)
{
    const auto end = GetTickCount64() + timeout;
    while (!predicate())
    {
        if (GetTickCount64() >= end) return false;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
    return true;
}
#include "dock_minimize_target_cases.h"
void RunPendingPresentationTests(HWND source, HWND receiver)
{
    for (const int outcome : {0, 1, 2})
    {
        ShowWindowAsync(source, SW_SHOWNOACTIVATE);
        Check(WaitFor([&] { return !IsIconic(source) && IsWindowVisible(source); }),
            "external pending fixture has a visible restored source");
        DWORD requestDeadline = 0;
        unsigned requests = 0;
        const unsigned cancelsBefore = minimizeCancels;
        minimizeRequest = [&](HWND target, DWORD deadline) -> LRESULT {
            Check(target == source && !IsIconic(source) && RequestIsCurrent(deadline, GetTickCount()),
                "actual CBT export reaches the host before native minimization");
            ++requests;
            requestDeadline = deadline;
            Check(SetPropW(source, kPresentationPendingProperty, receiver) != FALSE,
                "host publishes a generation-bound pending presentation");
            return 1;
        };
        const ULONGLONG start = GetTickCount64();
        PostMessageW(source, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        Check(WaitFor([&] { return requests == 1; }, 1000) && !IsIconic(source),
            "host returns to its message loop while the original native action remains pending");
        DWORD_PTR ignored = 0;
        Check(SendMessageTimeoutW(source, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 80, &ignored) != 0,
            "pending CBT continues servicing sent messages without consuming posted input");
        bool acquired = false;
        if (outcome == 0)
        {
            Check(SUCCEEDED(snowdesktop::dock_source_cloak::Acquire(source, receiver, acquired)) && acquired &&
                !IsIconic(source), "cross-process source cloak is applied before releasing the pending original action");
            RemovePropW(source, kPresentationPendingProperty);
        }
        else if (outcome == 2) RemovePropW(receiver, kRevisionProperty);
        Check(WaitFor([&] { return IsIconic(source); }, 700) && requests == 1,
            "acknowledgement, timeout and host loss all retain exactly one original native action");
        if (outcome == 0)
        {
            Check(minimizeCancels == cancelsBefore &&
                SUCCEEDED(snowdesktop::dock_source_cloak::Release(source, receiver)),
                "acknowledged minimize finishes without timeout notification or a stranded source cloak");
        }
        else
        {
            Check(WaitFor([&] { return minimizeCancels == cancelsBefore + 1; }, 500) &&
                GetTickCount64() - start < 700 &&
                (outcome == 2 || !RequestIsCurrent(requestDeadline, GetTickCount())),
                "unacknowledged presentation releases at its original deadline or owner loss and reports cancellation");
        }
        SetPropW(receiver, kRevisionProperty, ULongToHandle(1));
        RemovePropW(source, kPresentationPendingProperty);
        minimizeRequest = {};
    }
    ShowWindowAsync(source, SW_SHOWNOACTIVATE);
    Check(WaitFor([&] { return !IsIconic(source); }), "pending Hook fixture restores its own target");
}
void RunLeaseMessageRaceTests()
{
    namespace lease = snowdesktop::dock_source_cloak;
    const HWND source = CreateWindowFixture(), first = CreateWindowFixture(), second = CreateWindowFixture();
    raceEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    raceRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(source && first && second && raceEntered && raceRelease, "lease-message race fixture initializes");
    if (!source || !first || !second || !raceEntered || !raceRelease) return;
    for (const HWND host : {first, second})
    {
        lease::RegisterOwner(host);
        SetPropW(host, kOwnerProperty, ULongToHandle(GetCurrentProcessId()));
        SetPropW(host, kRevisionProperty, ULongToHandle(1));
    }
    SetPropW(source, kRaceFixture, ULongToHandle(1));
    SetPropW(source, kTargetProperty, first);
    CloakDispatcher dispatcher;
    Check(dispatcher.Start(GetModuleHandleW(nullptr), SnowDesktopDockSourceCloakHook,
        {RaceInstall, RaceRemove}), "real dispatcher starts without installing a native Hook");
    SetPropW(source, kCloakDispatcherProperty, dispatcher.Window());
    bool acquired = false;
    Check(SUCCEEDED(lease::Acquire(source, first, acquired)) && acquired &&
        SUCCEEDED(lease::StartRecoveryTimer(source)), "old lease is cloaked with its real source-thread recovery timer");
    const auto ownerA = lease::ReadOwner(source);
    const auto oldRequest = lease::MakeRequest(source, ownerA, FALSE);
    pauseRaceInstall = true;
    std::atomic<bool> sent{false};
    std::thread sender([&] {
        DWORD_PTR ignored = 0;
        SendCloakRequest(dispatcher.Window(), oldRequest, 500, ignored);
        sent = true;
    });
    Check(WaitForSingleObject(raceEntered, 500) == WAIT_OBJECT_0,
        "old uncloak message pauses in the actual dispatcher after validating lease A");
    SetPropW(source, lease::RecoveryProperty, ownerA.token);
    lease::RecoverOnSourceThread(source, WM_TIMER,
        reinterpret_cast<UINT_PTR>(GetPropW(source, lease::TimerProperty)), GetTickCount());
    Check(!lease::ReadOwner(source).window, "source-thread recovery retires A before its delayed release is delivered");
    SetPropW(source, kTargetProperty, second);
    acquired = false;
    Check(SUCCEEDED(lease::Acquire(source, second, acquired)) && acquired &&
        SUCCEEDED(lease::StartRecoveryTimer(source)), "a new real lease B is cloaked before the old request resumes");
    const auto ownerB = lease::ReadOwner(source);
    const HANDLE timerB = GetPropW(source, lease::TimerProperty);
    SetEvent(raceRelease);
    Check(WaitFor([&] { return sent.load(); }), "delayed dispatcher request completes through the real target Hook export");
    sender.join();
    const auto unchanged = [&] {
        DWORD flags = 0;
        return SUCCEEDED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &flags, sizeof(flags))) &&
            (flags & DWM_CLOAKED_APP) && lease::SameOwner(source, ownerB) &&
            GetPropW(source, lease::TimerProperty) == timerB &&
            GetPropW(source, lease::RecoveryProperty) != ownerB.token;
    };
    Check(unchanged(), "target Hook rejects A after delivery; B remains cloaked with its timer and lease");
    const auto attempts = dispatcher.Statistics().attempts;
    DWORD_PTR result = 0;
    Check(SendCloakRequest(dispatcher.Window(), oldRequest, 180, result) &&
        static_cast<HRESULT>(result) == E_ABORT && dispatcher.Statistics().attempts == attempts && unchanged(),
        "dispatcher rejects an already stale complete lease before registering or forwarding");
    auto wrongOwner = lease::MakeRequest(source, ownerB, FALSE);
    wrongOwner.owner = oldRequest.owner;
    Check(SendCloakRequest(dispatcher.Window(), wrongOwner, 180, result) &&
        static_cast<HRESULT>(result) == E_ABORT && unchanged(),
        "matching token alone cannot authorize a different owner window");
    SendCloakRequest(source, oldRequest, 120, result);
    Check(unchanged(), "direct source delivery also preserves the frozen old owner and token");
    auto malformed = lease::MakeRequest(source, ownerB, FALSE);
    malformed.version = 99;
    SendCloakRequest(dispatcher.Window(), malformed, 180, result);
    malformed.version = 2;
    COPYDATASTRUCT truncated{kCloakRequestTag, sizeof(malformed) - 1, &malformed};
    SendMessageTimeoutW(dispatcher.Window(), WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&truncated),
        SMTO_ABORTIFHUNG, 180, &result);
    Check(dispatcher.Statistics().attempts == attempts && unchanged(),
        "bad version or byte count cannot mutate an active source lease");
    Check(SUCCEEDED(lease::Release(source, ownerB)), "current frozen owner can still release its source");
    lease::StopRecoveryTimer(source);
    RemovePropW(source, kCloakDispatcherProperty);
    dispatcher.Stop();
    DestroyWindow(source); DestroyWindow(first); DestroyWindow(second);
    CloseHandle(raceEntered); CloseHandle(raceRelease);
    raceEntered = raceRelease = nullptr;
}
int Target(const wchar_t* name)
{
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return 1;
    auto* fixture = static_cast<Fixture*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Fixture)));
    if (!fixture) return 2;
    fixture->window = CreateWindowFixture();
    ShowWindow(fixture->window, SW_SHOWNOACTIVATE);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    UnmapViewOfFile(fixture); CloseHandle(mapping); return 0;
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--target") return Target(argv[2]);
    if (argc != 2) return 2;
    RunProcessBitnessCacheTests();
    RunLeaseMessageRaceTests();
    if (std::wstring_view(argv[1]) == L"--lease-race-only") return failures ? 1 : 0;
    auto library = std::filesystem::path(argv[1]);
    if (!std::filesystem::is_regular_file(library))
        library = library.parent_path() / L"SnowDesktop.Runtime" / library.filename();
    const auto name = L"Local\\SnowDesktop.Dock.HookResource." + std::to_wstring(GetCurrentProcessId());
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Fixture), name.c_str());
    auto* fixture = mapping ? static_cast<Fixture*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Fixture))) : nullptr;
    if (!fixture) return 3;
    *fixture = {};
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --target " + name;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION target{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &target)) return 4;
    Check(WaitFor([&] { return fixture->window != nullptr; }), "fixture ready");
    const HWND receiver = CreateWindowFixture();
    Check(receiver && snowdesktop::dock_source_cloak::RegisterOwner(receiver), "owner ready");
    SetPropW(receiver, kOwnerProperty, ULongToHandle(GetCurrentProcessId()));
    SetPropW(receiver, kRevisionProperty, ULongToHandle(1));
    SetPropW(fixture->window, kTargetProperty, receiver);
    const DWORD thread = GetWindowThreadProcessId(fixture->window, nullptr);
    const std::array<DWORD,1> threads{thread};
    {
        ThreadHooks denied({Deny, Remove});
        Check(denied.Start(library.wstring()), "denied hook module loads");
        for (unsigned refresh=0;refresh<10000;++refresh) denied.Sync(threads);
        Check(denied.RegistrationStatistics().attempts == 1 && denied.RegistrationStatistics().failures == 1,
            "10000 failed refreshes perform one registration attempt");
        denied.Sync({}); denied.Sync(threads);
        Check(denied.RegistrationStatistics().attempts == 2, "new target generation resets backoff");
    }
    {
        ThreadHooks hooks({Install, Remove});
        Check(hooks.Start(library.wstring()), "hook module loads");
        for(unsigned refresh=0;refresh<10000;++refresh) hooks.Sync(threads);
        Check(cbt==1 && cloak==0 && hooks.HookCount()==1, "idle target keeps one CBT and zero cloak hooks");
        SetPropW(fixture->window, kCloakDispatcherProperty, hooks.CloakWindow());
        for(unsigned cycle=0;cycle<8;++cycle)
        {
            bool acquired = false;
            Check(SUCCEEDED(snowdesktop::dock_source_cloak::Acquire(fixture->window, receiver, acquired)) && acquired,
                "temporary hook applies source cloak");
            DWORD flags=0; DwmGetWindowAttribute(fixture->window,DWMWA_CLOAKED,&flags,sizeof(flags));
            Check((flags&DWM_CLOAKED_APP)!=0,"source is actually cloaked");
            Check(SUCCEEDED(snowdesktop::dock_source_cloak::Release(fixture->window,receiver)),"temporary hook releases source cloak");
            Check(WaitFor([&] { DWORD value=0; DwmGetWindowAttribute(fixture->window,DWMWA_CLOAKED,&value,sizeof(value)); return !(value&DWM_CLOAKED_APP); }),
                "source is actually uncloaked");
        }
        Check(cloak==16 && removed==16 && hooks.CloakStatistics().successes==16,
            "each command registers and immediately releases its hook");
        RunPendingPresentationTests(fixture->window, receiver);
        RemovePropW(fixture->window,kCloakDispatcherProperty);
        hooks.Stop();
        Check(removed==cloak+1,"stop removes the one resident CBT hook after every temporary cloak hook");
    }
    RemovePropW(fixture->window,kTargetProperty);
    DockExternalMinimize monitor;
    Check(monitor.Start(receiver, library.wstring(), {}, {}), "real external minimize monitor starts");
    RunTargetMaintenanceTests(monitor, receiver, fixture->window);
    PostMessageW(fixture->window,WM_CLOSE,0,0);
    Check(WaitFor([&] {return WaitForSingleObject(target.hProcess,0)==WAIT_OBJECT_0;}),"owned target exits");
    const std::array<HWND, 1> exited{fixture->window};
    monitor.UpdateTargets(exited);
    Check(!monitor.OwnsTarget(fixture->window) && monitor.HookCount() == 0,
        "exited process and thread are removed even if the snapshot still contains the old HWND");
    monitor.Stop();
    Check(!GetPropW(receiver, kOwnerProperty) && !GetPropW(receiver, kRevisionProperty),
        "stopping maintenance removes the host ownership protocol");
    DestroyWindow(receiver);
    if(WaitForSingleObject(target.hProcess,0)!=WAIT_OBJECT_0) TerminateProcess(target.hProcess,99);
    CloseHandle(target.hThread);CloseHandle(target.hProcess);UnmapViewOfFile(fixture);CloseHandle(mapping);
    std::cout<<(failures?"FAIL":"PASS")<<" idleCBT="<<cbt<<" cloakCommands="<<cloak<<" unregistered="<<removed<<'\n';
    return failures?1:0;
}
