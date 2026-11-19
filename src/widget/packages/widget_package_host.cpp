#include "widget_package_host.h"
#include "widget_package_manager.h"
#include "widget_package_sources.h"

#include "common/utils.h"
#include "data/data_paths.h"
#include "diagnostics/diagnostic_log.h"
#include "diagnostics/performance_trace.h"
#include "steam/steam_entitlement.h"
#include "steam/steam_workshop_diagnostics.h"
#include "steam/steam_workshop_source.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/runtime/widget_engine.h"
#include "widget/storage/widget_storage_runtime.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
void TrimWidgetPackageHistory(snowdesktop::widget::WidgetPackageManager& manager,
    std::string_view packageId)
{
    std::string error;
    if (!manager.TrimInstalledVersions(error, packageId))
        WriteDiagnosticLogEntry(Utf8ToWide(error).c_str(), DiagnosticLogLevel::Warning);
}

snowdesktop::widget::WidgetPackageManager& GetWidgetPackageManager()
{
    static snowdesktop::widget::WidgetPackageManager manager;
    static const bool initialized = [] {
        std::string error;
        return manager.Initialize(error);
    }();
    (void)initialized;
    return manager;
}

namespace
{
static std::unordered_map<std::string,
    std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>>&
GetWidgetPackageSources()
{
    static std::unordered_map<std::string,
        std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>> sources;
    static const bool initialized = [&]
    {
        auto& manager = GetWidgetPackageManager();
        auto builtin =
            std::make_shared<snowdesktop::widget::BuiltinPackageSource>(
                manager);
        sources[builtin->ProviderId()] = builtin;
        auto local =
            std::make_shared<snowdesktop::widget::LocalDirectorySource>(
                manager.Paths().development);
        sources[local->ProviderId()] = local;
        auto steam =
            std::make_shared<snowdesktop::widget::SteamWorkshopSource>();
        sources[steam->ProviderId()] = steam;
        return true;
    }();
    (void)initialized;
    return sources;
}

struct SteamWorkshopPackageAssociationCache
{
    struct RejectedArchive
    {
        std::string digest;
        std::string error;
    };
    std::mutex mutex;
    std::unordered_map<std::string, std::string> associations;
    std::vector<snowdesktop::widget::SteamWorkshopInstallFailure>
        installFailures;
    std::unordered_map<std::string, RejectedArchive> rejectedArchives;
};

static SteamWorkshopPackageAssociationCache&
GetSteamWorkshopPackageAssociationCache()
{
    static SteamWorkshopPackageAssociationCache cache;
    return cache;
}

static std::optional<std::string> RejectedSteamWorkshopArchive(
    const snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
    const std::string& item)
{
    const auto digest = snapshot.archiveDigests.find(item);
    if (digest == snapshot.archiveDigests.end() || digest->second.empty())
        return std::nullopt;
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    const auto rejected = cache.rejectedArchives.find(item);
    if (rejected == cache.rejectedArchives.end() || rejected->second.digest != digest->second)
        return std::nullopt;
    return rejected->second.error;
}

static void RememberRejectedSteamWorkshopArchive(const std::string& item,
    std::string digest, std::string error)
{
    if (digest.empty()) return;
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    if (cache.rejectedArchives.size() >= 256 && !cache.rejectedArchives.contains(item))
        cache.rejectedArchives.erase(cache.rejectedArchives.begin());
    cache.rejectedArchives[item] = {std::move(digest), std::move(error)};
}

static void ForgetRejectedSteamWorkshopArchive(const std::string& item)
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    cache.rejectedArchives.erase(item);
}

static void CacheSteamWorkshopInstallFailure(
    snowdesktop::widget::SteamWorkshopInstallFailure failure)
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    auto existing = std::find_if(cache.installFailures.begin(),
        cache.installFailures.end(), [&](const auto& candidate)
        {
            return candidate.packageId == failure.packageId;
        });
    if (existing == cache.installFailures.end())
        cache.installFailures.push_back(std::move(failure));
    else
        *existing = std::move(failure);
}

static void ClearSteamWorkshopInstallFailure(
    const std::string& packageId)
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    std::erase_if(cache.installFailures, [&](const auto& failure)
    {
        return failure.packageId == packageId;
    });
}

}

std::wstring ResolveWidgetPath(const std::wstring& packageId)
{
    auto& manager = GetWidgetPackageManager();
    const auto id = WideToUtf8(packageId);
    const auto entry = snowdesktop::widget_runtime::IsDryLoad()
        ? manager.ResolveSelectedEntry(id) : manager.ResolveEntry(id);
    return entry ? entry->wstring() : std::wstring{};
}

bool RecoverWidgetPackage(const std::string& packageId,
    const std::optional<std::string>& preferredVersion,
    std::string_view rejectedVersion)
{
    auto& manager = GetWidgetPackageManager();
    const auto packages = manager.ListPackages();
    auto tryVersion = [&](const std::string& version) {
        std::string error;
        return manager.Rollback(packageId, version, error);
    };
    if (preferredVersion && *preferredVersion != rejectedVersion && tryVersion(*preferredVersion))
        return true;
    std::vector<std::string> candidates;
    bool activeUserPackage = false;
    for (const auto& package : packages)
    {
        if (package.manifest.id != packageId ||
            package.builtin || package.development)
            continue;
        if (package.active) activeUserPackage = true;
        else if (package.manifest.version != rejectedVersion)
            candidates.push_back(package.manifest.version);
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const std::string& left, const std::string& right)
        {
            return snowdesktop::widget::WidgetPackageValidator::
                IsNewerSemVer(left, right);
        });
    for (const auto& version : candidates)
        if (tryVersion(version)) return true;
    if (activeUserPackage)
    {
        std::string error;
        if (manager.Uninstall(packageId, error))
            return manager.ResolveSelected(packageId).has_value();
    }
    return false;
}

