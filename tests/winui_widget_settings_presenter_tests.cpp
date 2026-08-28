#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

bool ContainsAll(
    std::string_view source,
    std::initializer_list<std::string_view> values)
{
    for (const auto value : values)
        if (source.find(value) == std::string_view::npos)
            return false;
    return true;
}

void TestPublicContract(const std::string& header)
{
    Check(ContainsAll(header, {
              "WidgetSettingsService& service",
              "ApplySnapshot(",
              "EventDispatchers() const",
              "FlushPendingEdits()",
              "FocusTarget(std::string_view settingKey)",
              "void Activate() noexcept",
              "void Deactivate() noexcept",
              "void Close() noexcept"}),
        "presenter exposes a cached service-backed UI lifecycle");
    Check(ContainsAll(header, {
              "WidgetSettingsService::SnapshotChangedCallback",
              "WidgetSettingsService::SearchCompletedCallback",
              "mutationCompleted",
              "diagnostic"}),
        "host receives dispatcher-safe service events, mutation results, and diagnostics");
    Check(header.find("const SettingsSnapshot&") == std::string::npos &&
            header.find("settings_controller.h") == std::string::npos,
        "widget presenter does not retain or accept the application settings snapshot");
}

void TestNativeControlMapping(
    const std::string& source,
    const std::string& sharedControls)
{
    const std::string nativeControlSources = source + sharedControls;
    for (const char* control : {
             "muxc::TextBox", "muxc::PasswordBox",
             "muxc::ToggleSwitch", "muxc::Slider", "muxc::NumberBox",
             "muxc::ColorPicker", "muxc::ComboBox", "muxc::ListView",
             "muxc::CheckBox", "muxc::CalendarDatePicker",
             "muxc::TimePicker", "muxc::AutoSuggestBox",
             "muxc::ProgressRing", "muxc::Expander", "muxc::Button"})
    {
        Check(nativeControlSources.find(control) != std::string::npos,
            "every declarative field family uses a native WinUI control");
    }

    for (const char* kind : {
             "WidgetSettingKind::Text", "WidgetSettingKind::Url",
             "WidgetSettingKind::Password", "WidgetSettingKind::Boolean",
             "WidgetSettingKind::Integer",
             "WidgetSettingKind::FloatingPoint", "WidgetSettingKind::Range",
             "WidgetSettingKind::Color", "WidgetSettingKind::Select",
             "WidgetSettingKind::MultiSelect", "WidgetSettingKind::Date",
             "WidgetSettingKind::Time", "WidgetSettingKind::AppSearch",
             "WidgetSettingKind::AppReference",
             "WidgetSettingKind::DesktopItemReference",
             "WidgetSettingKind::FileReference",
             "WidgetSettingKind::FolderReference",
             "WidgetSettingKind::FileHandle",
             "WidgetSettingKind::FolderHandle", "WidgetSettingKind::Unknown"})
    {
        Check(source.find(kind) != std::string::npos,
            "every v2 setting kind has an explicit presenter branch");
    }
}

