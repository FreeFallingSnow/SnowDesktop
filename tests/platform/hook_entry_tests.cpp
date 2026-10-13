// Forty implementation-image paths are loaded through one registered entry.
// All targets are owned fixture processes, never Explorer or user windows.
#include "platform/hook_entry_deployment.h"
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <functional>

using namespace snowdesktop::hook_entry;
struct Fixture { HWND window = nullptr; };
constexpr UINT kMarkerMessage = WM_APP + 91, kClose = WM_APP + 92;
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == kClose) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
bool WaitFor(const std::function<bool()>& condition)
{
    const auto end = GetTickCount64() + 4000;
    while (!condition()) { if (GetTickCount64() >= end) return false; Sleep(5); }
    return true;
}
int Target(const wchar_t* name)
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return 2;
    auto fixture = static_cast<Fixture*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Fixture)));
    if (!fixture) return 3;
    WNDCLASSW definition{};
    definition.hInstance = GetModuleHandleW(nullptr); definition.lpfnWndProc = WindowProcedure;
    definition.lpszClassName = L"SnowDesktop.Hook.Entry.Fixture";
    if (!RegisterClassW(&definition)) return 4;
    fixture->window = CreateWindowExW(0, definition.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, definition.hInstance, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    UnmapViewOfFile(fixture); CloseHandle(mapping);
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--target") return Target(argv[2]);
    if (argc != 4) return 2;
    const DWORD owner = GetCurrentProcessId();
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktop-hook-entry-test-" + std::to_wstring(owner) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto name = L"Local\\SnowDesktop.Entry.Fixture." + std::to_wstring(owner);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Fixture), name.c_str());
    auto fixture = mapping ? static_cast<Fixture*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Fixture))) : nullptr;
    if (!fixture) return 3;
    *fixture = {};
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --target " + name;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION target{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &target)) return 4;
    bool passed = WaitFor([&] { return fixture->window != nullptr; });
    DWORD targetOwner = 0;
    const DWORD thread = GetWindowThreadProcessId(fixture->window, &targetOwner);
    passed = passed && thread && targetOwner == target.dwProcessId;
    const auto first = root / L"implementation-0.dll";
    std::filesystem::copy_file(argv[2], first);
    const auto entry = Prepare(root / L"cache", argv[1], Payload::Taskbar, first);
    passed = passed && !entry.empty() && HasEntryAbi(entry, IMAGE_FILE_MACHINE_AMD64) &&
        !HasEntryAbi(entry, IMAGE_FILE_MACHINE_I386);
    const HMODULE module = entry.empty() ? nullptr : LoadLibraryW(entry.c_str());
    const auto procedure = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module, "SnowDesktopTaskbarHookProc")) : nullptr;
    SetPropW(fixture->window, kOwnerProperty, ULongToHandle(owner));
    const HHOOK hook = procedure ? SetWindowsHookExW(WH_CALLWNDPROC, procedure, module, thread) : nullptr;
    passed = passed && hook;
    unsigned dispatched = 0;
    for (unsigned version = 0; version < 40 && passed; ++version)
    {
        const auto image = version ? root / (L"implementation-" + std::to_wstring(version) + L".dll") : first;
        if (version) std::filesystem::copy_file(version % 2 ? argv[3] : argv[2], image);
        const auto selected = Prepare(root / L"cache", argv[1], Payload::Taskbar, image);
        passed = selected == entry;
        DWORD_PTR ignored = 0;
        passed = passed && SendMessageTimeoutW(fixture->window, kMarkerMessage, owner, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored) != 0;
        passed = passed && GetPropW(fixture->window, L"SnowDesktop.Entry.Fixture.Marker") == ULongToHandle(version % 2 ? 2 : 1);
        if (passed) ++dispatched;
    }
    unsigned entryImages = 0, implementationImages = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, target.dwProcessId);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W image{sizeof(image)};
        if (Module32FirstW(snapshot, &image)) do {
            const auto path = std::filesystem::path(image.szExePath);
            if (path == std::filesystem::path(entry)) ++entryImages;
            if (path.parent_path() == root && path.filename().wstring().starts_with(L"implementation-")) ++implementationImages;
        } while (Module32NextW(snapshot, &image));
        CloseHandle(snapshot);
    }
    passed = passed && dispatched == 40 && entryImages == 1 && implementationImages == 40;
    // A live PID is insufficient: a mismatched creation stamp cannot dispatch.
    HANDLE catalogMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, CatalogName(owner).c_str());
    auto catalog = catalogMapping ? static_cast<Catalog*>(MapViewOfFile(catalogMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Catalog))) : nullptr;
    if (catalog)
    {
        const auto creation = catalog->ownerCreation;
        InterlockedIncrement(&catalog->sequence); catalog->ownerCreation ^= 1; InterlockedIncrement(&catalog->sequence);
        RemovePropW(fixture->window, L"SnowDesktop.Entry.Fixture.Marker");
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(fixture->window, kMarkerMessage, owner, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored);
        passed = passed && !GetPropW(fixture->window, L"SnowDesktop.Entry.Fixture.Marker");
        InterlockedIncrement(&catalog->sequence); catalog->ownerCreation = creation; InterlockedIncrement(&catalog->sequence);
        UnmapViewOfFile(catalog);
    }
    else passed = false;
    if (catalogMapping) CloseHandle(catalogMapping);
    if (hook) passed = UnhookWindowsHookEx(hook) && passed;
    PostMessageW(fixture->window, kClose, 0, 0);
    const bool exited = WaitForSingleObject(target.hProcess, 5000) == WAIT_OBJECT_0;
    passed = passed && exited;
    if (!exited) { TerminateProcess(target.hProcess, 99); WaitForSingleObject(target.hProcess, 2000); }
    if (module) FreeLibrary(module);
    CloseHandle(target.hThread); CloseHandle(target.hProcess);
    UnmapViewOfFile(fixture); CloseHandle(mapping);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << (passed ? "PASS" : "FAIL") << " implementations=" << dispatched << " entryImages=" << entryImages
        << " implementationImages=" << implementationImages << " HookRegistrations=" << (hook ? 1 : 0) << '\n';
    return passed ? 0 : 1;
}
