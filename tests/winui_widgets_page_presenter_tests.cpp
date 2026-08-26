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
    const std::string shellXaml = ReadText(
        repository / "src/winui/SettingsShell.xaml");

    Check(!header.empty() && !source.empty() && !shellXaml.empty(),
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

    const auto sourceRegion = [&source](const char* beginMarker,
                                        const char* endMarker) {
        const auto begin = source.find(beginMarker);
        if (begin == std::string::npos)
            return std::string{};
        const auto end = source.find(endMarker, begin);
        return source.substr(begin, end == std::string::npos
                ? std::string::npos : end - begin);
    };
    const auto stretchesAfterContent = [](const std::string& region,
                                          const char* contentCall,
                                          const char* stretchCall) {
        const auto content = region.find(contentCall);
        if (content == std::string::npos)
            return false;
        const auto afterContent = content + std::string(contentCall).size();
        return region.find(stretchCall, afterContent) != std::string::npos;
    };

    const std::string iconReferenceRegion = sourceRegion(
        "void InitializeIconReference(", "void BuildControls()");
    Check(stretchesAfterContent(iconReferenceRegion,
            "expander.Content(body)",
            "StretchExpanderBody(expander, body)"),
        "icon reference Expander content is stretched after attachment");

    const std::string controlsRegion = sourceRegion(
        "void BuildControls()", "void HookStaticEvents()");
    Check(stretchesAfterContent(controlsRegion,
            "includedExpander.Content(includedRows)",
            "StretchExpanderBody(includedExpander, includedRows)"),
        "included components Expander content is stretched after attachment");

    const std::string developmentOverrideRegion = sourceRegion(
        "void AddDevelopmentOverrideRow(", "void AddDiagnosticRow(");
    Check(stretchesAfterContent(developmentOverrideRegion,
            "row.Content(body)", "StretchExpanderBody(row, body)"),
        "development override Expander content is stretched after attachment");

    const std::string diagnosticRegion = sourceRegion(
        "void AddDiagnosticRow(", "void RenderDeveloperRows()");
    Check(diagnosticRegion.find("muxcp::ToggleButton disclosure") !=
                std::string::npos &&
            diagnosticRegion.find(
                "body.Visibility(mux::Visibility::Collapsed)") !=
                std::string::npos &&
            diagnosticRegion.find("HookClick(disclosure") !=
                std::string::npos &&
            diagnosticRegion.find("muxc::Expander row") ==
                std::string::npos,
        "diagnostic disclosure avoids the crashing nested Expander path");

    const std::string permissionRegion = sourceRegion(
        "void AddPermissionControls(", "void AddInstanceControls(");
    Check(stretchesAfterContent(permissionRegion,
            "permissionsExpander.Content(permissionBody)",
            "StretchExpanderBody(permissionsExpander, permissionBody)"),
        "permissions Expander content is stretched after attachment");

    const std::string instanceRegion = sourceRegion(
        "void AddInstanceControls(", "void AddLegacyPackageActions(");
    Check(stretchesAfterContent(instanceRegion,
            "instancesExpander.Content(rows)",
            "StretchExpanderBody(instancesExpander, rows)"),
        "widget instance Expander content is stretched after attachment");

    const std::string legacyActionsRegion = sourceRegion(
        "void AddLegacyPackageActions(", "void AddPackageRow(");
    Check(stretchesAfterContent(legacyActionsRegion,
            "versions.Content(versionRows)",
            "StretchExpanderBody(versions, versionRows)"),
        "version Expander content is stretched after attachment");
    Check(stretchesAfterContent(legacyActionsRegion,
            "advanced.Content(actionsPanel)",
            "StretchExpanderBody(advanced, actionsPanel)"),
        "advanced Expander content is stretched after attachment");

    const auto packageRowStart = source.find("void AddPackageRow(");
    const auto packageRowEnd = source.find("void RequestUninstall(",
        packageRowStart);
    const auto packageContentStretch = source.find(
        "row.HorizontalContentAlignment(", packageRowStart);
    Check(source.find("presenter_controls::SettingRow managementRow") !=
                std::string::npos &&
            source.find("presenter_controls::SettingRow primaryActionsRow") !=
                std::string::npos &&
            source.find("presenter_controls::SettingRow developmentRow") !=
                std::string::npos &&
            source.find("includedExpander.IsExpanded(false)") !=
                std::string::npos &&
            source.find("allFilterButton") != std::string::npos &&
            source.find("installedFilterButton.Visibility") !=
                std::string::npos &&
            source.find("developmentFilterButton.Visibility") !=
                std::string::npos &&
            source.find("PackageFilter::BuiltIn") == std::string::npos &&
            controlsRegion.find(
                "managementRow.Initialize(managementActions, 0.0)") !=
                std::string::npos &&
            controlsRegion.find("searchBox.MaxWidth(") ==
                std::string::npos &&
            controlsRegion.find(
                "filterActions.HorizontalAlignment("
                "mux::HorizontalAlignment::Left)") !=
                std::string::npos &&
            source.find("managementRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("primaryActionsRow.SetControlAlignment(") !=
                std::string::npos,
        "My Components actions use their measured width, search stretches, filter tags align left, and setting-row actions remain responsive");
    Check(packageRowStart != std::string::npos &&
            packageRowEnd != std::string::npos &&
            packageContentStretch != std::string::npos &&
            packageContentStretch < packageRowEnd &&
            stretchesAfterContent(sourceRegion(
                    "void AddPackageRow(", "void RequestUninstall("),
                "row.Content(body)", "StretchExpanderBody(row, body)") &&
            source.find("app.settings.widgets_package_id") !=
                std::string::npos &&
            source.find("app.settings.widgets_provider_id") !=
                std::string::npos &&
            source.find("permissionsExpander.HorizontalContentAlignment(") !=
                std::string::npos &&
            source.find("instancesExpander.HorizontalContentAlignment(") !=
                std::string::npos &&
            source.find("advanced.HorizontalContentAlignment(") !=
                std::string::npos &&
            source.find("versions.HorizontalContentAlignment(") !=
                std::string::npos &&
            source.find("expander.HorizontalContentAlignment(") !=
                std::string::npos &&
            source.find(
              "button.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "button.VerticalAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos &&
            source.find("failureActions.HorizontalAlignment(") !=
                std::string::npos,
        "package and nested Expander content stretches across each card while natural-width actions align at the right edge");

    const std::string packageRowRegion = sourceRegion(
        "void AddPackageRow(", "void RequestUninstall(");
    const std::string installedPatchRegion = sourceRegion(
        "[[nodiscard]] bool TryPatchInstalledRows(",
        "void RenderInstalledRows()");
    const std::string inlinePackageChangeRegion = sourceRegion(
        "[[nodiscard]] static bool HasOnlyInlinePackageStateChanges(",
        "[[nodiscard]] std::vector<std::wstring> VisiblePackageIds(");
    const std::string packagePatchRegion = sourceRegion(
        "void PatchPackageRowState(",
        "[[nodiscard]] bool TryPatchInstalledRows(");
    const std::string installedRenderRegion = sourceRegion(
        "void RenderInstalledRows()", "void AddCatalogResult(");
    Check(packageRowRegion.find("packageExpansionState.find(") !=
                std::string::npos &&
            packageRowRegion.find("row.IsExpanded(savedExpansion") !=
                std::string::npos &&
            packageRowRegion.find("row.IsExpanded(true)") ==
                std::string::npos &&
            developmentOverrideRegion.find("row.IsExpanded(false)") !=
                std::string::npos,
        "component package Expanders start collapsed and restore only expansion explicitly retained for the Presenter lifetime");
    Check(source.find("void PopulatePackageTags(") != std::string::npos &&
            packageRowRegion.find("PopulatePackageTags(tags, package)") !=
                std::string::npos &&
            packagePatchRegion.find(
                "PopulatePackageTags(binding.tags, package)") !=
                std::string::npos &&
            source.find("IsInstalledPackage(package)") !=
                std::string::npos &&
            source.find("IsDevelopmentPackage(package)") !=
                std::string::npos &&
            installedRenderRegion.find(
                "if (filter != PackageFilter::All)") !=
                std::string::npos,
        "each component card carries category tags whose shared predicates drive visible filtering, while included components stay in All");
    Check(source.find("struct PackageRowBinding") != std::string::npos &&
            source.find("CapturePackageExpansionState();") !=
                std::string::npos &&
            installedRenderRegion.find("CapturePackageExpansionState();") <
                installedRenderRegion.find(
                    "installedRows.Children().Clear();") &&
            packageRowRegion.find("FindPackage(packageId)") !=
                std::string::npos,
        "package rows retain expansion and state-changing actions resolve the current snapshot instead of capturing stale enabled values");
    Check(installedPatchRegion.find("PatchPackageRowState(") !=
                std::string::npos &&
            installedPatchRegion.find("changedRows.push_back(index)") !=
                std::string::npos &&
            installedPatchRegion.find(
                "for (const std::size_t index : changedRows)") !=
                std::string::npos &&
            installedPatchRegion.find("Children().Clear()") ==
                std::string::npos &&
            installedPatchRegion.find("previousIds != currentIds") !=
                std::string::npos &&
            installedPatchRegion.find(
                "HasOnlyInlinePackageStateChanges") !=
                std::string::npos,
        "enabled, active and add-to-desktop state echoes patch existing package controls while structural package changes fall back to reconciliation");
    Check(source.find("muxc::TextBlock name{nullptr}") !=
                std::string::npos &&
            source.find("muxc::TextBlock description{nullptr}") !=
                std::string::npos &&
            source.find("muxc::TextBlock version{nullptr}") !=
                std::string::npos &&
            source.find("muxc::TextBlock author{nullptr}") !=
                std::string::npos &&
            source.find("muxc::TextBlock sourceId{nullptr}") !=
                std::string::npos &&
            packageRowRegion.find("description.Visibility(") !=
                std::string::npos &&
            packageRowRegion.find("version.Visibility(") !=
                std::string::npos &&
            packageRowRegion.find("author.Visibility(") !=
                std::string::npos,
        "package display fields retain controls so source switches can patch text and visibility without rebuilding the row");
    Check(inlinePackageChangeRegion.find("previousStructure.name") !=
                std::string::npos &&
            inlinePackageChangeRegion.find("previousStructure.description") !=
                std::string::npos &&
            inlinePackageChangeRegion.find("previousStructure.version") !=
                std::string::npos &&
            inlinePackageChangeRegion.find("previousStructure.author") !=
                std::string::npos &&
            inlinePackageChangeRegion.find("previousStructure.sourceId") !=
                std::string::npos &&
            inlinePackageChangeRegion.find("previousStructure.sourceName") !=
                std::string::npos &&
            inlinePackageChangeRegion.find(
                "previousStructure.sourceExternalItemId") !=
                std::string::npos &&
            inlinePackageChangeRegion.find("previousStructure.development") !=
                std::string::npos &&
            packagePatchRegion.find("binding.name.Text(displayName)") !=
                std::string::npos &&
            packagePatchRegion.find(
                "binding.expander, packageState") !=
                std::string::npos,
        "development source display changes update the existing row and its accessible status in place");
    Check(permissionRegion.find("FindPackage(packageId)") !=
                std::string::npos &&
            permissionRegion.find("OpenPermissionEditor(*current)") !=
                std::string::npos &&
            legacyActionsRegion.find("FindPackage(packageId)") !=
                std::string::npos &&
            legacyActionsRegion.find("L\"steam-workshop\"") !=
                std::string::npos &&
            packageRowRegion.find("PackageDisplayName(*current)") !=
                std::string::npos &&
            installedRenderRegion.find("RevokePackageRowEvents();") !=
                std::string::npos,
        "package actions resolve the latest snapshot, Workshop identity remains stable, and row-local event handlers are revoked on structural rebuilds");

    const std::string sourceGroupRegion = sourceRegion(
        "void AddSourceGroup(", "void RenderSourceRows()");
    Check(stretchesAfterContent(sourceGroupRegion,
            "expander.Content(body)",
            "StretchExpanderBody(expander, body)"),
        "source and Workshop Expander content is stretched after attachment");

    const auto commandRowsStart = source.find("const auto addCommandRow =");
    const auto commandRowsEnd = source.find(
        "addCommandRow(\"app.settings.widgets_cli_capabilities\"",
        commandRowsStart);
    const std::string commandRows =
        commandRowsStart != std::string::npos &&
            commandRowsEnd != std::string::npos
        ? source.substr(commandRowsStart, commandRowsEnd - commandRowsStart)
        : std::string{};
    Check(commandRows.find("muxc::Grid controls;") != std::string::npos &&
            commandRows.find("controls.ColumnSpacing(8.0);") !=
                std::string::npos &&
            commandRows.find(
              "valueColumn.Width(mux::GridLengthHelper::FromValueAndType(") !=
                std::string::npos &&
            commandRows.find("1.0, mux::GridUnitType::Star") !=
                std::string::npos &&
            commandRows.find(
              "copyColumn.Width(mux::GridLengthHelper::Auto())") !=
                std::string::npos &&
            commandRows.find("muxc::Grid::SetColumn(copy, 1)") !=
                std::string::npos &&
            commandRows.find("value.Width(300.0)") == std::string::npos &&
            commandRows.find("row.Initialize(controls);") !=
                std::string::npos &&
            commandRows.find("row.Initialize(controls, 420.0)") ==
                std::string::npos,
        "developer CLI commands stretch in the default setting column while copy actions remain at the right edge");

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

    Check(source.find("XamlReader::Load") == std::string::npos &&
            source.find("grid.ItemTemplate(itemTemplate)") !=
                std::string::npos &&
            shellXaml.find("SettingsShellFluentGlyphTemplate") !=
                std::string::npos &&
            shellXaml.find("SettingsShellFontAwesomeGlyphTemplate") !=
                std::string::npos &&
            shellXaml.find(
                "ms-appx:///Assets/Fonts/FluentSystemIcons-Regular.ttf#FluentSystemIcons-Regular") !=
                std::string::npos &&
            shellXaml.find(
                "ms-appx:///Assets/Fonts/fa-solid-900.ttf#Font Awesome 6 Free") !=
                std::string::npos,
        "icon references use compiled XAML templates and deployed font resources");

    const std::string applyRegion = sourceRegion(
        "bool ApplySnapshot(const WidgetsPageSnapshot& snapshot)",
        "void RefreshLocalizedText()");
    Check(applyRegion.find("installedPresentationChanged") !=
                std::string::npos &&
            applyRegion.find("if (installedPresentationChanged)") !=
                std::string::npos &&
            applyRegion.find("if (developerPresentationChanged)") !=
                std::string::npos &&
            applyRegion.find("if (debugPresentationChanged)") !=
                std::string::npos &&
            applyRegion.find("if (sourcePresentationChanged)") !=
                std::string::npos,
        "snapshot updates rebuild only presentation regions whose data changed");
    Check(applyRegion.find("TryPatchInstalledRows(previousPackages") !=
                std::string::npos &&
            applyRegion.find("if (!patched)") != std::string::npos &&
            applyRegion.find("RenderInstalledRows();",
                applyRegion.find("if (!patched)")) != std::string::npos,
        "package state snapshots use an in-place row patch before the structural rebuild fallback");

    Check(source.find("actions.confirm(requestGeneration") !=
                std::string::npos &&
            source.find(
                "app.settings.widgets_unsubscribe_and_uninstall") !=
                std::string::npos &&
            source.find("primaryButtonText") != std::string::npos &&
            source.find("WidgetsPageCommand::UninstallPackage") !=
                std::string::npos &&
            source.find("if (!confirmed || !invoke") != std::string::npos,
        "uninstall keeps its legacy normal or Workshop-specific action label and waits for an asynchronous host-owned ContentDialog result");

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