std::optional<snowdesktop::widget::InstalledPackage>
ResolveWidgetEntryPackage(const std::filesystem::path& path)
{
    auto& manager = GetWidgetPackageManager();
    return snowdesktop::widget_runtime::IsDryLoad()
        ? manager.ResolveSelectedEntryPath(path) : manager.ResolveEntryPath(path);
}

}

using namespace snowdesktop::widget_runtime;

bool WidgetEngine::InstallWidgetPackage(const std::wstring& manifestPath,
    std::wstring& error, bool allowSourceChange,
    bool allowPermissionExpansion,
    snowdesktop::widget::InstalledPackage* installedResult)
{
    const std::filesystem::path input(manifestPath);
    auto& manager = GetWidgetPackageManager();
    snowdesktop::widget::InstalledPackage installed;
    snowdesktop::widget::ValidationReport report;
    snowdesktop::widget::PackageManifest manifest;
    std::string installError;
    bool ok = false;
    if (_wcsicmp(input.extension().c_str(), L".snowwidget") == 0)
    {
        report = manager.ValidateArchive(input, &manifest);
        if (report.Ok())
            ok = manager.InstallArchive(input,
                { "local-import", manifest.id },
                allowSourceChange, installed, report, installError,
                allowPermissionExpansion);
    }
    else
    {
        const std::filesystem::path root =
            _wcsicmp(input.filename().c_str(), L"widget.json") == 0
            ? input.parent_path() : input;
        report = manager.ValidateDirectory(root, &manifest);
        if (report.Ok())
            ok = manager.InstallDirectory(root,
                { "local-import", manifest.id },
                allowSourceChange, installed, report, installError,
                allowPermissionExpansion);
    }
    if (!ok)
    {
        if (installError.empty()) installError = "package validation failed";
        error = Utf8ToWide(installError);
        if (!report.Ok())
            error += L"\n" + Utf8ToWide(report.ToJson());
        return false;
    }
    if (installedResult) *installedResult = installed;
    error.clear();
    return true;
}

bool WidgetEngine::VerifyInstalledWidgetPackage(const std::string& packageId,
    const std::optional<std::string>& previousVersion, std::wstring& error,
    std::uint64_t diagnosticQueryId,
    const snowdesktop::widget::InstalledPackage* workshopCandidate,
    bool* deterministicFailure)
{
    const auto trace = [&](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(diagnosticQueryId,
            "package=" + packageId + " " + std::string(message));
    };
    trace("phase=verify.begin previous=" + previousVersion.value_or("none"));
    if (snowdesktop::widget_runtime::HasStorageOverlay())
    {
        error = L"Another component storage migration is already running.";
        trace("phase=verify.end result=failed reason=storage_overlay_busy");
        return false;
    }

    const auto selected = GetWidgetPackageManager().ResolveSelected(packageId);
    if (!selected)
    {
        error = L"The installed component selection is unavailable.";
        trace("phase=verify.end result=failed reason=selection_unavailable");
        return false;
    }
    const bool verifyingWorkshopCandidate = workshopCandidate &&
        selected->root == workshopCandidate->root;
    if (!verifyingWorkshopCandidate) deterministicFailure = nullptr;
    std::vector<std::wstring> liveInstances;
    for (const auto& widget : widgets_)
        if (selected->enabled && widget.packageId == packageId)
            liveInstances.push_back(widget.widgetId);

    std::unordered_map<std::string, std::string> migratedStorage = PersistentStorage();
    std::vector<std::wstring> updatedInstances;
    bool ok = true;
    std::string failure;
    {
        StorageOverlayScope migrationScope(&migratedStorage);
        if (liveInstances.empty())
        {
            const std::wstring dryId = Utf8ToWide(
                "__package_dry_load_" +
                snowdesktop::widget::WidgetPackageManager::GenerateUuid());
            DryLoadScope dryLoadScope;
            try
            {
                trace("phase=dry_load.begin version=" + selected->manifest.version);
                const std::wstring path =
                    ResolveWidgetPath(Utf8ToWide(packageId));
                ok = !path.empty() &&
                    LoadWidget(path, dryId, false, nullptr, nullptr, nullptr, deterministicFailure);
                if (!ok)
                {
                    if (const auto runtimeFailure = GetWidgetRuntimeFailure(dryId))
                        failure = runtimeFailure->detail;
                }
                trace("phase=dry_load.end result=" + std::string(ok ? "ok" : "failed") +
                    " pathResolved=" + std::to_string(!path.empty()) + " error=" + failure);
            }
            catch (...)
            {
                DeleteWidgetInstance(dryId);
                throw;
            }
            DeleteWidgetInstance(dryId);
        }
        else
        {
            for (const auto& widgetId : liveInstances)
            {
                if (!ReloadWidget(widgetId, deterministicFailure))
                {
                    ok = false;
                    if (const auto runtimeFailure = GetWidgetRuntimeFailure(widgetId))
                        failure = runtimeFailure->detail;
                    break;
                }
                updatedInstances.push_back(widgetId);
            }
        }
    }
    if (ok)
    {
        MutablePersistentStorage() = std::move(migratedStorage);
        SaveStorageFile();
        TrimWidgetPackageHistory(GetWidgetPackageManager(), packageId);
        error.clear();
        trace("phase=verify.end result=ok");
        return true;
    }

    const bool rejectedManaged = !selected->builtin && !selected->development;
    const bool recovered = RecoverWidgetPackage(packageId, previousVersion,
        rejectedManaged ? std::string_view(selected->manifest.version) : std::string_view{});
    trace("phase=verify.recovery recovered=" + std::to_string(recovered) +
        " retained=" + std::to_string(GetWidgetPackageManager().ContainsPackage(packageId)));
    for (const auto& widgetId : updatedInstances)
        (void)ReloadWidget(widgetId);
    if (rejectedManaged && recovered)
    {
        // A retained failed version otherwise satisfies the next sync plan.
        std::string quarantineError;
        if (!GetWidgetPackageManager().QuarantineInactiveVersion(*selected, quarantineError))
            trace("phase=verify.quarantine result=failed error=" + quarantineError);
    }
    if (failure.empty()) failure = "Component load or storage migration failed.";
    if (recovered) failure += "\nRestored last-known-good component version.";
    error = Utf8ToWide(failure);
    trace("phase=verify.end result=failed error=" + WideToUtf8(error));
    return false;
}

