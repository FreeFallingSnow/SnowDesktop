#include "shell_extension_service.h"
#include "shell_extension_diagnostics.h"
#include "shell_extension_discovery.h"
#include "shell_extension_attribution.h"
#include "shell_extension_demand.h"
#include "shell_menu_read_projection.h"
#include "shell_extension_nvidia_compat.h"
#include "shell_context_menu_invoke.h"
#include "shell_catalogue_workers.h"
#include "diagnostics/performance_trace.h"
#include "platform/foreground_fullscreen.h"
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
bool BackgroundCatalogueFullscreen()
{
    // A fullscreen workload on another monitor also needs protection. Reuse
    // the native visibility/client-area policy, excluding our own Shell UI.
    bool found = false;
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        if (!snowdesktop::fullscreen::ObserveWindow(window)) return TRUE;
        *reinterpret_cast<bool *>(context) = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}
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
    request.visibleMenuProviders.reset();
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
bool ShortcutSelection(const Request &request)
{
    return !request.background && std::any_of(request.paths.begin(), request.paths.end(), [](const auto &path) {
        return lstrcmpiW(PathFindExtensionW(path.c_str()), L".lnk") == 0;
    });
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
std::uint64_t MenuRevision(const Registration &row)
{
    return row.menuRevision ? row.menuRevision : row.revision;
}
std::uint64_t InventoryDependency(const Catalogue &catalogue)
{
    if (!catalogue.revision) return 0;
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto &row : catalogue.rows)
    {
        for (unsigned char c : row.id) { hash ^= c; hash *= 1099511628211ull; }
        hash ^= MenuRevision(row); hash *= 1099511628211ull;
    }
    return hash;
}
std::uint64_t Dependency(const Catalogue &catalogue, const Request &request, unsigned contexts)
{
    if (!catalogue.revision) return 0;
    // ShellLink can expose commands belonging to its target's type. The .lnk
    // suffix alone cannot prove those dependencies without resolving targets
    // (which may be offline), so shortcuts retain the full inventory proof.
    if (ShortcutSelection(request)) return InventoryDependency(catalogue);
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto &row : catalogue.rows)
        if (DependsOn(row, request, contexts))
        {
            for (unsigned char c : row.id) { hash ^= c; hash *= 1099511628211ull; }
            hash ^= MenuRevision(row); hash *= 1099511628211ull;
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
        std::uint64_t due = 0, used = 0, completed = 0, retryAt = 0, sequence = 0, dependency = 0, bytes = 0, snapshotRevision = 0;
        unsigned failures = 0;
        Key identity, snapshotIdentity;
        std::uint64_t snapshotSignature = 0, snapshotDependency = 0;
        bool snapshotTargetChecked = false, registryInvalid = false, snapshotManagementProof = false;
        std::optional<MenuSnapshotCache::Ticket> provisionalTicket;
        std::uint64_t provisionalRegistryRevision = 0;
        std::uint64_t expires = 0; bool checkRequested = false, checkInspection = false;
        // A notification can request target validation without authorizing a
        // foreground root query. Only an explicit Queue supplies this intent.
        std::optional<QueryPriority> targetCheckDemand;
        bool queued = false, force = false, invalid = false, inspection = false;
        // A finite management request, independent of ordinary query priority
        // and of whether settings happened to be opened earlier in this process.
        bool managementAttributionRequested = false;
        bool fullMenuRequested = false;
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
        bool managementOnly = false;
        std::uint64_t managementGeneration = 0;
    };
    struct SourceJob
    {
        Key key;
        Request request, selection;
        Registration source;
        Reply actual;
        std::vector<Key> attempts;
        QueryWork work;
        Key identity;
        std::uint64_t dependency = 0, sequence = 0, snapshotRevision = 0, configuration = 0, started = 0;
        std::optional<Reply> reply;
        ULONGLONG replyReadyAt = 0;
        std::uint64_t managementGeneration = 0;
    };
    std::unique_ptr<SourceJob> sourceJob;
    std::set<Key> sourceAttempts, sourceSelections;
    std::set<std::string> failedSources;
    bool sourcePending = false;
    bool managementActive = false;
    std::uint64_t managementGeneration = 1, workGeneration = 1, scanWorkGeneration = 0, scanManagementGeneration = 0;
    std::mutex mutex;
    std::map<Key, Row> rows;
    std::map<Key, Key> requestedViews;
    Catalogue catalogue;
    Catalogue sourceCatalogue;
    std::future<Catalogue> sourceScan;
    SourceCatalogueReader readSources;
    bool sourceScanRequested = false, sourceScanning = false;
    std::uint64_t checkedSourceRegistryRevision = 0, checkedSourceConfiguration = 0, sourceScanRegistryRevision = 0,
        sourceScanConfiguration = 0, sourceScanWorkGeneration = 0, sourceScanManagementGeneration = 0;
    std::vector<Registration> sourceScanSeeds;
    CatalogueForegroundDemand sourceScanDemand{false, BackgroundCatalogueFullscreen};
    ULONGLONG sourceScanStarted = 0;
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
    std::array<std::vector<std::string>, 2> readProjection;
    std::array<std::set<std::string>, 4> shown;
    Request management;
    Key inspectedSelection;
    bool stop = false, scanRequested = false, scanning = false, configured = false, startupWarm = false, inspected = false, desktopInspection = false, catalogueDirty = false, catalogueStale = false;
    bool foregroundScanRequested = false;
    std::uint64_t configurationRevision = 0;
    CatalogueForegroundDemand foregroundScanDemand{false, [wasPaused = false]() mutable {
        const bool paused = BackgroundCatalogueFullscreen();
        if (paused != wasPaused) MenuTrace("catalogue", paused ? "paused.fullscreen" : "resumed.fullscreen");
        wasPaused = paused;
        return paused;
    }};
    std::uint64_t clock = 0;
    std::uint64_t registryRevision = 0, checkedRegistryRevision = 0, failedRegistryRevision = 0, scanRegistryRevision = 0,
        checkedManagementGeneration = 0;
    struct ScopeVerification
    {
        std::future<std::uint64_t> scan;
        std::uint64_t checked = 0, signature = 0, attempt = 0, scanRevision = 0, workGeneration = 0;
        CatalogueForegroundDemand demand{false, BackgroundCatalogueFullscreen};
    };
    std::array<ScopeVerification, 3> scopedVerification;
    ULONGLONG scanStarted = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;
    std::filesystem::path directory;
    QueryFactory factory;
    bool nativeFactory = false;
    ULONGLONG nextPreparation = 0;
    bool prepareFileDemand = false;
    CatalogueReader readCatalogue;
    FolderVerifier verifyFolder;
    BackgroundVerifier verifyBackground;
    std::vector<Running> running;
    std::future<Catalogue> scan;
    explicit Impl(std::filesystem::path path, QueryFactory f, CatalogueReader r, FolderVerifier v, BackgroundVerifier b, SourceCatalogueReader sr)
        : readSources(std::move(sr)), directory(std::move(path)), factory(std::move(f)), readCatalogue(std::move(r)), verifyFolder(std::move(v)), verifyBackground(std::move(b))
    {
        if (!factory) { nativeFactory = true; factory = [this](const Request &request) {
            auto session = std::make_shared<Session>(request, request.startPinOnly ? 2000 : 8000, true);
            nextPreparation = GetTickCount64() + 1000;
            if (request.visibleMenuProviders && !request.background && request.sourceClsid.empty() &&
                !request.startPinOnly && !request.originalShortcutOnly && !ShortcutSelection(request) && Local(request))
                prepareFileDemand = true;
            return QueryWork{[session] { return session->Poll(); }, [session](UINT token, POINT p) { session->Invoke(token, p); },
                [session](UINT token, POINT p, HWND owner) { session->Invoke(token, p, owner); },
                [session] { return session->SelectionContexts(); }};
        }; }
        if (!readCatalogue)
        {
            readCatalogue = [this] {
                auto value = ReadCatalogueWithForegroundDemand(HKEY_CLASSES_ROOT, true, foregroundScanDemand);
                const auto waited = foregroundScanDemand.BudgetWaitMilliseconds();
                snowdesktop::performance::Value("shell.catalogue", "background_budget_wait_ms", {}, static_cast<double>(waited));
                MenuTrace("catalogue", "budget_wait", static_cast<double>(waited));
                return value;
            };
            if (!verifyFolder) verifyFolder = [this] {
                return ReadFolderCatalogueRevision(HKEY_CLASSES_ROOT, true, &scopedVerification[0].demand);
            };
            if (!verifyBackground) verifyBackground = [this](Context context) {
                const auto index = context == Context::Desktop ? 2 : 1;
                return ReadBackgroundCatalogueRevision(context, HKEY_CLASSES_ROOT, true, &scopedVerification[index].demand);
            };
        }
        if (!readSources) readSources = [this](const auto &sources) { return ReadCatalogueSources(sources, &sourceScanDemand); };
        worker = std::thread([this] { Run(); });
    }
    QueryWork DispatchQuery(Request request)
    {
        if (request.visibleMenuProviders)
        {
            const auto contexts = Contexts(request);
            std::lock_guard lock(mutex);
            request.visibleMenuProviders = ResolveMenuReadProjection(*request.visibleMenuProviders, catalogue, contexts);
        }
        // Factory adapters may pass the request directly into a real Session.
        // Internal cache-policy tags must never cross that query boundary.
        return factory(request);
    }
    ~Impl() { Stop(); if (wake) CloseHandle(wake); }
    // Called with mutex held. A menu can promote an already running refresh;
    // only an outstanding later scan retains demand for its own launch.
    void RequestForegroundScan()
    {
        if (managementActive)
        {
            if (scanning) foregroundScanDemand.Promote();
            if (scanRequested || catalogueStale || (!scanning && !catalogue.revision)) foregroundScanRequested = true;
        }
        else
        {
            if (sourceScanning) sourceScanDemand.Promote();
            if (sourceScanRequested) foregroundScanRequested = true;
        }
    }
    void Stop()
    {
        { std::lock_guard lock(mutex); stop = true; }
        foregroundScanDemand.Cancel();
        sourceScanDemand.Cancel();
        for (auto &scope : scopedVerification) scope.demand.Cancel();
        SetEvent(wake);
        if (worker.joinable()) worker.join();
        // The registry scan runs separately from the scheduler. Drain it before
        // returning to host teardown, while their static metadata caches exist.
        if (scan.valid()) scan.wait();
        if (sourceScan.valid()) sourceScan.wait();
        for (auto &scope : scopedVerification) if (scope.scan.valid()) scope.scan.wait();
    }
    ULONGLONG ActiveScanElapsed() const
    {
        const auto elapsed = GetTickCount64() - scanStarted;
        // Neither fullscreen suspension nor intentional soft-budget waits
        // represent a stalled registry/extension operation.
        const auto paused = foregroundScanDemand.PausedMilliseconds() + foregroundScanDemand.BudgetWaitMilliseconds();
        return elapsed > paused ? elapsed - paused : 0;
    }
    // Called while mutex holds the same row and catalogue used for display.
    bool AttributionPending(const Request &request, const Preferences &fallback) const
    {
        if (request.startPinOnly || catalogue.revision || (!scanning && !scanRequested) ||
            // Inventory attribution enumerates the whole machine and can outlast a
            // single helper. Known rows remain visible throughout this bounded wait.
            (scanning && ActiveScanElapsed() >= kInventoryWaitMs)) return false;
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
                if (!request.sourceClsid.empty())
                {
                    const auto *source = DemandSource(catalogue, available, id, 1u << i);
                    if (!source) continue;
                    const std::wstring clsid(source->verbs.front().begin(), source->verbs.front().end());
                    if (lstrcmpiW(clsid.c_str(), request.sourceClsid.c_str()) != 0) continue;
                }
                bool allowed = true;
                for (int j = 0; contexts && j < 4; ++j)
                    if ((contexts & (1u << j)) && !shown[j].contains(id)) allowed = false;
                if (!allowed) continue;
                const auto registration = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == id; });
                // Unknown dynamic providers remain eligible; never infer their
                // applicability from a caption or an incomplete observation.
                const bool sourceChecked = sourceCatalogue.revision && checkedSourceRegistryRevision == registryRevision &&
                    checkedSourceConfiguration == configurationRevision && !sourceScanning && !sourceScanRequested;
                const bool registrationProven = sourceChecked && std::any_of(sourceCatalogue.rows.begin(), sourceCatalogue.rows.end(),
                    [&](const auto &row) { return row.id == id; });
                if (!registrationProven || registration == catalogue.rows.end() ||
                    (registration->systemEnabled && DependsOn(*registration, request, candidates))) return true;
            }
        return false;
    }
    bool AnyEnabled() const
    {
        return std::any_of(shown.begin(), shown.end(), [](const auto &ids) { return !ids.empty(); });
    }
    bool ManagementAttributionPending() const
    {
        return std::any_of(rows.begin(), rows.end(), [](const auto &item) {
            return item.second.managementAttributionRequested;
        });
    }
    bool AcceptedExecutionPending() const
    {
        return std::any_of(rows.begin(), rows.end(), [](const auto &item) { return !item.second.clicks.empty(); });
    }
    bool BackgroundWorkAllowed() const
    {
        return !configured || AnyEnabled() || managementActive || AcceptedExecutionPending();
    }
    std::optional<Registration> ConfiguredSource(const std::string &id, unsigned contexts) const
    {
        const auto direct = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &row) {
            return row.id == id && row.kind != RegistrationKind::Observed && (row.contexts & contexts);
        });
        if (direct != catalogue.rows.end()) return *direct;
        if (id.starts_with("reg:"))
        {
            Registration row; row.id = id; row.kind = RegistrationKind::Verb; row.contexts = contexts;
            return row; // The exact reader decodes this registration path, without a type enumeration.
        }
        if (const auto *source = DemandSource(catalogue, available, id, contexts)) return *source;
        std::set<std::string> owners;
        for (const auto &association : catalogue.associations)
            if (association.provider == id && (ContextBit(association.context) & contexts)) owners.insert(association.registration);
        if (owners.empty() && id.starts_with("verb:"))
            for (const auto &row : catalogue.rows)
                if ((row.contexts & contexts) && row.kind != RegistrationKind::Observed &&
                    std::find(row.verbs.begin(), row.verbs.end(), id.substr(5)) != row.verbs.end()) owners.insert(row.id);
        if (owners.size() != 1) return {};
        const auto found = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &row) { return row.id == *owners.begin(); });
        return found == catalogue.rows.end() ? std::optional<Registration>{} : *found;
    }
    std::vector<Registration> ConfiguredSources() const
    {
        std::map<std::string, Registration> selected;
        for (int i = 0; i < 4; ++i)
            for (const auto &id : shown[i])
                if (auto source = ConfiguredSource(id, 1u << i);
                    source && (source->kind == RegistrationKind::Verb || source->kind == RegistrationKind::Handler))
                {
                    const auto [found, inserted] = selected.emplace(source->id, *source);
                    if (!inserted) found->second.contexts |= source->contexts;
                }
        std::vector<Registration> result;
        for (auto &[id, source] : selected) result.push_back(std::move(source));
        return result;
    }
    bool KnownSelectionSources(unsigned contexts) const
    {
        bool wanted = false;
        for (int i = 0; i < 4; ++i) if (contexts & (1u << i))
            for (const auto &id : shown[i])
            {
                wanted = true;
                const auto source = ConfiguredSource(id, 1u << i);
                if (!source || (source->kind != RegistrationKind::Verb && source->kind != RegistrationKind::Handler)) return false;
            }
        return wanted;
    }
    bool RuntimeProof(const Request &request, unsigned contexts) const
    {
        if ((ShortcutSelection(request) && request.sourceClsid.empty()) || !contexts || !sourceCatalogue.revision || checkedSourceRegistryRevision != registryRevision ||
            checkedSourceConfiguration != configurationRevision || sourceScanning || sourceScanRequested) return false;
        const auto proven = [&](const Registration &source) {
            return std::any_of(sourceCatalogue.rows.begin(), sourceCatalogue.rows.end(), [&](const auto &row) {
                return row.id == source.id && row.systemEnabled && (row.contexts & contexts) == contexts;
            });
        };
        if (!request.sourceClsid.empty())
        {
            for (const auto &row : catalogue.rows)
                if (row.kind == RegistrationKind::Handler && std::any_of(row.verbs.begin(), row.verbs.end(), [&](const auto &verb) {
                    const std::wstring clsid(verb.begin(), verb.end());
                    return lstrcmpiW(clsid.c_str(), request.sourceClsid.c_str()) == 0;
                })) return proven(row);
            return false;
        }
        bool wanted = false;
        for (int i = 0; i < 4; ++i) if (contexts & (1u << i))
            for (const auto &id : shown[i])
            {
                wanted = true;
                const auto source = ConfiguredSource(id, 1u << i);
                if (!source || !proven(*source)) return false;
            }
        return wanted;
    }
    void RefreshConfiguredSources(MenuSnapshotCache &cache)
    {
        std::vector<Request> affected;
        if (sourceScan.valid() && sourceScan.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            auto value = sourceScan.get();
            std::lock_guard lock(mutex); sourceScanning = false;
            if (sourceScanWorkGeneration == workGeneration && sourceScanManagementGeneration == managementGeneration &&
                sourceScanConfiguration == configurationRevision &&
                sourceScanRegistryRevision == registryRevision && AnyEnabled() && !managementActive)
            {
                sourceCatalogue = std::move(value);
                checkedSourceRegistryRevision = sourceScanRegistryRevision;
                checkedSourceConfiguration = sourceScanConfiguration;
                std::vector<Registration> changed;
                for (const auto &seed : sourceScanSeeds)
                {
                    std::erase_if(catalogue.rows, [&](const auto &row) { return row.id == seed.id; });
                    const auto current = std::find_if(sourceCatalogue.rows.begin(), sourceCatalogue.rows.end(), [&](const auto &row) { return row.id == seed.id; });
                    if (current != sourceCatalogue.rows.end())
                    {
                        catalogue.rows.push_back(*current);
                        if (MenuRevision(seed) && (MenuRevision(seed) != MenuRevision(*current) || seed.systemEnabled != current->systemEnabled))
                            changed.push_back(seed);
                    }
                    else
                    {
                        if (MenuRevision(seed)) changed.push_back(seed);
                        // Missing, unsupported and failed exact reads all mean
                        // unproven. Only a returned row can prove disabled policy.
                        catalogue.rows.push_back(seed);
                    }
                }
                for (auto &[key, row] : rows)
                    if (std::any_of(changed.begin(), changed.end(), [&](const auto &source) {
                        return ShortcutSelection(row.request) || DependsOn(source, row.request,
                            row.view.contexts ? row.view.contexts : (row.request.background ? 12u : 3u));
                    }))
                    {
                        row.invalid = true; ++row.dependency; row.view.snapshot.reset(); row.bytes = 0; ++row.view.revision;
                        affected.push_back(row.request);
                    }
                catalogueDirty = true;
                for (auto &[key, row] : rows)
                    if (row.registryInvalid && row.view.snapshot && !row.view.pending) row.checkRequested = true;
                RebuildAvailable();
                MenuTrace("catalogue.sources", "completed", 0, static_cast<unsigned>(sourceCatalogue.rows.size()));
            }
        }
        for (const auto &request : affected) cache.Erase(request);
        std::vector<Registration> seeds;
        {
            std::lock_guard lock(mutex);
            if (!AnyEnabled() || managementActive) sourceScanRequested = false;
            if (managementActive || sourceScan.valid() || !sourceScanRequested) return;
            sourceScanRequested = false; seeds = ConfiguredSources();
            sourceScanConfiguration = configurationRevision; sourceScanRegistryRevision = registryRevision;
            sourceScanWorkGeneration = workGeneration; sourceScanManagementGeneration = managementGeneration;
            sourceScanSeeds = seeds;
            if (seeds.empty())
            {
                sourceCatalogue = {}; checkedSourceRegistryRevision = registryRevision;
                checkedSourceConfiguration = configurationRevision; foregroundScanRequested = false; return;
            }
            sourceScanning = true; sourceScanStarted = GetTickCount64();
            sourceScanDemand.Reset(std::exchange(foregroundScanRequested, false));
        }
        sourceScan = std::async(std::launch::async, [read = readSources, seeds = std::move(seeds)] {
            const HRESULT ole = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Catalogue result;
            try { result = read(seeds); } catch (...) { MenuTrace("catalogue.sources", "failure"); }
            if (SUCCEEDED(ole)) CoUninitialize();
            return result;
        });
    }
    void ReapClosedManagement()
    {
        // Only the worker owns sessions/futures. EndInspection changes demand
        // in memory and wakes this cancellation boundary; the UI never waits.
        std::vector<size_t> retired;
        bool retireSource = false, releasePrepared = false;
        {
            std::lock_guard lock(mutex);
            const bool allowed = BackgroundWorkAllowed();
            if (sourceJob && (!managementActive || sourceJob->managementGeneration != managementGeneration))
            {
                for (const auto &attempt : sourceJob->attempts) sourceAttempts.erase(attempt);
                retireSource = true;
            }
            for (size_t i = 0; i < running.size(); ++i)
            {
                const auto &job = running[i];
                const auto found = rows.find(job.key);
                const bool same = found != rows.end() && found->second.sequence == job.sequence;
                const bool execute = same && !found->second.clicks.empty();
                const bool ordinary = AnyEnabled() && same && found->second.priority != QueryPriority::Inspect;
                const bool closed = job.managementOnly && job.managementGeneration != managementGeneration;
                if (!execute && ((!allowed) || (closed && !ordinary)))
                {
                    if (found != rows.end() && found->second.sequence == job.sequence)
                        found->second.queued = found->second.view.pending = found->second.inspection = false;
                    retired.push_back(i);
                }
            }
            if (!allowed)
            {
                prepareFileDemand = false;
                scanRequested = foregroundScanRequested = startupWarm = false;
                for (auto &[key, row] : rows)
                    if (row.clicks.empty())
                    {
                        row.queued = row.view.pending = row.checkRequested = row.checkInspection = false;
                        row.targetCheckDemand.reset();
                    }
                releasePrepared = true;
            }
        }
        if (retireSource) sourceJob.reset();
        for (auto it = retired.rbegin(); it != retired.rend(); ++it) running.erase(running.begin() + *it);
        if (releasePrepared) Session::ReleasePreparedMenuWorker();
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
        if (configured && request.sourceClsid.empty() && !request.startPinOnly)
            request.visibleMenuProviders = readProjection[static_cast<int>(
                request.background || request.context == Context::Desktop ? Category::Background : Category::Objects)];
        request.omitNvidiaCompatibility = configured && request.background &&
            request.context == Context::Desktop && request.sourceClsid.empty() &&
            !NvidiaCompatibilityShown(shown[static_cast<int>(Context::Desktop)]);
        return request;
    }
    std::uint64_t SnapshotDependency(const Request &request, unsigned contexts, bool managementProof) const
    {
        if (managementProof) return managementActive && checkedManagementGeneration == managementGeneration &&
            (checkedRegistryRevision >= registryRevision || ScopeChecked(contexts, registryRevision)) ?
                Dependency(catalogue, request, contexts) : 0;
        return RuntimeProof(request, contexts) ? Dependency(sourceCatalogue, request, contexts) : 0;
    }
    bool VerifiedBackgroundSnapshot(const Row &row) const
    {
        if (!managementActive || !row.snapshotManagementProof) return false;
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
            (row.snapshotManagementProof ? (!managementActive || checkedRegistryRevision < registryRevision) :
                !RuntimeProof(row.request, row.view.contexts)) || !ticket ||
            row.snapshotIdentity != ticket.identity || !row.snapshotDependency ||
            row.snapshotDependency != ticket.dependency) return false;
        row.invalid = row.registryInvalid = false;
        row.checkRequested = row.checkInspection = false; row.targetCheckDemand.reset();
        row.snapshotTargetChecked = true;
        if (row.queued) row.queued = row.view.pending = row.inspection = false;
        ++row.view.revision;
        sourcePending |= row.managementAttributionRequested;
        SetEvent(wake);
        MenuTrace("schedule", "registry_snapshot_reused");
        return true;
    }
    Key PopupKey(const Request &request, bool retainPublished = false) const
    {
        auto popup = PopupRequest(request);
        if (popup.omitNvidiaCompatibility || popup.visibleMenuProviders)
        {
            auto full = popup;
            full.visibleMenuProviders.reset();
            // Fresh execution can have complete ordinary details while still
            // omitting optional compatibility work. Prefer that healthy row
            // before consulting the independent management query.
            for (int i = 0; i < (popup.omitNvidiaCompatibility ? 2 : 1); ++i)
            {
                if (i) full.omitNvidiaCompatibility = false;
                const auto found = rows.find(SelectionKey(full));
                if (found != rows.end())
                {
                    const auto &row = found->second;
                    const bool verifiedBackground = VerifiedBackgroundSnapshot(row);
                    if (row.view.snapshot && (row.view.error.empty() || verifiedBackground) &&
                        row.expires > MenuSnapshotCache::Now() &&
                        (!row.invalid || verifiedBackground ||
                            (retainPublished && row.view.error.empty()))) return found->first;
                }
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
        const auto direct = SelectionKey(request);
        if (rows.contains(direct)) return direct;
        return PopupKey(request);
    }
    Key Queue(const Request &request, QueryPriority priority, bool force, unsigned delay = 0)
    {
        if (request.paths.empty() || request.paths.size() > 256) return {};
        // Opening/refreshing a Start-only popup is served by its dedicated
        // request. Inspection and explicit execution still retain full queries.
        if (priority == QueryPriority::Menu && !request.startPinOnly && OnlyStartShown(request)) return {};
        if ((priority == QueryPriority::Menu || priority == QueryPriority::Prewarm || priority == QueryPriority::Execute) &&
            !Enabled(request)) return {};
        if (catalogueStale)
        {
            if (priority == QueryPriority::Inspect || managementActive) scanRequested = true;
            else if (AnyEnabled() && !sourceScanning &&
                (checkedSourceRegistryRevision != registryRevision || checkedSourceConfiguration != configurationRevision))
                sourceScanRequested = true;
        }
        auto target = priority == QueryPriority::Inspect ? request : PopupRequest(request);
        if (priority == QueryPriority::Inspect || priority == QueryPriority::Execute) target.visibleMenuProviders.reset();
        target.catalogueOnly = false;
        if (priority == QueryPriority::Inspect) target.omitNvidiaCompatibility = false;
        // Reuse an already complete full snapshot, but never make a fresh
        // popup/click inherit the optional resolver of an in-flight inspection.
        if ((target.omitNvidiaCompatibility || target.visibleMenuProviders) && !force && priority != QueryPriority::Execute)
        {
            auto full = target;
            full.visibleMenuProviders.reset();
            for (int i = 0; i < (target.omitNvidiaCompatibility ? 2 : 1); ++i)
            {
                if (i) full.omitNvidiaCompatibility = false;
                const auto old = rows.find(SelectionKey(full));
                if (old != rows.end() && old->second.view.snapshot && !old->second.invalid &&
                    old->second.view.error.empty() && old->second.expires > MenuSnapshotCache::Now())
                {
                    target = std::move(full);
                    break;
                }
            }
        }
        const auto key = SelectionKey(target);
        auto &row = rows[key];
        const bool inspection = row.inspection || priority == QueryPriority::Inspect;
        row.request = std::move(target); row.used = ++clock;
        if (priority == QueryPriority::Inspect)
        {
            const bool opening = !managementActive;
            if (opening)
            {
                sourceScanRequested = false;
                sourceScanDemand.Cancel();
            }
            managementActive = true;
            if (!row.request.startPinOnly && row.request.sourceClsid.empty())
            {
                scanRequested |= opening;
                row.managementAttributionRequested = true;
                sourcePending = true;
            }
        }
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
        row.targetCheckDemand = row.targetCheckDemand ? std::min(*row.targetCheckDemand, priority) : priority;
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
            sourceSelections.erase(victim->first);
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
            // Independent metadata must not teach the inventory that a command
            // exists in the real aggregate; routing evidence comes from inspection.
            if (!row.request.sourceClsid.empty()) continue;
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
        row.provisionalTicket.reset();
        row.snapshotDependency = SnapshotDependency(row.request, contexts, row.snapshotManagementProof);
        const int scope = VerificationIndex(contexts);
        row.snapshotSignature = scope > 0 ? VerificationSignature(scope) : 0;
        row.snapshotTargetChecked = !row.snapshotIdentity.empty();
        row.expires = written + MenuSnapshotCache::LifetimeMs;
        row.view.snapshot = std::move(reply); row.view.contexts = contexts; ++row.view.revision;
        ++row.snapshotRevision;
        row.used = ++clock;
        RebuildAvailable();
        sourcePending |= row.managementAttributionRequested;
    }
    std::unique_ptr<SourceJob> NextSource()
    {
        // Called only by the worker after ordinary menu/execute work has been
        // dispatched. One metadata probe leaves the other process slot free.
        std::lock_guard lock(mutex);
        if (!sourcePending || catalogueStale || scanning || scanRequested || !catalogue.revision) return {};
        for (auto &[key, row] : rows)
        {
            if (!row.managementAttributionRequested) continue;
            if (row.view.pending || row.queued) continue;
            const auto finish = [&] {
                row.managementAttributionRequested = false;
                sourceSelections.erase(key);
            };
            if (!row.view.snapshot || row.snapshotIdentity.empty() || !row.view.error.empty() ||
                row.expires <= MenuSnapshotCache::Now() || !Local(row.request))
            {
                finish();
                continue;
            }
            if (row.invalid) continue; // A requested live verification still owns the snapshot.
            const auto managementProof = SnapshotDependency(row.request, row.view.contexts, true);
            if (!managementProof) { finish(); continue; }
            const auto visible = VisibleSnapshot(preferences, *row.view.snapshot, row.view.contexts);
            const auto routable = [](const auto &entry) {
                return !entry.separator && !entry.provider.empty() && entry.registration.empty() &&
                    !StatePairForVerb(entry.key) && (!entry.key.empty() || !entry.pixels.empty());
            };
            const bool missingApplication = std::any_of(row.view.snapshot->entries.begin(), row.view.snapshot->entries.end(), [&](const auto &entry) {
                const auto found = observed.find(entry.registration.empty() ? entry.provider : entry.registration);
                return found != observed.end() && found->second.application.id.empty();
            });
            const bool missingRoute = std::any_of(visible.begin(), visible.end(), [&](const auto &entry) {
                return routable(entry) && !DemandSource(catalogue, available, entry.provider, row.view.contexts);
            });
            if (missingApplication || missingRoute) sourceSelections.insert(key);
            if (!sourceSelections.contains(key)) { finish(); continue; }
            for (const auto &source : catalogue.rows)
            {
                if ((source.kind != RegistrationKind::Handler && source.kind != RegistrationKind::Packaged) || !source.systemEnabled ||
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
                job->selection = row.request;
                job->attempts = std::move(attempts);
                // Attribution is metadata-only, independent of the original-file
                // executable recovery lane that produced these observed rows.
                job->request.originalShortcutOnly = false;
                job->request.sourceClsid.assign(source.verbs.front().begin(), source.verbs.front().end());
                job->request.sourceKey = std::move(typeKey);
                job->identity = row.snapshotIdentity;
                // This row still has explicit management attribution demand.
                // Its latest snapshot may come from Execute and deliberately
                // carry no ordinary disk proof. Keep this separate full proof
                // on the metadata job, without promoting that ordinary snapshot.
                job->dependency = managementProof;
                job->sequence = row.sequence; job->snapshotRevision = row.snapshotRevision;
                job->configuration = configurationRevision;
                job->managementGeneration = managementGeneration;
                return job;
            }
            finish();
        }
        sourcePending = false;
        return {};
    }
    bool CompleteSource(SourceJob &job, const Reply &reply, MenuSnapshotCache &cache)
    {
        // Target I/O remains on the service worker, outside the publication lock.
        const auto target = cache.Capture(job.selection);
        std::lock_guard lock(mutex);
        if (!managementActive || job.managementGeneration != managementGeneration)
        {
            MenuTrace("attribution", "management_closed");
            return true;
        }
        if (!reply.ok && !reply.error.empty()) failedSources.insert(job.source.id);
        const auto current = rows.find(job.key);
        const auto discard = [&] {
            for (const auto &attempt : job.attempts) sourceAttempts.erase(attempt);
            sourcePending |= current != rows.end() && current->second.managementAttributionRequested;
            MenuTrace("attribution", "proof_discarded");
            return true;
        };
        if (!reply.ok) { MenuTrace("attribution", "failed"); return true; }
        if (current != rows.end() && (!target || target.identity != job.identity) &&
            current->second.snapshotIdentity == job.identity)
        {
            current->second.checkRequested = true;
            current->second.invalid = true; current->second.registryInvalid = false;
        }
        if (!target || target.identity != job.identity || current == rows.end() ||
            current->second.sequence != job.sequence || current->second.snapshotRevision != job.snapshotRevision ||
            current->second.expires <= MenuSnapshotCache::Now() || !current->second.view.snapshot ||
            configurationRevision != job.configuration) return discard();
        const auto &row = current->second;
        // A notification changes display state, not the captured menu content.
        // Hold a successful reply until live proof settles instead of querying
        // its extension again for each notification / snapshot restoration.
        if ((catalogueStale || checkedRegistryRevision < registryRevision) &&
            !ScopeChecked(row.view.contexts, registryRevision))
        {
            if (failedRegistryRevision >= registryRevision ||
                GetTickCount64() - job.replyReadyAt >= kInventoryWaitMs) return discard();
            scanRequested |= catalogueStale;
            return false;
        }
        if ((row.invalid && !row.registryInvalid) || !job.dependency ||
            SnapshotDependency(row.request, row.view.contexts, true) != job.dependency ||
            std::none_of(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &source) {
                return source.id == job.source.id && source.systemEnabled && MenuRevision(source) == MenuRevision(job.source);
            })) return discard();
        for (const auto &provider : MatchSourceCommands(job.actual, reply))
            if (const auto found = observed.find(provider); found != observed.end() && found->second.kind == RegistrationKind::Observed)
            {
                RecordSourceApplication(found->second, job.source, catalogue);
                const auto proof = L"routing:" + std::to_wstring(current->second.view.contexts) + L":" +
                    std::wstring(job.source.id.begin(), job.source.id.end());
                if (std::find(found->second.sources.begin(), found->second.sources.end(), proof) == found->second.sources.end())
                    found->second.sources.push_back(proof);
            }
        RebuildAvailable();
        MenuTrace("attribution", "completed", double(GetTickCount64() - job.started),
            static_cast<unsigned>(reply.entries.size()));
        return true;
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
            const bool sameSequence = row.sequence == job.sequence;
            if (job.managementOnly && job.managementGeneration != managementGeneration &&
                !(sameSequence && (!row.clicks.empty() || (AnyEnabled() && row.priority != QueryPriority::Inspect))))
            {
                MenuTrace("query", "management_closed");
                return;
            }
            current = targetCurrent && row.sequence == job.sequence && row.dependency == job.dependency;
            if (sameSequence) clicks = std::move(row.clicks);
            if (!current && sameSequence)
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
                    if (request.sourceClsid.empty()) Associate(catalogue, resolved, reply);
                    catalogueDirty |= associations != catalogue.associations.size();
                    row.failures = 0; row.retryAt = 0; row.completed = GetTickCount64();
                    row.invalid = row.registryInvalid = false;
                    row.force = false;
                    // Completion and display publication are one observable state.
                    // Settings must not stop polling before the snapshot appears.
                    row.snapshotManagementProof = job.managementOnly && managementActive &&
                        job.managementGeneration == managementGeneration;
                    Publish(job.key, reply, contexts);
                    job.ticket.dependency = row.snapshotDependency;
                    if (row.snapshotManagementProof && job.ticket && !job.ticket.dependency)
                    {
                        // Known verbs can appear before initial attribution.
                        // Retain this exact issued ticket until the initial live
                        // inventory proves the same registry generation.
                        row.provisionalTicket = job.ticket;
                        row.provisionalRegistryRevision = job.verificationRevision;
                    }
                }
                else
                {
                    row.managementAttributionRequested = false;
                    sourceSelections.erase(job.key);
                    row.failures = std::min(row.failures + 1, 5u);
                    row.retryAt = GetTickCount64() + std::min(30000u, 1000u << row.failures);
                    MenuTrace("schedule", "backoff", double(std::min(30000u, 1000u << row.failures)));
                }
            }
        }
        if (current && reply.ok && job.ticket.dependency)
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
                          ticket.dependency = SnapshotDependency(saved.request, saved.contexts, false); }
                        // Replace the disk row without extending the lifetime
                        // of its ordinary commands or persisting the old pin.
                        if (!ticket.dependency || !cache.Store(cache.Begin(std::move(ticket)), saved.reply, saved.written)) cache.Erase(saved.request);
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
            std::vector<std::pair<std::string, std::uint64_t>> menuProof;
            for (const auto &row : value.rows) menuProof.emplace_back(row.id, row.menuRevision);
            const auto bytes = settings_ipc::Pack(std::uint32_t(5), value, menuProof);
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
            std::uint32_t schema = 0; Catalogue value;
            settings_ipc::Reader reader(bytes); reader(schema, value);
            if (schema == 5)
            {
                std::vector<std::pair<std::string, std::uint64_t>> menuProof;
                reader(menuProof);
                if (menuProof.size() != value.rows.size()) return;
                for (size_t i = 0; i < menuProof.size(); ++i)
                {
                    if (menuProof[i].first != value.rows[i].id) return;
                    value.rows[i].menuRevision = menuProof[i].second;
                }
            }
            else if (schema != 4) return;
            reader.Finish();
            std::lock_guard lock(mutex); catalogue = std::move(value);
        }
        catch (...) {}
    }
    void RefreshCatalogue(MenuSnapshotCache &cache)
    {
        if (scan.valid() && scan.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            auto value = scan.get(); std::vector<Request> affected; bool retirePreparation = false;
            {
                std::lock_guard lock(mutex);
                if (scanWorkGeneration != workGeneration || !managementActive || scanManagementGeneration != managementGeneration)
                {
                    scanning = false;
                    MenuTrace("catalogue", "closed_generation_discarded");
                    return;
                }
            }
            if (!value.revision)
            {
                std::lock_guard lock(mutex); scanning = false; typesRequested = false; catalogueStale = true;
                failedRegistryRevision = std::max(failedRegistryRevision, scanRegistryRevision);
                return; // An unsuccessful scan cannot masquerade as an empty registry.
            }
            {
                std::lock_guard lock(mutex);
                checkedRegistryRevision = std::max(checkedRegistryRevision, scanRegistryRevision);
                checkedManagementGeneration = managementGeneration;
                const bool initial = !catalogue.revision;
                const bool commandInventoryChanged = initial ||
                    InventoryDependency(value) != InventoryDependency(catalogue);
                if (!initial && value.revision == catalogue.revision && !commandInventoryChanged)
                {
                    // The scanner hashes the complete registration inventory.
                    // Keep existing associations and observed icons when only a
                    // registry notification repeated the same inventory. Forced
                    // discovery and source queries remain queued independently.
                    catalogue.folderRevision = value.folderRevision;
                    catalogue.backgroundRevisions = value.backgroundRevisions;
                    scanning = false;
                    sourcePending |= ManagementAttributionPending();
                    MenuTrace("catalogue", "unchanged");
                }
                else
                {
                    // Preparation contains registrations for unseen selections
                    // too; retiring it cannot depend on existing affected rows.
                    retirePreparation = commandInventoryChanged;
                    std::vector<Registration> changed;
                    for (const auto &old : catalogue.rows)
                        if (std::none_of(value.rows.begin(), value.rows.end(), [&](const auto &r) { return r.id == old.id && MenuRevision(r) == MenuRevision(old); })) changed.push_back(old);
                    for (const auto &next : value.rows)
                        if (std::none_of(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == next.id && MenuRevision(r) == MenuRevision(next); })) changed.push_back(next);
                    for (const auto &a : catalogue.associations)
                        if (std::any_of(value.rows.begin(), value.rows.end(), [&](auto &r) { if (r.id != a.registration || !r.systemEnabled) return false; r.linked = true; return true; })) value.associations.push_back(a);
                    catalogue = std::move(value); scanning = false; catalogueDirty = true;
                    sourcePending |= ManagementAttributionPending();
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
                        if (managementActive) typesRequested = true;
                    }
                    for (auto &[key, row] : rows)
                        if (!initial && std::any_of(changed.begin(), changed.end(), [&](const auto &r) {
                            return ShortcutSelection(row.request) || DependsOn(r, row.request,
                                row.view.contexts ? row.view.contexts : (row.request.background ? 12u : 3u));
                        }))
                        {
                            row.invalid = true; ++row.dependency; row.view.snapshot.reset(); row.bytes = 0; ++row.view.revision;
                            affected.push_back(row.request);
                            if (managementActive && std::any_of(discovery.begin(), discovery.end(), [&](const auto &r) { return SelectionKey(r) == key; })) Queue(row.request, QueryPriority::Inspect, true);
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
                // Resume learning from exact snapshots only after the live
                // inventory and target checks restore their original proof.
                if (sourcePending)
                    for (auto &[key, row] : rows)
                        if (row.registryInvalid && row.view.snapshot && !row.view.pending)
                            row.checkRequested = true;
            }
            for (const auto &request : affected) cache.Erase(request);
            if (retirePreparation)
            {
                Session::ReleasePreparedMenuWorker();
            }
            if (!affected.empty()) Session::ReleaseIdleWorker();
            SaveCatalogue(cache);
        }
        bool requested = false;
        {
            std::lock_guard lock(mutex);
            if (!managementActive)
            {
                sourceScanRequested |= scanRequested && AnyEnabled();
                scanRequested = false;
            }
            if (!stop && managementActive && !scan.valid() && scanRequested)
            {
                scanWorkGeneration = workGeneration;
                scanManagementGeneration = managementGeneration;
                scanRegistryRevision = registryRevision;
                snowdesktop::performance::Value("shell.catalogue", "request.registry_revision", {},
                    static_cast<double>(scanRegistryRevision));
                snowdesktop::performance::Value("shell.catalogue", "request.foreground", {},
                    foregroundScanRequested ? 1 : 0);
                scanStarted = GetTickCount64();
                failedRegistryRevision = 0;
                foregroundScanDemand.Reset(foregroundScanRequested);
                foregroundScanRequested = false;
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
    void StoreProvisionalSnapshots(MenuSnapshotCache &cache)
    {
        std::vector<std::pair<MenuSnapshotCache::Ticket, Reply>> stores;
        {
            std::lock_guard lock(mutex);
            if (!managementActive || !catalogue.revision) return;
            for (auto &[key, row] : rows)
            {
                if (!row.snapshotManagementProof || !row.provisionalTicket) continue;
                if (checkedRegistryRevision < row.provisionalRegistryRevision) continue;
                auto ticket = std::move(*row.provisionalTicket);
                row.provisionalTicket.reset();
                // A notification, newer query, explicit invalidation or failure
                // prevents an unproven first result from acquiring new proof.
                if (registryRevision != row.provisionalRegistryRevision || row.invalid ||
                    row.view.pending || !row.view.snapshot || !row.view.error.empty() ||
                    row.snapshotIdentity != ticket.identity || row.expires <= MenuSnapshotCache::Now()) continue;
                ticket.dependency = Dependency(catalogue, row.request, row.view.contexts);
                if (!ticket.dependency) continue;
                row.snapshotDependency = ticket.dependency;
                const int scope = VerificationIndex(row.view.contexts);
                row.snapshotSignature = scope > 0 ? VerificationSignature(scope) : 0;
                stores.emplace_back(std::move(ticket), *row.view.snapshot);
            }
        }
        // Store checks current target identity, cache epoch and issued sequence
        // again; a changed object or superseding query cannot become durable.
        for (auto &[ticket, reply] : stores)
            if (cache.Store(ticket, reply)) MenuTrace("schedule", "initial_snapshot_verified");
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
            if (!managementActive || scope.workGeneration != managementGeneration) continue;
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
            if (!managementActive) return;
            if (!VerificationSignature(index) || checkedRegistryRevision >= required ||
                ScopeChecked(contexts, required) || registryRevision <= scope.attempt) return;
            scope.scanRevision = scope.attempt = registryRevision;
            scope.workGeneration = managementGeneration;
            // The previous future has been consumed above before a new scan
            // can reset this scope's cancellation token.
            scope.demand.Reset(true);
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
        // Inventory loaded from disk is routing metadata only. Snapshot lookup
        // waits for explicit management proof or a current exact-source check.
        for (;;)
        {
            { std::lock_guard lock(mutex); if (stop) break; }
            ReapClosedManagement();
            bool discover = false;
            std::uint64_t iterationManagementGeneration = 0;
            { std::lock_guard lock(mutex); discover = managementActive && discoverRequested;
              iterationManagementGeneration = managementGeneration; }
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
                if (managementActive && iterationManagementGeneration == managementGeneration)
                {
                    discovery = std::move(targets); discoverRequested = false;
                    const bool force = std::exchange(discoverForce, false);
                    for (const auto &request : discovery) Queue(request, QueryPriority::Inspect, force);
                }
            }
            bool warmDesktop = false, inspectDesktop = false;
            { std::lock_guard lock(mutex); warmDesktop = std::exchange(startupWarm, false);
              inspectDesktop = managementActive && desktopInspection; }
            if (warmDesktop || inspectDesktop)
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &path)))
                {
                    Request request; request.paths = {path}; request.background = true; request.context = Context::Desktop; CoTaskMemFree(path);
                    if (Local(request))
                    {
                        std::lock_guard lock(mutex);
                        if (inspectDesktop && managementActive && iterationManagementGeneration == managementGeneration)
                        { management = request; Queue(request, QueryPriority::Inspect, true); }
                        if (warmDesktop) Queue(request, QueryPriority::Prewarm, false);
                    }
                }
                if (inspectDesktop) { std::lock_guard lock(mutex);
                    if (iterationManagementGeneration == managementGeneration) desktopInspection = false; }
            }
            if (TakeMenuRegistryChanges())
            {
                // A standby holds already loaded COM providers. A fresh bind
                // cannot make those DLLs current after registration changes.
                Session::ReleasePreparedMenuWorker();
                std::lock_guard lock(mutex);
                // Classes notifications also include unrelated Shell caches.
                // Mark cached views stale, but retain published payloads for an open
                // popup until the inventory confirms an actual registration change.
                catalogueStale = true;
                ++registryRevision;
                // Preparation also needs verification before it has acquired a
                // worker. Otherwise an unrelated notification leaves it behind
                // catalogueStale forever, until a foreground click starts the scan.
                const bool allowed = BackgroundWorkAllowed();
                scanRequested |= managementActive && (sourcePending || static_cast<bool>(sourceJob));
                sourceScanRequested |= !managementActive && AnyEnabled();
                for (auto &[key, row] : rows)
                {
                    row.registryInvalid = row.view.snapshot && (!row.invalid || row.registryInvalid);
                    row.invalid = true; row.snapshotTargetChecked = false; ++row.view.revision;
                    if (allowed && row.view.snapshot) row.checkRequested = true;
                    // Classes includes Shell caches written by the query itself.
                    // Verify registration changes before retiring in-flight work;
                    // otherwise every successful reply can trigger another query.
                    if (row.view.pending)
                        scanRequested |= managementActive && !row.request.startPinOnly;
                    else
                        ++row.dependency;
                }
                MenuTrace("catalogue", "stale.registry");
            }
            RefreshCatalogue(cache);
            RefreshConfiguredSources(cache);
            StoreProvisionalSnapshots(cache);
            RefreshScopeVerification();
            Catalogue typeCatalogue; bool discoverTypes = false;
            {
                std::lock_guard lock(mutex);
                if (managementActive && typesRequested && !scanning && !scanRequested && catalogue.revision)
                {
                    typesRequested = false; typesPreparing = true; discoverTypes = true;
                    typeCatalogue = catalogue; typeBatchForce = std::exchange(typesForce, false);
                }
            }
            if (discoverTypes)
            {
                auto targets = DiscoverFileTypes(typeCatalogue, cache.Directory());
                std::lock_guard lock(mutex);
                if (managementActive && iterationManagementGeneration == managementGeneration)
                {
                    typeDiscovery = std::move(targets); nextType = 0; typesPreparing = false;
                    // Keep the polling set bounded across repeated refreshes.
                    std::erase_if(discovery, [&](const auto &r) { return !r.paths.empty() && std::filesystem::path(r.paths.front()).parent_path() == cache.Directory() / L"discovery-samples-v1"; });
                }
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
                bool verifiedSource = false;
                {
                    std::lock_guard lock(mutex);
                    const auto completedRow = rows.find(job.key);
                    verifiedSource = nativeFactory && completedRow != rows.end() &&
                        !completedRow->second.request.sourceClsid.empty() && !completedRow->second.request.sourceKey.empty();
                }
                // The native metadata helper checks its exact live registration
                // and enabled policy before and after querying. Unrelated registry
                // scans must not delay that fresh, non-executable display payload.
                if (job.managementOnly && job.reply->ok && !job.startPinOnly && !verifiedSource)
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
                if (!sourceJob->reply)
                {
                    try { sourceJob->reply = sourceJob->work.poll(); } catch (...) { sourceJob->reply = Reply{false, {}, "source query exception"}; }
                    if (!sourceJob->reply && GetTickCount64() - sourceJob->started >= 8000)
                        sourceJob->reply = Reply{false, {}, "source query timeout"};
                    if (sourceJob->reply) sourceJob->replyReadyAt = GetTickCount64();
                }
                if (sourceJob->reply && CompleteSource(*sourceJob, *sourceJob->reply, cache)) sourceJob.reset();
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
                Key key; Request request; QueryPriority priority; std::uint64_t sequence = 0, dependency = 0, dispatchManagementGeneration = 0; bool invalid = false;
                {
                    std::lock_guard lock(mutex);
                    // Background queries share one slot, including metadata probes.
                    // Keep the second slot available for an explicit popup or click.
                    if (managementActive && normalJobs < 1 && nextType < typeDiscovery.size())
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
                    dispatchManagementGeneration = managementGeneration;
                    sequence = row.sequence; dependency = row.dependency; invalid = row.invalid; row.queued = false;
                }
                if (priority == QueryPriority::Prewarm && !Local(request))
                {
                    std::lock_guard lock(mutex); rows[key].view.pending = false; continue;
                }
                auto ticket = cache.Capture(request);
                const auto contexts = Contexts(request);
                std::uint64_t verifyCachedRevision = 0;
                bool diskAllowed = false;
                {
                    std::lock_guard lock(mutex);
                    auto &row = rows[key];
                    if (!row.inspection && priority != QueryPriority::Execute && !Enabled(request, contexts))
                    {
                        row.view.pending = false; row.checkRequested = row.checkInspection = false; row.targetCheckDemand.reset();
                        MenuTrace("schedule", "no_enabled_items"); continue;
                    }
                    if (priority != QueryPriority::Inspect && priority != QueryPriority::Execute &&
                        request.sourceClsid.empty() && !request.startPinOnly && sourceScanning &&
                        KnownSelectionSources(contexts) && GetTickCount64() - sourceScanStarted < kInventoryWaitMs)
                    {
                        row.queued = true; row.due = GetTickCount64() + 20;
                        break;
                    }
                    ticket.dependency = SnapshotDependency(request, contexts, priority == QueryPriority::Inspect);
                    diskAllowed = ticket.dependency != 0;
                    row.snapshotTargetChecked = ticket && row.snapshotIdentity == ticket.identity;
                    if (RestoreRegistrySnapshot(row, ticket))
                    {
                        row.view.pending = row.inspection = false;
                        continue;
                    }
                    if (priority == QueryPriority::Inspect && contexts >= 1 && contexts <= 3 && row.registryInvalid &&
                        !row.force && row.view.snapshot && row.snapshotTargetChecked &&
                        checkedRegistryRevision < registryRevision &&
                        failedRegistryRevision < registryRevision &&
                        (!scanning || ActiveScanElapsed() < kInventoryWaitMs))
                    {
                        // Wait for the already-requested inventory rather than
                        // launching an extension process just to discard it.
                        row.queued = true; row.due = GetTickCount64() + 20;
                        scanRequested |= catalogueStale;
                        break;
                    }
                    const int scope = VerificationIndex(contexts);
                    if (priority == QueryPriority::Inspect && row.invalid && !row.force && row.view.snapshot && row.snapshotTargetChecked && scope > 0 &&
                        row.snapshotSignature && row.snapshotSignature == VerificationSignature(scope))
                        verifyCachedRevision = registryRevision;
                    row.identity = ticket.identity; row.checkRequested = row.checkInspection = false;
                    row.targetCheckDemand.reset();
                }
                // A previous background snapshot can be verified while the new
                // Shell query is still running, including a stalled extension.
                // Target identity and the snapshot's own live scope proof must
                // both match; full/explicit invalidation never retains this data.
                if (verifyCachedRevision) StartScopeVerification(contexts, verifyCachedRevision);
                if (invalid) cache.Erase(request);
                if (auto disk = diskAllowed ? cache.Find(ticket) : std::optional<Reply>{})
                {
                    std::lock_guard lock(mutex);
                    auto &row = rows[key];
                    if (!row.view.snapshot && row.dependency == dependency)
                    {
                        row.snapshotManagementProof = priority == QueryPriority::Inspect;
                        Publish(key, *disk, contexts, cache.Written(ticket));
                    }
                    if (!row.force && row.clicks.empty() && row.sequence == sequence && row.dependency == dependency && row.view.snapshot)
                    {
                        row.view.pending = false; row.inspection = false;
                        MenuTrace("schedule", "snapshot_reused"); continue;
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    auto &row = rows[key];
                    // Classification and planning run on the worker. UI reads
                    // only this plan and immutable, separately keyed snapshots.
                    if (!row.inspection && priority != QueryPriority::Execute &&
                        !row.fullMenuRequested && configured &&
                        request.sourceClsid.empty() && !request.startPinOnly && !request.originalShortcutOnly)
                    {
                        auto plan = PlanDemandQueries(catalogue, available, preferences, request, contexts);
                        const bool verifying = !RuntimeProof(request, contexts);
                        // Only a current exact-source check can remove the root
                        // fallback. Old complete inventories are routing hints.
                        if (verifying) plan.full = true;
                        row.view.sources = plan.sources;
                        row.view.sourcesOnly = !plan.full;
                        row.view.contexts = contexts;
                        for (const auto &source : plan.sources) Queue(source, priority, row.force || verifying);
                        if (priority == QueryPriority::Prewarm && plan.full)
                        {
                            // Selection work warms proven sources only. Unknown
                            // commands still need an explicit foreground request.
                            row.view.pending = false; row.view.sourcesOnly = true;
                            row.force = row.fullMenuRequested = false;
                            MenuTrace("schedule", "prewarm_sources_only", 0, static_cast<unsigned>(plan.sources.size()));
                            continue;
                        }
                        if (!plan.full)
                        {
                            row.view.pending = false; row.view.error.clear(); row.checkRequested = row.checkInspection = false;
                            row.targetCheckDemand.reset();
                            row.force = false; ++row.view.revision;
                            MenuTrace("schedule", "sources_only", 0, static_cast<unsigned>(plan.sources.size()));
                            continue;
                        }
                        if (!plan.sources.empty())
                        {
                            // Start useful partial results before the aggregate
                            // fallback. A concurrent background query may already
                            // occupy the other slot; consuming both with full
                            // queries would defeat incremental display entirely.
                            row.fullMenuRequested = true;
                            row.queued = true; row.due = GetTickCount64() + 1;
                            continue;
                        }
                    }
                    else row.view.sourcesOnly = false;
                }
                if (priority == QueryPriority::Prewarm && request.sourceClsid.empty() && !request.startPinOnly)
                {
                    std::lock_guard lock(mutex);
                    auto &row = rows[key]; row.view.pending = false; row.fullMenuRequested = false;
                    MenuTrace("schedule", "prewarm_no_aggregate");
                    continue;
                }
                Running job{key, sequence, dependency, cache.Begin(ticket), {}, GetTickCount64(), request.startPinOnly, contexts, {}, 0, 0};
                job.managementOnly = priority == QueryPriority::Inspect;
                job.managementGeneration = dispatchManagementGeneration;
                { std::lock_guard lock(mutex);
                  if (job.managementOnly && (!managementActive || job.managementGeneration != managementGeneration)) continue;
                  rows[key].fullMenuRequested = false; }
                try { job.work = DispatchQuery(request); running.push_back(std::move(job)); }
                catch (...) { Complete(job, Reply{false, {}, "helper start failed"}, cache); }
            }
            if (!sourceJob && std::count_if(running.begin(), running.end(), [](const auto &job) {
                    return !job.startPinOnly;
                }) == 0)
                if (auto job = NextSource())
                {
                    job->started = GetTickCount64();
                    try { job->work = DispatchQuery(job->request); sourceJob = std::move(job); }
                    catch (...) { std::lock_guard lock(mutex); failedSources.insert(job->source.id); MenuTrace("attribution", "start_failed"); }
                }
            std::vector<std::tuple<Key, Request, bool, std::optional<QueryPriority>, std::uint64_t>> checks;
            {
                std::lock_guard lock(mutex);
                for (auto &[key, row] : rows) if (row.checkRequested &&
                    (!row.view.pending || (row.invalid && row.view.snapshot && VerificationIndex(row.view.contexts) > 0)))
                {
                    if (!BackgroundWorkAllowed())
                    {
                        row.checkRequested = row.checkInspection = false; row.targetCheckDemand.reset(); continue;
                    }
                    const bool inspection = row.checkInspection || (managementActive && row.snapshotManagementProof);
                    checks.emplace_back(key, row.request, inspection, row.targetCheckDemand, row.sequence);
                    row.checkRequested = row.checkInspection = false; row.targetCheckDemand.reset();
                }
                Trim();
            }
            for (const auto &[key, request, inspection, demand, sequence] : checks)
            {
                { std::lock_guard lock(mutex);
                  if (!BackgroundWorkAllowed() || (inspection && (!managementActive || iterationManagementGeneration != managementGeneration))) continue; }
                auto ticket = cache.Capture(request);
                unsigned verifyContexts = 0; std::uint64_t verifyRevision = 0;
                {
                    std::lock_guard lock(mutex);
                    const auto found = rows.find(key); if (found == rows.end()) continue;
                    auto &row = found->second;
                    // Capture performs target I/O outside the lock. A new row
                    // sequence owns a newer validation, and a closed management
                    // generation must not be reopened by this old check.
                    const bool effectiveInspection = inspection || row.checkInspection;
                    if (row.sequence != sequence || !BackgroundWorkAllowed() ||
                        (effectiveInspection && (!managementActive || iterationManagementGeneration != managementGeneration))) continue;
                    auto effectiveDemand = demand;
                    if (row.targetCheckDemand)
                        effectiveDemand = effectiveDemand ? std::min(*effectiveDemand, *row.targetCheckDemand) : *row.targetCheckDemand;
                    ticket.dependency = SnapshotDependency(request, row.view.contexts, row.snapshotManagementProof);
                    if (row.view.snapshot && row.identity != ticket.identity)
                    {
                        row.view.snapshot.reset(); row.bytes = 0; row.invalid = true; ++row.dependency; ++row.view.revision;
                        RebuildAvailable();
                        // Pure notification checks cannot turn an old, unseen
                        // ordinary row into a foreground aggregate query. A real
                        // Menu cache hit still carries its explicit priority.
                        Queue(request, effectiveInspection ? QueryPriority::Inspect : effectiveDemand.value_or(QueryPriority::Prewarm), true);
                    }
                    else if (row.targetCheckDemand)
                    {
                        // A new memory-hit Query arrived during Capture. Leave
                        // its intent pending for a fresh target capture rather
                        // than consuming it with this older observation.
                        continue;
                    }
                    RestoreRegistrySnapshot(row, ticket);
                    const int scope = VerificationIndex(row.view.contexts);
                    if (row.snapshotManagementProof && managementActive && row.invalid && !row.force && row.view.snapshot && scope > 0)
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
            // The owning worker also owns the thread-local prepared process.
            // It is consumed by one real selection, never recycled after loading
            // a user's menu. Do not occupy the interactive reserve while queued
            // work, management or a fullscreen workload needs the resources.
            auto preparation = Session::PreparationState::Absent;
            bool preparationWanted = false;
            if (nativeFactory)
            {
                bool prepare = false;
                {
                    std::lock_guard lock(mutex);
                    prepare = prepareFileDemand && !stop && !managementActive && configured &&
                        std::any_of(shown[0].begin(), shown[0].end(), [](const auto &id) {
                            return StateVisibilityId(id) != "state:start-pin";
                        });
                }
                preparationWanted = prepare;
                if (!prepare) Session::ReleasePreparedMenuWorker();
                else
                {
                    preparation = Session::PollPreparedMenuWorker();
                    const bool idle = running.empty() && !sourceJob;
                    bool queued = false;
                    { std::lock_guard lock(mutex);
                      queued = std::any_of(rows.begin(), rows.end(), [](const auto &row) { return row.second.queued; }); }
                    if (preparation == Session::PreparationState::Absent && idle && !queued &&
                        GetTickCount64() >= nextPreparation && !BackgroundCatalogueFullscreen())
                    {
                        // One attempt per actual query. A broken extension must
                        // not create an idle failure/relaunch loop.
                        nextPreparation = UINT64_MAX;
                        if (Session::PrepareFirstMenuWorker()) preparation = Session::PreparationState::Pending;
                    }
                }
            }
            bool watchesComplete = false;
            auto handles = MenuRegistryWaitHandles(watchesComplete);
            handles.insert(handles.begin(), wake);
            DWORD wait = watchesComplete ? INFINITE : 1000;
            if (preparationWanted && preparation == Session::PreparationState::Absent && nextPreparation != UINT64_MAX)
                wait = std::min(wait, DWORD(1000));
            if (preparation == Session::PreparationState::Pending) wait = std::min(wait, DWORD(20));
            if (!running.empty() || sourceJob || scan.valid() || sourceScan.valid() ||
                std::any_of(scopedVerification.begin(), scopedVerification.end(), [](const auto &scope) { return scope.scan.valid(); })) wait = 20;
            {
                std::lock_guard lock(mutex);
                const auto now = GetTickCount64();
                if (stop || discoverRequested || startupWarm || desktopInspection || scanRequested || sourceScanRequested ||
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
MenuService::MenuService(std::filesystem::path directory, QueryFactory factory, CatalogueReader reader, FolderVerifier verifier, BackgroundVerifier backgroundVerifier, SourceCatalogueReader sourceReader)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(factory), std::move(reader), std::move(verifier), std::move(backgroundVerifier), std::move(sourceReader))) {}
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
    const auto key = impl_->Queue(request, priority, force);
    if (!key.empty() && priority != QueryPriority::Prewarm) impl_->RequestForegroundScan();
    impl_->RememberView(request, key);
}
void MenuService::QueryFullMenu(const Request &request)
{
    std::lock_guard lock(impl_->mutex);
    const auto key = impl_->Queue(request, QueryPriority::Menu, true);
    if (!key.empty())
    {
        impl_->RequestForegroundScan();
        impl_->rows[key].fullMenuRequested = true;
    }
    impl_->RememberView(request, key);
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
    if (impl_->configured && !impl_->Enabled(request))
    {
        // Current all-off rules are authoritative even if an older projected
        // row is still being verified. An empty menu is ready without a query.
        MenuView view; view.snapshot = Reply{true, {}, {}};
        timing.Record("disabled");
        return view;
    }
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
        const auto previousConfiguration = impl_->configurationRevision;
        const bool desktop = !impl_->shown[static_cast<int>(Context::Desktop)].empty();
        if (!impl_->configured || impl_->preferences != preferences)
            ++impl_->configurationRevision;
        impl_->preferences = std::move(preferences); impl_->configured = true;
        for (int i = 0; i < 4; ++i) impl_->shown[i] = EffectiveShownIds(impl_->preferences, static_cast<Context>(i));
        for (int i = 0; i < 2; ++i)
            impl_->readProjection[i] = MenuReadPolicy(impl_->preferences, static_cast<Category>(i));
        impl_->sourcePending |= impl_->ManagementAttributionPending();
        if (previousConfiguration != impl_->configurationRevision && impl_->AnyEnabled())
        {
            if (impl_->managementActive) impl_->scanRequested = true;
            else impl_->sourceScanRequested = true;
            MenuTrace("catalogue.sources", "request.configure");
        }
        if (!desktop && !impl_->shown[static_cast<int>(Context::Desktop)].empty()) impl_->startupWarm = true;
        for (auto &[key, row] : impl_->rows)
            if (row.queued && !row.inspection && row.priority != QueryPriority::Execute && !impl_->Enabled(row.request, row.view.contexts))
            {
                row.queued = row.view.pending = row.checkRequested = row.checkInspection = false; row.targetCheckDemand.reset();
            }
        if (enabled && !impl_->AnyEnabled() && !impl_->managementActive)
        {
            ++impl_->workGeneration;
            impl_->scanRequested = impl_->foregroundScanRequested = impl_->startupWarm = false;
            impl_->catalogueStale = true;
            impl_->foregroundScanDemand.Cancel();
            impl_->sourceScanRequested = false;
            impl_->sourceScanDemand.Cancel();
        }
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
    const bool reopening = !impl_->managementActive;
    impl_->managementActive = true;
    if (reopening)
    {
        impl_->sourceScanRequested = false;
        impl_->sourceScanDemand.Cancel();
    }
    if (impl_->catalogueStale && !impl_->scanRequested)
    {
        impl_->scanRequested = true;
        MenuTrace("catalogue", "request.stale_inspect");
    }
    if (refresh || !impl_->inspected || reopening)
    {
        MenuTrace("catalogue", refresh ? "request.refresh" : "request.inspect");
        impl_->sourcePending = true;
        if (refresh) { impl_->sourceAttempts.clear(); impl_->sourceSelections.clear(); impl_->failedSources.clear(); }
        impl_->scanRequested = true; impl_->discoverRequested = true;
        impl_->discoverForce |= refresh;
        impl_->typesRequested = true; impl_->typesForce |= refresh;
    }
    impl_->RequestForegroundScan();
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
    if (!selection.paths.empty() && (refresh || reopening || selectionKey != impl_->inspectedSelection || invalidated))
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
void MenuService::EndInspection()
{
    std::lock_guard lock(impl_->mutex);
    if (!impl_->managementActive) return;
    impl_->managementActive = false;
    ++impl_->managementGeneration;
    for (auto &scope : impl_->scopedVerification)
    {
        scope.demand.Cancel();
        scope.attempt = scope.checked = scope.signature = 0;
    }
    impl_->scanRequested = false;
    impl_->foregroundScanDemand.Cancel();
    impl_->sourceScanRequested |= impl_->AnyEnabled();
    impl_->sourcePending = impl_->discoverRequested = impl_->discoverForce = impl_->desktopInspection = false;
    impl_->typesRequested = impl_->typesPreparing = impl_->typesForce = impl_->typeBatchForce = false;
    impl_->typeDiscovery.clear(); impl_->nextType = 0;
    impl_->discovery.clear(); impl_->sourceSelections.clear();
    for (auto &[key, row] : impl_->rows)
    {
        row.managementAttributionRequested = false;
        if (row.snapshotManagementProof)
        {
            row.snapshotManagementProof = false; row.snapshotDependency = 0;
            row.invalid = true; row.registryInvalid = false;
        }
        row.inspection = row.checkInspection = false;
        if (row.targetCheckDemand == QueryPriority::Inspect) row.targetCheckDemand.reset();
        if (row.priority == QueryPriority::Inspect && row.clicks.empty())
        {
            row.queued = row.view.pending = row.checkRequested = false; row.targetCheckDemand.reset();
        }
    }
    if (!impl_->AnyEnabled())
    {
        ++impl_->workGeneration;
        impl_->scanRequested = impl_->foregroundScanRequested = impl_->startupWarm = false;
        impl_->catalogueStale = true;
        // Cancel is checked by the production reader. Reset is performed only
        // after this future has drained, immediately before a later scan starts.
        impl_->foregroundScanDemand.Cancel();
        impl_->sourceScanRequested = false;
        impl_->sourceScanDemand.Cancel();
    }
    SetEvent(impl_->wake);
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
            impl_->RequestForegroundScan();
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
    impl_->RequestForegroundScan();
}
}
