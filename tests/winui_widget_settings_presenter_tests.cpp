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

void TestNativeControlMapping(const std::string& source)
{
    for (const char* control : {
             "muxc::TextBox", "muxc::PasswordBox",
             "muxc::ToggleSwitch", "muxc::Slider", "muxc::NumberBox",
             "muxc::ColorPicker", "muxc::ComboBox", "muxc::ListView",
             "muxc::CheckBox", "muxc::CalendarDatePicker",
             "muxc::TimePicker", "muxc::AutoSuggestBox",
             "muxc::ProgressRing", "muxc::Expander", "muxc::Button"})
    {
        Check(source.find(control) != std::string::npos,
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
              "snapshot.requestId < field.searchRequestId",
              "snapshot->requestId != hint.requestId",
              "service.CancelSearch(",
              "service.CommitSearchResult("}),
        "async search results are request-, generation-, and close-scoped");
}

void TestDeclarativeBehavior(const std::string& source)
{
    Check(ContainsAll(source, {
              "schema.collapsible",
              "schema.defaultExpanded",
              "rememberedGroupExpansion",
              "state.visible",
              "state.enabled",
              "state.valid",
              "state.validationError"}),
        "groups, dependency visibility/enabled state, and validation come from the snapshot");
    Check(ContainsAll(source, {
              "service.ApplyPreset(",
              "service.Reset(",
              "cachedPresets",
              "defaultPresetId"}),
        "presets and ordinary reset use transactional service operations");
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

    Check(!header.empty() && !source.empty(),
        "widget settings presenter sources are readable");
    TestPublicContract(header);
    TestNativeControlMapping(source);
    TestOpaqueChannels(source);
    TestSnapshotAndAsyncSafety(source);
    TestDeclarativeBehavior(source);
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