bool WidgetEngine::InstallAndVerifyWidgetPackage(const std::wstring& path,
    std::wstring& error, bool allowSourceChange,
    bool allowPermissionExpansion)
{
    snowdesktop::widget::PackageManifest incoming;
    snowdesktop::widget::ValidationReport report;
    const std::filesystem::path input(path);
    auto& manager = GetWidgetPackageManager();
    if (_wcsicmp(input.extension().c_str(), L".snowwidget") == 0)
        report = manager.ValidateArchive(input, &incoming);
    else
        report = manager.ValidateDirectory(
            _wcsicmp(input.filename().c_str(), L"widget.json") == 0
                ? input.parent_path() : input, &incoming);
    if (!report.Ok())
    {
        error = Utf8ToWide(report.ToJson());
        return false;
    }
    std::optional<std::string> previousVersion;
    if (const auto previous = manager.ResolveSelected(incoming.id))
        if (!previous->builtin && !previous->development)
            previousVersion = previous->manifest.version;
    snowdesktop::widget::InstalledPackage installed;
    if (!InstallWidgetPackage(path, error, allowSourceChange,
        allowPermissionExpansion, &installed))
        return false;
    return VerifyInstalledWidgetPackage(
        installed.manifest.id, previousVersion, error);
}

snowdesktop::widget::ProviderStatus
WidgetEngine::GetStaticWidgetCatalogStatus(
    const std::filesystem::path& catalogPath)
{
    snowdesktop::widget::StaticCatalogSource source(catalogPath);
    return source.Status();
}

void WidgetEngine::RegisterWidgetPackageSource(
    std::shared_ptr<snowdesktop::widget::IWidgetPackageSource> source)
{
    if (!source || source->ProviderId().empty()) return;
    GetWidgetPackageSources()[source->ProviderId()] = std::move(source);
}

bool WidgetEngine::ConfigureStaticWidgetCatalog(
    const std::filesystem::path& catalogPath, std::string& error)
{
    auto source =
        std::make_shared<snowdesktop::widget::StaticCatalogSource>(
            catalogPath);
    const auto status = source->Status();
    if (!status.available)
    {
        error = status.message;
        return false;
    }
    RegisterWidgetPackageSource(source);
    error.clear();
    return true;
}

std::vector<snowdesktop::widget::PackageSourceInfo>
WidgetEngine::ListWidgetPackageSources()
{
    std::vector<snowdesktop::widget::PackageSourceInfo> result;
    for (const auto& [providerId, source] : GetWidgetPackageSources())
        result.push_back({
            providerId, source->Capabilities(), source->Status() });
    std::sort(result.begin(), result.end(),
        [](const auto& left, const auto& right)
        {
            return left.providerId < right.providerId;
        });
    return result;
}

std::vector<std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>>
WidgetEngine::SnapshotWidgetPackageSourceProviders()
{
    std::vector<std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>> result;
    for (const auto& [id, source] : GetWidgetPackageSources())
    {
        (void)id;
        result.push_back(source);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left->ProviderId() < right->ProviderId();
    });
    return result;
}

std::vector<snowdesktop::widget::PackageDetails>
WidgetEngine::QueryWidgetPackageSource(const std::string& providerId,
    const snowdesktop::widget::PackageQuery& query, std::string& error)
{
    const auto source = GetWidgetPackageSources().find(providerId);
    if (source == GetWidgetPackageSources().end())
    {
        error = "component source is not registered: " + providerId;
        return {};
    }
    return source->second->Query(query, error);
}

bool WidgetEngine::IsSteamWorkshopBridgeAvailable()
{
    const std::filesystem::path bridge =
        std::filesystem::path(GetExecutableDirectoryPath()) /
        L"SnowDesktopSteamBridge.exe";
    static const bool compatible = snowdesktop::steam_entitlement::
        IsSteamBridgeExecutableCompatible(bridge, SNOWDESKTOP_VERSION);
    return compatible;
}

bool WidgetEngine::UnsubscribeSteamWorkshopItem(
    const std::string& externalItemId, std::string& error)
{
    const auto source = GetWidgetPackageSources().find("steam-workshop");
    if (source == GetWidgetPackageSources().end())
    {
        error = "Steam Workshop source is unavailable";
        return false;
    }
    const auto workshop = std::dynamic_pointer_cast<
        snowdesktop::widget::SteamWorkshopSource>(source->second);
    if (!workshop)
    {
        error = "Steam Workshop source is incompatible";
        return false;
    }
    return workshop->Unsubscribe(externalItemId, error);
}

