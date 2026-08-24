#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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
             "WidgetsPageCommand::BrowseInstallPackage",
             "WidgetsPageCommand::SearchSources",
             "WidgetsPageCommand::CancelTask",
             "WidgetsPageCommand::InstallCatalogItem",
             "WidgetsPageCommand::SetPackageEnabled",
             "WidgetsPageCommand::UninstallPackage",
             "WidgetsPageCommand::SetPermissionDecision",
             "WidgetsPageCommand::SetDevelopmentOverride",
             "WidgetsPageCommand::OpenWorkshop",
             "WidgetsPageCommand::SynchronizeSource",
             "WidgetsPageCommand::AddPackageToDesktop"})
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
            source.find("engine.GetWidgets()") != std::string::npos,
        "persisted host instances are injected and reconciled with live runtimes");

    for (const char* operation : {
             "InstallAndVerifyWidgetPackage(",
             "InstallAndVerifyWidgetPackageFromSource(",
             "SetWidgetPackageEnabled(",
             "ApplyWidgetPermissionDecision(",
             "SetWidgetDevelopmentOverride(",
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

void TestInjectedHostCapabilities(const std::string& header,
    const std::string& source)
{
    for (const char* boundary : {
             "pickPackage", "confirmInstall", "openWorkshop",
             "addPackageToDesktop", "canSynchronizeSource",
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

void TestBackendContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/widgets_page_backend.h");
    const std::string source = ReadText(
        repository / "src/winui/widgets_page_backend.cpp");
    Check(!header.empty() && !source.empty(),
        "Widgets backend sources are readable");
    if (header.empty() || source.empty()) return;

    TestPresenterBoundary(header, source);
    TestCoreStateAndMutations(header, source);
    TestInjectedHostCapabilities(header, source);
    TestAsyncIdentityAndStaleResultRejection(header, source);
    TestV2OnlyContract(source);
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
