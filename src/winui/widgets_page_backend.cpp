#include "pch.h"

#include "widgets_page_backend.h"

#include "../utils.h"
#include "../widget_engine.h"
#include "../widget_permission_broker.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snowdesktop::winui
{
namespace
{
using snowdesktop::widget::InstalledPackage;
using snowdesktop::widget::PackageDetails;
using snowdesktop::widget::PackageManifest;
using snowdesktop::widget::PackageSourceInfo;

constexpr std::size_t kMaximumSourceResults = 50;

struct SourceQueryRecord
{
    PackageSourceInfo source;
    std::vector<PackageDetails> results;
    std::string error;
};

struct SourceSearchResult
{
    std::vector<SourceQueryRecord> sources;
    std::string error;
    bool cancelled = false;
};

struct SourceSearchWork
{
    std::uint64_t generation = 0;
    std::uint64_t activation = 0;
    std::uint64_t taskId = 0;
    std::uint64_t searchRevision = 0;
    std::wstring query;
    std::string locale;
    std::vector<InstalledPackage> installed;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::function<void(SourceSearchResult)> completion;
};

bool IsBuiltinSource(std::string_view sourceId) noexcept
{
    return sourceId == "builtin";
}

bool IsManagedPackage(const InstalledPackage& package) noexcept
{
    return !package.builtin && !package.development;
}

std::wstring SourceFallbackName(std::string_view providerId)
{
    if (providerId == "builtin") return L"Included with SnowDesktop";
    if (providerId == "local-directory") return L"Local components";
    if (providerId == "static-catalog") return L"Component catalog";
    if (providerId == "steam-workshop")
        return L"Steam Workshop subscriptions";
    if (providerId == "local" || providerId == "local-import")
        return L"Local components";
    return Utf8ToWide(std::string(providerId));
}

bool ContainsPackageId(
    const std::vector<InstalledPackage>& installed,
    std::string_view packageId,
    bool managedOnly)
{
    return std::any_of(installed.begin(), installed.end(),
        [packageId, managedOnly](const InstalledPackage& package) {
            return package.manifest.id == packageId &&
                (!managedOnly || IsManagedPackage(package));
        });
}

const InstalledPackage* ActiveManagedPackage(
    const std::vector<InstalledPackage>& installed,
    std::string_view packageId)
{
    const auto found = std::find_if(installed.begin(), installed.end(),
        [packageId](const InstalledPackage& package) {
            return package.manifest.id == packageId &&
                IsManagedPackage(package) && package.selected;
        });
    return found == installed.end() ? nullptr : &*found;
}

bool SearchTextMatches(const PackageManifest& manifest,
    std::wstring_view query)
{
    if (query.empty()) return true;
    std::wstring searchable = Utf8ToWide(manifest.name + "\n" +
        manifest.description + "\n" + manifest.author + "\n" +
        manifest.id + "\n" + manifest.version);
    std::wstring needle(query);
    std::transform(searchable.begin(), searchable.end(), searchable.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return searchable.find(needle) != std::wstring::npos;
}

SourceSearchResult QuerySources(SourceSearchWork& work)
{
    SourceSearchResult result;
    const auto cancelled = [&]() noexcept {
        return work.cancellation && work.cancellation->load();
    };
    try
    {
        if (cancelled())
        {
            result.cancelled = true;
            return result;
        }
        const auto sources = WidgetEngine::ListWidgetPackageSources();
        result.sources.reserve(sources.size());
        for (const PackageSourceInfo& source : sources)
        {
            if (cancelled())
            {
                result.cancelled = true;
                break;
            }
            SourceQueryRecord record;
            record.source = source;
            if (source.capabilities.query && source.status.available)
            {
                if (IsBuiltinSource(source.providerId))
                {
                    for (const InstalledPackage& package : work.installed)
                    {
                        if (!package.builtin ||
                            record.results.size() >= kMaximumSourceResults)
                        {
                            continue;
                        }
                        PackageManifest manifest =
                            snowdesktop::widget::LocalizePackageManifest(
                                package.manifest, work.locale);
                        if (!SearchTextMatches(manifest, work.query))
                            continue;
                        record.results.push_back({std::move(manifest),
                            package.source,
                            {package.manifest.version}, false});
                    }
                }
                else
                {
                    snowdesktop::widget::PackageQuery query;
                    query.text = WideToUtf8(work.query);
                    query.locale = work.locale.empty()
                        ? "en-US" : work.locale;
                    query.limit = kMaximumSourceResults;
                    record.results = WidgetEngine::QueryWidgetPackageSource(
                        source.providerId, query, record.error);
                }
            }
            if (cancelled())
            {
                result.cancelled = true;
                break;
            }
            result.sources.push_back(std::move(record));
        }
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
    }
    catch (...)
    {
        result.error = "component source query failed";
    }
    return result;
}

class SourceSearchWorker final
{
public:
    SourceSearchWorker() = default;
    ~SourceSearchWorker() { Shutdown(); }

    SourceSearchWorker(const SourceSearchWorker&) = delete;
    SourceSearchWorker& operator=(const SourceSearchWorker&) = delete;

    bool Submit(SourceSearchWork work)
    {
        std::optional<SourceSearchWork> displaced;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            displaced = std::exchange(pending_, std::nullopt);
            pending_ = std::move(work);
            if (!worker_.joinable())
            {
                worker_ = std::jthread(
                    [this](std::stop_token stopToken) {
                        WorkerMain(stopToken);
                    });
            }
        }
        if (displaced && displaced->completion)
        {
            SourceSearchResult cancelled;
            cancelled.cancelled = true;
            displaced->completion(std::move(cancelled));
        }
        condition_.notify_one();
        return true;
    }

    bool RequestCancel(std::uint64_t taskId)
    {
        std::optional<SourceSearchWork> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (pending_ && pending_->taskId == taskId)
                cancelled = std::exchange(pending_, std::nullopt);
            else if (activeTaskId_ == taskId && activeCancellation_)
                activeCancellation_->store(true);
            else
                return false;
        }
        if (cancelled && cancelled->completion)
        {
            SourceSearchResult result;
            result.cancelled = true;
            cancelled->completion(std::move(result));
        }
        return true;
    }

    void Shutdown() noexcept
    {
        std::optional<SourceSearchWork> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            cancelled = std::exchange(pending_, std::nullopt);
        }
        if (cancelled && cancelled->completion)
        {
            try
            {
                SourceSearchResult result;
                result.cancelled = true;
                cancelled->completion(std::move(result));
            }
            catch (...)
            {
            }
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    void WorkerMain(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            std::optional<SourceSearchWork> work;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return stopping_ || pending_.has_value() ||
                        stopToken.stop_requested();
                });
                if (stopping_ || stopToken.stop_requested()) return;
                work = std::exchange(pending_, std::nullopt);
                if (work)
                {
                    activeTaskId_ = work->taskId;
                    activeCancellation_ = work->cancellation;
                }
            }
            if (!work) continue;
            SourceSearchResult result = QuerySources(*work);
            {
                std::lock_guard lock(mutex_);
                if (work->cancellation && work->cancellation->load())
                    result.cancelled = true;
                if (activeTaskId_ == work->taskId)
                {
                    activeTaskId_ = 0;
                    activeCancellation_.reset();
                }
            }
            if (work->completion)
            {
                try
                {
                    work->completion(std::move(result));
                }
                catch (...)
                {
                }
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<SourceSearchWork> pending_;
    std::uint64_t activeTaskId_ = 0;
    std::shared_ptr<std::atomic_bool> activeCancellation_;
    std::jthread worker_;
    bool stopping_ = false;
};

struct PackageGroup
{
    std::string id;
    const InstalledPackage* builtin = nullptr;
    const InstalledPackage* managed = nullptr;
    const InstalledPackage* development = nullptr;
};

std::vector<PackageGroup> GroupPackages(
    const std::vector<InstalledPackage>& packages)
{
    std::vector<PackageGroup> groups;
    std::unordered_map<std::string, std::size_t> indexes;
    for (const InstalledPackage& package : packages)
    {
        if (!package.active && !package.selected && !package.development)
            continue;
        const auto [found, inserted] = indexes.emplace(
            package.manifest.id, groups.size());
        if (inserted)
            groups.push_back(PackageGroup{package.manifest.id});
        PackageGroup& group = groups[found->second];
        if (package.development)
        {
            if (!group.development || package.active)
                group.development = &package;
        }
        else if (package.builtin)
        {
            if (!group.builtin || package.active)
                group.builtin = &package;
        }
        else if (package.selected || !group.managed)
        {
            group.managed = &package;
        }
    }
    return groups;
}

const InstalledPackage* DisplayPackage(const PackageGroup& group) noexcept
{
    if (group.development && group.development->active)
        return group.development;
    if (group.managed && group.managed->selected && group.managed->active)
        return group.managed;
    if (group.builtin && group.builtin->active)
        return group.builtin;
    if (group.managed && group.managed->active)
        return group.managed;
    if (group.development) return group.development;
    if (group.managed) return group.managed;
    return group.builtin;
}

bool IsGranted(std::span<const std::string> granted,
    std::string_view permission)
{
    return std::find(granted.begin(), granted.end(), permission) !=
        granted.end();
}

} // namespace