void TestResponsiveFieldRows(
    const std::string& source,
    const std::string& sharedControls)
{
    Check(ContainsAll(source, {
              "#include \"settings_presenter_controls.h\"",
              "presenter_controls::SettingRow row",
              "field.row.Initialize(field.editorHost)",
              "field.row.SetText(",
              "field.editorHost.Children().Append(",
              "field.toggle.HorizontalAlignment(mux::HorizontalAlignment::Right)",
              "field.opaqueActions.HorizontalAlignment(",
              "field.numericEditors.HorizontalAlignment(",
              "mux::HorizontalAlignment::Stretch);",
              "followGlobalRow.SetControlAlignment(",
              "restoreScriptDefault.VerticalAlignment(",
              "reset.VerticalAlignment(mux::VerticalAlignment::Center)",
              "group.expander.HorizontalContentAlignment(",
              "field.content.Children().Append(field.validation)",
              "field.content.Children().Append(field.diagnostic)"}),
        "widget fields keep full-width editors and validation while compact controls and reset actions align right and group expanders stretch");
    Check(ContainsAll(sharedControls, {
              "kSettingControlWidth = 300.0",
              "controlWidth, mux::GridUnitType::Pixel",
              "muxc::Grid::SetColumn(text, 0)",
              "muxc::Grid::SetColumn(controlHost, 1)",
              "controlHost.HorizontalContentAlignment(",
              "mux::HorizontalAlignment::Stretch);",
              "void SetControlAlignment(",
              "controlHost.HorizontalAlignment(alignment)",
              "controlHost.HorizontalContentAlignment(alignment)",
              "controlHost.Content().try_as<muxc::ToggleSwitch>()",
              "toggle.MinWidth(0.0)"}),
        "shared setting rows retain a 300-DIP editor column at regular widths and preserve compact-control alignment");
    Check(ContainsAll(sharedControls, {
              "kSettingRowStackThreshold = 700.0",
              "muxc::RowDefinition controlRow",
              "root.RowDefinitions().Append(controlRow)",
              "root.SizeChanged(",
              "args.NewSize().Width < kSettingRowStackThreshold",
              "grid.ColumnSpacing(stacked ? 0.0 : 20.0)",
              "grid.RowSpacing(stacked ? 10.0 : 0.0)",
              "muxc::Grid::SetColumn(responsiveControlHost,",
              "stacked ? 0 : 1",
              "muxc::Grid::SetRow(responsiveControlHost,",
              "stacked ? 1 : 0"}),
        "shared setting rows move the existing editor host below its label at narrow widths instead of compressing both columns");
    Check(ContainsAll(sharedControls, {
              "winrt::make_weak(controlColumn)",
              "winrt::make_weak(text)",
              "winrt::make_weak(controlHost)",
              "[weakControlColumn, weakText, weakControlHost, controlWidth,",
              "autoControlWidth]",
              "weakControlColumn.get()",
              "weakText.get()",
              "weakControlHost.get()"}),
        "responsive row callbacks do not retain or dereference short-lived SettingRow builders");
    Check(ContainsAll(sharedControls, {
              "AutomationProperties::SetName(controlHost, label.Text())",
              "AutomationProperties::SetHelpText(controlHost, help.Text())",
              "controlHost.IsEnabled(enabled)"}) &&
            ContainsAll(source, {
              "mux::FrameworkElement FocusTarget(",
              "if (field.text) return field.text",
              "if (field.password) return field.password",
              "if (field.toggle) return field.toggle",
              "if (field.slider) return field.slider",
              "if (field.colorEditor) return field.colorEditor->button",
              "return field.root;"}),
        "responsive widget rows keep editor automation context, disabled-state semantics, and concrete keyboard focus targets");
}

