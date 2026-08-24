#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../src/winui/widgets_page_backend_state.h"

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void TestPresenterBoundary(const std::string& header,
    const std::string& source)
{
    Check(header.find("#include \"widgets_page_presenter.h\"") !=
                std::string::npos &&
            header.find("class WidgetsPageBackend final") !=
                std::string::npos &&
            header.find("WidgetsPageRequest request") !=
                std::string::npos &&
            header.find("std::shared_ptr<const WidgetsPageSnapshot>") !=
                std::string::npos,
        "backend consumes the presenter request and snapshot DTO contract");

    for (const char* command : {
             "WidgetsPageCommand::Refresh",
             "WidgetsPageCommand::BrowseInstallPackage",
             "WidgetsPageCommand::SearchSources",
             "WidgetsPageCommand::CancelTask",
             "WidgetsPageCommand::InstallCatalogItem",
             "WidgetsPageCommand::RetryWorkshopInstall",
             "WidgetsPageCommand::SetPackageEnabled",
             "WidgetsPageCommand::UninstallPackage",
             "WidgetsPageCommand::SetPermissionDecision",
             "WidgetsPageCommand::SetDevelopmentOverride",
             "WidgetsPageCommand::CreateDevelopmentProject",
             "WidgetsPageCommand::InstallDevelopmentSnapshot",
             "WidgetsPageCommand::RollbackPackage",
             "WidgetsPageCommand::PublishDevelopmentPackage",
             "WidgetsPageCommand::OpenWorkshop",
             "WidgetsPageCommand::OpenWorkshopItem",
             "WidgetsPageCommand::SynchronizeSource",
             "WidgetsPageCommand::AddPackageToDesktop",
             "WidgetsPageCommand::RefreshAgentSkills",
             "WidgetsPageCommand::ApplyAgentSkillSelection",
             "WidgetsPageCommand::SetAgentSkillTargetSelection",
             "WidgetsPageCommand::OpenDevelopmentFolder",
             "WidgetsPageCommand::PublishDevelopmentWorkspace",
             "WidgetsPageCommand::ClearWidgetErrors"})
    {
        Check(source.find(command) != std::string::npos,
            "every Widgets presenter command reaches an application action");
    }
}

void TestCoreStateAndMutations(const std::string& header,
    const std::string& source)
{
    Check(source.find("WidgetEngine::ListWidgetPackages()") !=
                std::string::npos &&
            source.find("LocalizePackageManifest(") != std::string::npos &&
            source.find("WidgetPermissionBroker::") != std::string::npos &&
            source.find("DeclaredPermissions(") != std::string::npos &&
            source.find("ClassifyPermissionRisk(") != std::string::npos,
        "installed packages and broker decisions are converted to DTOs");

    Check(header.find("std::function<std::vector<WidgetsPageHostInstance>()>") !=
                std::string::npos &&
            source.find("result = options.instances()") !=
                std::string::npos &&
            source.find("engine.GetWidgets()") != std::string::npos &&
            header.find("diagnosticsVisible") != std::string::npos &&
            source.find("engine.GetWidgetDiagnostics()") !=
                std::string::npos &&
            source.find("index < diagnostic.logs.size()") !=
                std::string::npos,
        "persisted host instances are injected and reconciled with live runtimes");

    for (const char* operation : {
             "InstallAndVerifyWidgetPackage(",
             "InstallAndVerifyWidgetPackageFromSource(",
             "SetWidgetPackageEnabled(",
             "ApplyWidgetPermissionDecision(",
             "CreateWidgetDevelopmentProject(",
             "SetWidgetDevelopmentOverride(",
             "RollbackWidgetPackage(",
             "UninstallWidgetPackage(",
             "RevokeFilesystemHandlesForPackage("})
    {
        Check(source.find(operation) != std::string::npos,
            "mutations reuse the existing WidgetEngine application API");
    }

    Check(header.find("unsubscribeWorkshop") !=
                std::string::npos &&
            source.find("options.unsubscribeWorkshop(") !=
                std::string::npos &&
            source.find("QuerySteamWorkshopSubscriptions(") ==
                std::string::npos &&
            source.find("ApplySteamWorkshopSubscriptions(") ==
                std::string::npos,
        "Workshop removal is delegated while subscription polling stays host-owned");
}