WidgetsPageHostOperationResult WidgetsPageHostOperationResult::Success(
    bool changed, std::wstring message)
{
    return {true, changed, std::move(message)};
}

WidgetsPageHostOperationResult WidgetsPageHostOperationResult::Failure(
    std::wstring message)
{
    return {false, false, std::move(message)};
}

namespace widgets_page_backend_detail
{
WidgetSourceKind SourceKindFor(std::string_view providerId) noexcept
{
    if (providerId == "builtin") return WidgetSourceKind::BuiltIn;
    if (providerId == "local" || providerId == "local-import")
        return WidgetSourceKind::Local;
    if (providerId == "static-catalog")
        return WidgetSourceKind::Catalog;
    if (providerId == "steam-workshop")
        return WidgetSourceKind::SteamWorkshop;
    if (providerId == "local-directory")
        return WidgetSourceKind::Development;
    return WidgetSourceKind::Other;
}

std::string SourceNameKeyFor(std::string_view providerId)
{
    switch (SourceKindFor(providerId))
    {
    case WidgetSourceKind::BuiltIn:
        return "app.settings.widgets_source_builtin";
    case WidgetSourceKind::Local:
    case WidgetSourceKind::Development:
        return "app.settings.widgets_source_local";
    case WidgetSourceKind::Catalog:
        return "app.settings.widgets_source_catalog";
    case WidgetSourceKind::SteamWorkshop:
        return "app.settings.widgets_source_steam";
    case WidgetSourceKind::Other:
    default:
        return {};
    }
}

WidgetPackagePermissionState PermissionStateFor(
    snowdesktop::widget::PermissionDecisionState state,
    snowdesktop::widget::PermissionRuntimeBlock runtimeBlock) noexcept
{
    using snowdesktop::widget::PermissionDecisionState;
    using snowdesktop::widget::PermissionRuntimeBlock;
    if (runtimeBlock == PermissionRuntimeBlock::MissingRequired)
        return WidgetPackagePermissionState::MissingRequired;
    switch (state)
    {
    case PermissionDecisionState::LegacyImplicit:
        return WidgetPackagePermissionState::LegacyImplicit;
    case PermissionDecisionState::Pending:
        return WidgetPackagePermissionState::Pending;
    case PermissionDecisionState::Granted:
        return WidgetPackagePermissionState::Granted;
    case PermissionDecisionState::Denied:
        return WidgetPackagePermissionState::Denied;
    default:
        return WidgetPackagePermissionState::Pending;
    }
}

WidgetPermissionRisk PermissionRiskFor(
    snowdesktop::widget::PermissionRiskClass risk) noexcept
{
    using snowdesktop::widget::PermissionRiskClass;
    switch (risk)
    {
    case PermissionRiskClass::Basic:
        return WidgetPermissionRisk::Basic;
    case PermissionRiskClass::SystemStatus:
        return WidgetPermissionRisk::SystemStatus;
    case PermissionRiskClass::PersonalData:
        return WidgetPermissionRisk::PersonalData;
    case PermissionRiskClass::ExternalCommunication:
        return WidgetPermissionRisk::ExternalCommunication;
    case PermissionRiskClass::ElevatedRead:
        return WidgetPermissionRisk::ElevatedRead;
    case PermissionRiskClass::Modification:
        return WidgetPermissionRisk::Modification;
    case PermissionRiskClass::UserScoped:
        return WidgetPermissionRisk::UserScoped;
    case PermissionRiskClass::Sensor:
        return WidgetPermissionRisk::Sensor;
    case PermissionRiskClass::Unknown:
    default:
        return WidgetPermissionRisk::Unknown;
    }
}

bool InstallFailureNeedsConfirmation(std::wstring_view message) noexcept
{
    return message.find(L"source changes require explicit confirmation") !=
            std::wstring_view::npos ||
        message.find(L"requests a new required permission") !=
            std::wstring_view::npos ||
        message.find(L"requests a new optional permission") !=
            std::wstring_view::npos ||
        message.find(L"requests a new network domain") !=
            std::wstring_view::npos;
}

bool CompletionIdentityMatches(
    std::uint64_t expectedGeneration,
    std::uint64_t expectedActivation,
    std::uint64_t expectedTaskId,
    std::uint64_t currentGeneration,
    std::uint64_t currentActivation,
    std::uint64_t currentTaskId) noexcept
{
    return expectedGeneration != 0 && expectedTaskId != 0 &&
        expectedGeneration == currentGeneration &&
        expectedActivation == currentActivation &&
        expectedTaskId == currentTaskId;
}
} // namespace widgets_page_backend_detail