snowdesktop::widget::SteamWorkshopSubscriptionSnapshot
WidgetEngine::QuerySteamWorkshopSubscriptions(
    const std::string& locale, std::uint64_t diagnosticQueryId)
{
    const auto trace = [diagnosticQueryId](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(diagnosticQueryId, message);
    };
    trace("phase=query.begin host=" SNOWDESKTOP_VERSION " locale=" + locale +
        " exe=" + WideToUtf8(GetExecutableDirectoryPath()) +
        " data=" + WideToUtf8(GetDataDirectoryPath()));
    snowdesktop::widget::SteamWorkshopSubscriptionSnapshot snapshot;
    try
    {
        snowdesktop::widget::SteamWorkshopSource source;
        snowdesktop::widget::PackageQuery query;
        query.locale = locale;
        query.limit = std::numeric_limits<std::size_t>::max();
        std::string error;
        // Runtime subscription reconciliation is deliberately local-only.
        // Steam's ACF is the subscription authority and content/<appid> is the
        // download authority; SteamAPI is reserved for explicit user actions.
        snapshot = source.QuerySubscriptions(query, error, trace);
        if (!error.empty()) snapshot.error = std::move(error);
        if (snapshot.authoritative)
        {
            auto associations = snowdesktop::widget::
                BuildSteamWorkshopPackageAssociations(snapshot);
            auto& cache = GetSteamWorkshopPackageAssociationCache();
            std::lock_guard lock(cache.mutex);
            cache.associations = std::move(associations);
        }
    }
    catch (const std::exception& exception)
    {
        snapshot = {};
        snapshot.error = "Steam Workshop subscription query failed: ";
        snapshot.error += exception.what();
    }
    catch (...)
    {
        snapshot = {};
        snapshot.error = "Steam Workshop subscription query failed";
    }
    snapshot.diagnosticQueryId = diagnosticQueryId;
    trace("phase=query.end subscribed=" + std::to_string(snapshot.subscribedPublishedFileIds.size()) +
        " installable=" + std::to_string(snapshot.installable.size()) +
        " failures=" + std::to_string(snapshot.discoveryFailures.size()) +
        " warning=" + snapshot.warning + " error=" + snapshot.error);
    return snapshot;
}

snowdesktop::widget::SteamWorkshopSubscriptionHistory
WidgetEngine::GetSteamWorkshopSubscriptionHistory()
{
    return GetWidgetPackageManager().SteamSubscriptionHistory();
}

snowdesktop::widget::SteamLibrarySubscriptionHistory
WidgetEngine::GetSteamWorkshopLibrarySubscriptionHistory()
{
    return GetWidgetPackageManager().SteamLibraryHistory();
}

snowdesktop::widget::InstalledPackageCatalog WidgetEngine::SnapshotWidgetPackageCatalog()
{
    return GetWidgetPackageManager().CatalogSnapshot();
}

snowdesktop::widget::SteamWorkshopSyncPlan WidgetEngine::PrepareSteamWorkshopSubscriptionArtifacts(
    snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
    const std::vector<snowdesktop::widget::InstalledPackage>& installed,
    const std::filesystem::path& stagingRoot)
{
    if (!snapshot.CanSynchronize()) return {};
    auto plan = snowdesktop::widget::BuildSteamWorkshopSyncPlan(
        installed, snapshot);
    const auto trace = [&](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(snapshot.diagnosticQueryId, message);
    };
    trace("phase=prepare.begin registered=" + std::to_string(installed.size()) +
        " actions=" + std::to_string(plan.actions.size()) +
        " conflicts=" + std::to_string(plan.conflicts.size()));
    for (const auto& package : installed)
    {
        if (std::none_of(snapshot.installable.begin(), snapshot.installable.end(),
            [&](const auto& available) { return available.manifest.id == package.manifest.id; }))
            continue;
        trace("phase=registry.candidate package=" + package.manifest.id +
            " version=" + package.manifest.version +
            " provider=" + package.source.providerId +
            " item=" + package.source.externalItemId +
            " active=" + std::to_string(package.active) +
            " builtin=" + std::to_string(package.builtin) +
            " development=" + std::to_string(package.development) +
            " unregistered=" + std::to_string(package.unregistered) +
            " path=" + WideToUtf8(package.root.wstring()));
    }
    for (const auto& conflict : plan.conflicts)
        trace("phase=plan.conflict error=" + conflict);
    if (plan.actions.empty())
    {
        trace("phase=prepare.end prepared=0 errors=0");
        return plan;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(stagingRoot, filesystemError);
    if (filesystemError)
    {
        snapshot.preparationErrors.push_back(
            "cannot create local Workshop staging directory: " +
            filesystemError.message());
        trace("phase=prepare.end result=failed error=" + filesystemError.message());
        return plan;
    }

    for (const auto& action : plan.actions)
    {
        trace("phase=plan.action item=" + action.externalItemId + " package=" + action.packageId +
            " kind=" + snowdesktop::widget::SteamWorkshopDiagnosticActionName(action.kind) +
            " version=" + action.version);
        if (action.kind == snowdesktop::widget::
                SteamWorkshopSyncActionKind::Uninstall)
            continue;
        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(action.externalItemId);
        if (RejectedSteamWorkshopArchive(snapshot, publishedFileId))
            continue;
        const auto artifact =
            snapshot.localArtifacts.find(publishedFileId);
        if (artifact == snapshot.localArtifacts.end())
        {
            snapshot.preparationErrors.push_back(action.packageId +
                ": detected Workshop package has no local artifact");
            continue;
        }
        const auto destination = stagingRoot /
            Utf8ToWide("workshop-" + action.packageId + "-" +
                snowdesktop::widget::WidgetPackageManager::GenerateUuid() +
                ".snowwidget");
        filesystemError.clear();
        trace("phase=stage.begin item=" + action.externalItemId);
        std::filesystem::copy_file(artifact->second, destination,
            std::filesystem::copy_options::overwrite_existing,
            filesystemError);
        if (filesystemError)
        {
            snapshot.preparationErrors.push_back(action.packageId +
                ": cannot stage detected Workshop package: " +
                filesystemError.message());
            trace("phase=stage.end item=" + action.externalItemId +
                " result=failed error=" + filesystemError.message());
            continue;
        }
        const auto digest = snapshot.archiveDigests.find(publishedFileId);
        if (digest != snapshot.archiveDigests.end() && !digest->second.empty() &&
            snowdesktop::widget::WidgetPackageManager::Sha256File(destination) != digest->second)
        {
            std::filesystem::remove(destination, filesystemError);
            snapshot.preparationErrors.push_back(action.packageId +
                ": Workshop archive changed while staging");
            continue;
        }
        snapshot.preparedArtifacts[publishedFileId] = destination;
        trace("phase=stage.end item=" + action.externalItemId + " result=ok");
    }
    trace("phase=prepare.end prepared=" + std::to_string(snapshot.preparedArtifacts.size()) +
        " errors=" + std::to_string(snapshot.preparationErrors.size()));
    return plan;
}

std::unordered_map<std::string, std::string>
WidgetEngine::CachedSteamWorkshopPackageAssociations()
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    return cache.associations;
}