void TestPopupColorEditing(
    const std::string& source,
    const std::string& sharedControls)
{
    Check(ContainsAll(source, {
              "presenter_controls::ColorFlyoutEditor",
              "SettingsUpdateMode mode",
              "service.SetOrdinary(guard, key,",
              "wr::MakeWidgetSettingInteger(value)",
              "CommitOpenColorEditors()",
              "field->colorEditor->Dismiss()",
              "field.colorEditor->Close()",
              "if (field.colorEditor) return field.colorEditor->button",
              "backgroundColorEditor",
              "borderColorEditor",
              "RunAppearancePatch",
              "commitAppearance(backgroundColorEditor)",
              "commitAppearance(borderColorEditor)",
              "QueueTransientOrdinary(field.schema.key,",
              "CommitTransientOwner(field.schema.key)"}),
        "field and host appearance colors preserve guarded coalesced preview and commit the visible value before teardown");
    Check(ContainsAll(sharedControls, {
              "muxc::Flyout",
              "button.Flyout(flyout)",
              "enum class EditState",
              "EditState::PendingPreview",
              "EditState::Committed",
              "EditState::RolledBack",
              "colorToken = picker.ColorChanged",
              "pointerReleasedToken = picker.PointerReleased",
              "lostFocusToken = picker.LostFocus",
              "keyDownToken = picker.KeyDown",
              "VirtualKey::Enter",
              "closedToken = flyout.Closed",
              "preview.Queue(picker.Color())",
              "changed(color, SettingsUpdateMode::Preview)",
              "changed(picker.Color(), SettingsUpdateMode::PreviewAndCommit)",
              "changed(original, SettingsUpdateMode::PreviewAndCommit)",
              "picker.PointerReleased(pointerReleasedToken)",
              "picker.LostFocus(lostFocusToken)",
              "picker.KeyDown(keyDownToken)",
              "swatch.Background",
              "kContinuousPreviewInterval{33}"}),
        "the shared swatch coalesces live previews, commits interaction boundaries, and unloads every handler");

    const auto cancelStart = sharedControls.find(
        "cancelToken = cancel.Click");
    const auto cancelEnd = sharedControls.find(
        "closedToken = flyout.Closed", cancelStart);
    const auto cancelRollback = sharedControls.find(
        "Rollback();", cancelStart);
    const auto cancelHide = sharedControls.find(
        "flyout.Hide();", cancelStart);
    const auto lightDismissStart = sharedControls.find(
        "closedToken = flyout.Closed");
    const auto lightDismissEnd = sharedControls.find(
        "UpdateSwatch();", lightDismissStart);
    const auto lightDismissCommit = sharedControls.find(
        "Commit();", lightDismissStart);
    const auto lightDismissDeactivate = sharedControls.find(
        "open = false;", lightDismissStart);
    const auto dismissStart = sharedControls.find("void Dismiss() noexcept");
    const auto dismissEnd = sharedControls.find(
        "void Close() noexcept", dismissStart);
    const auto dismissCommit = sharedControls.find("Commit();", dismissStart);
    const auto dismissHide = sharedControls.find("flyout.Hide();", dismissStart);
    const auto closeStart = dismissEnd;
    const auto closeCommit = sharedControls.find("Commit();", closeStart);
    const auto closeDisable = sharedControls.find("closed = true;", closeStart);
    Check(cancelStart != std::string::npos &&
            cancelEnd != std::string::npos &&
            cancelRollback < cancelHide && cancelHide < cancelEnd &&
            sharedControls.find("applyToken = apply.Click") ==
                std::string::npos &&
            sharedControls.find("actions.Children().Append(apply)") ==
                std::string::npos &&
            lightDismissStart != std::string::npos &&
            lightDismissCommit < lightDismissDeactivate &&
            lightDismissDeactivate < lightDismissEnd &&
            dismissStart != std::string::npos &&
            dismissCommit < dismissHide && dismissHide < dismissEnd &&
            closeStart != std::string::npos &&
            closeCommit < closeDisable,
        "explicit Cancel restores the opening color, while light-dismiss, navigation, and close commit the visible color without an Apply button");
    const auto rollbackStart = sharedControls.find("void Rollback()");
    const auto rollbackEnd = sharedControls.find(
        "void UpdateSwatch()", rollbackStart);
    const std::string_view rollbackBody =
        rollbackStart != std::string::npos &&
                rollbackEnd != std::string::npos
            ? std::string_view(sharedControls).substr(
                  rollbackStart, rollbackEnd - rollbackStart)
            : std::string_view{};
    Check(rollbackBody.find("picker.Color(original)") !=
                std::string_view::npos &&
            rollbackBody.find(
              "changed(original, SettingsUpdateMode::PreviewAndCommit)") !=
                std::string_view::npos &&
            rollbackBody.find("EditState::PendingPreview") ==
                std::string_view::npos,
        "Cancel formally commits the opening color even after an intermediate interaction commit");
    Check(source.find("CommitOpenColorEditors") != std::string::npos,
        "widget teardown commits any visible color session before flushing transient edits");
}

void TestOpaqueChannels(const std::string& source)
{
    Check(ContainsAll(source, {
              "service.SetSecret(",
              "service.ChooseFilesystemHandle(",
              "service.OpenEntityReferencePicker(",
              "service.ClearOpaque(",
              "SecureZeroMemory"}),
        "secret, owner-scoped handles, and logical references use dedicated channels");
    Check(source.find("Password(L\"\")") != std::string::npos &&
            source.find("state.opaque.displayLabel") != std::string::npos,
        "password UI is cleared and opaque values expose only display state");
    Check(source.find("MakeWidgetSettingString(state.opaque") ==
            std::string::npos &&
            source.find("SetOrdinary(guard, key, plaintext") ==
            std::string::npos,
        "opaque values never degrade into ordinary persisted strings");
}

