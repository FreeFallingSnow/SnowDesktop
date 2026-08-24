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
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void TestWidgetsPagePresenterContract(
    const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/widgets_page_presenter.h");
    const std::string source = ReadText(
        repository / "src/winui/widgets_page_presenter.cpp");

    Check(!header.empty() && !source.empty(),
        "Widgets presenter sources are readable");

    Check(header.find("struct WidgetsPageSnapshot") != std::string::npos &&
            header.find("std::uint64_t generation") != std::string::npos &&
            header.find("std::uint64_t revision") != std::string::npos &&
            header.find("std::uint64_t searchRevision") !=
                std::string::npos &&
            header.find("InstalledWidgetPackageSnapshot") !=
                std::string::npos &&
            header.find("WidgetSourceGroupSnapshot") != std::string::npos &&
            header.find("WidgetCatalogItemSnapshot") != std::string::npos &&
            header.find("WidgetInstanceSnapshot") != std::string::npos &&
            header.find("WidgetRuntimeDiagnosticSnapshot") !=
                std::string::npos &&
            header.find(
                "std::vector<WidgetRuntimeDiagnosticSnapshot> diagnostics") !=
                std::string::npos,
        "host data is published through a typed immutable-style snapshot");

    Check(header.find("WidgetSourceKind kind") != std::string::npos &&
            header.find("SteamWorkshop") != std::string::npos &&
            header.find("supportsSynchronization") != std::string::npos &&
            header.find("std::vector<WidgetCatalogItemSnapshot> results") !=
                std::string::npos,
        "source and Workshop results retain explicit grouping capabilities");

    for (const char* control : {
             "muxc::AutoSuggestBox", "muxc::Expander",
             "muxc::ToggleSwitch", "muxcp::ToggleButton",
             "muxc::CheckBox",
             "muxc::Button", "muxc::TextBox", "muxc::GridView",
             "muxc::ProgressRing",
             "muxc::ProgressBar", "muxc::InfoBar"})
    {
        Check(source.find(control) != std::string::npos,
            "Widgets page is composed from native WinUI controls");
    }

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
            "package and source work is emitted as a typed host action");
    }

    Check(header.find("WidgetRestorableVersionSnapshot") !=
                std::string::npos &&
            header.find("canCreateDevelopmentProject") !=
                std::string::npos &&
            header.find("canInstallDevelopmentSnapshot") !=
                std::string::npos &&
            header.find("canPublishDevelopmentPackage") !=
                std::string::npos &&
            header.find("workshopExternalItemId") != std::string::npos &&
            header.find("setDeveloperToolsEnabled") != std::string::npos &&
            source.find("package.canCreateDevelopmentProject") !=
                std::string::npos &&
            source.find("package.canInstallDevelopmentSnapshot") !=
                std::string::npos &&
            source.find("package.canPublishDevelopmentPackage") !=
                std::string::npos,
        "legacy development, rollback, publishing and Workshop actions are capability-gated host seams");

    Check(source.find("presenter_controls::SettingRow managementRow") !=
                std::string::npos &&
            source.find("presenter_controls::SettingRow enabledRow") !=
                std::string::npos &&
            source.find("presenter_controls::SettingRow developmentRow") !=
                std::string::npos &&
            source.find("includedCard") != std::string::npos &&
            source.find("allFilterButton") != std::string::npos &&
            source.find("installedFilterButton.Visibility") !=
                std::string::npos &&
            source.find("developmentFilterButton.Visibility") !=
                std::string::npos &&
            source.find("PackageFilter::BuiltIn") == std::string::npos,
        "legacy My Components toolbar, filter tags, included group and responsive setting rows are retained");

    Check(header.find("WidgetsPageRequest request)> invoke") !=
                std::string::npos &&
            header.find("std::uint64_t generation") !=
                std::string::npos &&
            source.find("actions.invoke(generation") != std::string::npos,
        "every action carries the active settings-session generation");
    Check(source.find("SettingsRoute::ForWidget(instanceId)") !=
                std::string::npos &&
            header.find("SettingsRoute route") != std::string::npos,
        "instance settings navigation uses the strongly typed WidgetSettings route");

    Check(header.find("DeveloperToolsContent()") != std::string::npos &&
            header.find("DebugContent()") != std::string::npos &&
            source.find("AddDevelopmentOverrideRow(") !=
                std::string::npos &&
            source.find("AddDiagnosticRow(") != std::string::npos &&
            source.find("actions.reloadWidgetInstance(") !=
                std::string::npos &&
            source.find(
                "WidgetsPageCommand::SetDevelopmentOverride") !=
                std::string::npos &&
            source.find("developerRefreshButton") !=
                std::string::npos &&
            source.find("debugRefreshButton") !=
                std::string::npos &&
            source.find("diagnostics.clear()") !=
                std::string::npos,
        "gated native pages expose real development overrides, reload actions, and runtime diagnostics");

    Check(header.find("WidgetAgentSkillTargetSnapshot") !=
                std::string::npos &&
            header.find("WidgetRuntimeErrorSnapshot") !=
                std::string::npos &&
            header.find("WidgetRuntimeViewNodeSnapshot") !=
                std::string::npos &&
            source.find("app.settings.widgets_agent_skill_description") !=
                std::string::npos &&
            source.find("app.settings.widgets_authoring_workspace") !=
                std::string::npos &&
            source.find("app.settings.widgets_component_cli") !=
                std::string::npos &&
            source.find("app.settings.widgets_authoring_publish") !=
                std::string::npos &&
            source.find("FluentGlyphItems") != std::string::npos &&
            source.find("FontAwesomeGlyphItems") != std::string::npos &&
            source.find("BuildErrorCopyText") != std::string::npos &&
            source.find("BuildDiagnosticsCopyText") != std::string::npos &&
            source.find("agentSkillActionsRow") != std::string::npos &&
            source.find("developmentWorkspaceRow") !=
                std::string::npos &&
            source.find("developerErrorActionsRow") !=
                std::string::npos &&
            source.find("developerDiagnosticActionsRow") !=
                std::string::npos,
        "Developer Tools retains Agent Skill, authoring, icon, error-log and diagnostics parity");

    Check(source.find("actions.confirm(requestGeneration") !=
                std::string::npos &&
            source.find("WidgetsPageCommand::UninstallPackage") !=
                std::string::npos &&
            source.find("if (!confirmed || !invoke") != std::string::npos,
        "uninstall waits for an asynchronous host-owned ContentDialog result");

    Check(header.find("WidgetPermissionEditorRequest") !=
                std::string::npos &&
            header.find("permissionScopeFingerprint") !=
                std::string::npos &&
            header.find("editPermissions") != std::string::npos &&
            source.find("OpenPermissionEditor(") != std::string::npos &&
            source.find("WidgetPermissionEditorAction::Revoke") !=
                std::string::npos &&
            source.find("command.scopeFingerprint =") !=
                std::string::npos &&
            source.find("ConfirmPermissionDecision(") ==
                std::string::npos,
        "permissions are edited as one identity-bound batch before one atomic host action");

    Check(header.find("InvalidWidgetPackageSourceSnapshot") !=
                std::string::npos &&
            header.find("WidgetWorkshopInstallFailureSnapshot") !=
                std::string::npos &&
            header.find("WidgetInstallConfirmationRequest") !=
                std::string::npos &&
            source.find("package.invalidSources") != std::string::npos &&
            source.find("package.workshopInstallFailures") !=
                std::string::npos &&
            source.find("sourceId = failure.sourceId") !=
                std::string::npos &&
            source.find("version = failure.version") !=
                std::string::npos &&
            source.find("if (!MatchesQuery(package))") !=
                std::string::npos &&
            source.find("app.settings.widgets_retry_install") !=
                std::string::npos,
        "included validation issues and identity-bound Workshop failures remain searchable and recoverable");

    Check(header.find("WidgetsPageTaskSnapshot") != std::string::npos &&
            header.find("std::optional<double> progress") !=
                std::string::npos &&
            header.find("bool cancellable") != std::string::npos &&
            source.find("taskRing.IsActive(running)") != std::string::npos &&
            source.find("request.taskId = task.taskId") !=
                std::string::npos,
        "long search, install and sync tasks expose progress and cancellation");

    Check(source.find("snapshot.generation != generation") !=
                std::string::npos &&
            source.find("snapshot.revision <= revision") !=
                std::string::npos &&
            source.find("snapshot.searchRevision >= requestedSearchRevision") !=
                std::string::npos &&
            source.find("request.searchRevision = ++requestedSearchRevision") !=
                std::string::npos &&
            source.find("CallbackGenerationGate") != std::string::npos &&
            source.find("gate->generation.load") != std::string::npos &&
            source.find("gate->activation.load") != std::string::npos &&
            source.find("activation.fetch_add") != std::string::npos &&
            source.find("gate->closed.load") != std::string::npos,
        "snapshot and delayed-dialog completions reject stale generations and route-family activations");

    Check(source.find("localize(key)") != std::string::npos &&
            source.find("RefreshLocalizedText()") != std::string::npos &&
            source.find("permission.labelKey") != std::string::npos &&
            source.find("source.nameKey") != std::string::npos,
        "static, permission and source labels use dynamic JSON localization");

    Check(source.find("AutomationProperties::SetName") !=
                std::string::npos &&
            source.find("AutomationProperties::SetHelpText") !=
                std::string::npos &&
            source.find("UseSystemFocusVisuals(true)") !=
                std::string::npos &&
            source.find("FocusTarget(std::string_view focusId)") !=
                std::string::npos &&
            source.find("widgets.permissions") != std::string::npos &&
            source.find("widgets.sources") != std::string::npos &&
            source.find("developer.overrides") != std::string::npos &&
            source.find("debug.runtime") != std::string::npos,
        "keyboard focus and accessibility metadata cover indexed cards");

    Check(source.find("WidgetPackageManager") == std::string::npos &&
            source.find("InstallArchive(") == std::string::npos &&
            source.find("SetPermissionDecision(") == std::string::npos &&
            source.find("ShellExecute") == std::string::npos &&
            source.find("WinHttp") == std::string::npos &&
            source.find("std::thread") == std::string::npos &&
            source.find("CreateThread") == std::string::npos &&
            source.find("std::filesystem") == std::string::npos,
        "presenter performs no file, package-manager, shell, network or worker IO");
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the Widgets presenter contract");
    if (argc == 2)
        TestWidgetsPagePresenterContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures
                  << " WinUI Widgets presenter check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI Widgets presenter checks passed\n";
    return EXIT_SUCCESS;
}