void TestLegacyPackageManagementParity(const std::string& header,
    const std::string& source)
{
    Check(source.find("ListInvalidWidgetPackages()") !=
                std::string::npos &&
            source.find("CachedSteamWorkshopInstallFailures()") !=
                std::string::npos &&
            source.find("snapshot.invalidSources.push_back") !=
                std::string::npos &&
            source.find("snapshot.workshopInstallFailures.push_back") !=
                std::string::npos &&
            source.find("RetryWorkshopInstall(") != std::string::npos,
        "invalid packages and Workshop installation failures remain visible and recoverable");

    Check(source.find("L\"steam-workshop\",") !=
                std::string::npos &&
            source.find("candidate.sourceId == request.sourceId") !=
                std::string::npos &&
            source.find("candidate.externalItemId == request.externalItemId") !=
                std::string::npos &&
            source.find("candidate.version == request.version") !=
                std::string::npos &&
            source.find("failure.sourceId == request.sourceId") !=
                std::string::npos &&
            source.find("failure.version == request.version") !=
                std::string::npos,
        "each cached Workshop failure action retains its own source, item, and version identity");

    Check(source.find("CachedSteamWorkshopPackageAssociations()") !=
                std::string::npos &&
            source.find("WorkshopExternalItemIdFor(") !=
                std::string::npos &&
            source.find("snapshot.workshopExternalItemId =") !=
                std::string::npos &&
            source.find("SteamPublishedFileId(") != std::string::npos,
        "installed packages retain their authoritative Workshop item identity");

    Check(source.find("snapshot.canCreateDevelopmentProject =") !=
                std::string::npos &&
            source.find("snapshot.canInstallDevelopmentSnapshot =") !=
                std::string::npos &&
            source.find("snapshot.canPublishDevelopmentPackage =") !=
                std::string::npos &&
            source.find("snapshot.restorableVersions.push_back") !=
                std::string::npos,
        "development, managed snapshot, publishing, and rollback capabilities are advertised from real package state");

    Check(source.find("snapshot.canAddToDesktop = display &&\n"
                      "            static_cast<bool>(options.addPackageToDesktop)") !=
                std::string::npos &&
            source.find("snapshot.canAddToDesktop = display.active") ==
                std::string::npos,
        "disabled and permission-blocked packages remain addable as persisted placeholders");

    Check(source.find("StageDevelopmentPackage(") != std::string::npos &&
            source.find("ScopedPackageIdentityLock sourceLock(development.root") !=
                std::string::npos &&
            source.find("manager.ExportDirectory(development.root") !=
                std::string::npos &&
            source.find("settings-review-development-") !=
                std::string::npos &&
            source.find("artifact.sha256 != snapshot->sha256") !=
                std::string::npos &&
            source.find("install.developmentPackageId = packageId") !=
                std::string::npos &&
            source.find("developmentOverrideWasActive") !=
                std::string::npos,
        "development installation exports one immutable reviewed archive and preserves override recovery state");

    Check(source.find("version.version == request.version") !=
                std::string::npos &&
            source.find("package->workshopExternalItemId == request.externalItemId") !=
                std::string::npos &&
            source.find("WideToUtf8(request.externalItemId)") !=
                std::string::npos,
        "rollback and Workshop item commands reject substituted or stale identities");

    Check(source.find("request.scopeFingerprint") != std::string::npos &&
            source.find("package->manifest.version != expectedVersion") !=
                std::string::npos &&
            source.find("package->source.providerId != expectedSourceId") !=
                std::string::npos &&
            source.find(
                "package->source.externalItemId != expectedExternalItemId") !=
                std::string::npos &&
            source.find(
                "currentScopeFingerprint != expectedScopeFingerprint") !=
                std::string::npos &&
            source.find("widgets_permissions_scope_changed") !=
                std::string::npos,
        "permission decisions reject stale version, source, and scope identities");

    for (const char* boundary : {
             "openWorkshopItem", "developmentProjectCreated",
             "canPublishDevelopmentPackage",
             "publishDevelopmentPackage"})
    {
        Check(header.find(boundary) != std::string::npos &&
                source.find(std::string("options.") + boundary) !=
                    std::string::npos,
            "process-owned legacy package action is an explicit host seam");
    }
}