void TestSnapshotAndAsyncSafety(const std::string& source)
{
    Check(ContainsAll(source, {
              "WidgetSettingMutationGuard Guard() const",
              "return {widgetId, generation, revision}",
              "service.Snapshot(snapshot.widgetId)",
              "snapshot.revision <= revision",
              "snapshot->generation != hint.generation",
              "snapshot->revision != hint.revision"}),
        "mutations and snapshot hints are guarded by current identity and revision");
    Check(ContainsAll(source, {
              "DispatcherQueue::GetForCurrentThread()",
              "dispatcher.TryEnqueue",
              "bridge->closed",
              "bridge->epoch != epoch",
              "dispatch->snapshot = {}",
              "dispatch->search = {}"}),
        "service hints cross a shutdown-aware DispatcherQueue bridge");
    Check(ContainsAll(source, {
              "service.SearchSnapshot(",
              "service.SetSearchQuery(",
              "field.search.Text(ToText(state.searchQuery))",
              "snapshot.requestId < field.searchRequestId",
              "snapshot->requestId != hint.requestId",
              "service.CancelSearch(",
              "service.CommitSearchResult("}),
        "async search results are request-, generation-, and close-scoped");

    const auto begin = source.find("void BeginSearch(");
    const auto commit = source.find("void CommitSearchResult(", begin);
    const std::string_view beginSearch =
        begin != std::string::npos && commit != std::string::npos
        ? std::string_view(source).substr(begin, commit - begin)
        : std::string_view{};
    const auto applied = beginSearch.find("ApplySnapshot(*current)");
    const auto resolved = beginSearch.find("fieldsByKey.find(key)");
    Check(ContainsAll(beginSearch, {
              "const std::string key = field.schema.key",
              "const bool queryEmpty = query.empty()",
              "WidgetFieldControl& currentField = *current->second"}) &&
            applied != std::string_view::npos &&
            resolved != std::string_view::npos && applied < resolved,
        "BeginSearch copies identity and re-resolves its control after a snapshot may rebuild fields");
    Check(source.find("field.search.MaxLength(") == std::string::npos,
        "AutoSuggestBox uses service byte validation instead of an unsupported MaxLength API");
}

void TestDeclarativeBehavior(const std::string& source)
{
    Check(ContainsAll(source, {
              "schema.collapsible",
              "schema.defaultExpanded",
              "rememberedGroupExpansion",
              "state.visible",
              "state.enabled",
              "state->valid",
              "state->validationError"}),
        "groups, dependency visibility/enabled state, and validation come from the snapshot");
    Check(ContainsAll(source, {
              "service.ApplyPreset(",
              "service.UpdateHostAppearance(",
              "service.Reset(",
              "cachedPresets",
              "defaultPresetId",
              "__global_dark", "__global_light",
              "__global_glass_dark", "__global_glass_light",
              "__global_acrylic_dark", "__global_acrylic_light",
              "__custom",
              "MakeAppearancePreset(",
              "restoreScriptDefault",
              "restore_default_settings"}),
        "appearance, immediate preset selection, script-default restore, and ordinary reset retain the legacy semantics");
    const auto ungrouped = source.find(
        "fieldsHost.Children().Append(ungrouped)");
    const auto orderedGroups = source.find(
        "fieldsHost.Children().Append(group.root)", ungrouped);
    Check(ungrouped != std::string::npos &&
            orderedGroups != std::string::npos &&
            ungrouped < orderedGroups,
        "ungrouped declarative fields precede authored groups");
    Check(source.find("apply_preset") == std::string::npos &&
            source.find("presetCombo.SelectionChanged") !=
                std::string::npos &&
            source.find("appearanceTheme.SelectionChanged") !=
                std::string::npos,
        "preset combos apply immediately without a second Apply button");

    std::size_t order = 0;
    for (const char* fragment : {
             "appearanceCard.content.Children().Append(followGlobalRow.root)",
             "appearanceCard.content.Children().Append(appearanceThemeRow.root)",
             "backgroundColorEditor->row.root",
             "customAppearanceHost.Children().Append(backgroundOpacity.row.root)",
             "customAppearanceHost.Children().Append(borderColorEditor->row.root)",
             "customAppearanceHost.Children().Append(borderOpacity.row.root)",
             "customAppearanceHost.Children().Append(gradientEndOpacity.row.root)",
             "customAppearanceHost.Children().Append(glassRow.root)",
             "customAppearanceHost.Children().Append(acrylicRow.root)",
             "customAppearanceHost.Children().Append(contentThemeRow.root)",
             "root.Children().Append(appearanceCard.root)",
             "root.Children().Append(stylePreviewCard.root)",
             "root.Children().Append(scriptSettingsTitle)",
             "root.Children().Append(fieldsHost)",
             "root.Children().Append(resetCard.root)"})
    {
        const auto next = source.find(fragment, order);
        Check(next != std::string::npos,
            "custom appearance, preset, fields, and reset controls retain the legacy order");
        if (next != std::string::npos) order = next + 1;
    }
    Check(ContainsAll(source, {
              "WidgetSettingKind::Unknown",
              "BuildTextEditor(*field)",
              "DiagnosticCode()",
              "unknown_type"}),
        "unknown field kinds fall back to text and emit a visible diagnostic");
    Check(ContainsAll(source, {
              "AutomationProperties::SetName",
              "AutomationProperties::SetItemStatus",
              "TextWrapping",
              "FocusTarget("}),
        "dynamic controls remain keyboard-focusable and expose automation text");
}

