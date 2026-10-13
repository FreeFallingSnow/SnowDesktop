// Real AttachCollector and collector callbacks in a separate fixture process.
// Warm reconnect deliberately denies all SetWindowsHookEx calls: success must
// come from the persistent resident broker and the new epoch acknowledgement.
#include "taskbar_hook/tray_connection.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <functional>

using namespace snowdesktop::tray;
struct FixtureState { HWND window = nullptr; DWORD owner = 0; };
constexpr UINT kClose = WM_APP + 1;
LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == kClose) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
bool WaitFor(const std::function<bool()>& condition, DWORD timeout = 4000)
{
    const auto deadline = GetTickCount64() + timeout;
    while (!condition() && GetTickCount64() < deadline) Sleep(5);
    return condition();
}
int Fixture(const wchar_t* name)
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return 2;
    auto state = static_cast<FixtureState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FixtureState)));
    if (!state) return 3;
    WNDCLASSW definition{};
    definition.hInstance = GetModuleHandleW(nullptr); definition.lpfnWndProc = FixtureProc;
    definition.lpszClassName = L"Shell_TrayWnd";
    if (!RegisterClassW(&definition)) return 4;
    state->window = CreateWindowExW(0, definition.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, definition.hInstance, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    UnmapViewOfFile(state); CloseHandle(mapping);
    return 0;
}
unsigned denied = 0;
HHOOK WINAPI DenyRegistration(int, HOOKPROC, HINSTANCE, DWORD)
{ ++denied; SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fixture") return Fixture(argv[2]);
    if (argc != 2) return 2;
    const auto host = GetCurrentProcessId();
    const std::wstring fixtureName = L"Local\\SnowDesktop.Tray.Resident.Fixture." + std::to_wstring(host);
    HANDLE fixtureMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FixtureState), fixtureName.c_str());
    auto fixture = fixtureMapping ? static_cast<FixtureState*>(MapViewOfFile(fixtureMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FixtureState))) : nullptr;
    if (!fixture) return 3;
    *fixture = {}; fixture->owner = host;
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --fixture " + fixtureName;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return 4;
    bool passed = WaitFor([&] { return fixture->window != nullptr; });
    DWORD owner = 0;
    GetWindowThreadProcessId(fixture->window, &owner);
    passed = passed && owner == child.dwProcessId;
    std::cout << "fixture ready=" << passed << " windowOwner=" << owner << " expected=" << child.dwProcessId << '\n';
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedState), ObjectName(host, L"State").c_str());
    auto state = mapping ? static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState))) : nullptr;
    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, ObjectName(host, L"Signal").c_str());
    HMODULE module = nullptr;
    if (!state || !signal) passed = false;
    else
    {
        new(state) SharedState;
        state->owner = host;
        FILETIME creation{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user);
        state->ownerCreation = static_cast<std::uint64_t>(creation.dwHighDateTime) << 32 | creation.dwLowDateTime;
        InterlockedExchange64(&state->epoch, 1001);
        const wchar_t* stage = L"start";
        const DWORD cold = AttachCollector(fixture->window, *state, module, stage, [&] { return std::wstring(argv[1]); });
        passed = passed && cold == 0 && WaitFor([&] { return Read(state->ready) != 0; });
        std::cout << "cold error=" << cold << " ready=" << Read(state->ready) << " valid=" << passed << '\n';
        for (unsigned cycle = 0; cycle < 8 && passed; ++cycle)
        {
            // A collector generation can end independently of its process.
            // Retire the old mapping before publishing a new object with the
            // same name; late worker messages retain the old epoch.
            InterlockedExchange(&state->stop, 1);
            DWORD_PTR ignored = 0;
            const bool detached = SendMessageTimeoutW(fixture->window, RegisterWindowMessageW(kDetachMessage), host,
                static_cast<LPARAM>(Read(state->epoch)), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored);
            std::cout << "detach delivered=" << detached << " error=" << GetLastError() << '\n';
            passed = WaitFor([&] { return !Read(state->ready); });
            std::cout << "worker retired=" << passed << '\n';
            UnmapViewOfFile(state); state = nullptr;
            CloseHandle(mapping); mapping = nullptr;
            passed = passed && WaitFor([&] {
                SetLastError(ERROR_SUCCESS);
                HANDLE candidate = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                    0, sizeof(SharedState), ObjectName(host, L"State").c_str());
                if (!candidate) return false;
                if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(candidate); return false; }
                mapping = candidate;
                return true;
            });
            std::cout << "mapping retired=" << passed << '\n';
            if (!passed) break;
            state = static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
            if (!state) { passed = false; break; }
            new(state) SharedState;
            state->owner = host;
            state->ownerCreation = static_cast<std::uint64_t>(creation.dwHighDateTime) << 32 | creation.dwLowDateTime;
            InterlockedExchange64(&state->epoch, 1002 + cycle);
            HMODULE warmModule = nullptr;
            const DWORD warm = AttachCollector(fixture->window, *state, warmModule, stage,
                [] { return std::wstring(L"path-must-not-be-resolved"); }, {DenyRegistration, UnhookWindowsHookEx});
            passed = passed && warm == 0 && !warmModule && WaitFor([&] { return Read(state->ready) != 0; });
            std::cout << "warm cycle=" << cycle << " error=" << warm << " registrations=" << denied << '\n';
        }
        if (state)
        {
            DWORD_PTR invalidAck = 0;
            SendMessageTimeoutW(fixture->window, RegisterWindowMessageW(kResumeMessage), host,
                static_cast<LPARAM>(Read(state->epoch) + 500), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &invalidAck);
            passed = passed && invalidAck == 0 && Read(state->ready) && denied == 0;
            InterlockedExchange(&state->stop, 1);
            PostMessageW(fixture->window, RegisterWindowMessageW(kDetachMessage), host, static_cast<LPARAM>(Read(state->epoch)));
        }
    }
    PostMessageW(fixture->window, kClose, 0, 0);
    const bool exitedNormally = WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0;
    passed = passed && exitedNormally;
    if (!exitedNormally) { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 2000); }
    if (module) FreeLibrary(module);
    if (state) UnmapViewOfFile(state);
    for (const HANDLE handle : {mapping, signal, child.hThread, child.hProcess}) if (handle) CloseHandle(handle);
    UnmapViewOfFile(fixture); CloseHandle(fixtureMapping);
    std::cout << (passed ? "PASS" : "FAIL") << " cold attach,8 denied-registration reconnects and stale epoch rejection\n";
    return passed ? 0 : 1;
}