std::vector<snowdesktop::widget::SteamWorkshopInstallFailure>
WidgetEngine::CachedSteamWorkshopInstallFailures()
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    return cache.installFailures;
}

snowdesktop::widget::SteamWorkshopSyncResult
WidgetEngine::ApplySteamWorkshopSubscriptions(
    const snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
    const snowdesktop::widget::InstalledPackageCatalog& preparedCatalog,
    const snowdesktop::widget::SteamWorkshopSyncPlan& preparedPlan)
{
    auto& manager = GetWidgetPackageManager();
    const auto currentCatalog = manager.CatalogSnapshot();
    std::optional<snowdesktop::widget::SteamWorkshopSyncPlan> updatedPlan;
    if (currentCatalog != preparedCatalog)
    {
        snowdesktop::performance::Scope planTrace("shared.maintenance", "steam.apply.plan");
        updatedPlan.emplace(snowdesktop::widget::BuildSteamWorkshopSyncPlan(*currentCatalog, snapshot));
    }
    const auto& plan = updatedPlan ? *updatedPlan : preparedPlan;
    const auto trace = [&](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(snapshot.diagnosticQueryId, message);
    };
    trace("phase=apply.begin actions=" + std::to_string(plan.actions.size()) +
        " conflicts=" + std::to_string(plan.conflicts.size()) +
        " prepared=" + std::to_string(snapshot.preparedArtifacts.size()));
    for (const auto& conflict : plan.conflicts)
        trace("phase=plan.conflict error=" + conflict);
    snowdesktop::widget::SteamWorkshopSyncResult result;
    result.warnings = plan.conflicts;
    result.installFailures = snapshot.discoveryFailures;
    for (const auto& failure : snapshot.discoveryFailures)
    {
        result.errors.push_back(failure.packageId + ": " + failure.error);
    }
    result.errors.insert(result.errors.end(),
        snapshot.preparationErrors.begin(), snapshot.preparationErrors.end());
    for (const auto& action : plan.actions)
    {
        snowdesktop::performance::Scope actionTrace("shared.maintenance", "steam.apply.package_action");
        trace("phase=apply.action item=" + action.externalItemId + " package=" + action.packageId +
            " kind=" + snowdesktop::widget::SteamWorkshopDiagnosticActionName(action.kind) +
            " version=" + action.version);
        const auto recordInstallFailure = [&](std::string error)
        {
            snowdesktop::widget::SteamWorkshopInstallFailure failure;
            failure.packageId = action.packageId;
            failure.externalItemId = action.externalItemId;
            failure.manifest = action.expectedManifest;
            failure.error = std::move(error);
            result.installFailures.push_back(std::move(failure));
        };
        if (action.kind ==
            snowdesktop::widget::SteamWorkshopSyncActionKind::Uninstall)
        {
            std::vector<std::wstring> instances;
            for (const auto& widget : widgets_)
                if (widget.packageId == action.packageId)
                    instances.push_back(widget.widgetId);
            for (const auto& instance : instances) UnloadWidget(instance);
            std::string error;
            if (manager.Uninstall(action.packageId, error))
            {
                if (filesystemHandleStore_)
                {
                    std::string revokeError;
                    (void)filesystemHandleStore_->RevokePackage(
                        action.packageId, revokeError);
                }
                ++result.uninstalled;
            }
            else
            {
                for (const auto& instance : instances)
                    (void)EnsureWidgetLoaded(instance, Utf8ToWide(action.packageId));
                result.errors.push_back(action.packageId + ": " + error);
            }
            continue;
        }

        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(action.externalItemId);
        if (const auto rejected = RejectedSteamWorkshopArchive(snapshot, publishedFileId))
        {
            result.errors.push_back(action.packageId + ": " + *rejected);
            recordInstallFailure(*rejected);
            continue;
        }
        const auto prepared =
            snapshot.preparedArtifacts.find(publishedFileId);
        if (prepared == snapshot.preparedArtifacts.end())
        {
            const std::string error =
                "detected Workshop package was not prepared locally";
            result.errors.push_back(action.packageId + ": " + error);
            recordInstallFailure(error);
            continue;
        }

        std::optional<std::string> previousVersion;
        if (const auto previous = manager.ResolveSelected(action.packageId))
            if (!previous->builtin && !previous->development)
                previousVersion = previous->manifest.version;
        snowdesktop::widget::InstalledPackage installed;
        snowdesktop::widget::ValidationReport report;
        std::string installError;
        const snowdesktop::widget::PackageSourceRef sourceRef{
            "steam-workshop", action.externalItemId };
        trace("phase=install.begin item=" + action.externalItemId + " package=" + action.packageId);
        const bool installedOk = manager.InstallArchive(prepared->second,
            sourceRef, false, installed, report, installError, false,
            &action.expectedManifest);
        trace("phase=install.end item=" + action.externalItemId + " package=" + action.packageId +
            " result=" + (installedOk ? "ok" : "failed") + " error=" + installError);
        std::error_code cleanupError;
        std::filesystem::remove(prepared->second, cleanupError);
        std::wstring error;
        bool deterministicFailure = false;
        const bool verified = installedOk && VerifyInstalledWidgetPackage(
            installed.manifest.id, previousVersion, error, snapshot.diagnosticQueryId,
            &installed, &deterministicFailure);
        if (installedOk && verified)
        {
            ForgetRejectedSteamWorkshopArchive(publishedFileId);
            if (action.kind ==
                snowdesktop::widget::SteamWorkshopSyncActionKind::Install)
                ++result.installed;
            else
                ++result.updated;
        }
        else
        {
            std::string displayError;
            if (!installedOk)
            {
                error = Utf8ToWide(installError);
                if (!report.Ok())
                    error += L"\n" + Utf8ToWide(report.ToJson());
                const auto issue = std::find_if(report.issues.begin(),
                    report.issues.end(), [](const auto& candidate)
                    {
                        return candidate.severity == snowdesktop::widget::
                            ValidationSeverity::Error;
                    });
                if (issue != report.issues.end())
                    displayError = "[" + issue->code + "] " +
                        issue->message;
            }
            const std::string fullError = WideToUtf8(error);
            if (displayError.empty()) displayError = fullError;
            if (deterministicFailure)
            {
                const auto digest = snapshot.archiveDigests.find(publishedFileId);
                if (digest != snapshot.archiveDigests.end())
                    RememberRejectedSteamWorkshopArchive(
                        publishedFileId, digest->second, displayError);
            }
            result.errors.push_back(action.packageId + ": " + fullError);
            recordInstallFailure(std::move(displayError));
        }
    }
    for (const auto& [publishedFileId, artifact] :
        snapshot.preparedArtifacts)
    {
        (void)publishedFileId;
        std::error_code cleanupError;
        std::filesystem::remove(artifact, cleanupError);
    }
    // Subscription identities describe the local manifest independently of
    // whether each package can be applied. Persist them even when one item
    // conflicts or fails; remaining managed copies are retried from their
    // installed source bindings on the next reconciliation.
    if (snapshot.CanSynchronize() &&
        !snapshot.activeSteamAccountId.empty())
    {
        std::string historyError;
        const auto [subscriptionHistory, libraryHistory] = [&] {
            snowdesktop::performance::Scope historyTrace("shared.maintenance", "steam.apply.history_build");
            return std::pair(snowdesktop::widget::BuildSteamWorkshopSubscriptionHistory(
                    snapshot, manager.SteamSubscriptionHistory()),
                snowdesktop::widget::BuildSteamWorkshopLibraryHistory(
                    snapshot, manager.SteamLibraryHistory(), *manager.CatalogSnapshot()));
        }();
        snowdesktop::performance::Scope historySaveTrace("shared.maintenance", "steam.apply.history_save");
        if (!manager.UpdateSteamSubscriptionHistory(
                snapshot.activeSteamAccountId, subscriptionHistory, historyError,
                &libraryHistory))
        {
            result.errors.push_back(
                "cannot save Steam subscription history: " + historyError);
        }
    }
    if (snapshot.authoritative || snapshot.partial)
    {
        auto& cache = GetSteamWorkshopPackageAssociationCache();
        std::lock_guard lock(cache.mutex);
        if (snapshot.authoritative)
            cache.installFailures = result.installFailures;
        else
        {
            std::erase_if(cache.installFailures, [&](const auto& failure) {
                const auto id = snowdesktop::widget::SteamPublishedFileId(failure.externalItemId);
                return std::find(snapshot.subscribedPublishedFileIds.begin(),
                    snapshot.subscribedPublishedFileIds.end(), id) !=
                    snapshot.subscribedPublishedFileIds.end();
            });
            cache.installFailures.insert(cache.installFailures.end(),
                result.installFailures.begin(), result.installFailures.end());
        }
    }
    for (const auto& error : result.errors)
        trace("phase=apply.error error=" + error);
    trace("phase=apply.end installed=" + std::to_string(result.installed) +
        " updated=" + std::to_string(result.updated) +
        " uninstalled=" + std::to_string(result.uninstalled) +
        " errors=" + std::to_string(result.errors.size()) +
        " registered=" + std::to_string(manager.CatalogSnapshot()->size()));
    return result;
}

