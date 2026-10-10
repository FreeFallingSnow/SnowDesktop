#include "shell_extension_service.h"
#include "shell_extension_diagnostics.h"
#include "shell_extension_discovery.h"
#include "shell_extension_attribution.h"
#include "shell_extension_nvidia_compat.h"
#include "shell_context_menu_invoke.h"
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
constexpr ULONGLONG kInventoryWaitMs = 30000;
Key SelectionKey(Request request)
{
    request.catalogueOnly = false;
    // Preserve the optional-source mode in memory as well as on disk.
    // A full inspection cannot claim or overwrite the ordinary popup lane.
    // File/Folder is an attribute of the actual selection, resolved off the UI.
    if (!request.background && request.context != Context::Desktop) request.context = Context::Automatic;
    for (auto &p : request.paths) p = std::filesystem::path(p).lexically_normal().wstring();
    return settings_ipc::Pack(request);
}
Key SelectionBaseKey(Request request)
{
    request.omitNvidiaCompatibility = false;
    return SelectionKey(std::move(request));
}
bool NetworkSelection(const Request &request)
{
    return std::any_of(request.paths.begin(), request.paths.end(), [](const auto &path) {
        if (PathIsNetworkPathW(path.c_str())) return true;
        const std::filesystem::path file(path);
        auto drive = file.root_path().wstring();
        if (drive.empty()) return false;
        if (file.has_root_name() && !file.has_root_directory()) drive += L"\\";
        return GetDriveTypeW(drive.c_str()) == DRIVE_REMOTE;
    });
}
unsigned Contexts(const Request &request)
{
    if (request.background || request.context == Context::Desktop) return ContextBit(ResolveContext(request));
    // An offline mapped drive can block even GetFileAttributes indefinitely.
    // Unknown network selections are classified by the supervised helper.
    if (NetworkSelection(request)) return 0;
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
    // ShellLink can expose commands belonging to its target's type. The .lnk
    // suffix alone cannot prove those dependencies without resolving targets
    // (which may be offline), so shortcuts retain the full inventory proof.
    if (!request.background && std::any_of(request.paths.begin(), request.paths.end(), [](const auto &path) {
        return lstrcmpiW(PathFindExtensionW(path.c_str()), L".lnk") == 0;
    })) return catalogue.revision;
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
    const auto key = StateVisibilityId(id);
    return std::any_of(prefs.rules.begin(), prefs.rules.end(), [&](const auto &r) { return StateVisibilityId(r.id) == key && r.category == CategoryOf(context); }) ||
        std::any_of(prefs.overrides.begin(), prefs.overrides.end(), [&](const auto &r) { return StateVisibilityId(r.id) == key && r.context == context && r.visibility != Visibility::Inherit; });
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
                const auto *pair = StatePairForVerb(entry.key);
                const auto id = pair ? std::string(pair->id) :
                    !entry.registration.empty() && Configured(prefs, entry.registration, context) ? entry.registration : entry.provider;
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
    struct Click { CommandReference reference; POINT point; std::function<void(bool)> completed; ShellInvocationOwner owner; };
    struct Row
    {
        Request request;
        MenuView view;
        QueryPriority priority = QueryPriority::Inspect;
        std::uint64_t due = 0, used = 0, completed = 0, retryAt = 0, sequence = 0, dependency = 0, bytes = 0;
        unsigned failures = 0;
        Key identity, snapshotIdentity;
        std::uint64_t snapshotSignature = 0, snapshotDependency = 0;
        bool snapshotTargetChecked = false, registryInvalid = false;
        std::uint64_t expires = 0; bool checkRequested = false, checkInspection = false;
        bool queued = false, force = false, invalid = false, inspection = false;
        std::vector<Click> clicks;
    };
    struct Running
    {
        Key key; std::uint64_t sequence, dependency;
        MenuSnapshotCache::Ticket ticket; QueryWork work;
        std::uint64_t started;
        bool startPinOnly = false;
        unsigned contexts = 0;
        std::optional<Reply> reply;
        std::uint64_t verificationRevision = 0;
        ULONGLONG replyReadyAt = 0;
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
    std::map<Key, Key> requestedViews;
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
    bool stop = false, scanRequested = false, scanning = false, configured = false, startupWarm = false, inspected = false, desktopInspection = false, catalogueDirty = false, catalogueStale = false;
    std::uint64_t clock = 0;
    std::uint64_t registryRevision = 0, checkedRegistryRevision = 0, failedRegistryRevision = 0, scanRegistryRevision = 0;
    struct ScopeVerification { std::future<std::uint64_t> scan; std::uint64_t checked = 0, signature = 0, attempt = 0, scanRevision = 0; };
    std::array<ScopeVerification, 3> scopedVerification;
    ULONGLONG scanStarted = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;
    std::filesystem::path directory;
    QueryFactory factory;
    bool nativeFactory = false, usePreparedQuery = false, preparationRequested = false;
    CatalogueReader readCatalogue;
    FolderVerifier verifyFolder;
    BackgroundVerifier verifyBackground;
    std::vector<Running> running;
    std::future<Catalogue> scan;
    explicit Impl(std::filesystem::path path, QueryFactory f, CatalogueReader r, FolderVerifier v, BackgroundVerifier b)
        : directory(std::move(path)), factory(std::move(f)), readCatalogue(std::move(r)), verifyFolder(std::move(v)), verifyBackground(std::move(b))
    {
        if (!factory) { nativeFactory = true; factory = [this](const Request &request) {
            auto session = std::make_shared<Session>(request, request.startPinOnly ? 2000 : 8000, usePreparedQuery);
            return QueryWork{[session] { return session->Poll(); }, [session](UINT token, POINT p) { session->Invoke(token, p); },
                [session](UINT token, POINT p, HWND owner) { session->Invoke(token, p, owner); },
                [session] { return session->SelectionContexts(); }};
        }; }
        if (!readCatalogue)
        {
            readCatalogue = [] { return ReadCatalogue(); };
            if (!verifyFolder) verifyFolder = [] { return ReadFolderCatalogueRevision(); };
            if (!verifyBackground) verifyBackground = [](Context context) { return ReadBackgroundCatalogueRevision(context); };
        }
        worker = std::thread([this] { Run(); });
    }
    ~Impl() { Stop(); if (wake) CloseHandle(wake); }
    void Stop()
    {
        { std::lock_guard lock(mutex); stop = true; }
        SetEvent(wake);
        if (worker.joinable()) worker.join();
        // The registry scan runs separately from the scheduler. Drain it before
        // returning to host teardown, while their static metadata caches exist.
        if (scan.valid()) scan.wait();
        for (auto &scope : scopedVerification) if (scope.scan.valid()) scope.scan.wait();
    }
    // Called while mutex holds the same row and catalogue used for display.
    bool AttributionPending(const Request &request, const Preferences &fallback) const
    {
        if (request.startPinOnly || catalogue.revision || (!scanning && !scanRequested) ||
            // Inventory attribution enumerates the whole machine and can outlast a
            // single helper. Known rows remain visible throughout this bounded wait.
            (scanning && GetTickCount64() - scanStarted >= kInventoryWaitMs)) return false;
        const auto it = rows.find(SelectionKey(request));
        const unsigned contexts = it != rows.end() && it->second.view.contexts ? it->second.view.contexts :
            request.context == Context::Desktop ? ContextBit(Context::Desktop) :
            request.background ? ContextBit(Context::FolderBackground) : 3u;
        const auto &prefs = configured ? preferences : fallback;
        for (int i = 0; i < 4; ++i) if (contexts & (1u << i))
            for (const auto &id : EffectiveShownIds(prefs, static_cast<Context>(i)))
                if (id.starts_with("reg:") || id.starts_with("clsid:") || id.starts_with("package:")) return true;
        return false;
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
                if (request.startPinOnly && StateVisibilityId(id) != "state:start-pin") continue;
                bool allowed = true;
                for (int j = 0; contexts && j < 4; ++j)
                    if ((contexts & (1u << j)) && !shown[j].contains(id)) allowed = false;
                if (!allowed) continue;
                const auto registration = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == id; });
                // Unknown dynamic providers remain eligible; never infer their
                // applicability from a caption or an incomplete observation.
                if (catalogueStale || scanning || registration == catalogue.rows.end() ||
                    (registration->systemEnabled && DependsOn(*registration, request, candidates))) return true;
            }
        return false;
    }
    bool AnyEnabled() const
    {
        return std::any_of(shown.begin(), shown.end(), [](const auto &ids) { return !ids.empty(); });
    }
    bool ObjectPreparationEnabled() const
    {
        for (int i = 0; i < 2; ++i)
            for (const auto &id : shown[i]) if (StateVisibilityId(id) != "state:start-pin") return true;
        return false;
    }
    bool OnlyStartShown(const Request &request, unsigned contexts = 0) const
    {
        if (!configured || request.background || request.context == Context::Desktop || request.paths.size() != 1) return false;
        bool start = false;
        for (int i = 0; i < 2; ++i) if (!contexts || (contexts & (1u << i)))
            for (const auto &id : shown[i])
            {
                if (StateVisibilityId(id) != "state:start-pin") return false;
                start = true;
            }
        return start;
    }
    Request PopupRequest(Request request) const
    {
        request.catalogueOnly = false;
        request.omitNvidiaCompatibility = configured && request.background &&
            request.context == Context::Desktop && request.sourceClsid.empty() &&
            !NvidiaCompatibilityShown(shown[static_cast<int>(Context::Desktop)]);
        return request;
    }
    bool VerifiedBackgroundSnapshot(const Row &row) const
    {
        const int scope = VerificationIndex(row.view.contexts);
        return row.invalid && !row.force && scope > 0 && row.snapshotTargetChecked &&
            row.snapshotSignature && row.snapshotSignature == VerificationSignature(scope) &&
            (checkedRegistryRevision >= registryRevision || ScopeChecked(row.view.contexts, registryRevision));
    }
    bool RestoreRegistrySnapshot(Row &row, const MenuSnapshotCache::Ticket &ticket)
    {
        // A Classes notification is a request to verify, not proof that this
        // object's menu changed. Reuse only after the live inventory and exact
        // target both confirm the snapshot's original dependencies.
        if (!row.registryInvalid || !row.view.snapshot || row.force || !row.clicks.empty() ||
            !row.view.error.empty() || row.expires <= MenuSnapshotCache::Now() ||
            checkedRegistryRevision < registryRevision || !ticket ||
            row.snapshotIdentity != ticket.identity || !row.snapshotDependency ||
            row.snapshotDependency != ticket.dependency) return false;
        row.invalid = row.registryInvalid = false;
        row.snapshotTargetChecked = true;
        if (row.queued) row.queued = row.view.pending = row.inspection = false;
        ++row.view.revision;
        MenuTrace("schedule", "registry_snapshot_reused");
        return true;
    }
    Key PopupKey(const Request &request, bool retainPublished = false) const
    {
        auto popup = PopupRequest(request);
        if (popup.omitNvidiaCompatibility)
        {
            auto full = popup; full.omitNvidiaCompatibility = false;
            const auto found = rows.find(SelectionKey(full));
            if (found != rows.end())
            {
                const auto &row = found->second;
                if (row.view.snapshot && row.view.error.empty() && row.expires > MenuSnapshotCache::Now() &&
                    (!row.invalid || VerifiedBackgroundSnapshot(row) ||
                        (retainPublished && row.view.error.empty()))) return found->first;
            }
        }
        return SelectionKey(popup);
    }
    void RememberView(const Request &request, const Key &key)
    {
        if (key.empty()) return;
        const auto base = SelectionBaseKey(request);
        if (key != SelectionKey(request) || requestedViews.contains(base)) requestedViews[base] = key;
    }
    Key ViewKey(const Request &request) const
    {
        const auto found = requestedViews.find(SelectionBaseKey(request));
        if (found != requestedViews.end() && rows.contains(found->second)) return found->second;
        return PopupKey(request);
    }
    Key Queue(const Request &request, QueryPriority priority, bool force, unsigned delay = 0)
    {
        if (request.paths.empty() || request.paths.size() > 256) return {};
        // Opening/refreshing a Start-only popup is served by its dedicated
        // request. Inspection and explicit execution still retain full queries.
        if (priority == QueryPriority::Menu && !request.startPinOnly && OnlyStartShown(request)) return {};
        if ((priority == QueryPriority::Menu || priority == QueryPriority::Prewarm) && !Enabled(request)) return {};
        if (catalogueStale && !scanRequested)
        {
            scanRequested = true;
            MenuTrace("catalogue", "request.stale_query");
        }
        auto target = priority == QueryPriority::Inspect ? request : PopupRequest(request);
        target.catalogueOnly = false;
        if (priority == QueryPriority::Inspect) target.omitNvidiaCompatibility = false;
        // Reuse an already complete full snapshot, but never make a fresh
        // popup/click inherit the optional resolver of an in-flight inspection.
        if (target.omitNvidiaCompatibility && !force && priority != QueryPriority::Execute)
        {
            auto full = target; full.omitNvidiaCompatibility = false;
            const auto old = rows.find(SelectionKey(full));
            if (old != rows.end() && old->second.view.snapshot && !old->second.invalid &&
                old->second.view.error.empty() && old->second.expires > MenuSnapshotCache::Now())
                target = std::move(full);
        }
        const auto key = SelectionKey(target);
        auto &row = rows[key];
        const bool inspection = row.inspection || priority == QueryPriority::Inspect;
        row.request = std::move(target); row.used = ++clock;
        const auto now = GetTickCount64();
        row.inspection = inspection;
        if (row.view.pending)
        {
            row.priority = std::min(row.priority, priority);
            row.force |= force;
            // An explicit popup must not inherit the selection prewarm debounce.
            // Joined work keeps its session and sequence; only queued dispatch advances.
            if (priority == QueryPriority::Execute || priority == QueryPriority::Menu) row.due = now;
            MenuTrace("schedule", "joined"); return key;
        }
        row.checkRequested = true;
        row.checkInspection |= priority == QueryPriority::Inspect;
        SetEvent(wake);
        if (!force && (now < row.retryAt || (row.view.snapshot && !row.invalid && row.expires > MenuSnapshotCache::Now())))
        {
            row.inspection = false;
            return key;
        }
        row.priority = priority; row.force = force; row.due = now + delay; row.queued = true;
        row.view.pending = true; ++row.sequence;
        SetEvent(wake);
        return key;
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
            bytes -= victim->second.bytes;
            std::erase_if(requestedViews, [&](const auto &view) { return view.second == victim->first; });
            rows.erase(victim);
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
        row.snapshotIdentity = row.identity;
        row.snapshotDependency = Dependency(catalogue, row.request, contexts);
        const int scope = VerificationIndex(contexts);
        row.snapshotSignature = scope > 0 ? VerificationSignature(scope) : 0;
        row.snapshotTargetChecked = !row.snapshotIdentity.empty();
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
            if (row.request.startPinOnly) continue;
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
                // Attribution is metadata-only, independent of the original-file
                // executable recovery lane that produced these observed rows.
                job->request.originalShortcutOnly = false;
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
        const auto contexts = job.contexts ? job.contexts : Contexts(request);
        auto resolved = request;
        resolved.context = request.background || request.context == Context::Desktop ?
            (request.context == Context::Desktop ? Context::Desktop : Context::FolderBackground) :
            contexts == ContextBit(Context::Folder) ? Context::Folder : Context::File;
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
                // A Start action can retain the validated ordinary snapshot
                // while retiring an older aggregate already in flight.
                Queue(request, row.inspection ? QueryPriority::Inspect : QueryPriority::Menu,
                    row.inspection || row.invalid || !row.view.snapshot);
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
                    row.failures = 0; row.retryAt = 0; row.completed = GetTickCount64();
                    row.invalid = row.registryInvalid = false;
                    row.force = false;
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
                if (!click.reference.empty() && StatePairForVerb(std::get<1>(click.reference.back()))) invoked = true;
                try
                {
                    if (job.work.invokeWithOwner) job.work.invokeWithOwner(token, click.point, click.owner.Resolve());
                    else job.work.invoke(token, click.point);
                    succeeded = invoked = true;
                }
                catch (...) {}
            }
            // A stateful invocation has completed in the helper before invoke
            // returns. Retire its pre-action snapshot for every menu variant.
            if (token && !click.reference.empty() && StatePairForVerb(std::get<1>(click.reference.back())))
            {
                struct Retained
                {
                    Request request; Reply reply; Key identity;
                    unsigned contexts; std::uint64_t dependency, written;
                };
                std::vector<Request> affected;
                std::vector<Retained> retained;
                const bool startAction = StatePairForVerb(std::get<1>(click.reference.back()))->id == "state:start-pin" &&
                    request.paths.size() == 1;
                {
                    std::lock_guard lock(mutex);
                    for (auto &[key, row] : rows) if (row.request.paths == request.paths)
                    {
                        if (startAction && !row.request.startPinOnly && row.view.snapshot && !row.invalid &&
                            !row.identity.empty() && row.expires > MenuSnapshotCache::Now())
                        {
                            auto ordinary = *row.view.snapshot;
                            std::erase_if(ordinary.entries, [](const auto &entry) {
                                const auto *pair = StatePairForVerb(entry.key);
                                return pair && pair->id == "state:start-pin";
                            });
                            ++row.dependency; ++row.view.revision;
                            row.bytes = settings_ipc::Pack(ordinary).size(); row.view.snapshot = ordinary;
                            retained.push_back({row.request, std::move(ordinary), row.identity, row.view.contexts,
                                row.dependency, row.expires - MenuSnapshotCache::LifetimeMs});
                            continue;
                        }
                        row.view.snapshot.reset(); row.bytes = 0; row.invalid = true;
                        ++row.dependency; ++row.view.revision;
                        affected.push_back(row.request);
                    }
                    RebuildAvailable();
                    for (const auto &target : affected) Queue(target, QueryPriority::Menu, true);
                }
                for (const auto &target : affected) cache.Erase(target);
                for (const auto &saved : retained)
                {
                    auto ticket = cache.Capture(saved.request);
                    if (ticket && ticket.identity == saved.identity)
                    {
                        { std::lock_guard lock(mutex);
                          ticket.dependency = Dependency(catalogue, saved.request, saved.contexts); }
                        // Replace the disk row without extending the lifetime
                        // of its ordinary commands or persisting the old pin.
                        if (!cache.Store(cache.Begin(std::move(ticket)), saved.reply, saved.written)) cache.Erase(saved.request);
                    }
                    else
                    {
                        {
                            std::lock_guard lock(mutex);
                            auto &row = rows[SelectionKey(saved.request)];
                            if (row.dependency == saved.dependency)
                            {
                                row.view.snapshot.reset(); row.bytes = 0; row.invalid = true;
                                ++row.dependency; ++row.view.revision;
                                Queue(saved.request, QueryPriority::Menu, true);
                            }
                        }
                        cache.Erase(saved.request);
                    }
                }
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
            auto value = scan.get(); std::vector<Request> affected; bool retirePreparation = false;
            if (!value.revision)
            {
                std::lock_guard lock(mutex); scanning = false; typesRequested = false; catalogueStale = true;
                failedRegistryRevision = std::max(failedRegistryRevision, scanRegistryRevision);
                return; // An unsuccessful scan cannot masquerade as an empty registry.
            }
            {
                std::lock_guard lock(mutex);
                checkedRegistryRevision = std::max(checkedRegistryRevision, scanRegistryRevision);
                const bool initial = !catalogue.revision;
                if (!initial && value.revision == catalogue.revision)
                {
                    // The scanner hashes the complete registration inventory.
                    // Keep existing associations and observed icons when only a
                    // registry notification repeated the same inventory. Forced
                    // discovery and source queries remain queued independently.
                    catalogue.folderRevision = value.folderRevision;
                    catalogue.backgroundRevisions = value.backgroundRevisions;
                    scanning = false;
                    sourcePending |= inspected;
                    MenuTrace("catalogue", "unchanged");
                }
                else
                {
                    // Preparation contains registrations for unseen selections
                    // too; retiring it cannot depend on existing affected rows.
                    retirePreparation = true;
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
            }
            for (const auto &request : affected) cache.Erase(request);
            if (retirePreparation)
            {
                Session::ReleasePreparedMenuWorker();
                std::lock_guard lock(mutex); preparationRequested |= ObjectPreparationEnabled();
            }
            if (!affected.empty()) Session::ReleaseIdleWorker();
            SaveCatalogue(cache);
        }
        bool requested = false;
        {
            std::lock_guard lock(mutex);
            if (!scan.valid() && scanRequested)
            {
                scanRegistryRevision = registryRevision;
                scanStarted = GetTickCount64();
                failedRegistryRevision = 0;
                scanRequested = catalogueStale = false; scanning = requested = true;
            }
        }
        if (requested) scan = std::async(std::launch::async, [read = readCatalogue] {
            MenuTrace("catalogue", "scan.start");
            const HRESULT ole = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Catalogue result;
            try { result = read(); } catch (...) { MenuTrace("catalogue", "failure"); }
            if (SUCCEEDED(ole)) CoUninitialize();
            return result;
        });
    }
    static int VerificationIndex(unsigned contexts)
    {
        if (contexts == ContextBit(Context::Folder)) return 0;
        if (contexts == ContextBit(Context::FolderBackground)) return 1;
        if (contexts == ContextBit(Context::Desktop)) return 2;
        return -1;
    }
    std::uint64_t VerificationSignature(int index) const
    {
        return index == 0 ? catalogue.folderRevision : catalogue.backgroundRevisions[index - 1];
    }
    bool ScopeChecked(unsigned contexts, std::uint64_t required) const
    {
        const int index = VerificationIndex(contexts);
        if (index < 0) return false;
        const auto &scope = scopedVerification[index];
        return scope.checked >= required && scope.signature && scope.signature == VerificationSignature(index);
    }
    // Live scoped proof excludes unrelated file classes while retaining all
    // registration, applicability, icon and policy inputs of this menu scope.
    // It never marks the whole registration inventory checked.
    void RefreshScopeVerification()
    {
        for (int index = 0; index < 3; ++index)
        {
            auto &scope = scopedVerification[index];
            if (!scope.scan.valid() || scope.scan.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) continue;
            const auto signature = scope.scan.get();
            std::lock_guard lock(mutex);
            if (signature && signature == VerificationSignature(index))
            {
                scope.checked = std::max(scope.checked, scope.scanRevision);
                scope.signature = signature;
                MenuTrace("scope.catalogue", "unchanged", 0, static_cast<unsigned>(index));
            }
            else MenuTrace("scope.catalogue", "changed_or_failed", 0, static_cast<unsigned>(index));
        }
    }
    void StartScopeVerification(unsigned contexts, std::uint64_t required)
    {
        const int index = VerificationIndex(contexts);
        if (index < 0 || (index == 0 ? !verifyFolder : !verifyBackground)) return;
        auto &scope = scopedVerification[index];
        if (scope.scan.valid()) return;
        {
            std::lock_guard lock(mutex);
            if (!VerificationSignature(index) || checkedRegistryRevision >= required ||
                ScopeChecked(contexts, required) || registryRevision <= scope.attempt) return;
            scope.scanRevision = scope.attempt = registryRevision;
        }
        const auto read = index == 0 ? verifyFolder : FolderVerifier([this, index] {
            return verifyBackground(index == 1 ? Context::FolderBackground : Context::Desktop);
        });
        try { scope.scan = std::async(std::launch::async, [read] {
            const HRESULT ole = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(ole)) return std::uint64_t(0);
            std::uint64_t result = 0;
            try { result = read(); } catch (...) { MenuTrace("scope.catalogue", "failure"); }
            CoUninitialize();
            return result;
        }); }
        catch (...) { MenuTrace("scope.catalogue", "start_failed"); }
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
            if (TakeMenuRegistryChanges())
            {
                const bool prepared = nativeFactory && Session::PollPreparedMenuWorker() != Session::PreparationState::Absent;
                std::lock_guard lock(mutex);
                // Classes notifications also include unrelated Shell caches.
                // Mark cached views stale, but retain published payloads for an open
                // popup until the inventory confirms an actual registration change.
                catalogueStale = true;
                ++registryRevision;
                // Verify a prepared provider inventory even when no visible
                // selection currently needs a refresh.
                scanRequested |= prepared;
                for (auto &[key, row] : rows)
                {
                    row.registryInvalid = row.view.snapshot && (!row.invalid || row.registryInvalid);
                    row.invalid = true; row.snapshotTargetChecked = false; ++row.view.revision;
                    if (row.view.snapshot && VerificationIndex(row.view.contexts) > 0) row.checkRequested = true;
                    // Classes includes Shell caches written by the query itself.
                    // Verify registration changes before retiring in-flight work;
                    // otherwise every successful reply can trigger another query.
                    if (row.view.pending)
                        scanRequested |= !row.request.startPinOnly;
                    else
                        ++row.dependency;
                }
                MenuTrace("catalogue", "stale.registry");
            }
            RefreshCatalogue(cache);
            RefreshScopeVerification();
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
                auto &job = running[i];
                if (!job.reply)
                {
                    try { job.reply = job.work.poll(); } catch (...) { job.reply = Reply{false, {}, "query exception"}; }
                    if (job.reply && job.reply->ok && !job.contexts)
                    {
                        unsigned contexts = 0;
                        try { if (job.work.selectionContexts) contexts = job.work.selectionContexts(); } catch (...) {}
                        if (contexts >= 1 && contexts <= 3) job.contexts = contexts;
                        else job.reply = Reply{false, {}, "selection context unavailable"};
                    }
                    if (job.reply && job.reply->ok)
                    {
                        std::lock_guard lock(mutex);
                        job.verificationRevision = registryRevision;
                        job.replyReadyAt = GetTickCount64();
                    }
                    if (!job.reply && GetTickCount64() - job.started >= 8000)
                        job.reply = Reply{false, {}, "query timeout"};
                }
                if (!job.reply) { ++i; continue; }
                if (job.reply->ok && !job.startPinOnly)
                {
                    StartScopeVerification(job.contexts, job.verificationRevision);
                    std::lock_guard lock(mutex);
                    // Verify notifications observed before this reply became ready.
                    // Initial discovery and later unrelated notifications must not
                    // keep extending an already-captured query's wait indefinitely.
                    if (checkedRegistryRevision < job.verificationRevision &&
                        !ScopeChecked(job.contexts, job.verificationRevision))
                    {
                        if (failedRegistryRevision >= job.verificationRevision ||
                            GetTickCount64() - job.replyReadyAt >= kInventoryWaitMs)
                            job.reply = Reply{false, {}, "registration verification failed or timed out"};
                        else
                        {
                            scanRequested |= catalogueStale;
                            ++i; continue;
                        }
                    }
                }
                Complete(job, std::move(*job.reply), cache);
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
            // Arbitrary extensions retain two supervised slots. Microsoft's
            // Start handler has one reserved slot and never inherits a worker
            // that has loaded third-party DLLs.
            while (true)
            {
                const auto startJobs = std::count_if(running.begin(), running.end(), [](const auto &job) {
                    return job.startPinOnly;
                });
                const auto normalJobs = static_cast<std::ptrdiff_t>(running.size()) - startJobs + (sourceJob ? 1 : 0);
                Key key; Request request; QueryPriority priority; std::uint64_t sequence = 0, dependency = 0; bool invalid = false;
                {
                    std::lock_guard lock(mutex);
                    // Background queries share one slot, including metadata probes.
                    // Keep the second slot available for an explicit popup or click.
                    if (normalJobs < 1 && nextType < typeDiscovery.size())
                    {
                        const auto &target = typeDiscovery[nextType++];
                        Queue(target, QueryPriority::Inspect, typeBatchForce);
                        discovery.push_back(target);
                    }
                    auto best = rows.end(); const auto now = GetTickCount64();
                    for (auto it = rows.begin(); it != rows.end(); ++it)
                        if (it->second.queued && it->second.due <= now &&
                            (it->second.request.startPinOnly ? startJobs < 1 :
                                normalJobs < (it->second.priority <= QueryPriority::Menu ? 2 : 1)) &&
                            (best == rows.end() || std::tie(it->second.priority, it->second.due) < std::tie(best->second.priority, best->second.due))) best = it;
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
                std::uint64_t verifyCachedRevision = 0;
                {
                    std::lock_guard lock(mutex);
                    auto &row = rows[key];
                    if (!row.inspection && priority != QueryPriority::Execute && !Enabled(request, contexts))
                    {
                        row.view.pending = false; row.checkRequested = false;
                        MenuTrace("schedule", "no_enabled_items"); continue;
                    }
                    ticket.dependency = Dependency(catalogue, request, contexts);
                    row.snapshotTargetChecked = ticket && row.snapshotIdentity == ticket.identity;
                    if (RestoreRegistrySnapshot(row, ticket))
                    {
                        row.view.pending = row.inspection = false;
                        continue;
                    }
                    if (contexts >= 1 && contexts <= 3 && row.registryInvalid &&
                        !row.force && row.view.snapshot && row.snapshotTargetChecked &&
                        checkedRegistryRevision < registryRevision &&
                        failedRegistryRevision < registryRevision &&
                        (!scanning || GetTickCount64() - scanStarted < kInventoryWaitMs))
                    {
                        // Wait for the already-requested inventory rather than
                        // launching an extension process just to discard it.
                        row.queued = true; row.due = GetTickCount64() + 20;
                        scanRequested |= catalogueStale;
                        break;
                    }
                    const int scope = VerificationIndex(contexts);
                    if (row.invalid && !row.force && row.view.snapshot && row.snapshotTargetChecked && scope > 0 &&
                        row.snapshotSignature && row.snapshotSignature == VerificationSignature(scope))
                        verifyCachedRevision = registryRevision;
                    row.identity = ticket.identity; row.checkRequested = row.checkInspection = false;
                }
                // A previous background snapshot can be verified while the new
                // Shell query is still running, including a stalled extension.
                // Target identity and the snapshot's own live scope proof must
                // both match; full/explicit invalidation never retains this data.
                if (verifyCachedRevision) StartScopeVerification(contexts, verifyCachedRevision);
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
                Running job{key, sequence, dependency, cache.Begin(ticket), {}, GetTickCount64(), request.startPinOnly, contexts, {}, 0, 0};
                usePreparedQuery = priority != QueryPriority::Inspect && !request.startPinOnly;
                if (nativeFactory && usePreparedQuery)
                {
                    bool unverified = false;
                    { std::lock_guard lock(mutex);
                      unverified = catalogueStale || scanning || checkedRegistryRevision < registryRevision;
                      preparationRequested |= ObjectPreparationEnabled(); }
                    if (unverified) Session::ReleasePreparedMenuWorker();
                }
                try { job.work = factory(request); running.push_back(std::move(job)); }
                catch (...) { Complete(job, Reply{false, {}, "helper start failed"}, cache); }
            }
            if (!sourceJob && std::count_if(running.begin(), running.end(), [](const auto &job) {
                    return !job.startPinOnly;
                }) == 0)
                if (auto job = NextSource())
                {
                    job->started = GetTickCount64();
                    usePreparedQuery = false;
                    try { job->work = factory(job->request); sourceJob = std::move(job); }
                    catch (...) { std::lock_guard lock(mutex); failedSources.insert(job->source.id); MenuTrace("attribution", "start_failed"); }
                }
            bool preparing = false;
            if (nativeFactory)
            {
                bool prepare = false, enabled = false;
                {
                    std::lock_guard lock(mutex);
                    enabled = ObjectPreparationEnabled();
                    if (!enabled) preparationRequested = false;
                    else if (preparationRequested && running.empty() && !sourceJob &&
                        catalogue.revision && !scanning && !scanRequested && !catalogueStale &&
                        !typesPreparing && nextType >= typeDiscovery.size() &&
                        std::none_of(rows.begin(), rows.end(), [](const auto &pair) { return pair.second.queued; }))
                        prepare = std::exchange(preparationRequested, false);
                }
                if (!enabled) Session::ReleasePreparedMenuWorker();
                if (prepare)
                {
                    // One attempt per idle opportunity; a failed or expired
                    // bootstrap must not become a perpetual background restart.
                    try { Session::PrepareFirstMenuWorker(); } catch (...) {}
                }
                preparing = Session::PollPreparedMenuWorker() == Session::PreparationState::Pending;
            }
            std::vector<std::tuple<Key, Request, bool>> checks;
            {
                std::lock_guard lock(mutex);
                for (auto &[key, row] : rows) if (row.checkRequested &&
                    (!row.view.pending || (row.invalid && row.view.snapshot && VerificationIndex(row.view.contexts) > 0)))
                {
                    checks.emplace_back(key, row.request, row.checkInspection);
                    row.checkRequested = row.checkInspection = false;
                }
                Trim();
            }
            for (const auto &[key, request, inspection] : checks)
            {
                auto ticket = cache.Capture(request);
                unsigned verifyContexts = 0; std::uint64_t verifyRevision = 0;
                {
                    std::lock_guard lock(mutex);
                    const auto found = rows.find(key); if (found == rows.end()) continue;
                    auto &row = found->second;
                    ticket.dependency = Dependency(catalogue, request, row.view.contexts);
                    if (row.view.snapshot && row.identity != ticket.identity)
                    {
                        row.view.snapshot.reset(); row.bytes = 0; row.invalid = true; ++row.dependency; ++row.view.revision;
                        RebuildAvailable();
                        Queue(request, inspection ? QueryPriority::Inspect : QueryPriority::Menu, true);
                    }
                    RestoreRegistrySnapshot(row, ticket);
                    const int scope = VerificationIndex(row.view.contexts);
                    if (row.invalid && !row.force && row.view.snapshot && scope > 0)
                    {
                        row.snapshotTargetChecked = ticket && row.snapshotIdentity == ticket.identity;
                        if (row.snapshotTargetChecked && row.snapshotSignature && row.snapshotSignature == VerificationSignature(scope))
                        { verifyContexts = row.view.contexts; verifyRevision = registryRevision; }
                    }
                }
                // Later notifications must also be verified while the original
                // refresh is still pending, without restarting that query.
                if (verifyRevision) StartScopeVerification(verifyContexts, verifyRevision);
            }
            SaveObserved(cache);
            bool watchesComplete = false;
            auto handles = MenuRegistryWaitHandles(watchesComplete);
            handles.insert(handles.begin(), wake);
            DWORD wait = watchesComplete ? INFINITE : 1000;
            if (preparing || !running.empty() || sourceJob || scan.valid() ||
                std::any_of(scopedVerification.begin(), scopedVerification.end(), [](const auto &scope) { return scope.scan.valid(); })) wait = 20;
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
MenuService::MenuService(std::filesystem::path directory, QueryFactory factory, CatalogueReader reader, FolderVerifier verifier, BackgroundVerifier backgroundVerifier)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(factory), std::move(reader), std::move(verifier), std::move(backgroundVerifier))) {}
MenuService::~MenuService() = default;
void MenuService::Shutdown() { impl_->Stop(); }
MenuService &SharedMenuService() { static MenuService service; return service; }
MenuView MenuService::View(const Request &request)
{
    MenuTiming timing("first_screen");
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->rows.find(impl_->ViewKey(request));
    if (it == impl_->rows.end()) { timing.Record("memory_miss"); return {}; }
    it->second.used = ++impl_->clock;
    if (it->second.expires && MenuSnapshotCache::Now() >= it->second.expires) { it->second.view.snapshot.reset(); it->second.bytes = 0; }
    timing.Record(it->second.view.snapshot ? "memory_hit" : "memory_miss");
    return it->second.view;
}
void MenuService::Query(const Request &request, QueryPriority priority, bool force)
{
    std::lock_guard lock(impl_->mutex);
    impl_->RememberView(request, impl_->Queue(request, priority, force));
}
bool MenuService::MenuEnabled(const Request &request, const Preferences &fallback)
{
    std::lock_guard lock(impl_->mutex);
    return impl_->configured ? impl_->Enabled(request) : HasOptIns(fallback,
        request.context == Context::Desktop ? Context::Desktop : request.background ? Context::FolderBackground : Context::Automatic);
}
MenuView MenuService::MenuDisplay(const Request &request, const Preferences &fallback, bool retainPublished)
{
    MenuTiming timing("first_screen");
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->rows.find(impl_->PopupKey(request, retainPublished));
    if (it == impl_->rows.end()) { timing.Record("memory_miss"); return {}; }
    auto &row = it->second; row.used = ++impl_->clock;
    auto view = row.view;
    view.attributionPending = impl_->AttributionPending(request, fallback);
    const bool verifiedBackground = impl_->VerifiedBackgroundSnapshot(row);
    if (row.expires <= MenuSnapshotCache::Now() ||
        (row.invalid && !verifiedBackground && (!retainPublished || !view.error.empty()))) view.snapshot.reset();
    const bool fullPopupSuperset = impl_->PopupRequest(request).omitNvidiaCompatibility &&
        !row.request.omitNvidiaCompatibility;
    if (view.snapshot && (verifiedBackground || fullPopupSuperset))
    {
        // Only the display projection is ready. Service/management state keeps
        // supervising the fresh query, and execution always resolves new tokens.
        view.pending = false; view.error.clear();
    }
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
bool MenuService::MenuAttributionPending(const Request &request, const Preferences &fallback)
{
    std::lock_guard lock(impl_->mutex);
    return impl_->AttributionPending(request, fallback);
}
void MenuService::Prewarm(const Request &request)
{
    std::lock_guard lock(impl_->mutex);
    if (!impl_->AnyEnabled()) return;
    // Only the latest still-queued selection survives the stability window.
    for (auto &[key, row] : impl_->rows)
        if (row.queued && row.priority == QueryPriority::Prewarm && !row.inspection) row.queued = row.view.pending = false;
    auto target = request;
    if (impl_->OnlyStartShown(request)) target.startPinOnly = true;
    impl_->RememberView(target, impl_->Queue(target, QueryPriority::Prewarm, false, 150));
}
void MenuService::Configure(Preferences preferences)
{
    Normalize(preferences);
    {
        std::lock_guard lock(impl_->mutex);
        const bool enabled = impl_->AnyEnabled();
        const bool desktop = !impl_->shown[static_cast<int>(Context::Desktop)].empty();
        const bool objectPreparation = impl_->ObjectPreparationEnabled();
        impl_->preferences = std::move(preferences); impl_->configured = true;
        for (int i = 0; i < 4; ++i) impl_->shown[i] = EffectiveShownIds(impl_->preferences, static_cast<Context>(i));
        if (!objectPreparation && impl_->ObjectPreparationEnabled()) impl_->preparationRequested = true;
        if (!enabled && impl_->AnyEnabled()) { impl_->scanRequested = true; MenuTrace("catalogue", "request.configure"); }
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
    if (impl_->catalogueStale && !impl_->scanRequested)
    {
        impl_->scanRequested = true;
        MenuTrace("catalogue", "request.stale_inspect");
    }
    if (refresh || !impl_->inspected)
    {
        MenuTrace("catalogue", refresh ? "request.refresh" : "request.inspect");
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
    auto selection = request.context == Context::Desktop && request.paths.empty() && impl_->desktopInspection
        ? request : request.paths.empty() ? impl_->management : request;
    selection.omitNvidiaCompatibility = false;
    const auto selectionKey = SelectionKey(selection);
    const auto previous = impl_->rows.find(selectionKey);
    const bool invalidated = previous != impl_->rows.end() && previous->second.invalid;
    // Polling observes the current discovery, never starts it over after the
    // ordinary query freshness window expires (or after cache eviction).
    if (!selection.paths.empty() && (refresh || selectionKey != impl_->inspectedSelection || invalidated))
    {
        impl_->inspectedSelection = selectionKey;
        impl_->RememberView(selection, impl_->Queue(selection, QueryPriority::Inspect, refresh));
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
void MenuService::Execute(const Request &request, CommandReference reference, POINT point, std::function<void(bool)> completed, HWND owner)
{
    bool queued = false;
    {
        std::lock_guard lock(impl_->mutex);
        const auto key = impl_->Queue(request, QueryPriority::Execute, true);
        impl_->RememberView(request, key);
        if (!key.empty())
        {
            impl_->rows[key].clicks.push_back({std::move(reference), point, std::move(completed), ShellInvocationOwner::Capture(owner)});
            queued = true;
        }
    }
    if (!queued && completed) completed(false);
}
void MenuService::Invalidate(const Request &request)
{
    std::lock_guard lock(impl_->mutex);
    const auto base = SelectionBaseKey(request);
    for (auto &[key, row] : impl_->rows)
        if (SelectionBaseKey(row.request) == base)
        {
            row.invalid = true; ++row.dependency;
            row.view.snapshot.reset(); row.bytes = 0; ++row.view.revision;
        }
    auto target = impl_->PopupRequest(request);
    auto &row = impl_->rows[SelectionKey(target)]; row.request = target;
    row.invalid = true;
    impl_->RebuildAvailable();
    if (!row.view.pending) impl_->RememberView(request, impl_->Queue(request, QueryPriority::Menu, true));
    else impl_->RememberView(request, SelectionKey(target));
}
}
