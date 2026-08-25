#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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

void TestPresenterContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/dock_page_presenter.h");
    const std::string source = ReadText(
        repository / "src/winui/dock_page_presenter.cpp");
    const std::string controls = ReadText(
        repository / "src/winui/settings_presenter_controls.h");

    Check(!header.empty() && !source.empty() && !controls.empty(),
        "Dock presenter and shared control sources are readable");
    Check(header.find("std::uint64_t generation") != std::string::npos &&
            header.find("SettingsUpdateMode mode") != std::string::npos &&
            header.find("GeneralEdit edit") != std::string::npos &&
            header.find("DockEdit edit") != std::string::npos,
        "edits carry generation and update mode without a stale snapshot");
    Check(source.find("snapshot.domainRevisions.general") !=
                std::string::npos &&
            source.find("snapshot.domainRevisions.dock") !=
                std::string::npos &&
            source.find("SettingsSnapshot snapshot_") == std::string::npos &&
            source.find("DockSettings settings_") == std::string::npos &&
            source.find("updatingControls") != std::string::npos,
        "only changed General and Dock domains patch cached controls");

    for (const char* field : {
             "dockEnabled", "position", "edgeAttached",
             "floatingShortcutMode", "floatingEdgeSwipeEnabled",
             "monitorScope", "showWindowsButton", "showFrequentItems",
             "frequentItemCount", "keepWhenDesktopHidden",
             "thicknessScale", "systemTaskbarAutoHide",
             "systemTaskbarAlignment", "systemTaskbarBackdropEnabled",
             "systemTaskbarFollowPersonalization",
             "systemTaskbarContentTheme", "systemTaskbarAppearance",
             "systemTaskbarVisibleWindow",
             "systemTaskbarMaximizedWindow", "systemTaskbarShellUi"})
    {
        Check(source.find(field) != std::string::npos,
            "the requested Dock/taskbar field has a real binding");
    }
    for (const char* control : {
             "muxc::ToggleSwitch", "muxc::ComboBox", "muxc::Slider",
             "muxc::NumberBox", "muxc::Button", "muxc::Expander",
             "muxc::InfoBar", "muxc::RadioButtons"})
    {
        Check(source.find(control) != std::string::npos,
            "the Dock page uses native WinUI controls");
    }
    Check(source.find("ColorFlyoutEditor editor") != std::string::npos &&
            controls.find("muxc::ColorPicker picker") != std::string::npos,
        "Dock colors use the shared native WinUI ColorPicker flyout");
    Check(source.find("void CommitOpenColorEditors() noexcept") !=
                std::string::npos &&
            source.find("impl_->CommitOpenColorEditors();") !=
                std::string::npos &&
            controls.find("pointerReleasedToken = picker.PointerReleased") !=
                std::string::npos &&
            controls.find("lostFocusToken = picker.LostFocus") !=
                std::string::npos &&
            controls.find("keyDownToken = picker.KeyDown") !=
                std::string::npos &&
            controls.find("changed(original, SettingsUpdateMode::PreviewAndCommit)") !=
                std::string::npos &&
            source.find("control.preview.Queue(value)") !=
                std::string::npos &&
            controls.find("kContinuousPreviewInterval{33}") !=
                std::string::npos &&
            controls.find("applyToken = apply.Click") ==
                std::string::npos,
        "Dock previews are frame-coalesced and colors commit on light-dismiss or page teardown while explicit Cancel rolls back");
    Check(source.find("SettingsUpdateMode::Preview,") !=
                std::string::npos &&
            source.find("SettingsUpdateMode::PreviewAndCommit,") !=
                std::string::npos &&
            source.find("PointerReleased") != std::string::npos &&
            source.find("LostFocus") != std::string::npos &&
            source.find("IsEnter(args)") != std::string::npos &&
            source.find("mux::DispatcherTimer") != std::string::npos &&
            source.find("std::chrono::milliseconds(650)") !=
                std::string::npos,
        "continuous controls preview and commit on release, focus, Enter, or keyboard idle");
    Check(source.find("QuantizeNumericValue(") != std::string::npos &&
            source.find("control.slider.StepFrequency()") !=
                std::string::npos &&
            controls.find("inline double QuantizeNumericValue(") !=
                std::string::npos,
        "Dock numeric snapshots are quantized to their declared slider step");
    Check(source.find("SetUnit(control, L\"%\")") !=
                std::string::npos &&
            source.find("SetUnit(control, L\"px\")") !=
                std::string::npos &&
            source.find("ExtractNumericUnit(") != std::string::npos &&
            source.find("app.settings.items_unit") != std::string::npos,
        "Dock percentages, blur pixels, and localized item-count units match the legacy surface");
    const auto taskbarBehaviorSection = source.find(
        "InitializeCard(taskbarCard, cardStyle, taskbarRoot)");
    const auto taskbarAppearanceSection = source.find(
        "InitializeCard(taskbarAppearanceCard, cardStyle, taskbarRoot)");
    const auto taskbarRulesSection = source.find(
        "InitializeCard(taskbarRulesCard, cardStyle, taskbarRoot)");
    const auto taskbarSystemPanelSection = source.find(
        "InitializeCard(taskbarSystemPanelCard, cardStyle, taskbarRoot)");
    Check(taskbarBehaviorSection != std::string::npos &&
            taskbarBehaviorSection < taskbarAppearanceSection &&
            taskbarAppearanceSection < taskbarRulesSection &&
            taskbarRulesSection < taskbarSystemPanelSection &&
            source.find("taskbarRoot.Children().RemoveAt(") ==
                std::string::npos,
        "Taskbar content is naturally grouped as behavior, default appearance, scenario overrides, then system panels");
    Check(source.find(
              "SetContinuousText(thicknessScale,\n            \"app.settings.dock_thickness\"") !=
                std::string::npos &&
            source.find("app.settings.dock_thickness_hint") !=
                std::string::npos,
        "Dock thickness retains its legacy explanatory text on the setting row");

    const auto edgeHintStart = source.find(
        "void UpdateEdgeSwipeHintVisibility()");
    const auto edgeHintEnd = source.find(
        "void RefreshTaskbarRuntimeStatus()", edgeHintStart);
    const std::string_view edgeHint =
        edgeHintStart != std::string::npos && edgeHintEnd != std::string::npos
            ? std::string_view(source).substr(
                  edgeHintStart, edgeHintEnd - edgeHintStart)
            : std::string_view{};
    const auto edgeToggleStart = source.find(
        "floatingEdgeSwipeToken = floatingEdgeSwipeToggle.Toggled(");
    const auto edgeToggleEnd = source.find(
        "showWindowsButtonToken", edgeToggleStart);
    const std::string_view edgeToggle =
        edgeToggleStart != std::string::npos &&
                edgeToggleEnd != std::string::npos
            ? std::string_view(source).substr(
                  edgeToggleStart, edgeToggleEnd - edgeToggleStart)
            : std::string_view{};
    Check(edgeHint.find("floatingEdgeSwipeRow.help.Visibility(") !=
                std::string_view::npos &&
            edgeHint.find("floatingEdgeSwipeToggle.IsOn()") !=
                std::string_view::npos &&
            edgeHint.find("dockEnabled") == std::string_view::npos &&
            edgeToggle.find("UpdateEdgeSwipeHintVisibility();") !=
                std::string_view::npos &&
            source.find(
              "RefreshTaskbarRuntimeStatus();\n        UpdateDependentStates();\n    }\n\n    void UpdateDependentStates()") !=
                std::string::npos &&
            source.find(
              "updatingControls = previousUpdating;\n        UpdateDependentStates();") !=
                std::string::npos,
        "edge-swipe help follows only its own toggle across toggle, snapshot, and localization refreshes");

    Check(source.find("actions.confirm") != std::string::npos &&
            source.find("Action::RestartExplorer") != std::string::npos &&
            source.find("invokeHost(expectedGeneration") !=
                std::string::npos &&
            source.find("RestartWindowsExplorer()") == std::string::npos,
        "Explorer restart requires confirmation and a typed host request");
    Check(source.find("IsSystemTaskbarAutoHideEnabled()") ==
                std::string::npos &&
            source.find("IsSystemTaskbarAlignmentCentered()") ==
                std::string::npos &&
            source.find("SyncSystemTaskbarSettingsFromWindows") ==
                std::string::npos,
        "the presenter never overwrites requested taskbar values by rereading Windows");
    Check(source.find("SystemTaskbarDynamicRule DockSettings::* member") !=
                std::string::npos &&
            source.find("PrepareDynamicRuleTheme") != std::string::npos &&
            source.find("contentTheme =") != std::string::npos,
        "all three dynamic rules share enabled, theme, and content-theme bindings");
    const auto shellUiRule = source.find(
        "InitializeDynamicRule(shellUiRule");
    const auto maximizedRule = source.find(
        "InitializeDynamicRule(maximizedWindowRule");
    const auto visibleRule = source.find(
        "InitializeDynamicRule(visibleWindowRule");
    Check(shellUiRule != std::string::npos &&
            shellUiRule < maximizedRule && maximizedRule < visibleRule &&
            source.find(
              "&shellUiRule, &maximizedWindowRule, &visibleWindowRule") !=
                std::string::npos,
        "scenario overrides display and iterate in shell-UI, maximized-window, visible-window priority order");
    const auto ruleToggle = source.find(
        "control.details.Children().Append(control.enabledRow.root)");
    const auto ruleAppearance = source.find(
        "control.details.Children().Append(control.appearanceDetails)");
    const auto ruleDisclosure = source.find(
        "control.root.Children().Append(control.expander)");
    Check(source.find("control.expander = muxc::Expander{}") !=
                std::string::npos &&
            source.find("control.expander.IsExpanded(false)") !=
                std::string::npos &&
            source.find("control.expander.Content(control.details)") !=
                std::string::npos &&
            ruleToggle != std::string::npos &&
            ruleAppearance != std::string::npos &&
            ruleDisclosure != std::string::npos &&
            ruleToggle < ruleAppearance &&
            ruleAppearance < ruleDisclosure &&
            source.find("control.detailTitle.Text(context)") !=
                std::string::npos &&
            source.find(
              "control.enabledRow.SetText(\n            L(\"app.settings.widgets_enabled\", L\"Enabled\"))") !=
                std::string::npos &&
            source.find("control->appearanceDetails.Visibility(enabled") !=
                std::string::npos &&
            source.find("? mux::Visibility::Visible") !=
                std::string::npos &&
            source.find(": mux::Visibility::Collapsed)") !=
                std::string::npos &&
            source.find("control->themeRow.SetEnabled(enabled)") !=
                std::string::npos &&
            source.find("return visibleWindowRule.expander") !=
                std::string::npos &&
            source.find("return maximizedWindowRule.expander") !=
                std::string::npos &&
            source.find("return shellUiRule.expander") !=
                std::string::npos &&
            source.find("Children().Clear()") == std::string::npos &&
            source.find("SetDynamicRuleAutomation(") !=
                std::string::npos &&
            source.find(
              "AutomationProperties::SetHelpText(control.expander") !=
                std::string::npos,
        "each condition is a named, collapsed disclosure whose nested enable switch gates its appearance children without rebuilding the page, and routed keyboard focus lands on the visible header");
    Check(source.find("floatingEdgeSwipeRow.SetEnabled(dockEnabled)") !=
                std::string::npos &&
            source.find("positionRow.SetEnabled(dockEnabled)") !=
                std::string::npos &&
            source.find("layoutRow.SetEnabled(dockEnabled)") !=
                std::string::npos &&
            source.find("thicknessScale.row.SetEnabled(dockEnabled)") !=
                std::string::npos &&
            source.find("showWindowsButtonRow.SetEnabled(dockEnabled)") !=
                std::string::npos &&
            source.find("showFrequentItemsRow.SetEnabled(dockEnabled)") !=
                std::string::npos &&
            source.find("taskbarAutoHideRow.SetEnabled(dockEnabled)") ==
                std::string::npos &&
            source.find("edgeSwipeCard.root.IsHitTestVisible(dockEnabled)") !=
                std::string::npos &&
            source.find("taskbarContentThemeRow.root.Visibility(taskbarStyled") !=
                std::string::npos &&
            source.find("ReplaceMainContentThemeItems") !=
                std::string::npos,
        "Dock descendants leave pointer and keyboard input when disabled while the independent taskbar controls retain legacy semantics");
    Check(source.find(
              "\"settings.dock.edgeSwipe\", L\"Floating Dock edge gesture\"") !=
                std::string::npos &&
            source.find(
              "\"settings.dock.layoutAndPosition\", L\"Position and layout\"") !=
                std::string::npos &&
            source.find(
              "\"settings.dock.itemsAndBehavior\", L\"Items and behavior\"") !=
                std::string::npos,
        "Dock cards use task-level group names instead of repeating the first row label");
    Check(source.find("taskbarHookRequired = settings.systemTaskbarBackdropEnabled") !=
                std::string::npos &&
            source.find("if (taskbarHookRequired)") !=
                std::string::npos &&
            source.find("muxc::InfoBar taskbarRuntimeStatus") !=
                std::string::npos &&
            source.find("taskbarRuntimeStatus.IsClosable(false)") !=
                std::string::npos &&
            source.find("muxc::InfoBarSeverity::Warning") !=
                std::string::npos &&
            source.find("muxc::InfoBarSeverity::Error") !=
                std::string::npos &&
            source.find("taskbarRuntimeStatus.IsOpen(!text.empty())") !=
                std::string::npos,
        "taskbar hook state uses a non-closable severity-aware InfoBar only when visual integration is requested");
    Check(source.find("control.editor.Close()") !=
                std::string::npos &&
            controls.find("picker.ColorChanged(colorToken)") !=
                std::string::npos &&
            source.find("ValueChanged(control.sliderChanged)") !=
                std::string::npos &&
            source.find("SelectionChanged(control.themeToken)") !=
                std::string::npos,
        "Close revokes custom appearance, continuous, and dynamic rule events");
    Check(source.find(
              "dockEnabledToggle.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "toggle.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find("taskbarAutoHideRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("taskbarGlassRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("control.enabledRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("control.glassRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("control.acrylicRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("taskbarAlignmentChoices = NewInlineChoices()") !=
                std::string::npos &&
            source.find("windowsSystemThemeChoices = NewInlineChoices()") !=
                std::string::npos &&
            source.find("choices.MaxColumns(2)") !=
                std::string::npos &&
            source.find(
              "windowsSystemThemeRow.Initialize(windowsSystemThemeChoices)") !=
                std::string::npos &&
            source.find(
              "restartExplorerRow.Initialize(restartExplorerButton)") !=
                std::string::npos &&
            source.find("restartExplorerRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find(
              "control.reset.VerticalAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos,
        "Dock toggles and compact actions align right while binary taskbar choices use responsive RadioButtons and Explorer restart has its own row");
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the Dock presenter contract");
    if (argc == 2)
        TestPresenterContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures
                  << " WinUI Dock presenter check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI Dock presenter checks passed\n";
    return EXIT_SUCCESS;
}
