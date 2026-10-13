#include "dock/dock_minimize_hook_threads.h"
#include "dock/dock_minimize_protocol.h"
#include "dock/dock_window_source_cloak.h"
#include <atomic>
#include <array>
#include <iostream>
#include <string>
#include <functional>

using namespace snowdesktop::dock_minimize;
namespace
{
std::atomic<unsigned> cbt{0}, cloak{0}, removed{0};
int failures = 0;
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
        Check(denied.Start(argv[1]), "denied hook module loads");
        for (unsigned refresh=0;refresh<10000;++refresh) denied.Sync(threads);
        Check(denied.RegistrationStatistics().attempts == 1 && denied.RegistrationStatistics().failures == 1,
            "10000 failed refreshes perform one registration attempt");
        denied.Sync({}); denied.Sync(threads);
        Check(denied.RegistrationStatistics().attempts == 2, "new target generation resets backoff");
    }
    {
        ThreadHooks hooks({Install, Remove});
        Check(hooks.Start(argv[1]), "hook module loads");
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
        RemovePropW(fixture->window,kCloakDispatcherProperty);
        hooks.Stop();
        Check(removed==17,"stop removes the one resident CBT hook");
    }
    RemovePropW(fixture->window,kTargetProperty);
    DestroyWindow(receiver);
    PostMessageW(fixture->window,WM_CLOSE,0,0);
    Check(WaitFor([&] {return WaitForSingleObject(target.hProcess,0)==WAIT_OBJECT_0;}),"owned target exits");
    if(WaitForSingleObject(target.hProcess,0)!=WAIT_OBJECT_0) TerminateProcess(target.hProcess,99);
    CloseHandle(target.hThread);CloseHandle(target.hProcess);UnmapViewOfFile(fixture);CloseHandle(mapping);
    std::cout<<(failures?"FAIL":"PASS")<<" idleCBT="<<cbt<<" cloakCommands="<<cloak<<" unregistered="<<removed<<'\n';
    return failures?1:0;
}