struct WidgetsPageBackend::Impl final
    : std::enable_shared_from_this<WidgetsPageBackend::Impl>
{
    struct PendingInstall
    {
        enum class Kind
        {
            LocalPath,
            SourceItem,
        };

        Kind kind = Kind::LocalPath;
        std::filesystem::path path;
        std::string packageId;
        std::string sourceId;
        std::string externalItemId;
        std::string version;
        bool allowSourceChange = false;
        bool allowPermissionExpansion = false;
    };

    explicit Impl(WidgetEngine& value, WidgetsPageBackendOptions valueOptions)
        : engine(value), options(std::move(valueOptions)),
          ownerThreadId(GetCurrentThreadId())
    {
        state = std::make_shared<WidgetsPageSnapshot>();
    }

    WidgetEngine& engine;
    WidgetsPageBackendOptions options;
    DWORD ownerThreadId = 0;
    SourceSearchWorker sourceWorker;

    std::shared_ptr<WidgetsPageSnapshot> state;
    std::vector<InstalledPackage> packages;
    std::uint64_t generation = 0;
    std::uint64_t activation = 0;
    std::uint64_t revision = 0;
    std::uint64_t requestedSearchRevision = 0;
    std::wstring requestedSearchQuery;
    std::uint64_t nextTaskId = 0;
    std::uint64_t activeTaskId = 0;
    std::uint64_t pickerRequestId = 0;
    std::uint64_t confirmationRequestId = 0;
    std::unordered_set<std::uint64_t> outstandingSearches;
    bool active = false;
    std::atomic_bool closed{false};
    bool awaitingPicker = false;
    bool awaitingConfirmation = false;

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return ownerThreadId != 0 && ownerThreadId == GetCurrentThreadId();
    }

    [[nodiscard]] std::uint64_t NextTaskId() noexcept
    {
        ++nextTaskId;
        if (nextTaskId == 0) ++nextTaskId;
        return nextTaskId;
    }

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback = {}) const
    {
        if (options.localize)
        {
            try
            {
                std::wstring value = options.localize(key);
                if (!value.empty()) return value;
            }
            catch (...)
            {
            }
        }
        return std::wstring(fallback);
    }

    [[nodiscard]] std::string Locale() const
    {
        if (options.locale)
        {
            try
            {
                std::string value = options.locale();
                if (!value.empty()) return value;
            }
            catch (...)
            {
            }
        }
        return "en-US";
    }

    void SetFeedback(WidgetsPageFeedbackSeverity severity,
        std::wstring message, std::string titleKey = {})
    {
        state->feedback.severity = severity;
        state->feedback.titleKey = std::move(titleKey);
        state->feedback.title.clear();
        state->feedback.message = std::move(message);
    }

    void ClearFeedback()
    {
        state->feedback = {};
    }

    void Publish()
    {
        if (closed || !active) return;
        state->generation = generation;
        state->revision = ++revision;
        const auto publication =
            std::make_shared<const WidgetsPageSnapshot>(*state);
        if (options.snapshotChanged)
        {
            try
            {
                options.snapshotChanged(publication);
            }
            catch (...)
            {
            }
        }
    }

    void MarshalToOwner(std::function<void()> callback)
    {
        if (!callback || closed) return;
        if (OnOwnerThread())
        {
            callback();
            return;
        }
        if (!options.dispatchToOwner) return;
        try
        {
            (void)options.dispatchToOwner(std::move(callback));
        }
        catch (...)
        {
        }
    }

    std::vector<WidgetsPageHostInstance> CaptureInstances()
    {
        std::vector<WidgetsPageHostInstance> result;
        if (options.instances)
        {
            try
            {
                result = options.instances();
            }
            catch (...)
            {
                result.clear();
            }
        }

        std::unordered_map<std::wstring, const LuaWidget*> live;
        for (const LuaWidget& widget : engine.GetWidgets())
        {
            if (widget.preview || widget.widgetId.empty() ||
                widget.packageId.empty())
            {
                continue;
            }
            live[widget.widgetId] = &widget;
        }

        if (result.empty() && !options.instances)
        {
            result.reserve(live.size());
            for (const auto& [instanceId, widget] : live)
            {
                result.push_back({instanceId,
                    Utf8ToWide(widget->packageId),
                    Utf8ToWide(widget->name),
                    widget->valid && widget->runtimeToken != 0});
            }
        }
        else
        {
            for (WidgetsPageHostInstance& instance : result)
            {
                const auto found = live.find(instance.instanceId);
                if (found == live.end()) continue;
                if (instance.packageId.empty())
                    instance.packageId =
                        Utf8ToWide(found->second->packageId);
                if (instance.displayName.empty())
                    instance.displayName =
                        Utf8ToWide(found->second->name);
                instance.settingsAvailable =
                    instance.settingsAvailable ||
                    (found->second->valid &&
                        found->second->runtimeToken != 0);
            }
        }

        std::erase_if(result, [](const WidgetsPageHostInstance& instance) {
            return instance.instanceId.empty() || instance.packageId.empty();
        });
        std::sort(result.begin(), result.end(),
            [](const WidgetsPageHostInstance& left,
               const WidgetsPageHostInstance& right) {
                if (left.packageId != right.packageId)
                    return left.packageId < right.packageId;
                return left.instanceId < right.instanceId;
            });
        result.erase(std::unique(result.begin(), result.end(),
            [](const WidgetsPageHostInstance& left,
               const WidgetsPageHostInstance& right) {
                return left.instanceId == right.instanceId;
            }), result.end());
        return result;
    }

    std::wstring SourceDisplayName(std::string_view providerId) const
    {
        const std::string key =
            widgets_page_backend_detail::SourceNameKeyFor(providerId);
        return key.empty()
            ? SourceFallbackName(providerId)
            : L(key, SourceFallbackName(providerId));
    }

    InstalledWidgetPackageSnapshot ConvertPackage(
        const PackageGroup& group,
        const InstalledPackage& display,
        const std::vector<WidgetsPageHostInstance>& instances) const
    {
        const PackageManifest manifest =
            snowdesktop::widget::LocalizePackageManifest(
                display.manifest, Locale());
        InstalledWidgetPackageSnapshot snapshot;
        snapshot.packageId = Utf8ToWide(group.id);
        snapshot.name = Utf8ToWide(manifest.name);
        snapshot.description = Utf8ToWide(manifest.description);
        snapshot.version = Utf8ToWide(manifest.version);
        snapshot.author = Utf8ToWide(manifest.author);
        snapshot.sourceId = Utf8ToWide(display.source.providerId);
        snapshot.sourceName = SourceDisplayName(display.source.providerId);
        snapshot.builtIn = display.builtin;
        snapshot.development = display.development;
        snapshot.enabled = group.managed
            ? group.managed->enabled : display.enabled;
        snapshot.active = display.active;
        snapshot.canEnable = group.managed != nullptr;
        snapshot.canUninstall = group.managed != nullptr;
        snapshot.canAddToDesktop = display.active && display.enabled &&
            static_cast<bool>(options.addPackageToDesktop);
        snapshot.canUseDevelopmentOverride =
            group.development != nullptr;
        snapshot.developmentOverrideActive = group.development &&
            group.development->active;

        const auto grant = snowdesktop::widget::WidgetPermissionBroker::
            Evaluate(display.permissionState,
                display.manifest.permissions,
                display.manifest.optionalPermissions,
                display.manifest.networkDomains,
                display.grantedPermissions,
                display.grantedNetworkDomains);
        snapshot.permissionState = widgets_page_backend_detail::
            PermissionStateFor(display.permissionState,
                grant.runtimeBlock);
        snapshot.grantedNetworkDomains.reserve(
            grant.networkDomains.size());
        snapshot.declaredNetworkDomains.reserve(
            display.manifest.networkDomains.size());
        for (const std::string& domain : display.manifest.networkDomains)
            snapshot.declaredNetworkDomains.push_back(Utf8ToWide(domain));
        for (const std::string& domain : grant.networkDomains)
            snapshot.grantedNetworkDomains.push_back(Utf8ToWide(domain));

        std::unordered_set<std::string> required(
            display.manifest.permissions.begin(),
            display.manifest.permissions.end());
        for (const std::string& permission :
            snowdesktop::widget::DeclaredPermissions(display.manifest))
        {
            WidgetPermissionSnapshot item;
            item.id = Utf8ToWide(permission);
            if (const char* key = snowdesktop::widget::
                    WidgetPermissionLabelLocalizationKey(permission))
            {
                item.labelKey = key;
            }
            else
            {
                item.label = Utf8ToWide(permission);
            }
            item.risk = widgets_page_backend_detail::PermissionRiskFor(
                snowdesktop::widget::ClassifyPermissionRisk(permission));
            item.required = required.contains(permission);
            item.granted = IsGranted(grant.permissions, permission);
            snapshot.permissions.push_back(std::move(item));
        }

        for (const WidgetsPageHostInstance& instance : instances)
        {
            if (instance.packageId != snapshot.packageId) continue;
            snapshot.instances.push_back({instance.instanceId,
                instance.displayName.empty()
                    ? instance.instanceId : instance.displayName,
                instance.settingsAvailable});
        }
        return snapshot;
    }

    void UpdateCatalogInstallationFlags()
    {
        for (WidgetSourceGroupSnapshot& source : state->sources)
        {
            const bool builtin = source.kind == WidgetSourceKind::BuiltIn;
            for (WidgetCatalogItemSnapshot& item : source.results)
            {
                const std::string packageId = WideToUtf8(item.packageId);
                if (builtin)
                {
                    item.installed = ContainsPackageId(
                        packages, packageId, false);
                    item.updateAvailable = false;
                    item.installAllowed = false;
                    continue;
                }
                const InstalledPackage* managed =
                    ActiveManagedPackage(packages, packageId);
                item.installed = managed != nullptr;
                item.updateAvailable = managed &&
                    snowdesktop::widget::WidgetPackageValidator::
                        IsNewerSemVer(WideToUtf8(item.version),
                            managed->manifest.version);
                item.installAllowed = source.supportsInstall;
            }
        }
    }

    bool CaptureInstalledState()
    {
        if (!OnOwnerThread()) return false;
        try
        {
            packages = WidgetEngine::ListWidgetPackages();
            const auto instances = CaptureInstances();
            std::vector<InstalledWidgetPackageSnapshot> converted;
            const auto groups = GroupPackages(packages);
            converted.reserve(groups.size());
            for (const PackageGroup& group : groups)
            {
                const InstalledPackage* display = DisplayPackage(group);
                if (!display ||
                    !snowdesktop::widget::IsExecutablePackageContract(
                        display->manifest))
                {
                    continue;
                }
                converted.push_back(
                    ConvertPackage(group, *display, instances));
            }
            std::sort(converted.begin(), converted.end(),
                [](const InstalledWidgetPackageSnapshot& left,
                   const InstalledWidgetPackageSnapshot& right) {
                    if (left.name != right.name) return left.name < right.name;
                    return left.packageId < right.packageId;
                });
            state->installed = std::move(converted);
            state->developerOverridesVisible =
                options.developerOverridesVisible &&
                options.developerOverridesVisible();
            UpdateCatalogInstallationFlags();
            return true;
        }
        catch (const std::exception& exception)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                Utf8ToWide(exception.what()), "settings.status.error");
        }
        catch (...)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"Component state could not be read.",
                "settings.status.error");
        }
        return false;
    }

    std::vector<WidgetSourceGroupSnapshot> ConvertSources(
        const SourceSearchResult& result) const
    {
        std::vector<WidgetSourceGroupSnapshot> converted;
        converted.reserve(result.sources.size());
        for (const SourceQueryRecord& record : result.sources)
        {
            WidgetSourceGroupSnapshot source;
            source.sourceId = Utf8ToWide(record.source.providerId);
            source.kind = widgets_page_backend_detail::SourceKindFor(
                record.source.providerId);
            source.nameKey = widgets_page_backend_detail::SourceNameKeyFor(
                record.source.providerId);
            source.name = SourceFallbackName(record.source.providerId);
            source.available = record.source.status.available &&
                record.error.empty();
            const std::string status = record.error.empty()
                ? record.source.status.message : record.error;
            source.status = Utf8ToWide(status);
            source.supportsSearch = record.source.capabilities.query;
            source.supportsInstall = source.kind != WidgetSourceKind::BuiltIn &&
                record.source.capabilities.query &&
                record.source.capabilities.details;
            source.workshop =
                source.kind == WidgetSourceKind::SteamWorkshop;
            source.supportsSynchronization =
                static_cast<bool>(options.synchronizeSource) &&
                static_cast<bool>(options.canSynchronizeSource) &&
                options.canSynchronizeSource(record.source.providerId);

            for (const PackageDetails& details : record.results)
            {
                if (!snowdesktop::widget::IsExecutablePackageContract(
                        details.manifest) || details.withdrawn)
                {
                    continue;
                }
                WidgetCatalogItemSnapshot item;
                item.sourceId = source.sourceId;
                item.externalItemId =
                    Utf8ToWide(details.source.externalItemId);
                item.packageId = Utf8ToWide(details.manifest.id);
                item.name = Utf8ToWide(details.manifest.name);
                item.description = Utf8ToWide(
                    details.manifest.description);
                item.version = Utf8ToWide(details.manifest.version);
                item.author = Utf8ToWide(details.manifest.author);
                item.installAllowed = source.supportsInstall;
                source.results.push_back(std::move(item));
            }
            converted.push_back(std::move(source));
        }
        return converted;
    }

    void CompleteSearch(const SourceSearchWork& work,
        SourceSearchResult result)
    {
        outstandingSearches.erase(work.taskId);
        if (closed || !active ||
            !widgets_page_backend_detail::CompletionIdentityMatches(
                work.generation, work.activation, work.taskId,
                generation, activation, activeTaskId) ||
            work.searchRevision != requestedSearchRevision)
        {
            return;
        }

        if (result.cancelled)
        {
            activeTaskId = 0;
            state->task = {};
            Publish();
            return;
        }

        activeTaskId = 0;
        state->task = {};
        if (!result.error.empty())
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                Utf8ToWide(result.error), "settings.status.error");
        }
        state->sources = ConvertSources(result);
        state->searchRevision = work.searchRevision;
        state->searchQuery = work.query;
        UpdateCatalogInstallationFlags();
        Publish();
    }

    bool StartSearch(std::uint64_t searchRevision,
        std::wstring query, bool internalRefresh = false)
    {
        if (!active || closed || !OnOwnerThread()) return false;
        if (!options.dispatchToOwner)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"Component source search requires an owner-thread dispatcher.",
                "settings.status.error");
            Publish();
            return false;
        }
        if (state->task.kind != WidgetsPageTaskKind::None &&
            state->task.kind != WidgetsPageTaskKind::Searching)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Warning,
                L"Another component operation is still running.");
            Publish();
            return false;
        }
        if (!internalRefresh &&
            searchRevision <= requestedSearchRevision &&
            !(searchRevision == 0 && requestedSearchRevision == 0))
        {
            return false;
        }

        requestedSearchRevision = searchRevision;
        requestedSearchQuery = query;
        const std::uint64_t taskId = NextTaskId();
        activeTaskId = taskId;
        outstandingSearches.insert(taskId);
        state->task.taskId = taskId;
        state->task.kind = WidgetsPageTaskKind::Searching;
        state->task.status = L("app.settings.widgets_search", L"Searching");
        state->task.cancellable = true;
        state->task.progress.reset();
        ClearFeedback();
        Publish();

        SourceSearchWork work;
        work.generation = generation;
        work.activation = activation;
        work.taskId = taskId;
        work.searchRevision = searchRevision;
        work.query = std::move(query);
        work.locale = Locale();
        work.installed = packages;
        work.cancellation = std::make_shared<std::atomic_bool>(false);
        const std::weak_ptr<Impl> weak = weak_from_this();
        work.completion = [weak,
            workGeneration = work.generation,
            workActivation = work.activation,
            workTaskId = work.taskId,
            workSearchRevision = work.searchRevision,
            workQuery = work.query](SourceSearchResult result) mutable {
            const auto self = weak.lock();
            if (!self) return;
            SourceSearchWork identity;
            identity.generation = workGeneration;
            identity.activation = workActivation;
            identity.taskId = workTaskId;
            identity.searchRevision = workSearchRevision;
            identity.query = std::move(workQuery);
            self->MarshalToOwner(
                [weak, identity = std::move(identity),
                    result = std::move(result)]() mutable {
                    if (const auto owner = weak.lock())
                        owner->CompleteSearch(identity, std::move(result));
                });
        };
        if (sourceWorker.Submit(std::move(work))) return true;

        outstandingSearches.erase(taskId);
        if (activeTaskId == taskId)
        {
            activeTaskId = 0;
            state->task = {};
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"Component source search could not be started.",
                "settings.status.error");
            Publish();
        }
        return false;
    }

    [[nodiscard]] bool BusyForMutation() const noexcept
    {
        return awaitingPicker || awaitingConfirmation ||
            state->task.kind != WidgetsPageTaskKind::None ||
            !outstandingSearches.empty();
    }

    bool ReportBusy()
    {
        SetFeedback(WidgetsPageFeedbackSeverity::Warning,
            L"Another component operation is still running.");
        Publish();
        return false;
    }

    const InstalledWidgetPackageSnapshot* FindSnapshotPackage(
        std::wstring_view packageId) const noexcept
    {
        const auto found = std::find_if(state->installed.begin(),
            state->installed.end(), [packageId](const auto& package) {
                return package.packageId == packageId;
            });
        return found == state->installed.end() ? nullptr : &*found;
    }

    const InstalledPackage* FindDisplayPackage(
        std::string_view packageId) const noexcept
    {
        const auto groups = GroupPackages(packages);
        const auto group = std::find_if(groups.begin(), groups.end(),
            [packageId](const PackageGroup& candidate) {
                return candidate.id == packageId;
            });
        return group == groups.end() ? nullptr : DisplayPackage(*group);
    }

    const InstalledPackage* FindManagedPackage(
        std::string_view packageId) const noexcept
    {
        return ActiveManagedPackage(packages, packageId);
    }

    void NotifyHostStateChanged()
    {
        if (!options.hostStateChanged) return;
        try
        {
            options.hostStateChanged();
        }
        catch (...)
        {
        }
    }

    void BeginTask(WidgetsPageTaskKind kind, std::wstring packageId = {},
        std::wstring sourceId = {}, bool cancellable = false)
    {
        activeTaskId = NextTaskId();
        state->task = {};
        state->task.taskId = activeTaskId;
        state->task.kind = kind;
        state->task.packageId = std::move(packageId);
        state->task.sourceId = std::move(sourceId);
        state->task.cancellable = cancellable;
        ClearFeedback();
        Publish();
    }

    void FinishTask(WidgetsPageHostOperationResult result,
        std::string_view successKey = {})
    {
        activeTaskId = 0;
        state->task = {};
        if (result.succeeded)
        {
            std::wstring message = std::move(result.message);
            if (message.empty() && !successKey.empty())
                message = L(successKey);
            SetFeedback(WidgetsPageFeedbackSeverity::Success,
                std::move(message));
            NotifyHostStateChanged();
            (void)CaptureInstalledState();
        }
        else
        {
            if (result.message.empty())
                result.message = L"The component operation failed.";
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(result.message), "settings.status.error");
        }
        Publish();
    }

    WidgetsPageHostOperationResult ExecuteInstall(
        const PendingInstall& install)
    {
        std::wstring error;
        bool installed = false;
        if (install.kind == PendingInstall::Kind::LocalPath)
        {
            installed = engine.InstallAndVerifyWidgetPackage(
                install.path.wstring(), error, install.allowSourceChange,
                install.allowPermissionExpansion);
        }
        else
        {
            installed = engine.InstallAndVerifyWidgetPackageFromSource(
                install.sourceId, install.externalItemId, install.version,
                error, install.allowSourceChange,
                install.allowPermissionExpansion);
        }
        return installed
            ? WidgetsPageHostOperationResult::Success()
            : WidgetsPageHostOperationResult::Failure(std::move(error));
    }

    void RequestInstallConfirmation(PendingInstall install,
        std::wstring reason)
    {
        activeTaskId = 0;
        state->task = {};
        if (!options.confirmInstall)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(reason), "settings.status.error");
            Publish();
            return;
        }

        const bool sourceChange = reason.find(
            L"source changes require explicit confirmation") !=
            std::wstring::npos;
        const bool permissionExpansion =
            reason.find(L"requests a new required permission") !=
                std::wstring::npos ||
            reason.find(L"requests a new optional permission") !=
                std::wstring::npos ||
            reason.find(L"requests a new network domain") !=
                std::wstring::npos;
        install.allowSourceChange =
            install.allowSourceChange || sourceChange;
        install.allowPermissionExpansion =
            install.allowPermissionExpansion || permissionExpansion;

        awaitingConfirmation = true;
        confirmationRequestId = NextTaskId();
        SetFeedback(WidgetsPageFeedbackSeverity::Warning, reason);
        Publish();
        const std::uint64_t requestGeneration = generation;
        const std::uint64_t requestActivation = activation;
        const std::uint64_t requestId = confirmationRequestId;
        const std::weak_ptr<Impl> weak = weak_from_this();
        const std::wstring title = L(
            "app.settings.widgets_confirm_install", L"Install component");
        const std::wstring message = L(
            "app.settings.widgets_install_confirm",
            L"This component requests additional access or a source change.") +
            (reason.empty() ? std::wstring{} : L"\n\n" + reason);
        try
        {
            options.confirmInstall(requestGeneration, title, message,
                [weak, requestGeneration, requestActivation, requestId,
                    install = std::move(install)](bool confirmed) mutable {
                    const auto self = weak.lock();
                    if (!self) return;
                    self->MarshalToOwner(
                        [weak, requestGeneration, requestActivation,
                            requestId, confirmed,
                            install = std::move(install)]() mutable {
                            const auto owner = weak.lock();
                            if (!owner || owner->closed || !owner->active ||
                                !owner->awaitingConfirmation ||
                                owner->confirmationRequestId != requestId ||
                                owner->generation != requestGeneration ||
                                owner->activation != requestActivation)
                            {
                                return;
                            }
                            owner->awaitingConfirmation = false;
                            owner->confirmationRequestId = 0;
                            if (!confirmed)
                            {
                                owner->ClearFeedback();
                                owner->Publish();
                                return;
                            }
                            owner->RunInstall(std::move(install));
                        });
                });
        }
        catch (...)
        {
            awaitingConfirmation = false;
            confirmationRequestId = 0;
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The install confirmation could not be shown.",
                "settings.status.error");
            Publish();
        }
    }

    void RunInstall(PendingInstall install)
    {
        BeginTask(WidgetsPageTaskKind::Installing,
            Utf8ToWide(install.packageId), Utf8ToWide(install.sourceId));
        WidgetsPageHostOperationResult result;
        try
        {
            result = ExecuteInstall(install);
        }
        catch (const std::exception& exception)
        {
            result = WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(exception.what()));
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L"The component could not be installed.");
        }

        const bool needsSourceConfirmation = !install.allowSourceChange &&
            result.message.find(
                L"source changes require explicit confirmation") !=
                std::wstring::npos;
        const bool needsPermissionConfirmation =
            !install.allowPermissionExpansion &&
            (result.message.find(L"requests a new required permission") !=
                    std::wstring::npos ||
                result.message.find(L"requests a new optional permission") !=
                    std::wstring::npos ||
                result.message.find(L"requests a new network domain") !=
                    std::wstring::npos);
        if (!result.succeeded &&
            (needsSourceConfirmation || needsPermissionConfirmation) &&
            widgets_page_backend_detail::InstallFailureNeedsConfirmation(
                result.message))
        {
            RequestInstallConfirmation(
                std::move(install), std::move(result.message));
            return;
        }
        FinishTask(std::move(result), "app.settings.widgets_install_ok");
    }

    bool BrowseInstallPackage()
    {
        if (BusyForMutation()) return ReportBusy();
        if (!options.pickPackage)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The package picker is unavailable.",
                "settings.status.error");
            Publish();
            return false;
        }
        awaitingPicker = true;
        pickerRequestId = NextTaskId();
        const std::uint64_t requestGeneration = generation;
        const std::uint64_t requestActivation = activation;
        const std::uint64_t requestId = pickerRequestId;
        const std::weak_ptr<Impl> weak = weak_from_this();
        try
        {
            options.pickPackage(requestGeneration,
                [weak, requestGeneration, requestActivation, requestId](
                    std::optional<std::filesystem::path> selected) mutable {
                    const auto self = weak.lock();
                    if (!self) return;
                    self->MarshalToOwner(
                        [weak, requestGeneration, requestActivation,
                            requestId, selected = std::move(selected)]() mutable {
                            const auto owner = weak.lock();
                            if (!owner || owner->closed || !owner->active ||
                                !owner->awaitingPicker ||
                                owner->pickerRequestId != requestId ||
                                owner->generation != requestGeneration ||
                                owner->activation != requestActivation)
                            {
                                return;
                            }
                            owner->awaitingPicker = false;
                            owner->pickerRequestId = 0;
                            if (!selected) return;
                            PendingInstall install;
                            install.kind = PendingInstall::Kind::LocalPath;
                            install.path = std::move(*selected);
                            owner->RunInstall(std::move(install));
                        });
                });
            return true;
        }
        catch (...)
        {
            awaitingPicker = false;
            pickerRequestId = 0;
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The package picker could not be opened.",
                "settings.status.error");
            Publish();
            return false;
        }
    }

    bool InstallCatalogItem(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const auto source = std::find_if(state->sources.begin(),
            state->sources.end(), [&](const WidgetSourceGroupSnapshot& item) {
                return item.sourceId == request.sourceId;
            });
        if (source == state->sources.end() || !source->available ||
            !source->supportsInstall)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The selected component source is unavailable.",
                "settings.status.error");
            Publish();
            return false;
        }
        const auto item = std::find_if(source->results.begin(),
            source->results.end(), [&](const WidgetCatalogItemSnapshot& value) {
                return value.packageId == request.packageId &&
                    value.externalItemId == request.externalItemId &&
                    value.version == request.version;
            });
        if (item == source->results.end() || !item->installAllowed)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The selected component result is stale or unavailable.",
                "settings.status.error");
            Publish();
            return false;
        }

        PendingInstall install;
        install.kind = PendingInstall::Kind::SourceItem;
        install.packageId = WideToUtf8(item->packageId);
        install.sourceId = WideToUtf8(item->sourceId);
        install.externalItemId = WideToUtf8(item->externalItemId);
        install.version = WideToUtf8(item->version);
        if (install.packageId.empty() || install.sourceId.empty() ||
            install.externalItemId.empty())
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The selected component identity is invalid.",
                "settings.status.error");
            Publish();
            return false;
        }
        RunInstall(std::move(install));
        return true;
    }

    bool SetPackageEnabled(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const std::string packageId = WideToUtf8(request.packageId);
        const InstalledPackage* managed = FindManagedPackage(packageId);
        if (!managed)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"This component cannot be enabled or disabled.",
                "settings.status.error");
            Publish();
            return false;
        }
        std::string error;
        if (!WidgetEngine::SetWidgetPackageEnabled(
                packageId, request.enabled, error))
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                Utf8ToWide(error), "settings.status.error");
            Publish();
            return false;
        }
        if (!request.enabled)
        {
            std::vector<std::wstring> instances;
            for (const LuaWidget& widget : engine.GetWidgets())
                if (!widget.preview && widget.packageId == packageId)
                    instances.push_back(widget.widgetId);
            for (const std::wstring& instance : instances)
                engine.UnloadWidget(instance);
        }
        NotifyHostStateChanged();
        (void)CaptureInstalledState();
        SetFeedback(WidgetsPageFeedbackSeverity::Success,
            L(request.enabled ? "app.settings.widgets_enabled_ok"
                              : "app.settings.widgets_disabled_ok"));
        Publish();
        return true;
    }

    bool SetPermissionDecision(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const std::string packageId = WideToUtf8(request.packageId);
        const InstalledPackage* package = FindDisplayPackage(packageId);
        if (!package)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The component package is unavailable.",
                "settings.status.error");
            Publish();
            return false;
        }

        using snowdesktop::widget::PermissionDecisionState;
        PermissionDecisionState stateValue;
        switch (request.permissionState)
        {
        case WidgetPackagePermissionState::Granted:
            stateValue = PermissionDecisionState::Granted;
            break;
        case WidgetPackagePermissionState::Denied:
            stateValue = PermissionDecisionState::Denied;
            break;
        default:
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The requested permission decision is invalid.",
                "settings.status.error");
            Publish();
            return false;
        }

        std::unordered_set<std::string> requestedPermissions;
        for (const std::wstring& permission : request.grantedPermissions)
            requestedPermissions.insert(WideToUtf8(permission));
        std::vector<std::string> grantedPermissions;
        if (stateValue == PermissionDecisionState::Granted)
        {
            for (const std::string& permission :
                snowdesktop::widget::DeclaredPermissions(package->manifest))
            {
                if (requestedPermissions.contains(permission))
                    grantedPermissions.push_back(permission);
            }
        }

        std::vector<std::string> grantedDomains;
        const bool grantsDeclaredNetwork =
            IsGranted(grantedPermissions, "network.http") ||
            IsGranted(grantedPermissions, "network.internet");
        if (grantsDeclaredNetwork)
        {
            std::unordered_set<std::string> requestedDomains;
            for (const std::wstring& domain :
                request.grantedNetworkDomains)
            {
                requestedDomains.insert(WideToUtf8(domain));
            }
            // The current presenter exposes network domains as a declared
            // package scope rather than individual toggles. A newly granted
            // network permission therefore grants the full declared list,
            // matching the legacy settings behavior.
            for (const std::string& domain : package->manifest.networkDomains)
            {
                if (requestedDomains.contains(domain))
                {
                    grantedDomains.push_back(domain);
                }
            }
        }

        BeginTask(WidgetsPageTaskKind::ApplyingPermissions,
            request.packageId);
        std::string error;
        const bool applied = engine.ApplyWidgetPermissionDecision(
            request.packageId, stateValue, grantedPermissions,
            grantedDomains, error);
        FinishTask(applied
                ? WidgetsPageHostOperationResult::Success()
                : WidgetsPageHostOperationResult::Failure(Utf8ToWide(error)),
            stateValue == PermissionDecisionState::Denied
                ? "app.settings.widgets_permissions_revoked_ok"
                : "app.settings.widgets_permissions_updated_ok");
        return applied;
    }

    bool SetDevelopmentOverride(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        if (!options.developerOverridesVisible ||
            !options.developerOverridesVisible())
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"Development overrides are disabled.",
                "settings.status.error");
            Publish();
            return false;
        }
        const std::string packageId = WideToUtf8(request.packageId);
        const InstalledWidgetPackageSnapshot* snapshotPackage =
            FindSnapshotPackage(request.packageId);
        if (!snapshotPackage)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The component package is unavailable.",
                "settings.status.error");
            Publish();
            return false;
        }
        const bool previous = snapshotPackage->developmentOverrideActive;
        if (previous == request.enabled)
            return true;
        const bool developmentExists = std::any_of(
            packages.begin(), packages.end(), [&](const InstalledPackage& item) {
                return item.development && item.manifest.id == packageId;
            });
        if (request.enabled && !developmentExists)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"The development package is unavailable.",
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::ApplyingDevelopmentOverride,
            request.packageId);
        std::string error;
        if (!WidgetEngine::SetWidgetDevelopmentOverride(
                packageId, request.enabled, error))
        {
            FinishTask(WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(error)));
            return false;
        }
        std::vector<std::wstring> instances;
        for (const LuaWidget& widget : engine.GetWidgets())
            if (!widget.preview && widget.packageId == packageId)
                instances.push_back(widget.widgetId);

        std::vector<std::wstring> reloadFailures;
        for (const std::wstring& instance : instances)
        {
            if (!engine.ReloadWidget(instance))
                reloadFailures.push_back(instance);
        }
        if (!reloadFailures.empty())
        {
            std::string rollbackError;
            const bool overrideRolledBack =
                WidgetEngine::SetWidgetDevelopmentOverride(
                    packageId, previous, rollbackError);
            std::vector<std::wstring> rollbackReloadFailures;
            if (overrideRolledBack)
            {
                for (const std::wstring& instance : instances)
                {
                    if (!engine.ReloadWidget(instance))
                        rollbackReloadFailures.push_back(instance);
                }
            }

            std::wstring message =
                L"One or more component instances could not be reloaded; "
                L"the development override was rolled back.";
            if (!overrideRolledBack)
            {
                message = L"One or more component instances could not be "
                    L"reloaded, and the development override rollback also "
                    L"failed: " + Utf8ToWide(rollbackError);
            }
            else if (!rollbackReloadFailures.empty())
            {
                message += L" Some instances could not be restored and need "
                    L"an application restart.";
            }
            NotifyHostStateChanged();
            FinishTask(WidgetsPageHostOperationResult::Failure(
                std::move(message)));
            return false;
        }
        FinishTask(WidgetsPageHostOperationResult::Success(),
            request.enabled
                ? "app.settings.widgets_development_activated_ok"
                : "app.settings.widgets_development_deactivated_ok");
        return true;
    }

    bool UninstallPackage(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const std::string packageId = WideToUtf8(request.packageId);
        if (!FindManagedPackage(packageId))
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"This component cannot be uninstalled.",
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::Uninstalling, request.packageId);
        if (const auto source = WidgetEngine::GetWidgetPackageSource(
                request.packageId);
            source && source->providerId == "steam-workshop")
        {
            if (!options.unsubscribeWorkshop)
            {
                FinishTask(WidgetsPageHostOperationResult::Failure(
                    L"Steam Workshop unsubscription is unavailable."));
                return false;
            }
            const std::uint64_t taskId = activeTaskId;
            const std::uint64_t taskGeneration = generation;
            const std::uint64_t taskActivation = activation;
            const std::weak_ptr<Impl> weak = weak_from_this();
            try
            {
                options.unsubscribeWorkshop(taskGeneration, taskId,
                    source->externalItemId,
                    [weak, taskGeneration, taskActivation, taskId](
                        WidgetsPageHostOperationResult result) mutable {
                        const auto self = weak.lock();
                        if (!self) return;
                        self->MarshalToOwner(
                            [weak, taskGeneration, taskActivation, taskId,
                                result = std::move(result)]() mutable {
                                const auto owner = weak.lock();
                                if (!owner || owner->closed || !owner->active ||
                                    !widgets_page_backend_detail::
                                        CompletionIdentityMatches(
                                            taskGeneration, taskActivation,
                                            taskId, owner->generation,
                                            owner->activation,
                                            owner->activeTaskId))
                                {
                                    return;
                                }
                                owner->FinishTask(std::move(result),
                                    "app.settings.widgets_uninstall_workshop_ok");
                            });
                    });
                return true;
            }
            catch (...)
            {
                FinishTask(WidgetsPageHostOperationResult::Failure(
                    L"Steam Workshop could not be unsubscribed."));
                return false;
            }
        }

        std::vector<std::wstring> instances;
        for (const LuaWidget& widget : engine.GetWidgets())
            if (!widget.preview && widget.packageId == packageId)
                instances.push_back(widget.widgetId);
        for (const std::wstring& instance : instances)
            engine.UnloadWidget(instance);

        std::string error;
        if (!WidgetEngine::UninstallWidgetPackage(packageId, error))
        {
            // UnloadWidget removes runtime records, so ReloadWidget cannot
            // resurrect them directly. Ask DesktopApp to reconcile its
            // persisted instance table against the still-installed package.
            NotifyHostStateChanged();
            FinishTask(WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(error)));
            return false;
        }
        engine.RevokeFilesystemHandlesForPackage(packageId);
        FinishTask(WidgetsPageHostOperationResult::Success(),
            "app.settings.widgets_uninstall_ok");
        return true;
    }

    bool OpenWorkshop(const WidgetsPageRequest& request)
    {
        if (!options.openWorkshop)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"Steam Workshop is unavailable.",
                "settings.status.error");
            Publish();
            return false;
        }
        WidgetsPageHostOperationResult result;
        try
        {
            const std::string sourceId = request.sourceId.empty()
                ? "steam-workshop" : WideToUtf8(request.sourceId);
            result = options.openWorkshop(sourceId);
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L"Steam Workshop could not be opened.");
        }
        if (!result.succeeded)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(result.message), "settings.status.error");
            Publish();
        }
        return result.succeeded;
    }

    bool AddPackageToDesktop(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const auto* package = FindSnapshotPackage(request.packageId);
        if (!package || !package->canAddToDesktop ||
            !options.addPackageToDesktop)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_add_to_desktop_failed",
                    L"The component could not be added to the desktop."),
                "settings.status.error");
            Publish();
            return false;
        }
        BeginTask(WidgetsPageTaskKind::AddingToDesktop, request.packageId);
        WidgetsPageHostOperationResult result;
        try
        {
            result = options.addPackageToDesktop(request.packageId);
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("app.settings.widgets_add_to_desktop_failed",
                    L"The component could not be added to the desktop."));
        }
        const bool succeeded = result.succeeded;
        FinishTask(std::move(result),
            "app.settings.widgets_add_to_desktop_ok");
        return succeeded;
    }

    void CompleteSynchronization(std::uint64_t expectedGeneration,
        std::uint64_t expectedActivation, std::uint64_t expectedTaskId,
        WidgetsPageHostOperationResult result)
    {
        if (closed || !active ||
            !widgets_page_backend_detail::CompletionIdentityMatches(
                expectedGeneration, expectedActivation, expectedTaskId,
                generation, activation, activeTaskId))
        {
            return;
        }
        const bool succeeded = result.succeeded;
        FinishTask(std::move(result));
        if (succeeded && active && !closed)
            (void)StartSearch(requestedSearchRevision,
                requestedSearchQuery, true);
    }

    bool SynchronizeSource(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        if (!options.synchronizeSource || !options.canSynchronizeSource)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"This component source cannot be synchronized.",
                "settings.status.error");
            Publish();
            return false;
        }
        const std::string sourceId = WideToUtf8(request.sourceId);
        const auto source = std::find_if(state->sources.begin(),
            state->sources.end(), [&](const WidgetSourceGroupSnapshot& item) {
                return item.sourceId == request.sourceId &&
                    item.supportsSynchronization && item.available;
            });
        if (source == state->sources.end() ||
            !options.canSynchronizeSource(sourceId))
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L"This component source cannot be synchronized.",
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::SynchronizingWorkshop, {},
            request.sourceId,
            static_cast<bool>(options.cancelAsyncOperation));
        const std::uint64_t taskId = activeTaskId;
        const std::uint64_t taskGeneration = generation;
        const std::uint64_t taskActivation = activation;
        const std::weak_ptr<Impl> weak = weak_from_this();
        try
        {
            options.synchronizeSource(taskGeneration, taskId, sourceId,
                [weak, taskGeneration, taskActivation, taskId](
                    WidgetsPageHostOperationResult result) mutable {
                    const auto self = weak.lock();
                    if (!self) return;
                    self->MarshalToOwner(
                        [weak, taskGeneration, taskActivation, taskId,
                            result = std::move(result)]() mutable {
                            if (const auto owner = weak.lock())
                                owner->CompleteSynchronization(
                                    taskGeneration, taskActivation, taskId,
                                    std::move(result));
                        });
                });
            return true;
        }
        catch (...)
        {
            FinishTask(WidgetsPageHostOperationResult::Failure(
                L"The component source could not be synchronized."));
            return false;
        }
    }

    bool CancelTask(const WidgetsPageRequest& request)
    {
        if (request.taskId == 0 || request.taskId != activeTaskId ||
            !state->task.cancellable)
        {
            return false;
        }
        const std::uint64_t cancelledTask = activeTaskId;
        if (state->task.kind == WidgetsPageTaskKind::Searching)
        {
            if (!sourceWorker.RequestCancel(cancelledTask))
                return false;
            // A provider call already in progress may not offer a cooperative
            // cancellation API. Keep the mutation gate closed until its
            // terminal completion instead of allowing overlapping package IO.
            state->task.cancellable = false;
            state->task.status = L"Cancelling";
            Publish();
            return true;
        }
        else if (options.cancelAsyncOperation)
        {
            try
            {
                options.cancelAsyncOperation(generation, cancelledTask);
            }
            catch (...)
            {
            }
        }
        activeTaskId = 0;
        state->task = {};
        Publish();
        return true;
    }

    bool Activate(std::uint64_t value)
    {
        if (closed || !OnOwnerThread() || value == 0) return false;
        ++activation;
        active = true;
        if (generation != value)
        {
            generation = value;
            revision = 0;
            requestedSearchRevision = 0;
            requestedSearchQuery.clear();
            state = std::make_shared<WidgetsPageSnapshot>();
            state->generation = generation;
        }
        activeTaskId = 0;
        awaitingPicker = false;
        awaitingConfirmation = false;
        pickerRequestId = 0;
        confirmationRequestId = 0;
        state->task = {};
        ClearFeedback();
        (void)CaptureInstalledState();
        Publish();
        return StartSearch(0, {}, true);
    }

    void Deactivate() noexcept
    {
        if (!OnOwnerThread()) return;
        ++activation;
        active = false;
        activeTaskId = 0;
        awaitingPicker = false;
        awaitingConfirmation = false;
        pickerRequestId = 0;
        confirmationRequestId = 0;
        state->task = {};
    }

    bool Refresh()
    {
        if (closed || !active || !OnOwnerThread()) return false;
        const bool captured = CaptureInstalledState();
        Publish();
        return captured;
    }

    bool Invoke(std::uint64_t requestGeneration,
        WidgetsPageRequest request)
    {
        if (closed || !active || !OnOwnerThread() ||
            requestGeneration != generation)
        {
            return false;
        }
        switch (request.command)
        {
        case WidgetsPageCommand::BrowseInstallPackage:
            return BrowseInstallPackage();
        case WidgetsPageCommand::SearchSources:
            return StartSearch(request.searchRevision,
                std::move(request.query));
        case WidgetsPageCommand::CancelTask:
            return CancelTask(request);
        case WidgetsPageCommand::InstallCatalogItem:
            return InstallCatalogItem(request);
        case WidgetsPageCommand::SetPackageEnabled:
            return SetPackageEnabled(request);
        case WidgetsPageCommand::UninstallPackage:
            return UninstallPackage(request);
        case WidgetsPageCommand::SetPermissionDecision:
            return SetPermissionDecision(request);
        case WidgetsPageCommand::SetDevelopmentOverride:
            return SetDevelopmentOverride(request);
        case WidgetsPageCommand::OpenWorkshop:
            return OpenWorkshop(request);
        case WidgetsPageCommand::SynchronizeSource:
            return SynchronizeSource(request);
        case WidgetsPageCommand::AddPackageToDesktop:
            return AddPackageToDesktop(request);
        default:
            return false;
        }
    }

    void Close() noexcept
    {
        if (closed.exchange(true)) return;
        active = false;
        ++activation;
        activeTaskId = 0;
        awaitingPicker = false;
        awaitingConfirmation = false;
        options.snapshotChanged = {};
        sourceWorker.Shutdown();
        outstandingSearches.clear();
    }
};

