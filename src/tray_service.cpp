#include "tray_service.h"
#include "deployment_context.h"
#include "diagnostic_log.h"
#include <map>
#include <thread>

namespace snowdesktop::tray
{
namespace
{
std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void ResolveApplication(Icon& icon)
{
    if (!icon.application.empty()) return;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, icon.identity.process);
    if (!process) return;
    wchar_t path[32768]{}; DWORD count = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &count))
    {
        icon.application.assign(path, count);
        if (icon.persistentKey.empty())
        {
            std::wstring normalized = icon.application;
            CharLowerBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
            icon.persistentKey = "app:" + Utf8(normalized) + ":" + std::to_string(icon.identity.id);
        }
    }
    CloseHandle(process);
}
struct Connection
{
    SharedState* shared = nullptr;
    HANDLE mapping = nullptr, signal = nullptr, explorer = nullptr;
    HMODULE module = nullptr;
    HWND window = nullptr;
    ~Connection()
    {
        if (shared)
        {
            InterlockedExchange(&shared->stop, 1);
            if (window) PostMessageW(window, RegisterWindowMessageW(kDetachMessage), shared->owner, 0);
            UnmapViewOfFile(shared);
        }
        for (HANDLE handle : {mapping, signal, explorer}) if (handle) CloseHandle(handle);
        if (module) FreeLibrary(module);
    }
};
}
struct Service::Impl
{
    mutable std::mutex mutex;
    Snapshot snapshot;
    std::shared_ptr<Connection> connection;
    std::map<std::string, Geometry> geometries;
    FocusReturnTracker focus;
    std::uint64_t focusSerial = 0;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::jthread worker;
    Impl()
    {
        if (stop) worker = std::jthread([this](std::stop_token token) { Run(token); });
        else { snapshot.degraded = true; snapshot.error = GetLastError(); }
    }
    ~Impl()
    {
        worker.request_stop(); if (stop) SetEvent(stop);
        if (worker.joinable()) worker.join();
        connection.reset();
        if (stop) CloseHandle(stop);
    }
    void PublishGeometries()
    {
        if (!connection || !connection->shared) return;
        auto& state = *connection->shared;
        InterlockedIncrement(&state.geometrySequence);
        state.geometryCount = 0;
        for (const auto& [key, geometry] : geometries)
        {
            (void)key;
            if (state.geometryCount == kGeometries) break;
            state.geometries[state.geometryCount++] = geometry;
        }
        MemoryBarrier(); InterlockedIncrement(&state.geometrySequence);
    }
    void PublishFocus()
    {
        if (connection && connection->shared)
            WriteFocusTicket(*connection->shared, focus.Current() ? focus.Current()->ticket : FocusTicket{});
    }
    void CancelFocus() { focus.Cancel(); PublishFocus(); }
    bool FocusIconExists() const
    {
        const auto* pending = focus.Current();
        return pending && std::any_of(snapshot.icons.begin(), snapshot.icons.end(), [&](const auto& icon) {
            return icon.key == pending->key && !(icon.state & NIS_HIDDEN) &&
                SameFocusIdentity(icon.identity, pending->ticket.identity);
        });
    }
    std::shared_ptr<Connection> Connect(DWORD& error, const wchar_t*& stage)
    {
        stage = L"findExplorer";
        auto result = std::make_shared<Connection>();
        result->window = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD shellProcess = 0, candidateProcess = 0;
        GetWindowThreadProcessId(GetShellWindow(), &shellProcess);
        GetWindowThreadProcessId(result->window, &candidateProcess);
        if (!shellProcess || candidateProcess != shellProcess)
        {
            struct Search { DWORD process; HWND window = nullptr; } search{shellProcess};
            EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
                auto& match = *reinterpret_cast<Search*>(parameter);
                DWORD process = 0; GetWindowThreadProcessId(window, &process);
                wchar_t name[64]{}; GetClassNameW(window, name, 64);
                if (process == match.process && wcscmp(name, L"Shell_TrayWnd") == 0)
                { match.window = window; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
            result->window = search.window;
        }
        DWORD explorer = 0;
        const DWORD thread = GetWindowThreadProcessId(result->window, &explorer);
        if (!thread) { error = ERROR_FILE_NOT_FOUND; return {}; }
        stage = L"openExplorer";
        result->explorer = OpenProcess(SYNCHRONIZE, FALSE, explorer);
        if (!result->explorer) { error = GetLastError(); return {}; }
        const auto pid = GetCurrentProcessId();
        stage = L"createMapping";
        result->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(SharedState), ObjectName(pid, L"State").c_str());
        if (!result->mapping) { error = GetLastError(); return {}; }
        if (GetLastError() == ERROR_ALREADY_EXISTS) { error = ERROR_ALREADY_EXISTS; return {}; }
        stage = L"mapView";
        result->shared = static_cast<SharedState*>(MapViewOfFile(result->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
        if (!result->shared) { error = GetLastError(); return {}; }
        new(result->shared) SharedState;
        auto& state = *result->shared;
        state.owner = pid;
        FILETIME creation{}, exit{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
        state.ownerCreation = static_cast<std::uint64_t>(creation.dwHighDateTime) << 32 | creation.dwLowDateTime;
        InterlockedExchange64(&state.epoch, static_cast<LONG64>(GetTickCount64()) + 1);
        stage = L"createSignal";
        result->signal = CreateEventW(nullptr, FALSE, FALSE, ObjectName(pid, L"Signal").c_str());
        if (!result->signal) { error = GetLastError(); return {}; }
        stage = L"loadHook";
        result->module = LoadLibraryExW(deployment::GetTaskbarHookPath().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!result->module || !result->signal) { error = GetLastError(); return {}; }
        stage = L"installHook";
        const auto proc = reinterpret_cast<HOOKPROC>(GetProcAddress(result->module, "SnowDesktopTrayHookProc"));
        HHOOK hook = proc ? SetWindowsHookExW(WH_CALLWNDPROC, proc, result->module, thread) : nullptr;
        if (!hook) { error = GetLastError(); return {}; }
        DWORD_PTR ignored = 0;
        stage = L"attachMessage";
        const bool delivered = SendMessageTimeoutW(result->window, RegisterWindowMessageW(kAttachMessage),
            pid, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored) != 0;
        if (!delivered) error = GetLastError();
        UnhookWindowsHookEx(hook);
        if (!delivered) return {};
        stage = L"collectorReady";
        HANDLE handles[]{stop, result->explorer, result->signal};
        const auto deadline = GetTickCount64() + 3000;
        while (!Read(state.ready) && GetTickCount64() < deadline)
        {
            const DWORD waited = WaitForMultipleObjects(3, handles, FALSE, 100);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_OBJECT_0 + 1) { error = ERROR_CANCELLED; return {}; }
            // The bootstrap supplement may fill the ring before Ready.
            if (Read(state.write) != Read(state.read)) break;
        }
        if (!Read(state.ready))
        {
            // Worker readiness is set before optional bootstrap in the collector.
            error = ERROR_TIMEOUT; return {};
        }
        stage = L"connected";
        error = ERROR_SUCCESS;
        return result;
    }
    void Reregister(const std::shared_ptr<Connection>& current)
    {
        DWORD recipients = BSM_APPLICATIONS;
        // Asynchronous broadcast follows successful collection attachment. It
        // never restarts Explorer and does not wait on third-party windows.
        BroadcastSystemMessageW(BSF_POSTMESSAGE | BSF_IGNORECURRENTTASK, &recipients,
            RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
        (void)current;
    }
    void Run(std::stop_token token)
    {
        DWORD reportedError = MAXDWORD;
        std::wstring reportedStage;
        while (!token.stop_requested())
        {
            DWORD error = 0;
            const wchar_t* stage = L"start";
            auto current = Connect(error, stage);
            if (error != reportedError || reportedStage != stage)
            {
                wchar_t message[256]{};
                swprintf_s(message, L"Tray connection stage=%s error=%lu pid=%lu", stage, error, GetCurrentProcessId());
                WriteDiagnosticLogEntry(message);
                reportedError = error; reportedStage = stage;
            }
            {
                std::lock_guard guard(mutex);
                snapshot.connected = current != nullptr; snapshot.degraded = !current;
                snapshot.error = error; snapshot.icons.clear(); ++snapshot.revision;
                connection = current; geometries.clear(); CancelFocus();
            }
            if (!current) { WaitForSingleObject(stop, 2000); continue; }
            Reregister(current);
            auto& state = *current->shared;
            LONG lost = 0;
            unsigned resyncAttempts = 0;
            ULONGLONG lastResync = 0;
            const auto connectedAt = GetTickCount64();
            bool reportedCollection = false;
            HANDLE handles[]{stop, current->explorer, current->signal};
            while (!token.stop_requested())
            {
                Event event;
                while (Consume(state, event))
                {
                    if (event.epoch != static_cast<std::uint64_t>(Read(state.epoch))) continue;
                    std::lock_guard guard(mutex);
                    if (event.operation == NIM_SETFOCUS)
                    {
                        if (FocusIconExists() && focus.Arrive(event, static_cast<std::uint64_t>(Read(state.epoch)),
                            reinterpret_cast<std::uint64_t>(GetForegroundWindow())))
                        {
                            const auto origin = reinterpret_cast<HWND>(focus.Current()->ticket.origin);
                            if (!PostMessageW(origin, FocusReturnMessage(), static_cast<WPARAM>(event.focusSerial), 0)) CancelFocus();
                        }
                        continue;
                    }
                    if (focus.Current() && (event.operation == NIM_ADD || event.operation == NIM_DELETE) &&
                        SameIdentity(event.identity, focus.Current()->ticket.identity)) CancelFocus();
                    if (Apply(snapshot.icons, event))
                    {
                        for (auto& icon : snapshot.icons) ResolveApplication(icon);
                        ++snapshot.revision;
                    }
                }
                if (!reportedCollection && GetTickCount64() - connectedAt >= 5000)
                {
                    std::lock_guard guard(mutex);
                    wchar_t message[256]{};
                    swprintf_s(message, L"Tray collection icons=%zu received=%ld decoded=%ld rejected=%ld lastSize=%ld resync=%ld",
                        snapshot.icons.size(), Read(state.received), Read(state.decoded), Read(state.rejected), Read(state.lastSize), Read(state.resync));
                    WriteDiagnosticLogEntry(message); reportedCollection = true;
                }
                if (Read(state.resync) != lost && GetTickCount64() - lastResync >= 3000)
                {
                    lost = Read(state.resync); lastResync = GetTickCount64();
                    { std::lock_guard guard(mutex); snapshot.degraded = true; ++snapshot.revision; }
                    // A persistent unknown Shell layout must not cause endless
                    // system-wide re-registration broadcasts.
                    if (resyncAttempts++ < 3)
                    {
                        InterlockedIncrement64(&state.epoch);
                        { std::lock_guard guard(mutex); snapshot.icons.clear();
                            geometries.clear(); PublishGeometries(); CancelFocus(); ++snapshot.revision; }
                        Reregister(current);
                    }
                }
                {
                    std::lock_guard guard(mutex);
                    const auto previous = snapshot.icons.size();
                    std::erase_if(snapshot.icons, [](const auto& icon) {
                        DWORD pid = 0; GetWindowThreadProcessId(reinterpret_cast<HWND>(icon.identity.window), &pid);
                        return !pid || pid != icon.identity.process;
                    });
                    if (previous != snapshot.icons.size()) ++snapshot.revision;
                    if (focus.Current() && !FocusIconExists()) CancelFocus();
                }
                const DWORD result = WaitForMultipleObjects(3, handles, FALSE, 1000);
                if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) break;
            }
            {
                std::lock_guard guard(mutex);
                CancelFocus(); connection.reset(); snapshot.icons.clear(); snapshot.connected = false; ++snapshot.revision;
            }
            current.reset();
        }
    }
};
Service::Service() : impl_(std::make_unique<Impl>()) {}
Service::~Service() = default;
Snapshot Service::Current() const { std::lock_guard guard(impl_->mutex); return impl_->snapshot; }
void Service::SetGeometry(const std::string& key, RECT rect)
{
    std::lock_guard guard(impl_->mutex);
    const auto found = std::find_if(impl_->snapshot.icons.begin(), impl_->snapshot.icons.end(), [&](const auto& icon) { return icon.key == key; });
    if (found == impl_->snapshot.icons.end()) return;
    if (IsRectEmpty(&rect)) impl_->geometries.erase(key);
    else impl_->geometries[key] = {found->identity, rect};
    impl_->PublishGeometries();
}
void Service::ClearGeometries()
{ std::lock_guard guard(impl_->mutex); impl_->geometries.clear(); impl_->PublishGeometries(); }
bool Service::Activate(const std::string& key, Activation action, POINT anchor, FocusOrigin origin)
{
    Icon icon;
    {
        std::lock_guard guard(impl_->mutex);
        const auto found = std::find_if(impl_->snapshot.icons.begin(), impl_->snapshot.icons.end(), [&](const auto& item) { return item.key == key; });
        if (found == impl_->snapshot.icons.end()) return false;
        icon = *found;
    }
    DWORD pid = 0;
    const HWND target = reinterpret_cast<HWND>(icon.identity.window);
    if (!GetWindowThreadProcessId(target, &pid) || pid != icon.identity.process || !icon.callback) return false;
    const bool gesture = action != Activation::Hover && action != Activation::Leave;
    std::uint64_t serial = 0;
    if (gesture)
    {
        DWORD originProcess = 0, sourceProcess = 0;
        GetWindowThreadProcessId(origin.target, &originProcess);
        if (!origin.source) origin.source = origin.target;
        GetWindowThreadProcessId(origin.source, &sourceProcess);
        std::lock_guard guard(impl_->mutex);
        impl_->CancelFocus();
        if (impl_->connection && impl_->snapshot.connected && originProcess == GetCurrentProcessId() &&
            sourceProcess == originProcess && IsWindowVisible(origin.target) && IsWindowVisible(origin.source))
        {
            FocusTicket ticket;
            ticket.epoch = static_cast<std::uint64_t>(Read(impl_->connection->shared->epoch));
            ticket.serial = serial = ++impl_->focusSerial;
            ticket.origin = reinterpret_cast<std::uint64_t>(origin.target);
            ticket.source = reinterpret_cast<std::uint64_t>(origin.source);
            ticket.identity = icon.identity; ticket.started = GetTickCount();
            ticket.keyboard = action == Activation::Keyboard || action == Activation::ContextKeyboard;
            impl_->focus.Arm(ticket, key);
            if (impl_->FocusIconExists()) impl_->PublishFocus();
            else { impl_->CancelFocus(); return false; }
        }
    }
    if (gesture) AllowSetForegroundWindow(pid);
    bool accepted = true;
    for (const auto callback : Callbacks(icon, action, anchor))
        accepted = SendNotifyMessageW(target, icon.callback, callback.wp, callback.lp) != FALSE && accepted;
    if (!accepted && serial)
    {
        std::lock_guard guard(impl_->mutex);
        if (impl_->focus.Current() && impl_->focus.Current()->ticket.serial == serial) impl_->CancelFocus();
    }
    return accepted;
}
void Service::CancelFocusReturn(HWND origin)
{
    std::lock_guard guard(impl_->mutex);
    if (!origin || (impl_->focus.Current() && impl_->focus.Current()->ticket.origin == reinterpret_cast<std::uint64_t>(origin)))
        impl_->CancelFocus();
}
void Service::ObserveForeground(HWND window, DWORD eventTime)
{
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    std::lock_guard guard(impl_->mutex);
    const bool nativeReturn = impl_->connection && impl_->focus.Current() && window == impl_->connection->window &&
        static_cast<std::uint64_t>(Read(impl_->connection->shared->focusClaimed)) == impl_->focus.Current()->ticket.serial;
    if (impl_->focus.ObserveForeground(reinterpret_cast<std::uint64_t>(window), process, eventTime, nativeReturn)) impl_->PublishFocus();
}
std::optional<FocusDelivery> Service::TakeFocusReturn(HWND origin, std::uint64_t serial)
{
    DWORD process = 0; GetWindowThreadProcessId(origin, &process);
    std::lock_guard guard(impl_->mutex);
    const auto* pending = impl_->focus.Current();
    if (!pending || pending->ticket.origin != reinterpret_cast<std::uint64_t>(origin) ||
        pending->ticket.serial != serial) return {};
    if (!impl_->connection || !impl_->FocusIconExists() || process != GetCurrentProcessId() || !IsWindowVisible(origin))
    { impl_->CancelFocus(); return {}; }
    auto result = impl_->focus.Take(reinterpret_cast<std::uint64_t>(origin), serial,
        static_cast<std::uint64_t>(Read(impl_->connection->shared->epoch)),
        reinterpret_cast<std::uint64_t>(GetForegroundWindow()));
    impl_->PublishFocus();
    return result;
}
void Service::OpenNativeTray()
{
    // Win+B is the supported keyboard entry into the native notification area;
    // it also works when a private protocol or elevated owner cannot be mirrored.
    INPUT input[4]{};
    for (auto& item : input) item.type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_LWIN; input[1].ki.wVk = 'B';
    input[2].ki.wVk = 'B'; input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].ki.wVk = VK_LWIN; input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, input, sizeof(INPUT));
}
}