void TestPendingEditCommitSafety(const std::string& source)
{
    Check(ContainsAll(source, {
              "WidgetSettingMutationResult FlushPendingEdits()",
              "if (!result.Succeeded())",
              "current->second->text.Text(ToWide(value))",
              "current->second->textDirty = true",
              "current->second->password.Password(restored)",
              "current->second->passwordDirty = true",
              "SecureZeroMemory(restored.data()"}),
        "failed text and secret commits retain the pending editor value");
    Check(source.find("const auto result = impl_->FlushPendingEdits()") !=
                std::string::npos &&
            source.find("if (!result.Succeeded())\n            return;") !=
                std::string::npos,
        "deactivation refuses to discard a failed final commit");
}

void TestTransientPreviewAndDraftValidation(
    const std::string& source, const std::string& shell)
{
    Check(ContainsAll(source, {
              "std::chrono::milliseconds(650)",
              "mux::DispatcherTimer",
              "QueueServiceHint(dispatch",
              "pendingTransientPreviews.insert_or_assign",
              "service.PreviewOrdinary(",
              "service.PreviewHostAppearance(",
              "service.CommitPreview(Guard())",
              "PointerReleased(",
              "LostFocus(",
              "if (IsEnter(args))",
              "CommitAllTransient()"}),
        "numeric and color changes coalesce on the dispatcher, preview live, and commit on release, focus, Enter, idle, or final flush");
    Check(ContainsAll(source, {
              "field.textDirty = true",
              "field.passwordDirty = true",
              "field.idleCommitTimer.Start()",
              "field.draftError",
              "result.errorCode",
              "field.schema.validationMessage",
              "RefreshFieldValidation(field)",
              "AutomationProperties::SetItemStatus"}),
        "text and secret idle commits retain drafts while row-scoped custom validation and error codes remain visible");
    Check(shell.find(
              "WidgetSettingMutationStatus::InvalidValue") !=
                std::string::npos,
        "the shell leaves invalid widget drafts to their SettingRow instead of replacing them with a generic InfoBar");
}

void TestNoLegacyImGuiPath(const std::string& source)
{
    Check(source.find("ImGui") == std::string::npos &&
            source.find("imgui") == std::string::npos &&
            source.find("lua_ImGui") == std::string::npos &&
            source.find("winui.") == std::string::npos,
        "v2 widget settings UI has no ImGui renderer or new Lua WinUI API");
}
}

int main(int argc, char** argv)
{
    const std::filesystem::path repository = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::current_path();
    const std::string header = ReadText(
        repository / "src/winui/widget_settings_presenter.h");
    const std::string source = ReadText(
        repository / "src/winui/widget_settings_presenter.cpp");
    const std::string sharedControls = ReadText(
        repository / "src/winui/settings_presenter_controls.h");
    const std::string shell = ReadText(
        repository / "src/winui/SettingsShell.xaml.cpp");

    Check(!header.empty() && !source.empty() && !sharedControls.empty() &&
            !shell.empty(),
        "widget settings presenter sources are readable");
    TestPublicContract(header);
    TestNativeControlMapping(source, sharedControls);
    TestResponsiveFieldRows(source, sharedControls);
    TestPopupColorEditing(source, sharedControls);
    TestOpaqueChannels(source);
    TestSnapshotAndAsyncSafety(source);
    TestDeclarativeBehavior(source);
    TestPendingEditCommitSafety(source);
    TestTransientPreviewAndDraftValidation(source, shell);
    TestNoLegacyImGuiPath(source);

    if (failures == 0)
    {
        std::cout << "WinUI widget settings presenter checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures
              << " WinUI widget settings presenter check(s) failed\n";
    return EXIT_FAILURE;
}
