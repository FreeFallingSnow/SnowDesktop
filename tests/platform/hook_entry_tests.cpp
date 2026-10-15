// Forty implementation-image paths are loaded through one registered entry.
// All targets are owned fixture processes, never Explorer or user windows.
#include "platform/hook_entry_deployment.h"
#include "platform/injectable_runtime_cache.h"
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <functional>

using namespace snowdesktop::hook_entry;
#ifdef _WIN64
constexpr Payload kTestPayload = Payload::Taskbar;
constexpr WORD kTestMachine = IMAGE_FILE_MACHINE_AMD64, kOtherMachine = IMAGE_FILE_MACHINE_I386;
constexpr char kTestExport[] = "SnowDesktopTaskbarHookProc";
#else
constexpr Payload kTestPayload = Payload::Dock32;
constexpr WORD kTestMachine = IMAGE_FILE_MACHINE_I386, kOtherMachine = IMAGE_FILE_MACHINE_AMD64;
constexpr char kTestExport[] = "_SnowDesktopDockSourceCloakHook@12";
#endif
struct Fixture
{
    std::uint64_t window = 0;
    wchar_t entryPath[32768]{};
    HWND Window() const noexcept { return reinterpret_cast<HWND>(static_cast<UINT_PTR>(window)); }
};
constexpr UINT kMarkerMessage = WM_APP + 91, kClose = WM_APP + 92, kDirect = WM_APP + 93;
Fixture* targetFixture = nullptr;
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == kDirect && targetFixture)
    {
        const HMODULE module = LoadLibraryW(targetFixture->entryPath);
        if (!module) return 0;
        const auto procedure = reinterpret_cast<HOOKPROC>(GetProcAddress(module, kTestExport));
        if (procedure)
        {
            CWPSTRUCT payload{}; payload.hwnd = window; payload.message = kMarkerMessage; payload.wParam = wp;
            procedure(HC_ACTION, 0, reinterpret_cast<LPARAM>(&payload));
        }
        FreeLibrary(module);
        // No User32 Hook has ever been registered in this target yet. Only
        // the entry's lifetime protection can retain this caller-owned load.
        return procedure && GetModuleHandleW(targetFixture->entryPath) == module ? 1 : 0;
    }
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
    targetFixture = fixture;
    WNDCLASSW definition{};
    definition.hInstance = GetModuleHandleW(nullptr); definition.lpfnWndProc = WindowProcedure;
    definition.lpszClassName = L"SnowDesktop.Hook.Entry.Fixture";
    if (!RegisterClassW(&definition)) return 4;
    fixture->window = reinterpret_cast<UINT_PTR>(CreateWindowExW(0, definition.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, definition.hInstance, nullptr));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    UnmapViewOfFile(fixture); CloseHandle(mapping);
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    if (argc == 6 && std::wstring_view(argv[1]) == L"--run32")
    {
        std::wstring command = L"\"" + std::wstring(argv[2]) + L"\"";
        for (int argument=3; argument<argc; ++argument)
            command += L" \"" + std::wstring(argv[argument]) + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        if (!CreateProcessW(argv[2], command.data(), nullptr, nullptr, FALSE,
                0, nullptr, nullptr, &startup, &child)) return 6;
        const bool exited = WaitForSingleObject(child.hProcess, 20000) == WAIT_OBJECT_0;
        DWORD result = 99;
        if (exited) GetExitCodeProcess(child.hProcess, &result);
        else { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 2000); }
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        return exited ? static_cast<int>(result) : 7;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--target") return Target(argv[2]);
#ifndef _WIN64
    if (argc == 6 && std::wstring_view(argv[1]) == L"--inject")
    {
        const HWND window = reinterpret_cast<HWND>(ULongToHandle(wcstoul(argv[2], nullptr, 10)));
        const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]);
        const HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[5]);
        const HMODULE module = LoadLibraryW(argv[3]);
        const auto procedure = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module, kTestExport)) : nullptr;
        const HHOOK hook = procedure ? SetWindowsHookExW(WH_CALLWNDPROC, procedure, module,
            GetWindowThreadProcessId(window, nullptr)) : nullptr;
        bool success = ready && stop && hook && SetEvent(ready);
        if (success) success = WaitForSingleObject(stop, 10000) == WAIT_OBJECT_0;
        if (hook) success = UnhookWindowsHookEx(hook) && success;
        if (module) FreeLibrary(module);
        if (ready) CloseHandle(ready);
        if (stop) CloseHandle(stop);
        return success ? 0 : 5;
    }
