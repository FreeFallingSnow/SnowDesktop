#include "shell_extension_service.h"
#include "shell_extension_diagnostics.h"
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <shlobj.h>
#include <shlwapi.h>

namespace snowdesktop::shell_extensions
{
namespace
{
using Key = settings_ipc::Bytes;
Key SelectionKey(Request request)
{
    request.catalogueOnly = false;
    // File/Folder is an attribute of the actual selection, resolved off the UI.
    if (!request.background && request.context != Context::Desktop) request.context = Context::Automatic;
    for (auto &p : request.paths) p = std::filesystem::path(p).lexically_normal().wstring();
    return settings_ipc::Pack(request);
}
unsigned Contexts(const Request &request)
{
    if (request.background || request.context == Context::Desktop) return ContextBit(ResolveContext(request));
    unsigned contexts = 0;
    for (const auto &path : request.paths)
    {
        const auto attrs = GetFileAttributesW(path.c_str());
        contexts |= ContextBit(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) ? Context::Folder : Context::File);
    }
    return contexts;
}
bool Local(const Request &request)
{
    for (const auto &path : request.paths)
    {
        const std::filesystem::path file(path);
        if (!file.is_absolute() || PathIsNetworkPathW(path.c_str()) || GetDriveTypeW(file.root_path().c_str()) == DRIVE_REMOTE) return false;
    }
    return !request.paths.empty();
}
bool Configured(const Preferences &prefs, const std::string &id, Context context)
{
    return std::any_of(prefs.rules.begin(), prefs.rules.end(), [&](const auto &r) { return r.id == id && r.category == CategoryOf(context); }) ||
        std::any_of(prefs.overrides.begin(), prefs.overrides.end(), [&](const auto &r) { return r.id == id && r.context == context && r.visibility != Visibility::Inherit; });
}
}
std::vector<Entry> VisibleSnapshot(const Preferences &prefs, const Reply &reply, unsigned contexts)
{
    std::vector<Entry> result;
    for (const auto &entry : reply.entries)
    {
        bool hidden = !contexts;
        for (int i = 0; i < 4; ++i)
            if (contexts & (1u << i))
            {
                const auto context = static_cast<Context>(i);
                const auto &id = !entry.registration.empty() && Configured(prefs, entry.registration, context) ? entry.registration : entry.provider;
                hidden |= IsHidden(prefs, id, context);
            }
        if (entry.separator) { if (!result.empty() && !result.back().separator) result.push_back(entry); }
        else if (!hidden) result.push_back(entry);
    }
    while (!result.empty() && result.back().separator) result.pop_back();
    return result;
}
struct MenuService::Impl
{
    struct Click { CommandReference reference; POINT point; std::function<void(bool)> completed; };
    struct Row
    {
        Request request;
        MenuView view;
        QueryPriority priority = QueryPriority::Inspect;
        std::uint64_t due = 0, used = 0, completed = 0, retryAt = 0, sequence = 0, dependency = 0, bytes = 0;
        unsigned failures = 0;
        bool queued = false, force = false, invalid = false;
        std::vector<Click> clicks;
    };
    struct Running
    {
        Key key; std::uint64_t sequence, dependency;
        MenuSnapshotCache::Ticket ticket; QueryWork work;
        std::uint64_t started;
    };
    std::mutex mutex;
    std::map<Key, Row> rows;
    Catalogue catalogue;
    Preferences preferences;
    Request management;
    bool stop = false, scanRequested = false, scanning = false, configured = false, startupWarm = false;
    std::uint64_t clock = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;
    std::filesystem::path directory;
    QueryFactory factory;
    CatalogueReader readCatalogue;
    std::vector<Running> running;
    std::future<Catalogue> scan;
    explicit Impl(std::filesystem::path path, QueryFactory f, CatalogueReader r)
        : directory(std::move(path)), factory(std::move(f)), readCatalogue(std::move(r))
    {
        if (!factory) factory = [](const Request &request) {
            auto session = std::make_shared<Session>(request);
            return QueryWork{[session] { return session->Poll(); }, [session](UINT token, POINT p) { session->Invoke(token, p); }};
        };
        if (!readCatalogue) readCatalogue = [] { return ReadCatalogue(); };
        worker = std::thread([this] { Run(); });
    }
    ~Impl() { Stop(); if (wake) CloseHandle(wake); }
    void Stop()
    {
        { std::lock_guard lock(mutex); stop = true; }
        SetEvent(wake);
        if (worker.joinable()) worker.join();
    }
    void Queue(const Request &request, QueryPriority priority, bool force, unsigned delay = 0)
    {
        if (request.paths.empty() || request.paths.size() > 256) return;
        const auto key = SelectionKey(request);
        auto &row = rows[key]; row.request = request; row.request.catalogueOnly = false; row.used = ++clock;
        const auto now = GetTickCount64();
        if (row.view.pending)
        {
            row.priority = std::min(row.priority, priority);
            row.force |= force;
            if (priority == QueryPriority::Execute) row.due = now;
            MenuTrace("schedule", "joined"); return;
        }
        if (!force && (now < row.retryAt || (row.completed && now - row.completed < 10000 && !row.invalid))) return;
        row.priority = priority; row.force = force; row.due = now + delay; row.queued = true;
        row.view.pending = true; ++row.sequence;
        SetEvent(wake);
    }
    void Trim()
    {
        std::uint64_t bytes = 0; for (const auto &[key, row] : rows) bytes += row.bytes;
        while (rows.size() > MenuSnapshotCache::MemoryEntries || bytes > MenuSnapshotCache::MemoryBytes)
        {
            auto victim = rows.end();
            for (auto it = rows.begin(); it != rows.end(); ++it)
            {
                if (it->second.view.pending || !it->second.clicks.empty()) continue;
                if (victim == rows.end() || std::pair{it->second.request.context == Context::Desktop, it->second.used} < std::pair{victim->second.request.context == Context::Desktop, victim->second.used}) victim = it;
            }
            if (victim == rows.end()) break;
            bytes -= victim->second.bytes; rows.erase(victim);
        }
    }
    void Publish(const Key &key, Reply reply, unsigned contexts)
    {
        auto &row = rows.at(key);
        std::function<void(std::vector<Entry>&)> sanitize = [&](auto &entries) { for (auto &e : entries) { e.token = 0; sanitize(e.children); } };
        sanitize(reply.entries);
        row.bytes = settings_ipc::Pack(reply).size();
        if (row.bytes > 2 * 1024 * 1024) { row.bytes = 0; return; }
        row.view.snapshot = std::move(reply); row.view.contexts = contexts; ++row.view.revision;
        row.used = ++clock;
    }
    void Complete(Running &job, Reply reply, MenuSnapshotCache &cache)
    {
        Request request; std::vector<Click> clicks; bool current = false;
        {
            std::lock_guard lock(mutex);
            const auto it = rows.find(job.key);
            if (it == rows.end()) return;
            auto &row = it->second; request = row.request;
            current = row.sequence == job.sequence && row.dependency == job.dependency;
            clicks = std::move(row.clicks);
            if (!current)
            {
                row.view.pending = false;
                Queue(request, QueryPriority::Menu, true);
            }
            if (current)
            {
                row.view.pending = row.queued = false;
                row.view.error = reply.error;
                if (reply.ok)
                {
                    Associate(catalogue, request, reply);
                    row.failures = 0; row.retryAt = 0; row.completed = GetTickCount64(); row.invalid = false;
                }
                else
                {
                    row.failures = std::min(row.failures + 1, 5u);
                    row.retryAt = GetTickCount64() + std::min(30000u, 1000u << row.failures);
                    MenuTrace("schedule", "backoff", double(std::min(30000u, 1000u << row.failures)));
                }
            }
        }
        if (current && reply.ok)
        {
            cache.Store(job.ticket, reply);
            const auto contexts = Contexts(request);
            std::lock_guard lock(mutex);
            if (auto it = rows.find(job.key); it != rows.end() && it->second.sequence == job.sequence && it->second.dependency == job.dependency) Publish(job.key, reply, contexts);
        }
        bool invoked = false;
        for (auto &click : clicks)
        {
            bool succeeded = false;
            const auto token = current && reply.ok && !invoked ? ResolveCommand(reply, click.reference) : 0;
            if (token)
            {
                try { job.work.invoke(token, click.point); succeeded = invoked = true; } catch (...) {}
            }
            if (click.completed) click.completed(succeeded);
        }
    }
    void SaveCatalogue(MenuSnapshotCache &cache)
    {
        Catalogue value;
        { std::lock_guard lock(mutex); value = catalogue; }
        try
        {
            const auto bytes = settings_ipc::Pack(std::uint32_t(2), value);
            if (bytes.size() > 32 * 1024 * 1024) return;
            std::error_code ignored; std::filesystem::create_directories(cache.Directory(), ignored);
            const auto temporary = cache.Directory() / L"catalogue.tmp";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); out.close();
            if (out) MoveFileExW(temporary.c_str(), (cache.Directory() / L"catalogue.bin").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        catch (...) {}
    }
    void LoadCatalogue(MenuSnapshotCache &cache)
    {
        try
        {
            const auto file = cache.Directory() / L"catalogue.bin"; std::error_code error;
            const auto size = std::filesystem::file_size(file, error); if (error || size > 32 * 1024 * 1024) return;
            settings_ipc::Bytes bytes(static_cast<size_t>(size)); std::ifstream in(file, std::ios::binary);
            if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) return;
            auto [schema, value] = settings_ipc::Unpack<std::tuple<std::uint32_t, Catalogue>>(bytes);
            if (schema != 2) return;
            std::lock_guard lock(mutex); catalogue = std::move(value);
        }
        catch (...) {}
    }
    void RefreshCatalogue(MenuSnapshotCache &cache)
    {
        if (scan.valid() && scan.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            auto value = scan.get(); std::vector<Request> affected;
            if (!value.revision)
            {
                std::lock_guard lock(mutex); scanning = false;
                return; // An unsuccessful scan cannot masquerade as an empty registry.
            }
            {
                std::lock_guard lock(mutex);
                std::vector<Registration> changed;
                for (const auto &old : catalogue.rows)
                    if (std::none_of(value.rows.begin(), value.rows.end(), [&](const auto &r) { return r.id == old.id && r.revision == old.revision; })) changed.push_back(old);
                for (const auto &next : value.rows)
                    if (std::none_of(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == next.id && r.revision == next.revision; })) changed.push_back(next);
                for (const auto &a : catalogue.associations)
                    if (std::any_of(value.rows.begin(), value.rows.end(), [&](auto &r) { if (r.id != a.registration || !r.systemEnabled) return false; r.linked = true; return true; })) value.associations.push_back(a);
                catalogue = std::move(value); scanning = false;
                for (auto &[key, row] : rows)
                    if (std::any_of(changed.begin(), changed.end(), [&](const auto &r) { return Applies(r, row.request); }))
                    {
                        row.invalid = true; ++row.dependency; row.view.snapshot.reset(); row.bytes = 0; ++row.view.revision;
                        affected.push_back(row.request);
                    }
                MenuTrace("catalogue", changed.empty() ? "unchanged" : "dependencies_changed", 0, static_cast<unsigned>(affected.size()));
            }
            for (const auto &request : affected) cache.Erase(request);
            if (!affected.empty()) Session::ReleaseIdleWorker();
            SaveCatalogue(cache);
        }
        bool requested = false;
        {
            std::lock_guard lock(mutex);
            if (!scan.valid() && scanRequested) { scanRequested = false; scanning = requested = true; }
        }
        if (requested) scan = std::async(std::launch::async, [read = readCatalogue] {
            const HRESULT ole = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Catalogue result;
            try { result = read(); } catch (...) { MenuTrace("catalogue", "failure"); }
            if (SUCCEEDED(ole)) CoUninitialize();
            return result;
        });
    }
    void Run()
    {
        const HRESULT ole = OleInitialize(nullptr);
        if (FAILED(ole)) return;
        MenuSnapshotCache cache(directory.empty() ? SharedMenuCache().Directory() : directory);
        LoadCatalogue(cache);
        for (auto &[request, reply] : cache.Warm())
        {
            const auto contexts = Contexts(request);
            std::lock_guard lock(mutex); const auto key = SelectionKey(request);
            auto &row = rows[key]; row.request = request;
            Publish(key, std::move(reply), contexts);
        }
        for (;;)
        {
            { std::lock_guard lock(mutex); if (stop) break; }
            bool warmDesktop = false;
            { std::lock_guard lock(mutex); warmDesktop = std::exchange(startupWarm, false); }
            if (warmDesktop)
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &path)))
                {
                    Request request; request.paths = {path}; request.background = true; request.context = Context::Desktop; CoTaskMemFree(path);
                    if (Local(request))
                    {
                        std::lock_guard lock(mutex); Queue(request, QueryPriority::Prewarm, false);
                        request.extended = true; Queue(request, QueryPriority::Prewarm, false);
                    }
                }
            }
            if (TakeMenuRegistryChanges()) { std::lock_guard lock(mutex); if (configured || !catalogue.rows.empty()) scanRequested = true; }
            RefreshCatalogue(cache);
            for (size_t i = 0; i < running.size();)
            {
                auto &job = running[i]; std::optional<Reply> reply;
                try { reply = job.work.poll(); } catch (...) { reply = Reply{false, {}, "query exception"}; }
                if (!reply && GetTickCount64() - job.started >= 8000) reply = Reply{false, {}, "query timeout"};
                if (!reply) { ++i; continue; }
                Complete(job, std::move(*reply), cache);
                running.erase(running.begin() + i);
                SaveCatalogue(cache);
            }
            while (running.size() < 2)
            {
                Key key; Request request; QueryPriority priority; std::uint64_t sequence = 0, dependency = 0; bool invalid = false;
                {
                    std::lock_guard lock(mutex);
                    auto best = rows.end(); const auto now = GetTickCount64();
                    for (auto it = rows.begin(); it != rows.end(); ++it)
                        if (it->second.queued && it->second.due <= now && (best == rows.end() || std::tie(it->second.priority, it->second.due) < std::tie(best->second.priority, best->second.due))) best = it;
                    if (best == rows.end()) break;
                    auto &row = best->second; key = best->first; request = row.request; priority = row.priority;
                    sequence = row.sequence; dependency = row.dependency; invalid = row.invalid; row.queued = false;
                }
                if (priority == QueryPriority::Prewarm && !Local(request))
                {
                    std::lock_guard lock(mutex); rows[key].view.pending = false; continue;
                }
                auto ticket = cache.Capture(request);
                if (invalid) cache.Erase(request);
                if (auto disk = cache.Find(ticket))
                {
                    const auto contexts = Contexts(request);
                    std::lock_guard lock(mutex);
                    auto &row = rows[key];
                    if (!row.view.snapshot && row.dependency == dependency) Publish(key, *disk, contexts);
                }
                Running job{key, sequence, dependency, cache.Begin(ticket), {}, GetTickCount64()};
                try { job.work = factory(request); running.push_back(std::move(job)); }
                catch (...) { Complete(job, Reply{false, {}, "helper start failed"}, cache); }
            }
            { std::lock_guard lock(mutex); Trim(); }
            MsgWaitForMultipleObjectsEx(1, &wake, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        running.clear(); Session::ReleaseIdleWorker(); OleUninitialize();
    }
};
MenuService::MenuService(std::filesystem::path directory, QueryFactory factory, CatalogueReader reader)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(factory), std::move(reader))) {}
MenuService::~MenuService() = default;
void MenuService::Shutdown() { impl_->Stop(); }
MenuService &SharedMenuService() { static MenuService service; return service; }
MenuView MenuService::View(const Request &request)
{
    MenuTiming timing("first_screen");
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->rows.find(SelectionKey(request));
    if (it == impl_->rows.end()) { timing.Record("memory_miss"); return {}; }
    it->second.used = ++impl_->clock;
    timing.Record(it->second.view.snapshot ? "memory_hit" : "memory_miss");
    return it->second.view;
}
void MenuService::Query(const Request &request, QueryPriority priority, bool force)
{
    std::lock_guard lock(impl_->mutex); impl_->Queue(request, priority, force);
}
void MenuService::Prewarm(const Request &request)
{
    std::lock_guard lock(impl_->mutex);
    if (!HasOptIns(impl_->preferences)) return;
    // Only the latest still-queued selection survives the stability window.
    for (auto &[key, row] : impl_->rows)
        if (row.queued && row.priority == QueryPriority::Prewarm) row.queued = row.view.pending = false;
    impl_->Queue(request, QueryPriority::Prewarm, false, 150);
}
void MenuService::Configure(Preferences preferences)
{
    bool warm = false;
    {
        std::lock_guard lock(impl_->mutex);
        warm = !HasOptIns(impl_->preferences) && HasOptIns(preferences);
        impl_->preferences = std::move(preferences); impl_->configured = true;
        if (warm) impl_->scanRequested = true;
    }
    if (warm) { std::lock_guard lock(impl_->mutex); impl_->startupWarm = true; }
    SetEvent(impl_->wake);
}
void MenuService::Manage(const Request &request)
{
    std::lock_guard lock(impl_->mutex); impl_->management = request;
}
CatalogueView MenuService::Inspect(const Request &request, bool refresh)
{
    std::lock_guard lock(impl_->mutex);
    if (refresh || impl_->catalogue.rows.empty()) impl_->scanRequested = true;
    const auto selection = request.paths.empty() ? impl_->management : request;
    if (!selection.paths.empty()) impl_->Queue(selection, QueryPriority::Inspect, refresh);
    CatalogueView result; result.catalogue = impl_->catalogue; result.selection = selection; result.scanning = impl_->scanning || impl_->scanRequested;
    if (const auto it = impl_->rows.find(SelectionKey(selection)); it != impl_->rows.end()) result.menu = it->second.view;
    SetEvent(impl_->wake); return result;
}
void MenuService::Execute(const Request &request, CommandReference reference, POINT point, std::function<void(bool)> completed)
{
    std::lock_guard lock(impl_->mutex);
    impl_->Queue(request, QueryPriority::Execute, true);
    impl_->rows[SelectionKey(request)].clicks.push_back({std::move(reference), point, std::move(completed)});
}
void MenuService::Invalidate(const Request &request)
{
    std::lock_guard lock(impl_->mutex);
    auto &row = impl_->rows[SelectionKey(request)]; row.request = request; row.invalid = true; ++row.dependency;
    row.view.snapshot.reset(); row.bytes = 0; ++row.view.revision;
    if (!row.view.pending) impl_->Queue(request, QueryPriority::Menu, true);
}
}
