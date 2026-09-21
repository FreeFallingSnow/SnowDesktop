#include "shell_extension_service.h"
#include "shell_extension_diagnostics.h"
#include "shell_extension_discovery.h"
#include "shell_extension_attribution.h"
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <array>
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
bool DependsOn(const Registration &row, const Request &request, unsigned contexts)
{
    if (!(row.contexts & contexts)) return false;
    if (row.types.empty() || std::find(row.types.begin(), row.types.end(), L"*") != row.types.end()) return true;
    return std::any_of(request.paths.begin(), request.paths.end(), [&](const auto &path) {
        auto extension = std::filesystem::path(path).extension().wstring();
        for (auto &c : extension) c = towlower(c);
        return std::find(row.types.begin(), row.types.end(), extension) != row.types.end();
    });
}
std::uint64_t Dependency(const Catalogue &catalogue, const Request &request, unsigned contexts)
{
    if (!catalogue.revision) return 0;
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto &row : catalogue.rows)
        if (DependsOn(row, request, contexts))
        {
            for (unsigned char c : row.id) { hash ^= c; hash *= 1099511628211ull; }
            hash ^= row.revision; hash *= 1099511628211ull;
        }
    return hash;
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
        Key identity; std::uint64_t expires = 0; bool checkRequested = false, checkInspection = false;
        bool queued = false, force = false, invalid = false, inspection = false;
        std::vector<Click> clicks;
    };
    struct Running
    {
        Key key; std::uint64_t sequence, dependency;
        MenuSnapshotCache::Ticket ticket; QueryWork work;
        std::uint64_t started;
    };
    struct SourceJob
    {
        Key key;
        Request request;
        Registration source;
        Reply actual;
        QueryWork work;
        std::uint64_t revision = 0, menuRevision = 0, started = 0;
    };
    std::unique_ptr<SourceJob> sourceJob;
    std::set<Key> sourceAttempts, sourceSelections;
    std::set<std::string> failedSources;
    bool sourcePending = false;
    std::mutex mutex;
    std::map<Key, Row> rows;
    Catalogue catalogue;
    Catalogue available;
    std::map<std::string, Registration> observed;
    bool observedDirty = false;
    Key availableSignature;
    std::vector<Request> discovery;
    bool discoverRequested = false, discoverForce = false;
    std::vector<Request> typeDiscovery;
    size_t nextType = 0;
    bool typesRequested = false, typesPreparing = false, typesForce = false, typeBatchForce = false;
    Preferences preferences;
    std::array<std::set<std::string>, 4> shown;
    Request management;
    Key inspectedSelection;
    bool stop = false, scanRequested = false, scanning = false, configured = false, startupWarm = false, inspected = false, desktopInspection = false, catalogueDirty = false;
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
    bool Enabled(const Request &request, unsigned contexts = 0) const
    {
        if (!configured) return true;
        if (!contexts && (request.background || request.context == Context::Desktop))
            contexts = ContextBit(request.context == Context::Desktop ? Context::Desktop : Context::FolderBackground);
        // Object attributes are resolved only on the worker. Until then accept
        // either scope; mixed selections require the same opt-in in both.
        const unsigned candidates = contexts ? contexts : 3u;
        for (int i = 0; i < 4; ++i) if (candidates & (1u << i))
            for (const auto &id : shown[i])
            {
                bool allowed = true;
                for (int j = 0; contexts && j < 4; ++j)
                    if ((contexts & (1u << j)) && !shown[j].contains(id)) allowed = false;
                if (!allowed) continue;
                const auto registration = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == id; });
                // Unknown dynamic providers remain eligible; never infer their
                // applicability from a caption or an incomplete observation.
                if (registration == catalogue.rows.end() ||
                    (registration->systemEnabled && DependsOn(*registration, request, candidates))) return true;
            }
        return false;
    }
    bool AnyEnabled() const
    {
        return std::any_of(shown.begin(), shown.end(), [](const auto &ids) { return !ids.empty(); });
    }
    void Queue(const Request &request, QueryPriority priority, bool force, unsigned delay = 0)
    {
        if (request.paths.empty() || request.paths.size() > 256) return;
        if ((priority == QueryPriority::Menu || priority == QueryPriority::Prewarm) && !Enabled(request)) return;
        const auto key = SelectionKey(request);
        auto &row = rows[key]; row.request = request; row.request.catalogueOnly = false; row.used = ++clock;
        const auto now = GetTickCount64();
        row.inspection |= priority == QueryPriority::Inspect;
        if (row.view.pending)
        {
            row.priority = std::min(row.priority, priority);
            row.force |= force;
            if (priority == QueryPriority::Execute) row.due = now;
            MenuTrace("schedule", "joined"); return;
        }
        row.checkRequested = true;
        row.checkInspection |= priority == QueryPriority::Inspect;
        SetEvent(wake);
        if (!force && (now < row.retryAt || (row.view.snapshot && !row.invalid && row.expires > MenuSnapshotCache::Now())))
        {
            row.inspection = false;
            return;
        }
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
    void RebuildAvailable()
    {
        // Observed management identities outlive exact-selection cache eviction.
        // Only root metadata is retained: no user paths, children or command tokens.
        std::map<std::string, const Registration *> registered;
        for (const auto &item : catalogue.rows) registered.emplace(item.id, &item);
        for (const auto &[key, row] : rows)
        {
            if (!row.view.snapshot || row.expires <= MenuSnapshotCache::Now()) continue;
            for (const auto &entry : row.view.snapshot->entries)
            {
                if (entry.separator || !entry.enabled || entry.provider.empty() || entry.label.empty() ||
                    entry.label == L"…" || entry.label == L"...") continue;
                const auto known = registered.find(entry.registration);
                if (known != registered.end() && !known->second->systemEnabled) continue;
                const auto id = entry.registration.empty() ? entry.provider : entry.registration;
                if (!observed.contains(id) && observed.size() >= 1024) continue;
                if (!entry.registration.empty() && entry.registration != entry.provider)
                    if (auto legacy = observed.find(entry.provider); legacy != observed.end())
                    {
                        legacy->second.contexts &= ~row.view.contexts;
                        if (!legacy->second.contexts) observed.erase(legacy);
                    }
                auto [it, inserted] = observed.try_emplace(id);
                auto &item = it->second;
                if (inserted)
                {
                    if (known != registered.end()) item = *known->second;
                    else { item.id = id; item.kind = RegistrationKind::Observed; }
                    item.linked = true; item.contexts = 0;
                }
                if (known != registered.end())
                {
                    item.commandIdentity = known->second->commandIdentity;
                    item.application = known->second->application;
                }
                else if (item.application.id.empty())
                {
                    auto selection = row.request;
                    selection.context = row.request.background ? (row.request.context == Context::Desktop ? Context::Desktop : Context::FolderBackground) :
                        row.view.contexts == ContextBit(Context::Folder) ? Context::Folder : Context::File;
                    item.application = RegisteredApplication(catalogue, selection, entry);
                }
                item.display = {};
                item.display.provider = entry.provider; item.display.registration = entry.registration;
                item.display.key = entry.key; item.display.label = entry.label; item.display.accessKey = entry.accessKey;
                item.display.width = entry.width; item.display.height = entry.height; item.display.pixels = entry.pixels;
                item.contexts |= row.view.contexts;
                if (known == registered.end())
                {
                    if (item.contexts & ~ContextBit(Context::File)) item.types = {L"*"};
                    else for (const auto &path : row.request.paths)
                    {
                        auto type = std::filesystem::path(path).extension().wstring();
                        for (auto &c : type) c = towlower(c);
                        if (!type.empty() && std::find(item.types.begin(), item.types.end(), type) == item.types.end()) item.types.push_back(std::move(type));
                    }
                }
            }
        }
        std::vector<Registration> result;
        size_t bytes = 0;
        for (auto it = observed.begin(); it != observed.end();)
        {
            const auto known = registered.find(it->first);
            bytes += settings_ipc::Pack(it->second).size();
            if (bytes > 8 * 1024 * 1024 || (catalogue.revision && !it->second.display.registration.empty() &&
                (known == registered.end() || !known->second->systemEnabled)))
            { it = observed.erase(it); continue; }
            result.push_back(it->second); ++it;
        }
        auto signature = settings_ipc::Pack(result, catalogue.associations);
        if (signature != availableSignature)
        {
            availableSignature = std::move(signature);
            available.rows = std::move(result); available.associations = catalogue.associations;
            ++available.revision;
            observedDirty = true;
        }
    }
    void SaveObserved(MenuSnapshotCache &cache)
    {
        std::vector<Registration> value;
        { std::lock_guard lock(mutex); if (!observedDirty) return; value = available.rows; observedDirty = false; }
        try
        {
            const auto bytes = settings_ipc::Pack(std::uint32_t(3), value);
            std::error_code error; std::filesystem::create_directories(cache.Directory(), error);
            const auto temporary = cache.Directory() / L"observed.tmp";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); out.close();
            if (out) MoveFileExW(temporary.c_str(), (cache.Directory() / L"observed.dat").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        catch (...) {}
    }
    void LoadObserved(MenuSnapshotCache &cache)
    {
        try
        {
            const auto file = cache.Directory() / L"observed.dat"; std::error_code error;
            const auto size = std::filesystem::file_size(file, error); if (error || size > 8 * 1024 * 1024 + 16) return;
            settings_ipc::Bytes bytes(static_cast<size_t>(size)); std::ifstream in(file, std::ios::binary);
            if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) return;
            auto [schema, value] = settings_ipc::Unpack<std::tuple<std::uint32_t, std::vector<Registration>>>(bytes);
            if (schema != 3 || value.size() > 1024) return;
            std::lock_guard lock(mutex);
            for (auto &entry : value)
                if (!entry.id.empty() && entry.linked && entry.systemEnabled && !entry.display.provider.empty())
                {
                    entry.display.children.clear(); entry.display.token = 0;
                    observed.emplace(entry.id, std::move(entry));
                }
            RebuildAvailable();
        }
        catch (...) {}
    }
    void Publish(const Key &key, Reply reply, unsigned contexts, std::uint64_t written = MenuSnapshotCache::Now())
    {
        auto &row = rows.at(key);
        std::function<void(std::vector<Entry>&)> sanitize = [&](auto &entries) { for (auto &e : entries) { e.token = 0; sanitize(e.children); } };
        sanitize(reply.entries);
        row.bytes = settings_ipc::Pack(reply).size();
        if (row.bytes > 2 * 1024 * 1024) { row.bytes = 0; return; }
        row.expires = written + MenuSnapshotCache::LifetimeMs;
        row.view.snapshot = std::move(reply); row.view.contexts = contexts; ++row.view.revision;
        row.used = ++clock;
        RebuildAvailable();
        sourcePending |= inspected;
    }
    std::unique_ptr<SourceJob> NextSource()
    {
        // Called only by the worker after ordinary menu/execute work has been
        // dispatched. One metadata probe leaves the other process slot free.
        std::lock_guard lock(mutex);
        if (!inspected || !sourcePending || scanning || scanRequested || !catalogue.revision) return {};
        for (const auto &[key, row] : rows)
        {
            if (!row.view.snapshot || row.view.pending || row.invalid || row.expires <= MenuSnapshotCache::Now() || !Local(row.request)) continue;
            const bool missing = std::any_of(row.view.snapshot->entries.begin(), row.view.snapshot->entries.end(), [&](const auto &entry) {
                const auto found = observed.find(entry.registration.empty() ? entry.provider : entry.registration);
                return found != observed.end() && found->second.application.id.empty();
            });
            if (missing) sourceSelections.insert(key);
            if (!sourceSelections.contains(key)) continue;
            for (const auto &source : catalogue.rows)
            {
                if ((source.kind != RegistrationKind::Handler && source.kind != RegistrationKind::Packaged) || !source.systemEnabled || source.application.id.empty() ||
                    source.verbs.empty() || failedSources.contains(source.id) || !DependsOn(source, row.request, row.view.contexts)) continue;
                // Repeated file suffixes share many already-inspected root
                // actions. Probe again only when this source sees a new action
                // identity in this location, not once per sample path or Shift.
                std::vector<Key> attempts;
                for (const auto &entry : row.view.snapshot->entries) if (!entry.separator && !entry.provider.empty())
                    attempts.push_back(settings_ipc::Pack(entry.provider, row.view.contexts, source.id, source.revision));
                if (std::all_of(attempts.begin(), attempts.end(), [&](const auto &attempt) { return sourceAttempts.contains(attempt); })) continue;
                std::wstring typeKey;
                for (const auto &path : source.sources)
                    if (const auto at = path.find(L"\\shellex\\ContextMenuHandlers\\"); at != std::wstring::npos) { typeKey = path.substr(0, at); break; }
                if (typeKey.empty() && source.kind != RegistrationKind::Packaged) continue;
                sourceAttempts.insert(attempts.begin(), attempts.end());
                auto job = std::make_unique<SourceJob>();
                job->key = key; job->request = row.request; job->source = source; job->actual = *row.view.snapshot;
                job->request.sourceClsid.assign(source.verbs.front().begin(), source.verbs.front().end());
                job->request.sourceKey = std::move(typeKey);
                job->revision = catalogue.revision; job->menuRevision = row.view.revision;
                return job;
            }
        }
        sourcePending = false;
        return {};
    }
    void CompleteSource(SourceJob &job, const Reply &reply)
    {
        std::lock_guard lock(mutex);
        MenuTrace("attribution", reply.ok ? "completed" : "failed", double(GetTickCount64() - job.started),
            static_cast<unsigned>(reply.entries.size()));
        if (!reply.ok && !reply.error.empty()) failedSources.insert(job.source.id);
        const auto current = rows.find(job.key);
        if (!reply.ok || catalogue.revision != job.revision || current == rows.end() || current->second.invalid ||
            current->second.view.revision != job.menuRevision) return;
        for (const auto &provider : MatchSourceCommands(job.actual, reply))
            if (const auto found = observed.find(provider); found != observed.end() && found->second.kind == RegistrationKind::Observed)
                RecordSourceApplication(found->second, job.source, catalogue);
        RebuildAvailable();
    }
    void Complete(Running &job, Reply reply, MenuSnapshotCache &cache)
    {
        Request request;
        { std::lock_guard lock(mutex); const auto it = rows.find(job.key); if (it == rows.end()) return; request = it->second.request; }
        const auto contexts = Contexts(request);
        auto resolved = request; resolved.context = ResolveContext(request);
        const bool targetCurrent = !job.ticket || cache.Capture(request).identity == job.ticket.identity;
        std::vector<Click> clicks; bool current = false;
        Preferences latestPreferences; bool enforcePreferences = false;
        {
            std::lock_guard lock(mutex);
            const auto it = rows.find(job.key);
            if (it == rows.end()) return;
            auto &row = it->second; request = row.request;
            current = targetCurrent && row.sequence == job.sequence && row.dependency == job.dependency;
            clicks = std::move(row.clicks);
            if (!current)
            {
                row.view.pending = false;
                Queue(request, row.inspection ? QueryPriority::Inspect : QueryPriority::Menu, true);
            }
            if (current)
            {
                row.view.pending = row.queued = false;
                row.inspection = false;
                row.view.error = reply.error;
                if (reply.ok)
                {
                    const auto associations = catalogue.associations.size();
                    Associate(catalogue, resolved, reply);
                    catalogueDirty |= associations != catalogue.associations.size();
                    row.failures = 0; row.retryAt = 0; row.completed = GetTickCount64(); row.invalid = false;
                    // Completion and display publication are one observable state.
                    // Settings must not stop polling before the snapshot appears.
                    Publish(job.key, reply, contexts);
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
        }
        auto executable = reply;
        { std::lock_guard lock(mutex); latestPreferences = preferences; enforcePreferences = configured; }
        if (enforcePreferences) executable.entries = VisibleSnapshot(latestPreferences, reply, contexts);
        bool invoked = false;
        for (auto &click : clicks)
        {
            bool succeeded = false;
            const auto token = current && reply.ok && !invoked ? ResolveCommand(executable, click.reference) : 0;
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
        { std::lock_guard lock(mutex); if (!catalogueDirty) return; value = catalogue; catalogueDirty = false; }
        try
        {
            const auto bytes = settings_ipc::Pack(std::uint32_t(4), value);
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
            if (schema != 4) return;
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
                std::lock_guard lock(mutex); scanning = false; typesRequested = false;
                return; // An unsuccessful scan cannot masquerade as an empty registry.
            }
            {
                std::lock_guard lock(mutex);
                const bool initial = !catalogue.revision;
                std::vector<Registration> changed;
                for (const auto &old : catalogue.rows)
                    if (std::none_of(value.rows.begin(), value.rows.end(), [&](const auto &r) { return r.id == old.id && r.revision == old.revision; })) changed.push_back(old);
                for (const auto &next : value.rows)
                    if (std::none_of(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == next.id && r.revision == next.revision; })) changed.push_back(next);
                for (const auto &a : catalogue.associations)
                    if (std::any_of(value.rows.begin(), value.rows.end(), [&](auto &r) { if (r.id != a.registration || !r.systemEnabled) return false; r.linked = true; return true; })) value.associations.push_back(a);
                catalogue = std::move(value); scanning = false; catalogueDirty = true;
                sourcePending |= inspected;
                if (!changed.empty()) { sourceAttempts.clear(); sourceSelections.clear(); failedSources.clear(); }
                if (!initial && !changed.empty())
                {
                    // Unknown providers also retain observed type/scope evidence,
                    // so an affected registration change retires stale switches.
                    std::erase_if(observed, [&](const auto &pair) {
                        const auto &item = pair.second;
                        return std::any_of(changed.begin(), changed.end(), [&](const auto &r) {
                            if (!(r.contexts & item.contexts)) return false;
                            const auto all = [](const auto &types) { return types.empty() || std::find(types.begin(), types.end(), L"*") != types.end(); };
                            return r.id == item.id || all(r.types) || all(item.types) ||
                                std::any_of(item.types.begin(), item.types.end(), [&](const auto &t) { return std::find(r.types.begin(), r.types.end(), t) != r.types.end(); });
                        });
                    });
                    if (inspected) typesRequested = true;
                }
                for (auto &[key, row] : rows)
                    if (!initial && std::any_of(changed.begin(), changed.end(), [&](const auto &r) { return DependsOn(r, row.request, row.view.contexts ? row.view.contexts : (row.request.background ? 12u : 3u)); }))
                    {
                        row.invalid = true; ++row.dependency; row.view.snapshot.reset(); row.bytes = 0; ++row.view.revision;
                        affected.push_back(row.request);
                        if (std::any_of(discovery.begin(), discovery.end(), [&](const auto &r) { return SelectionKey(r) == key; })) Queue(row.request, QueryPriority::Inspect, true);
                    }
                for (auto &[key, row] : rows)
                    if (row.view.snapshot)
                    {
                        auto resolved = row.request;
                        resolved.context = row.request.background ? (row.request.context == Context::Desktop ? Context::Desktop : Context::FolderBackground) : row.view.contexts == 2 ? Context::Folder : Context::File;
                        Associate(catalogue, resolved, *row.view.snapshot);
                    }
                RebuildAvailable();
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
        LoadObserved(cache);
        for (auto &[request, reply] : cache.Warm())
        {
            const auto contexts = Contexts(request);
            auto ticket = cache.Capture(request);
            ticket.dependency = Dependency(catalogue, request, contexts);
            auto current = cache.Find(ticket);
            if (!current) continue;
            std::lock_guard lock(mutex); const auto key = SelectionKey(request);
            auto &row = rows[key]; row.request = request;
            row.identity = ticket.identity;
            Publish(key, std::move(*current), contexts, cache.Written(ticket));
        }
        for (;;)
        {
            { std::lock_guard lock(mutex); if (stop) break; }
            bool discover = false;
            { std::lock_guard lock(mutex); discover = discoverRequested; }
            if (discover)
            {
                std::vector<Request> targets;
                wchar_t executable[32768]{};
                if (GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))))
                {
                    Request file; file.paths = {executable}; targets.push_back(file);
                    Request folder; folder.paths = {std::filesystem::path(executable).parent_path().wstring()}; targets.push_back(folder);
                    folder.background = true; folder.context = Context::FolderBackground; targets.push_back(folder);
                }
                PWSTR desktop = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &desktop)))
                {
                    Request background; background.paths = {desktop}; background.background = true; background.context = Context::Desktop;
                    targets.push_back(background); CoTaskMemFree(desktop);
                }
                std::erase_if(targets, [](const auto &request) { return !Local(request); });
                std::lock_guard lock(mutex);
                discovery = std::move(targets); discoverRequested = false;
                const bool force = std::exchange(discoverForce, false);
                for (const auto &request : discovery) Queue(request, QueryPriority::Inspect, force);
            }
            bool warmDesktop = false, inspectDesktop = false;
            { std::lock_guard lock(mutex); warmDesktop = std::exchange(startupWarm, false); inspectDesktop = desktopInspection; }
            if (warmDesktop || inspectDesktop)
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &path)))
                {
                    Request request; request.paths = {path}; request.background = true; request.context = Context::Desktop; CoTaskMemFree(path);
                    if (Local(request))
                    {
                        std::lock_guard lock(mutex);
                        if (inspectDesktop) { management = request; Queue(request, QueryPriority::Inspect, true); }
                        if (warmDesktop) { Queue(request, QueryPriority::Prewarm, false); request.extended = true; Queue(request, QueryPriority::Prewarm, false); }
                    }
                }
                if (inspectDesktop) { std::lock_guard lock(mutex); desktopInspection = false; }
            }
            if (TakeMenuRegistryChanges()) { std::lock_guard lock(mutex); if (inspected || AnyEnabled()) scanRequested = true; }
            RefreshCatalogue(cache);
            Catalogue typeCatalogue; bool discoverTypes = false;
            {
                std::lock_guard lock(mutex);
                if (typesRequested && !scanning && !scanRequested && catalogue.revision)
                {
                    typesRequested = false; typesPreparing = true; discoverTypes = true;
                    typeCatalogue = catalogue; typeBatchForce = std::exchange(typesForce, false);
                }
            }
            if (discoverTypes)
            {
                auto targets = DiscoverFileTypes(typeCatalogue, cache.Directory());
                std::lock_guard lock(mutex);
                typeDiscovery = std::move(targets); nextType = 0; typesPreparing = false;
                // Keep the polling set bounded across repeated refreshes.
                std::erase_if(discovery, [&](const auto &r) { return !r.paths.empty() && std::filesystem::path(r.paths.front()).parent_path() == cache.Directory() / L"discovery-samples-v1"; });
            }
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
            if (sourceJob)
            {
                std::optional<Reply> reply;
                try { reply = sourceJob->work.poll(); } catch (...) { reply = Reply{false, {}, "source query exception"}; }
                if (!reply && GetTickCount64() - sourceJob->started >= 8000) reply = Reply{false, {}, "source query timeout"};
                if (reply) { CompleteSource(*sourceJob, *reply); sourceJob.reset(); }
            }
            while (running.size() + (sourceJob ? 1 : 0) < 2)
            {
                Key key; Request request; QueryPriority priority; std::uint64_t sequence = 0, dependency = 0; bool invalid = false;
                {
                    std::lock_guard lock(mutex);
                    // Dispatch only one background sample at a time into the
                    // shared scheduler; interactive queries keep their priority.
                    if (nextType < typeDiscovery.size())
                    {
                        const auto &target = typeDiscovery[nextType++];
                        Queue(target, QueryPriority::Inspect, typeBatchForce);
                        discovery.push_back(target);
                    }
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
                const auto contexts = Contexts(request);
                {
                    std::lock_guard lock(mutex);
                    auto &row = rows[key];
                    if (!row.inspection && priority != QueryPriority::Execute && !Enabled(request, contexts))
                    {
                        row.view.pending = false; row.checkRequested = false;
                        MenuTrace("schedule", "no_enabled_items"); continue;
                    }
                    ticket.dependency = Dependency(catalogue, request, contexts);
                    rows[key].identity = ticket.identity; rows[key].checkRequested = rows[key].checkInspection = false;
                }
                if (invalid) cache.Erase(request);
                if (auto disk = cache.Find(ticket))
                {
                    std::lock_guard lock(mutex);
                    auto &row = rows[key];
                    if (!row.view.snapshot && row.dependency == dependency) Publish(key, *disk, contexts, cache.Written(ticket));
                    if (!row.force && row.clicks.empty() && row.sequence == sequence && row.dependency == dependency && row.view.snapshot)
                    {
                        row.view.pending = false; row.inspection = false;
                        MenuTrace("schedule", "snapshot_reused"); continue;
                    }
                }
                Running job{key, sequence, dependency, cache.Begin(ticket), {}, GetTickCount64()};
                try { job.work = factory(request); running.push_back(std::move(job)); }
                catch (...) { Complete(job, Reply{false, {}, "helper start failed"}, cache); }
            }
            if (!sourceJob && running.size() < 2)
                if (auto job = NextSource())
                {
                    job->started = GetTickCount64();
                    try { job->work = factory(job->request); sourceJob = std::move(job); }
                    catch (...) { std::lock_guard lock(mutex); failedSources.insert(job->source.id); MenuTrace("attribution", "start_failed"); }
                }
            std::vector<std::tuple<Key, Request, bool>> checks;
            {
                std::lock_guard lock(mutex);
                for (auto &[key, row] : rows) if (row.checkRequested && !row.view.pending)
                {
                    checks.emplace_back(key, row.request, row.checkInspection);
                    row.checkRequested = row.checkInspection = false;
                }
                Trim();
            }
            for (const auto &[key, request, inspection] : checks)
            {
                const auto ticket = cache.Capture(request);
                std::lock_guard lock(mutex);
                const auto found = rows.find(key); if (found == rows.end()) continue;
                auto &row = found->second;
                if (row.view.snapshot && row.identity != ticket.identity)
                {
                    row.view.snapshot.reset(); row.bytes = 0; row.invalid = true; ++row.dependency; ++row.view.revision;
                    RebuildAvailable();
                    Queue(request, inspection ? QueryPriority::Inspect : QueryPriority::Menu, true);
                }
            }
            SaveObserved(cache);
            bool watchesComplete = false;
            auto handles = MenuRegistryWaitHandles(watchesComplete);
            handles.insert(handles.begin(), wake);
            DWORD wait = watchesComplete ? INFINITE : 1000;
            if (!running.empty() || sourceJob || scan.valid()) wait = 20;
            {
                std::lock_guard lock(mutex);
                const auto now = GetTickCount64();
                if (stop || discoverRequested || startupWarm || desktopInspection || scanRequested ||
                    nextType < typeDiscovery.size()) wait = std::min(wait, DWORD(20));
                for (const auto &[key, row] : rows)
                    if (row.queued) wait = std::min(wait, static_cast<DWORD>(row.due > now ? std::min(row.due - now, std::uint64_t(MAXDWORD - 1)) : 20));
            }
            MsgWaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(), wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        sourceJob.reset(); running.clear(); Session::ReleaseIdleWorker(); OleUninitialize();
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
    if (it->second.expires && MenuSnapshotCache::Now() >= it->second.expires) { it->second.view.snapshot.reset(); it->second.bytes = 0; }
    timing.Record(it->second.view.snapshot ? "memory_hit" : "memory_miss");
    return it->second.view;
}
void MenuService::Query(const Request &request, QueryPriority priority, bool force)
{
    std::lock_guard lock(impl_->mutex); impl_->Queue(request, priority, force);
}
bool MenuService::MenuEnabled(const Request &request, const Preferences &fallback)
{
    std::lock_guard lock(impl_->mutex);
    return impl_->configured ? impl_->Enabled(request) : HasOptIns(fallback,
        request.context == Context::Desktop ? Context::Desktop : request.background ? Context::FolderBackground : Context::Automatic);
}
MenuView MenuService::MenuDisplay(const Request &request, const Preferences &fallback)
{
    MenuTiming timing("first_screen");
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->rows.find(SelectionKey(request));
    if (it == impl_->rows.end()) { timing.Record("memory_miss"); return {}; }
    auto &row = it->second; row.used = ++impl_->clock;
    auto view = row.view;
    if (row.expires <= MenuSnapshotCache::Now()) view.snapshot.reset();
    if (view.snapshot)
    {
        // Snapshots created before attribution still use provider IDs. Apply
        // only a unique, proven association when consulting current rules.
        for (auto &entry : view.snapshot->entries) if (entry.registration.empty())
        {
            std::string registration;
            bool ambiguous = false;
            unsigned associatedContexts = 0;
            for (const auto &a : impl_->catalogue.associations)
                if (a.provider == entry.provider && (ContextBit(a.context) & view.contexts))
                {
                    if (!registration.empty() && registration != a.registration) ambiguous = true;
                    registration = a.registration;
                    associatedContexts |= ContextBit(a.context);
                }
            if (!ambiguous && associatedContexts == view.contexts) entry.registration = std::move(registration);
        }
        view.snapshot->entries = VisibleSnapshot(impl_->configured ? impl_->preferences : fallback, *view.snapshot, view.contexts);
    }
    timing.Record(view.snapshot ? "memory_hit" : "memory_miss");
    return view;
}
void MenuService::Prewarm(const Request &request)
{
    std::lock_guard lock(impl_->mutex);
    if (!impl_->AnyEnabled()) return;
    // Only the latest still-queued selection survives the stability window.
    for (auto &[key, row] : impl_->rows)
        if (row.queued && row.priority == QueryPriority::Prewarm && !row.inspection) row.queued = row.view.pending = false;
    impl_->Queue(request, QueryPriority::Prewarm, false, 150);
}
void MenuService::Configure(Preferences preferences)
{
    {
        std::lock_guard lock(impl_->mutex);
        const bool enabled = impl_->AnyEnabled();
        const bool desktop = !impl_->shown[static_cast<int>(Context::Desktop)].empty();
        impl_->preferences = std::move(preferences); impl_->configured = true;
        for (int i = 0; i < 4; ++i) impl_->shown[i] = EffectiveShownIds(impl_->preferences, static_cast<Context>(i));
        if (!enabled && impl_->AnyEnabled()) impl_->scanRequested = true;
        if (!desktop && !impl_->shown[static_cast<int>(Context::Desktop)].empty()) impl_->startupWarm = true;
        for (auto &[key, row] : impl_->rows)
            if (row.queued && !row.inspection && row.priority != QueryPriority::Execute && !impl_->Enabled(row.request, row.view.contexts))
                row.queued = row.view.pending = row.checkRequested = false;
    }
    SetEvent(impl_->wake);
}
void MenuService::Manage(const Request &request)
{
    std::lock_guard lock(impl_->mutex); impl_->management = request; impl_->inspectedSelection.clear();
}
CatalogueView MenuService::Inspect(const Request &request, bool refresh)
{
    std::lock_guard lock(impl_->mutex);
    if (refresh || !impl_->inspected)
    {
        impl_->sourcePending = true;
        if (refresh) { impl_->sourceAttempts.clear(); impl_->sourceSelections.clear(); impl_->failedSources.clear(); }
        impl_->scanRequested = true; impl_->discoverRequested = true;
        impl_->discoverForce |= refresh;
        impl_->typesRequested = true; impl_->typesForce |= refresh;
    }
    impl_->inspected = true;
    if (request.context == Context::Desktop && request.paths.empty() && impl_->management.context != Context::Desktop) impl_->desktopInspection = true;
    // Keep an explicit desktop inspection selected while its path is resolved
    // on the worker; returning the previous file here reroutes the settings UI.
    const auto selection = request.context == Context::Desktop && request.paths.empty() && impl_->desktopInspection
        ? request : request.paths.empty() ? impl_->management : request;
    const auto selectionKey = SelectionKey(selection);
    const auto previous = impl_->rows.find(selectionKey);
    const bool invalidated = previous != impl_->rows.end() && previous->second.invalid;
    // Polling observes the current discovery, never starts it over after the
    // ordinary query freshness window expires (or after cache eviction).
    if (!selection.paths.empty() && (refresh || selectionKey != impl_->inspectedSelection || invalidated))
    {
        impl_->inspectedSelection = selectionKey;
        impl_->Queue(selection, QueryPriority::Inspect, refresh);
    }
    CatalogueView result; result.catalogue = impl_->available; result.selection = selection;
    result.scanning = impl_->sourcePending || impl_->scanning || impl_->scanRequested || impl_->desktopInspection || impl_->discoverRequested ||
        impl_->typesRequested || impl_->typesPreparing || impl_->nextType < impl_->typeDiscovery.size();
    for (const auto &target : impl_->discovery)
        if (const auto it = impl_->rows.find(SelectionKey(target)); it != impl_->rows.end()) result.scanning |= it->second.view.pending;
    if (const auto it = impl_->rows.find(SelectionKey(selection)); it != impl_->rows.end())
    {
        result.menu.pending = it->second.view.pending; result.menu.revision = it->second.view.revision;
        result.menu.contexts = it->second.view.contexts; result.menu.error = it->second.view.error;
    }
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
    impl_->RebuildAvailable();
    if (!row.view.pending) impl_->Queue(request, QueryPriority::Menu, true);
}
}
