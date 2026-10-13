#include "../platform/hook_entry_protocol.h"
#include <memory>
#include <mutex>
#include <map>
#include <filesystem>
#include <atomic>

extern "C" __declspec(dllexport) const DWORD SnowDesktopHookEntryAbi = snowdesktop::hook_entry::kAbi;

namespace
{
using namespace snowdesktop::hook_entry;
struct Module
{
    HMODULE value = nullptr;
    ~Module() { if (value) FreeLibrary(value); }
};
std::mutex modulesMutex;
std::map<std::wstring, std::shared_ptr<Module>> modules;
thread_local bool loadingPayload = false;
std::atomic_bool entryPinned{false};

std::shared_ptr<Module> LoadPayload(DWORD owner, Payload payload)
{
    if (!owner || loadingPayload) return {};
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
    if (!process) return {};
    const auto creation = ProcessCreation(process);
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    if (!alive || !creation) return {};
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, CatalogName(owner).c_str());
    if (!mapping) return {};
    const auto catalog = static_cast<const Catalog*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Catalog)));
    Snapshot snapshot;
    const bool valid = catalog && Read(*catalog, owner, payload, snapshot) && snapshot.ownerCreation == creation;
    if (catalog) UnmapViewOfFile(catalog);
    CloseHandle(mapping);
    if (!valid || !std::filesystem::path(snapshot.path).is_absolute()) return {};
    {
        std::lock_guard lock(modulesMutex);
        const auto found = modules.find(snapshot.path);
        if (found != modules.end()) return found->second;
    }
    struct Loading
    {
        Loading() { loadingPayload = true; }
        ~Loading() { loadingPayload = false; }
    } loading;
    // Loading while holding the cache lock could reenter a window callback
    // through DllMain. Only the short publication step is serialized.
    auto candidate = std::make_shared<Module>();
    candidate->value = LoadLibraryExW(snapshot.path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!candidate->value) return {};
    std::lock_guard lock(modulesMutex);
    return modules.emplace(snapshot.path, candidate).first->second;
}

LRESULT Dispatch(DWORD owner, Payload payload, const char* name, int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0)
    {
        try
        {
            // A timeout can unhook while a callback is still executing. The
            // implementation's own pin does not retain this forwarding frame.
            if (!entryPinned.load(std::memory_order_acquire))
            {
                HMODULE entry = nullptr;
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        reinterpret_cast<LPCWSTR>(&Dispatch), &entry))
                    return CallNextHookEx(nullptr, code, wp, lp);
                entryPinned.store(true, std::memory_order_release);
            }
            const auto module = LoadPayload(owner, payload);
            const auto callback = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module->value, name)) : nullptr;
            if (callback) return callback(code, wp, lp);
        }
        catch (...) { /* Never unwind through a target window thread. */ }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD TargetOwner(HWND source)
{
    const auto receiver = reinterpret_cast<HWND>(GetPropW(source, L"SnowDesktop.Dock.MinimizeTarget.v1"));
    DWORD process = 0;
    GetWindowThreadProcessId(receiver, &process);
    return process;
}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopTaskbarHookProc(int code, WPARAM wp, LPARAM lp)
{
    const auto message = reinterpret_cast<const CWPSTRUCT*>(lp);
    const DWORD owner = code >= 0 && message
        ? HandleToULong(GetPropW(message->hwnd, snowdesktop::hook_entry::kOwnerProperty)) : 0;
    return Dispatch(owner, snowdesktop::hook_entry::Payload::Taskbar, "SnowDesktopTaskbarHookProc", code, wp, lp);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopTrayHookProc(int code, WPARAM wp, LPARAM lp)
{
    const auto message = reinterpret_cast<const CWPSTRUCT*>(lp);
    return Dispatch(code >= 0 && message ? static_cast<DWORD>(message->wParam) : 0,
        snowdesktop::hook_entry::Payload::Taskbar, "SnowDesktopTrayHookProc", code, wp, lp);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopRegistryQueryHookProc(int code, WPARAM wp, LPARAM lp)
{
    const auto message = reinterpret_cast<const CWPSTRUCT*>(lp);
    return Dispatch(code >= 0 && message ? static_cast<DWORD>(message->wParam) : 0,
        snowdesktop::hook_entry::Payload::Taskbar, "SnowDesktopRegistryQueryHookProc", code, wp, lp);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopStartPinHookProc(int code, WPARAM wp, LPARAM lp)
{
    const auto message = reinterpret_cast<const CWPSTRUCT*>(lp);
    return Dispatch(code >= 0 && message ? static_cast<DWORD>(message->wParam) : 0,
        snowdesktop::hook_entry::Payload::Taskbar, "SnowDesktopStartPinHookProc", code, wp, lp);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopDockMinimizeHook(int code, WPARAM wp, LPARAM lp)
{
    if (code != HCBT_MINMAX) return CallNextHookEx(nullptr, code, wp, lp);
#ifdef _WIN64
    constexpr auto payload = snowdesktop::hook_entry::Payload::Dock64;
    constexpr char name[] = "SnowDesktopDockMinimizeHook";
#else
    constexpr auto payload = snowdesktop::hook_entry::Payload::Dock32;
    constexpr char name[] = "_SnowDesktopDockMinimizeHook@12";
#endif
    return Dispatch(TargetOwner(reinterpret_cast<HWND>(wp)), payload, name, code, wp, lp);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopDockSourceCloakHook(int code, WPARAM wp, LPARAM lp)
{
    const auto message = reinterpret_cast<const CWPSTRUCT*>(lp);
#ifdef _WIN64
    constexpr auto payload = snowdesktop::hook_entry::Payload::Dock64;
    constexpr char name[] = "SnowDesktopDockSourceCloakHook";
#else
    constexpr auto payload = snowdesktop::hook_entry::Payload::Dock32;
    constexpr char name[] = "_SnowDesktopDockSourceCloakHook@12";
#endif
    return Dispatch(code >= 0 && message ? TargetOwner(message->hwnd) : 0, payload, name, code, wp, lp);
}