WidgetsPageBackend::WidgetsPageBackend(
    WidgetEngine& engine, WidgetsPageBackendOptions options)
    : impl_(std::make_shared<Impl>(engine, std::move(options)))
{
}

WidgetsPageBackend::~WidgetsPageBackend()
{
    Close();
}

void WidgetsPageBackend::SetSnapshotChangedCallback(
    WidgetsPageBackendOptions::SnapshotChangedCallback callback)
{
    if (impl_ && !impl_->closed && impl_->OnOwnerThread())
        impl_->options.snapshotChanged = std::move(callback);
}

bool WidgetsPageBackend::Activate(std::uint64_t generation)
{
    return impl_ && impl_->Activate(generation);
}

void WidgetsPageBackend::Deactivate() noexcept
{
    if (impl_) impl_->Deactivate();
}

bool WidgetsPageBackend::Refresh()
{
    return impl_ && impl_->Refresh();
}

bool WidgetsPageBackend::Invoke(
    std::uint64_t generation, WidgetsPageRequest request)
{
    return impl_ && impl_->Invoke(generation, std::move(request));
}

std::shared_ptr<const WidgetsPageSnapshot>
WidgetsPageBackend::Snapshot() const noexcept
{
    return impl_ ? std::make_shared<const WidgetsPageSnapshot>(
                       *impl_->state)
                 : nullptr;
}

std::uint64_t WidgetsPageBackend::Generation() const noexcept
{
    return impl_ ? impl_->generation : 0;
}

bool WidgetsPageBackend::IsGenerationCurrent(
    std::uint64_t generation) const noexcept
{
    return impl_ && impl_->active && !impl_->closed &&
        impl_->generation == generation;
}

void WidgetsPageBackend::Close() noexcept
{
    if (impl_) impl_->Close();
}

} // namespace snowdesktop::winui