int WidgetEngine::ApplySafeWidgetPackageUpdates(
    const std::string& providerId, std::string& report)
{
    report.clear();
    const auto source = GetWidgetPackageSources().find(providerId);
    if (source == GetWidgetPackageSources().end()) return 0;
    std::vector<snowdesktop::widget::PackageVersionRef> current;
    for (const auto& package : GetWidgetPackageManager().ListPackages())
    {
        if (!package.active || package.builtin || package.development ||
            package.source.providerId != providerId)
            continue;
        current.push_back({
            package.manifest.id, package.manifest.version });
    }
    if (current.empty()) return 0;
    std::string sourceError;
    const auto updates =
        source->second->CheckUpdates(current, sourceError);
    if (!sourceError.empty())
    {
        report = sourceError;
        return 0;
    }
    int applied = 0;
    for (const auto& update : updates)
    {
        std::wstring installError;
        if (InstallAndVerifyWidgetPackageFromSource(providerId,
            update.available.source.externalItemId,
            update.available.manifest.version, installError, false, false))
        {
            ++applied;
        }
        else
        {
            if (!report.empty()) report += '\n';
            report += update.current.packageId + ": " +
                WideToUtf8(installError);
        }
    }
    return applied;
}

std::vector<snowdesktop::widget::PackageDetails>
WidgetEngine::QueryStaticWidgetCatalog(
    const std::filesystem::path& catalogPath,
    const snowdesktop::widget::PackageQuery& query, std::string& error)
{
    snowdesktop::widget::StaticCatalogSource source(catalogPath);
    return source.Query(query, error);
}

