#include "dock_thumbnail_toolbar.h"
#include "taskbar_hook/taskbar_symbol_resolver.h"
#include "platform/deployment_context.h"
#include "platform/hook_entry_protocol.h"
#include "platform/runtime_module_policy.h"
#include "data/data_paths.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <objbase.h>

namespace snowdesktop::thumbnail_toolbar
{
Target Capture(HWND window)
{
    Target target;
    target.window = reinterpret_cast<std::uintptr_t>(window);
    target.thread = GetWindowThreadProcessId(window, &target.process);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.process);
    if (process) { target.processCreation = hook_entry::ProcessCreation(process); CloseHandle(process); }
    return target;
}
DWORD QueryNative(HWND taskband, HMODULE entry, const Adapter& adapter, const Target& target,
    Snapshot& snapshot, const Click* click, bool* dismiss)
{
    if (dismiss) *dismiss = false;
    if (!runtime_modules::Allowed(runtime_modules::Module::Taskbar)) return ERROR_ACCESS_DISABLED_BY_POLICY;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(taskband, &process);
    DWORD shell = 0; GetWindowThreadProcessId(GetShellWindow(), &shell);
    if (!thread || process != shell || Capture(reinterpret_cast<HWND>(target.window)) != target)
        return ERROR_INVALID_WINDOW_HANDLE;
    static std::atomic<std::uint64_t> sequence{GetTickCount64()};
    const auto nonce = ++sequence;
    const DWORD owner = GetCurrentProcessId();
    const auto name = MappingName(owner, nonce);
    struct Handles
    {
        HANDLE mapping = nullptr; Request* request = nullptr; HHOOK hook = nullptr;
        ~Handles() { if (hook) UnhookWindowsHookEx(hook); if (request) UnmapViewOfFile(request); if (mapping) CloseHandle(mapping); }
    } resources;
    resources.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Request), name.c_str());
    if (!resources.mapping) return GetLastError();
    if (GetLastError() == ERROR_ALREADY_EXISTS) return ERROR_ALREADY_EXISTS;
    resources.request = static_cast<Request*>(MapViewOfFile(resources.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)));
    if (!resources.request) return GetLastError();
    auto& request = *resources.request;
    new(&request) Request;
    request.owner = owner;
    request.ownerCreation = hook_entry::ProcessCreation(GetCurrentProcess());
    request.adapter = adapter; request.target = target;
    if (click) { request.click = TRUE; request.button = click->button; request.expectedIdentity = click->identity; }
    const auto procedure = reinterpret_cast<HOOKPROC>(GetProcAddress(entry, "SnowDesktopRegistryQueryHookProc"));
    if (!procedure) return ERROR_PROC_NOT_FOUND;
    resources.hook = SetWindowsHookExW(WH_CALLWNDPROC, procedure, entry, thread);
    if (!resources.hook) return GetLastError();
    runtime_modules::Activity active(runtime_modules::Module::Taskbar);
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(taskband, RegisterWindowMessageW(kMessage), owner,
        static_cast<LPARAM>(nonce), SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &ignored))
        return GetLastError() ? GetLastError() : ERROR_TIMEOUT;
    if (!InterlockedCompareExchange(&request.complete, 0, 0)) return ERROR_NOT_READY;
    if (request.error) return request.error;
    if (request.snapshot.target != target || request.snapshot.count > kMaximumButtons ||
        Capture(reinterpret_cast<HWND>(target.window)) != target) return ERROR_INVALID_DATA;
    snapshot = request.snapshot;
    if (dismiss) *dismiss = request.dismiss != FALSE;
    return ERROR_SUCCESS;
}
struct Client::State
{
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Target> targets;
    std::deque<Click> clicks;
    std::deque<Snapshot> cache;
    Update update;
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
    std::uint64_t generation = 0;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::jthread worker;
    State() : worker([this](std::stop_token token) { Run(token); }) {}
    ~State()
    {
        worker.request_stop();
        if (stop) SetEvent(stop);
        wake.notify_all();
        if (worker.joinable()) worker.join();
        if (stop) CloseHandle(stop);
    }
    void Run(std::stop_token token) noexcept
    {
        try
        {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            struct Apartment { HRESULT result; ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); } } apartment{initialized};
            std::optional<Adapter> adapter;
            HMODULE entry = nullptr;
            struct ModuleOwner { HMODULE& module; ~ModuleOwner() { if (module) FreeLibrary(module); } } moduleOwner{entry};
            bool warmup = true;
            while (!token.stop_requested())
            {
                std::vector<Target> current;
                std::deque<Click> actions;
                std::uint64_t revision = 0;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return token.stop_requested() || warmup || !targets.empty(); });
                    if (token.stop_requested()) break;
                    current = targets; actions.swap(clicks); revision = generation;
                    warmup = false;
                }
                if (!runtime_modules::Allowed(runtime_modules::Module::Taskbar))
                {
                    std::unique_lock lock(mutex);
                    update = {};
                    wake.wait_for(lock, std::chrono::milliseconds(500), [&] { return token.stop_requested() || revision != generation; });
                    continue;
                }
                if (!adapter)
                {
                    const auto root = std::filesystem::path(GetDataDirectoryPath());
                    const auto result = taskbar_hook::RunTaskbarSymbolHelper(
                        std::filesystem::path(GetExecutableDirectoryPath()) / L"SnowDesktop.exe", root / L"ShellHookSymbols", stop);
                    if (result.thumbnailError == ERROR_SUCCESS) adapter = result.thumbnail;
                    else
                    {
                        std::unique_lock lock(mutex);
                        update = {};
                        wake.wait_for(lock, std::chrono::seconds(30), [&] { return token.stop_requested() || revision != generation; });
                        continue;
                    }
                }
                if (token.stop_requested()) break;
                if (!entry)
                {
                    const auto path = deployment::GetTaskbarHookPath();
                    if (!path.empty()) entry = LoadLibraryExW(path.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
                HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
                HWND band = taskbar ? reinterpret_cast<HWND>(GetPropW(taskbar, L"TaskbandHWND")) : nullptr;
                Update next;
                if (entry && band)
                {
                    for (const auto& click : actions)
                    {
                        if (token.stop_requested()) break;
                        // Hiding/changing the group cancels queued input.
                        { std::lock_guard lock(mutex); if (revision != generation) break; }
                        Snapshot snapshot; bool dismiss = false;
                        if (!QueryNative(band, entry, *adapter, click.target, snapshot, &click, &dismiss) && dismiss)
                            next.dismiss.push_back(click.target);
                    }
                    for (const auto& target : current)
                    {
                        if (token.stop_requested()) break;
                        { std::lock_guard lock(mutex); if (revision != generation) break; }
                        Snapshot snapshot;
                        if (!QueryNative(band, entry, *adapter, target, snapshot)) next.snapshots.push_back(std::move(snapshot));
                    }
                }
                {
                    std::unique_lock lock(mutex);
                    if (revision == generation)
                    {
                        for (const auto& snapshot : next.snapshots)
                        {
                            std::erase_if(cache, [&](const Snapshot& saved) { return saved.target.window == snapshot.target.window; });
                            cache.push_back(snapshot);
                        }
                        while (cache.size() > 32) cache.pop_front();
                        const bool changed = update.snapshots != next.snapshots || !next.dismiss.empty();
                        update.snapshots = std::move(next.snapshots);
                        update.dismiss.insert(update.dismiss.end(), next.dismiss.begin(), next.dismiss.end());
                        // Completion reaches the preview directly, without a
                        // second polling interval after native work finishes.
                        if (changed && notifyWindow && notifyMessage)
                            PostMessageW(notifyWindow, notifyMessage, 0, 0);
                    }
                    wake.wait_for(lock, std::chrono::milliseconds(350),
                        [&] { return token.stop_requested() || revision != generation || !clicks.empty(); });
                }
            }
        }
        catch (...) { std::lock_guard lock(mutex); update = {}; }
    }
};
Client::Client() : state_(std::make_unique<State>()) {}
Client::~Client() = default;
void Client::SetTargets(const std::vector<HWND>& windows)
{
    std::vector<Target> targets;
    for (HWND window : windows)
    {
        const auto target = Capture(window);
        if (target.thread && target.processCreation) targets.push_back(target);
    }
    std::lock_guard lock(state_->mutex);
    if (targets == state_->targets) return;
    state_->targets = std::move(targets); ++state_->generation;
    state_->clicks.clear(); state_->update = {};
    for (const auto& target : state_->targets)
        for (const auto& snapshot : state_->cache)
            if (snapshot.target == target) { state_->update.snapshots.push_back(snapshot); break; }
    state_->wake.notify_all();
}
void Client::SetNotification(HWND window, UINT message)
{
    std::lock_guard lock(state_->mutex);
    state_->notifyWindow = window; state_->notifyMessage = message;
}
void Client::Invoke(Click click)
{
    std::lock_guard lock(state_->mutex);
    if (state_->clicks.size() >= 8 ||
        std::find(state_->targets.begin(), state_->targets.end(), click.target) == state_->targets.end()) return;
    state_->clicks.push_back(click); state_->wake.notify_all();
}
Update Client::Take()
{
    std::lock_guard lock(state_->mutex);
    Update result = state_->update;
    state_->update.dismiss.clear();
    return result;
}
}