#endif
    const bool cross = argc == 6 && std::wstring_view(argv[1]) == L"--cross32";
    if (argc != 4 && !cross) return 2;
    const int offset = cross ? 1 : 0;
    const Payload payload = cross ? Payload::Dock32 : kTestPayload;
    const WORD machine = cross ? IMAGE_FILE_MACHINE_I386 : kTestMachine;
    auto entrySource = std::filesystem::path(argv[1 + offset]);
    if (!std::filesystem::is_regular_file(entrySource))
        entrySource = entrySource.parent_path() / L"SnowDesktop.Runtime" / entrySource.filename();
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
    const std::wstring targetExecutable = cross ? argv[5] : executable;
    std::wstring command = L"\"" + targetExecutable + L"\" --target " + name;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION target{};
    if (!CreateProcessW(targetExecutable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &target)) return 4;
    bool passed = WaitFor([&] { return fixture->window != 0; });
    DWORD targetOwner = 0;
    const DWORD thread = GetWindowThreadProcessId(fixture->Window(), &targetOwner);
    passed = passed && thread && targetOwner == target.dwProcessId;
    const auto first = root / L"implementation-0.dll";
    std::filesystem::copy_file(argv[2 + offset], first);
    // A fresh per-run path creates a new User32 module identity even though
    // only the payload changes. Keep this test-only entry stable across runs;
    // private payloads/catalogs/targets still belong to the current test.
    const auto entryCache = std::filesystem::temp_directory_path() /
        L"SnowDesktopHookEntryTests" / L"abi-1";
    const auto entry = Prepare(entryCache, entrySource, payload, first);
    if (!entry.empty() && snowdesktop::deployment::InjectableRuntimeCache::Digest(entry) !=
            snowdesktop::deployment::InjectableRuntimeCache::Digest(entrySource))
    {
        std::cerr << "FAIL stable test entry differs from this run's source image; "
            "a prior fixture may still own the old image\n";
        passed = false;
    }
    passed = passed && entry.size() < std::size(fixture->entryPath);
    if (passed) wcscpy_s(fixture->entryPath, entry.c_str());
    passed = passed && !entry.empty() && HasEntryAbi(entry, machine) &&
        !HasEntryAbi(entry, machine == IMAGE_FILE_MACHINE_I386 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386);
    const HMODULE module = entry.empty() || cross ? nullptr : LoadLibraryW(entry.c_str());
    const auto procedure = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module, kTestExport)) : nullptr;
    SetPropW(fixture->Window(), kOwnerProperty, ULongToHandle(owner));
    // Dock32 routes through the marked receiver. The host's catalog remains
    // pointer-free; no target address is dereferenced across this boundary.
    WNDCLASSW hostType{};
    hostType.hInstance = GetModuleHandleW(nullptr); hostType.lpfnWndProc = WindowProcedure;
    hostType.lpszClassName = L"SnowDesktop.Entry.Host.Fixture";
    RegisterClassW(&hostType);
    const HWND receiver = CreateWindowExW(0, hostType.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hostType.hInstance, nullptr);
    passed = passed && receiver && SetPropW(fixture->Window(), L"SnowDesktop.Dock.MinimizeTarget.v1", receiver);
    DWORD_PTR retained = 0;
    passed = passed && SendMessageTimeoutW(fixture->Window(), kDirect, owner, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &retained) && retained == 1;
    std::cout << "entryRetainedAfterCallerRelease=" << retained << " beforeUser32Registration=1\n";
    SetLastError(ERROR_SUCCESS);
    const HHOOK hook = procedure ? SetWindowsHookExW(WH_CALLWNDPROC, procedure, module, thread) : nullptr;
    const DWORD registrationError = hook ? ERROR_SUCCESS : GetLastError();
    if (!hook && !cross)
        std::cerr << "Hook registration failed: error=" << registrationError
            << "; valid loaded entry/export and private target do not prove "
            "a new session module-path slot is available\n";
    const auto readyName = name + L".ready", stopName = name + L".stop";
    HANDLE ready = nullptr, stop = nullptr;
    PROCESS_INFORMATION injector{};
    bool registered = hook != nullptr;
    if (cross && passed)
    {
        ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        stop = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
        command = L"\"" + targetExecutable + L"\" --inject " + std::to_wstring(HandleToULong(fixture->Window())) +
            L" \"" + entry + L"\" " + readyName + L" " + stopName;
        passed = ready && stop && CreateProcessW(targetExecutable.c_str(), command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &injector);
        registered = passed && WaitForSingleObject(ready, 4000) == WAIT_OBJECT_0;
    }
    passed = passed && registered;
    unsigned dispatched = 0;
    for (unsigned version = 0; version < 40 && passed; ++version)
    {
        const auto image = version ? root / (L"implementation-" + std::to_wstring(version) + L".dll") : first;
        if (version) std::filesystem::copy_file(version % 2 ? argv[3 + offset] : argv[2 + offset], image);
        const auto selected = Prepare(entryCache, entrySource, payload, image);
        passed = selected == entry;
        DWORD_PTR ignored = 0;
        passed = passed && SendMessageTimeoutW(fixture->Window(), kMarkerMessage, owner, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored) != 0;
        passed = passed && GetPropW(fixture->Window(), L"SnowDesktop.Entry.Fixture.Marker") == ULongToHandle(version % 2 ? 2 : 1);
        if (passed) ++dispatched;
    }
    unsigned entryImages = 0, implementationImages = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, target.dwProcessId);
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
        RemovePropW(fixture->Window(), L"SnowDesktop.Entry.Fixture.Marker");
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(fixture->Window(), kMarkerMessage, owner, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored);
        passed = passed && !GetPropW(fixture->Window(), L"SnowDesktop.Entry.Fixture.Marker");
        InterlockedIncrement(&catalog->sequence); catalog->ownerCreation = creation; InterlockedIncrement(&catalog->sequence);
        UnmapViewOfFile(catalog);
    }
    else passed = false;
    if (catalogMapping) CloseHandle(catalogMapping);
    if (hook) passed = UnhookWindowsHookEx(hook) && passed;
    if (stop) SetEvent(stop);
    if (injector.hProcess)
    {
        DWORD result = 99;
        const bool done = WaitForSingleObject(injector.hProcess, 4000) == WAIT_OBJECT_0;
        passed = done && GetExitCodeProcess(injector.hProcess, &result) && result == 0 && passed;
        if (!done) TerminateProcess(injector.hProcess, 99);
        CloseHandle(injector.hProcess); CloseHandle(injector.hThread);
    }
    if (ready) CloseHandle(ready);
    if (stop) CloseHandle(stop);
    DestroyWindow(receiver);
    PostMessageW(fixture->Window(), kClose, 0, 0);
    const bool exited = WaitForSingleObject(target.hProcess, 5000) == WAIT_OBJECT_0;
    passed = passed && exited;
    if (!exited) { TerminateProcess(target.hProcess, 99); WaitForSingleObject(target.hProcess, 2000); }
    if (module) FreeLibrary(module);
    CloseHandle(target.hThread); CloseHandle(target.hProcess);
    UnmapViewOfFile(fixture); CloseHandle(mapping);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << (passed ? "PASS" : "FAIL") << " implementations=" << dispatched << " entryImages=" << entryImages
        << " implementationImages=" << implementationImages << " HookRegistrations=" << (registered ? 1 : 0)
        << " cross32=" << cross << '\n';
    return passed ? 0 : 1;
}