bool WidgetEngine::InstallAndVerifyWidgetPackageFromSource(
    const std::string& providerId, const std::string& externalItemId,
    const std::string& version, std::wstring& error,
    bool allowSourceChange, bool allowPermissionExpansion, bool retryWorkshop)
{
    const auto source = GetWidgetPackageSources().find(providerId);
    if (source == GetWidgetPackageSources().end())
    {
        error = Utf8ToWide(
            "component source is not registered: " + providerId);
        return false;
    }
    const bool workshop = providerId == "steam-workshop";
    const std::string workshopItem = workshop
        ? snowdesktop::widget::SteamPublishedFileId(externalItemId) : std::string{};
    std::optional<snowdesktop::widget::SteamWorkshopSource> freshWorkshopSource;
    auto* provider = source->second.get();
    if (workshop && retryWorkshop)
    {
        // Manual installation bypasses background suppression. Keep the
        // previous entry until success so a staged poll cannot lose its reason.
        provider = &freshWorkshopSource.emplace();
    }
    std::string sourceError;
    const auto details = version.empty()
        ? provider->GetDetails(externalItemId, sourceError)
        : provider->GetVersionDetails(
            externalItemId, version, sourceError);
    if (!details)
    {
        error = Utf8ToWide(sourceError);
        return false;
    }
    const std::string requestedVersion = version.empty()
        ? details->manifest.version : version;
    std::optional<std::string> previousVersion;
    auto& manager = GetWidgetPackageManager();
    if (const auto previous = manager.ResolveSelected(details->manifest.id))
        if (!previous->builtin && !previous->development)
            previousVersion = previous->manifest.version;
    if (workshop && retryWorkshop)
    {
        // Old hosts kept rejected versions alongside the active fallback.
        for (const auto& package : manager.ListPackages())
        {
            if (package.active || package.builtin || package.development ||
                package.manifest.id != details->manifest.id ||
                package.manifest.version != requestedVersion ||
                package.source.providerId != providerId ||
                snowdesktop::widget::SteamPublishedFileId(package.source.externalItemId) != workshopItem)
                continue;
            if (!manager.QuarantineInactiveVersion(package, sourceError))
            {
                error = Utf8ToWide(sourceError);
                return false;
            }
            break;
        }
    }
    snowdesktop::widget::InstalledPackage installed;
    snowdesktop::widget::ValidationReport report;
    if (!manager.InstallFromSource(*provider, externalItemId,
        requestedVersion,
        allowSourceChange, installed, report, sourceError,
        allowPermissionExpansion))
    {
        error = Utf8ToWide(sourceError);
        if (!report.Ok()) error += L"\n" + Utf8ToWide(report.ToJson());
        if (workshop)
        {
            std::string displayError = sourceError;
            const auto issue = std::find_if(report.issues.begin(),
                report.issues.end(), [](const auto& candidate)
                {
                    return candidate.severity == snowdesktop::widget::
                        ValidationSeverity::Error;
                });
            if (issue != report.issues.end())
                displayError = "[" + issue->code + "] " + issue->message;
            CacheSteamWorkshopInstallFailure({ details->manifest.id,
                externalItemId, details->manifest,
                std::move(displayError) });
        }
        return false;
    }
    const bool verified = VerifyInstalledWidgetPackage(
        installed.manifest.id, previousVersion, error, 0, workshop ? &installed : nullptr);
    if (workshop)
    {
        if (verified)
        {
            ForgetRejectedSteamWorkshopArchive(workshopItem);
            ClearSteamWorkshopInstallFailure(installed.manifest.id);
        }
        else
        {
            CacheSteamWorkshopInstallFailure({ details->manifest.id,
                externalItemId, details->manifest,
                WideToUtf8(error) });
        }
    }
    return verified;
}