void TestInjectedHostCapabilities(const std::string& header,
    const std::string& source)
{
    for (const char* boundary : {
             "pickPackage", "confirmInstall", "openWorkshop",
             "openWorkshopItem", "addPackageToDesktop",
             "agentSkillTargetMask", "setAgentSkillTargetMask",
             "openDevelopmentFolder",
             "developmentProjectCreated",
             "canPublishDevelopmentPackage",
             "publishDevelopmentPackage", "canSynchronizeSource",
             "synchronizeSource", "unsubscribeWorkshop",
             "cancelAsyncOperation",
             "dispatchToOwner", "hostStateChanged"})
    {
        Check(header.find(boundary) != std::string::npos,
            "host-only capability is an explicit injected boundary");
    }

    Check(source.find("options.canSynchronizeSource(") !=
                std::string::npos &&
            source.find("options.synchronizeSource(") !=
                std::string::npos &&
            source.find("supportsSynchronization =") !=
                std::string::npos,
        "manual source sync is advertised only through the host watcher seam");

    Check(source.find("ContentDialog") == std::string::npos &&
            source.find("FileOpenPicker") == std::string::npos &&
            source.find("ShellExecute") == std::string::npos,
        "backend does not own HWND dialogs, file pickers, or shell APIs");

    Check(source.find("DefaultAgentSkillTargets()") !=
                std::string::npos &&
            source.find("InspectAgentSkill(") != std::string::npos &&
            source.find("InstallOrUpdateAgentSkill(") !=
                std::string::npos &&
            source.find("UninstallAgentSkill(") != std::string::npos &&
            source.find("engine.GetWidgetErrors()") !=
                std::string::npos &&
            source.find("engine.ClearWidgetErrors()") !=
                std::string::npos &&
            source.find("desktopViewNodes") != std::string::npos &&
            source.find("auxiliaryViewNodes") != std::string::npos,
        "legacy Developer Tools status, synchronization, errors and view diagnostics reach the typed snapshot");
}

void TestAsyncIdentityAndStaleResultRejection(const std::string& header,
    const std::string& source)
{
    Check(source.find("std::jthread") != std::string::npos &&
            source.find("std::stop_token") != std::string::npos &&
            source.find("std::atomic_bool closed") != std::string::npos &&
            source.find("options.dispatchToOwner(") != std::string::npos,
        "source IO uses a close-gated worker and returns to its owner thread");

    Check(source.find("sourceWorker.RequestCancel(") != std::string::npos &&
            source.find("state->task.cancellable = false") !=
                std::string::npos &&
            source.find("result.cancelled = true") != std::string::npos,
        "search cancellation keeps the mutation gate until terminal completion");

    Check(header.find("CompletionIdentityMatches(") != std::string::npos &&
            source.find("work.generation, work.activation, work.taskId") !=
                std::string::npos &&
            source.find("work.searchRevision != requestedSearchRevision") !=
                std::string::npos &&
            source.find("requestGeneration != generation") !=
                std::string::npos &&
            source.find("state->revision = ++revision") !=
                std::string::npos,
        "generation, activation, task, search revision, and publication revision gate results");

    Check(source.find("owner->pickerRequestId != requestId") !=
                std::string::npos &&
            source.find("owner->confirmationRequestId != requestId") !=
                std::string::npos &&
            source.find("expectedTaskId == currentTaskId") !=
                std::string::npos,
        "late picker, confirmation, search, and sync completions are discarded");
}

void TestOutstandingOperationLedgerBehavior()
{
    using namespace snowdesktop::winui::widgets_page_backend_detail;
    ReviewedPackageFileIdentity reviewedIdentity;
    reviewedIdentity.volumeSerialNumber = 9;
    reviewedIdentity.fileId[0] = 42;
    ReviewedPackageFileIdentity sameIdentity = reviewedIdentity;
    ReviewedPackageFileIdentity replacementIdentity = reviewedIdentity;
    replacementIdentity.fileId[0] = 43;
    ReviewedPackageFileIdentity otherVolume = reviewedIdentity;
    otherVolume.volumeSerialNumber = 10;
    Check(reviewedIdentity == sameIdentity &&
            reviewedIdentity != replacementIdentity &&
            reviewedIdentity != otherVolume,
        "reviewed package identity binds both volume and FileId");

    OutstandingOperationLedger ledger;
    const OutstandingOperationIdentity search{
        7, 11, 101, OutstandingOperationKind::Search};
    const OutstandingOperationIdentity synchronization{
        7, 11, 102, OutstandingOperationKind::SourceSynchronization};

    Check(ledger.Begin(search) && ledger.Begin(synchronization) &&
            ledger.Busy() && ledger.Contains(search.taskId),
        "nonterminal search and synchronization operations close the mutation gate");
    Check(!ledger.Complete({7, 12, search.taskId,
                OutstandingOperationKind::Search}) &&
            ledger.Busy() && ledger.Contains(search.taskId),
        "a completion from another activation cannot release an old operation");
    Check(ledger.Complete(search) && ledger.Busy() &&
            !ledger.Contains(search.taskId) &&
            ledger.Tasks(OutstandingOperationKind::Search).empty(),
        "the exact terminal search completion releases only its own ledger entry");
    Check(ledger.Complete(synchronization) && !ledger.Busy(),
        "the mutation gate opens only after every background operation is terminal");
}