bool WidgetEngine::InstallAndVerifyStaticWidgetPackage(
    const std::filesystem::path& catalogPath,
    const std::string& externalItemId, const std::string& version,
    std::wstring& error, bool allowSourceChange,
    bool allowPermissionExpansion)
{
    std::string sourceError;
    if (!ConfigureStaticWidgetCatalog(catalogPath, sourceError))
    {
        error = Utf8ToWide(sourceError);
        return false;
    }
    return InstallAndVerifyWidgetPackageFromSource("static-catalog",
        externalItemId, version, error, allowSourceChange,
        allowPermissionExpansion);
}

snowdesktop::widget::PackagePaths WidgetEngine::GetWidgetPackagePaths()
{
    return GetWidgetPackageManager().Paths();
}

bool WidgetEngine::RefreshWidgetPackages(std::string& error)
{
    return GetWidgetPackageManager().RefreshCatalog(error);
}

std::vector<snowdesktop::widget::InstalledPackage>
WidgetEngine::ListWidgetPackages()
{
    return GetWidgetPackageManager().ListPackages();
}

std::vector<snowdesktop::widget::InvalidPackage>
WidgetEngine::ListInvalidWidgetPackages()
{
    return GetWidgetPackageManager().ListInvalidPackages();
}

std::optional<snowdesktop::widget::InstalledPackage>
WidgetEngine::GetWidgetPackage(const std::wstring& packageId)
{
    const std::string requestedId = WideToUtf8(packageId);
    if (requestedId.empty()) return std::nullopt;
    return GetWidgetPackageManager().Resolve(requestedId);
}

std::optional<snowdesktop::widget::PackageSourceRef>
WidgetEngine::GetWidgetPackageSource(const std::wstring& packageId)
{
    const std::string requestedId = WideToUtf8(packageId);
    if (requestedId.empty()) return std::nullopt;

    std::optional<snowdesktop::widget::PackageSourceRef> fallback;
    for (const auto& package : GetWidgetPackageManager().ListPackages())
    {
        if (package.manifest.id != requestedId ||
            package.source.providerId.empty() ||
            package.source.externalItemId.empty())
            continue;
        if (package.source.providerId == "steam-workshop" &&
            !snowdesktop::widget::SteamPublishedFileId(
                package.source.externalItemId).empty())
            return package.source;
        if (!fallback || (package.active && package.enabled))
            fallback = package.source;
    }
    return fallback;
}

bool WidgetEngine::IsWidgetPackageAvailable(const std::wstring& packageId)
{
    return !packageId.empty() &&
        GetWidgetPackageManager().ResolveEntry(
            WideToUtf8(packageId)).has_value();
}

bool WidgetEngine::IsWidgetPackageInstalled(const std::wstring& packageId)
{
    const std::string requestedId = WideToUtf8(packageId);
    return !requestedId.empty() &&
        GetWidgetPackageManager().ContainsPackage(requestedId);
}

bool WidgetEngine::SetWidgetPackageEnabled(const std::string& packageId,
    bool enabled, std::string& error)
{
    return GetWidgetPackageManager().SetEnabled(packageId, enabled, error);
}

bool WidgetEngine::CreateWidgetDevelopmentProject(
    const std::string& packageId, std::filesystem::path& projectRoot,
    std::string& error)
{
    return GetWidgetPackageManager().CreateDevelopmentProject(
        packageId, projectRoot, error);
}

bool WidgetEngine::SetWidgetDevelopmentOverride(
    const std::string& packageId, bool active, std::string& error)
{
    return GetWidgetPackageManager().SetDevelopmentOverride(
        packageId, active, error);
}

bool WidgetEngine::RollbackWidgetPackage(const std::string& packageId,
    const std::string& version, std::string& error)
{
    return GetWidgetPackageManager().Rollback(packageId, version, error);
}

bool WidgetEngine::UninstallWidgetPackage(const std::string& packageId,
    std::string& error)
{
    return GetWidgetPackageManager().Uninstall(packageId, error);
}

bool WidgetEngine::GetWidgetDefaultSpan(const std::wstring& filename, int& columns, int& rows)
{
    LuaWidgetManifest manifest = GetWidgetManifest(filename);
    columns = std::max(1, manifest.defaultColumns);
    rows = std::max(1, manifest.defaultRows);
    return manifest.hasManifest;
}

std::wstring WidgetEngine::GetWidgetDisplayName(const std::wstring& filename)
{
    LuaWidgetManifest manifest = GetWidgetManifest(filename);
    if (!manifest.name.empty())
        return Utf8ToWide(manifest.name);

    std::wstring fallback = filename;
    if (fallback.size() > 4 && fallback.substr(fallback.size() - 4) == L".lua")
        fallback = fallback.substr(0, fallback.size() - 4);
    return fallback;
}

bool WidgetEngine::IsWidgetDefaultName(const std::wstring& filename,
    const std::wstring& title)
{
    if (title.empty())
        return false;
    const LuaWidgetManifest manifest = GetWidgetManifest(filename);
    std::vector<std::string> managedKeys = manifest.titleKeys;
    if (!manifest.nameKey.empty())
        managedKeys.push_back(manifest.nameKey);
    for (const auto& [language, catalog] : manifest.locales)
    {
        (void)language;
        for (const std::string& key : managedKeys)
        {
            auto value = catalog.find(key);
            if (value != catalog.end() &&
                Utf8ToWide(value->second) == title)
                return true;
        }
    }
    return !manifest.name.empty() && Utf8ToWide(manifest.name) == title;
}