void TestV2OnlyContract(const std::string& source)
{
    Check(source.find("IsExecutablePackageContract(") !=
                std::string::npos,
        "installed and catalog records are filtered by the existing v2 contract");
    Check(source.find("schemaVersion =") == std::string::npos &&
            source.find("apiVersion =") == std::string::npos &&
            source.find("luaopen_") == std::string::npos &&
            source.find("ImGui") == std::string::npos,
        "backend neither invents a schema nor introduces Lua UI or ImGui APIs");

    Check(source.find("value.packageId == request.packageId") !=
                std::string::npos &&
            source.find("value.externalItemId == request.externalItemId") !=
                std::string::npos &&
            source.find("value.version == request.version") !=
                std::string::npos,
        "catalog mutation rejects stale or substituted result identities");
}

void TestLocalInstallIdentityBinding(const std::string& source)
{
    const auto stage = source.find(
        "install.localSnapshot = owner->StageLocalPackage");
    const auto confirm = source.find("owner->RequestInstallConfirmation(",
        stage);
    Check(source.find("struct LocalPackageSnapshot") != std::string::npos &&
            source.find("std::filesystem::copy_file(selected,") !=
                std::string::npos &&
            source.find("settings-review-") != std::string::npos &&
            source.find("WidgetPackageManager::Sha256File(") !=
                std::string::npos,
        "a selected local package is copied to private staging and fingerprinted");
    Check(source.find("actualSha256 != install.localSnapshot->sha256") !=
                std::string::npos &&
            source.find("currentManifest.id != install.localSnapshot->manifest.id") !=
                std::string::npos &&
            source.find("currentManifest.version !=") != std::string::npos &&
            source.find("settings.widgets.install.identityChanged") !=
                std::string::npos,
        "the staged archive identity is revalidated immediately before install");
    const auto finalLock = source.find(
        "const ScopedPackageIdentityLock packageLock(install.path,");
    const auto finalHash = source.find(
        "const std::string actualSha256", finalLock);
    const auto finalInstall = source.find(
        "engine.InstallAndVerifyWidgetPackage(", finalHash);
    Check(source.find("GetFileInformationByHandleEx(handle, FileIdInfo") !=
                std::string::npos &&
            source.find("volumeSerialNumber") != std::string::npos &&
            source.find("ancestorHandles_") != std::string::npos &&
            source.find("FILE_SHARE_READ | FILE_SHARE_WRITE") !=
                std::string::npos &&
            source.find("FILE_FLAG_OPEN_REPARSE_POINT") !=
                std::string::npos &&
            source.find("FILE_ATTRIBUTE_REPARSE_POINT") !=
                std::string::npos &&
            source.find("RemoveAbandonedSettingsReviewPackages(") !=
                std::string::npos &&
            finalLock != std::string::npos &&
            finalHash != std::string::npos &&
            finalInstall != std::string::npos &&
            finalLock < finalHash && finalHash < finalInstall,
        "volume/FileId binding and locked non-reparse ancestors span final validation and install");
    Check(source.find("packageLock.MatchesPathIdentity()") !=
                std::string::npos &&
            source.find("install.localSnapshot->identity") !=
                std::string::npos,
        "every path-based package read is bounded by a strict identity comparison");
    Check(stage != std::string::npos && confirm != std::string::npos &&
            stage < confirm &&
            source.find("dialogRequest.packageId =") !=
                std::string::npos &&
            source.find("dialogRequest.sha256 =") !=
                std::string::npos,
        "local install confirmation is bound to package identity and fingerprint");
}

void TestStructuredInstallConfirmation(const std::string& header,
    const std::string& source)
{
    Check(header.find("WidgetInstallConfirmationRequest request") !=
                std::string::npos &&
            source.find("ParseInstallConfirmationReasons(") !=
                std::string::npos &&
            source.find("WidgetInstallConfirmationReasonKind::NewPermission") !=
                std::string::npos &&
            source.find("WidgetInstallConfirmationReasonKind::NewWebsite") !=
                std::string::npos &&
            source.find("WidgetInstallConfirmationReasonKind::SourceChange") !=
                std::string::npos &&
            source.find("dialogRequest.technicalDetails = reason") !=
                std::string::npos,
        "install confirmation carries distinct access changes and raw technical details to the Shell");
    Check(source.find("requests a new required permission: ") !=
                std::string::npos &&
            source.find("requests a new optional permission: ") !=
                std::string::npos &&
            source.find("requests a new network domain: ") !=
                std::string::npos &&
            source.find("source changes require explicit confirmation") !=
                std::string::npos &&
            source.find("valueLabelKey") != std::string::npos,
        "all current expansion reasons are split into independently localizable rows");
}

void TestWorkshopAuthoritativeCompletion(
    const std::string& appHeader, const std::string& appRun,
    const std::string& timerDispatch)
{
    Check(appHeader.find("expectedUnsubscribedPublishedFileId") !=
                std::string::npos &&
            appHeader.find("expectedRemovedPackageIds") !=
                std::string::npos,
        "Workshop settings completions retain the requested reconciliation identity");
    Check(appRun.find("queryId = pollState->nextQueryId") !=
                std::string::npos &&
            appRun.find("pollState->settingsCompletions.push_back") !=
                std::string::npos &&
            appRun.find("pollState->refreshPending.store(true)") !=
                std::string::npos &&
            appRun.find("expectedRemovedInstanceIds") ==
                std::string::npos,
        "unsubscribe waits for a new authoritative subscription query");
    Check(timerDispatch.find("ApplySteamWorkshopSubscriptions(") !=
                std::string::npos &&
            timerDispatch.find("stillSubscribed") != std::string::npos &&
            timerDispatch.find("managedPackageRemains") !=
                std::string::npos &&
            timerDispatch.find("expectedInstanceRemains") ==
                std::string::npos &&
            timerDispatch.find(
                "settings.widgets.workshop.unsubscribeNotReconciled") !=
                std::string::npos,
        "completion verifies authoritative subscription and managed package state");
    Check(timerDispatch.find("hostReloadWouldDefer") !=
                std::string::npos &&
            timerDispatch.find("settingsCompletionReady") !=
                std::string::npos &&
            timerDispatch.find("intentionally retained as placeholders") !=
                std::string::npos,
        "Workshop completion waits for a real host reload and retains desktop placeholders");
}

void TestBackendContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/widgets_page_backend.h");
    const std::string source = ReadText(
        repository / "src/winui/widgets_page_backend.cpp");
    const std::string appHeader = ReadText(repository / "src/app/app.h");
    const std::string appRun = ReadText(
        repository / "src/app/app_run.cpp");
    const std::string timerDispatch = ReadText(
        repository / "src/app/app_timer_dispatch.cpp");
    Check(!header.empty() && !source.empty() && !appHeader.empty() &&
            !appRun.empty() && !timerDispatch.empty(),
        "Widgets backend sources are readable");
    if (header.empty() || source.empty() || appHeader.empty() ||
        appRun.empty() || timerDispatch.empty()) return;

    TestPresenterBoundary(header, source);
    TestCoreStateAndMutations(header, source);
    TestLegacyPackageManagementParity(header, source);
    TestInjectedHostCapabilities(header, source);
    TestAsyncIdentityAndStaleResultRejection(header, source);
    TestOutstandingOperationLedgerBehavior();
    TestV2OnlyContract(source);
    TestLocalInstallIdentityBinding(source);
    TestStructuredInstallConfirmation(header, source);
    TestWorkshopAuthoritativeCompletion(appHeader, appRun, timerDispatch);
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the Widgets backend contract");
    if (argc == 2)
        TestBackendContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures << " Widgets backend check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Widgets backend checks passed\n";
    return EXIT_SUCCESS;
}
