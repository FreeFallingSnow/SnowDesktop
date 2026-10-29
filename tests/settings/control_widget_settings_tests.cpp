#include "settings/control_widget_settings.h"
#include <iostream>
#include <stdexcept>

using namespace snowdesktop;
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
using namespace snowdesktop::widget_runtime;

namespace
{
int failures = 0;
void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class F> void Reject(F operation, std::string_view code, const char* message)
{
    try { operation(); Check(false, message); }
    catch (const Error& error) { Check(error.code == code, message); }
}

// A service boundary double observes forwarding and prevents accidental secret
// reads. Validation/storage behavior itself is covered by widget_settings_service.
class Service final : public IWidgetSettingsService
{
public:
    WidgetSettingsSnapshot state;
    int mutations = 0, loads = 0, secretReads = 0, clears = 0;
    Service()
    {
        state.widgetId = L"instance"; state.packageId = "example";
        state.generation = 7; state.revision = 11;
        WidgetSettingFieldState enabled;
        enabled.schema.key = "enabled"; enabled.schema.rawType = "bool";
        enabled.currentValue = MakeWidgetSettingBoolean(true);
        enabled.defaultValue = MakeWidgetSettingBoolean(false);
        state.fields.push_back(enabled);
        WidgetSettingFieldState secret;
        secret.schema.key = "apiKey"; secret.schema.rawType = "password";
        secret.currentValue = MakeWidgetSettingString("do-not-expose");
        secret.defaultValue = MakeWidgetSettingString("do-not-expose-default");
        secret.opaque.configured = true; secret.opaque.available = true;
        secret.opaque.canClear = true; state.fields.push_back(secret);
    }
    WidgetSettingMutationResult Ok() { ++mutations; ++state.revision; return {WidgetSettingMutationStatus::Applied, state.generation, state.revision}; }
    WidgetSettingMutationResult Unsupported() { return {WidgetSettingMutationStatus::Unavailable, state.generation, state.revision, "unavailable"}; }
    void SetEventCallbacks(SnapshotChangedCallback, SearchCompletedCallback) override {}
    WidgetSettingsLoadResult Load(std::wstring) override { ++loads; return {WidgetSettingMutationStatus::Applied, state}; }
    WidgetSettingsLoadResult Reload(std::wstring_view) override { return {WidgetSettingMutationStatus::Applied, state}; }
    std::optional<WidgetSettingsSnapshot> Snapshot(std::wstring_view id) const override { return id == state.widgetId ? std::optional(state) : std::nullopt; }
    void Close(std::wstring_view) override {}
    void CloseAll() override {}
    WidgetSettingMutationResult SetOrdinary(const WidgetSettingMutationGuard& guard, std::string_view key, const InteractionValue& value) override
    {
        if (!guard.Matches(state)) return {WidgetSettingMutationStatus::StaleSnapshot, 0, 0, "staleSnapshot"};
        if (key == "apiKey") return {WidgetSettingMutationStatus::WrongValueChannel, 0, 0, "wrongValueChannel"};
        if (key != "enabled") return {WidgetSettingMutationStatus::SettingNotFound, 0, 0, "settingNotFound"};
        if (value.type != InteractionValue::Type::Boolean) return {WidgetSettingMutationStatus::InvalidValue, 0, 0, "expectedBoolean"};
        state.fields[0].currentValue = value; return Ok();
    }
    WidgetSettingMutationResult PreviewOrdinary(const WidgetSettingMutationGuard&, std::string_view, const InteractionValue&) override { return Unsupported(); }
    WidgetSettingMutationResult SetSearchQuery(const WidgetSettingMutationGuard&, std::string_view, std::string) override { return Unsupported(); }
    WidgetSettingSecretReadResult ReadSecret(const WidgetSettingMutationGuard&, std::string_view) override { ++secretReads; return {}; }
    WidgetSettingMutationResult SetSecret(const WidgetSettingMutationGuard&, std::string_view key, std::string_view value) override
    { if (key != "apiKey" || value.empty()) return Unsupported(); state.fields[1].opaque.configured = true; return Ok(); }
    WidgetSettingMutationResult ChooseFilesystemHandle(const WidgetSettingMutationGuard&, std::string_view) override { return Unsupported(); }
    WidgetSettingMutationResult OpenEntityReferencePicker(const WidgetSettingMutationGuard&, std::string_view) override { return Unsupported(); }
    WidgetSettingMutationResult ClearOpaque(const WidgetSettingMutationGuard&, std::string_view key) override
    { if (key != "apiKey") return Unsupported(); ++clears; state.fields[1].opaque.configured = false; return Ok(); }
    WidgetSettingMutationResult ApplyPreset(const WidgetSettingMutationGuard&, std::string_view) override { return Unsupported(); }
    WidgetSettingMutationResult UpdateHostAppearance(const WidgetSettingMutationGuard&, const WidgetHostAppearancePatch& patch) override
    { if (patch.backgroundOpacity) state.hostAppearance.backgroundOpacity = *patch.backgroundOpacity; return Ok(); }
    WidgetSettingMutationResult PreviewHostAppearance(const WidgetSettingMutationGuard&, const WidgetHostAppearancePatch&) override { return Unsupported(); }
    WidgetSettingMutationResult CommitPreview(const WidgetSettingMutationGuard&) override { return Unsupported(); }
    WidgetSettingMutationResult RevertPreview(const WidgetSettingMutationGuard&) override { return Unsupported(); }
    WidgetSettingMutationResult ResetField(const WidgetSettingMutationGuard&, std::string_view key) override
    { if (key != "enabled") return {WidgetSettingMutationStatus::WrongValueChannel, 0, 0, "wrongValueChannel"}; state.fields[0].currentValue = state.fields[0].defaultValue; return Ok(); }
    WidgetSettingMutationResult Reset(const WidgetSettingMutationGuard& guard) override { return ResetField(guard, "enabled"); }
    WidgetSettingMutationResult StartSearch(const WidgetSettingMutationGuard&, std::string_view, std::string, std::size_t) override { return Unsupported(); }
    WidgetSettingMutationResult CancelSearch(const WidgetSettingMutationGuard&, std::string_view) override { return Unsupported(); }
    WidgetSettingMutationResult CommitSearchResult(const WidgetSettingMutationGuard&, std::string_view, std::uint64_t, std::string_view) override { return Unsupported(); }
    std::optional<WidgetSettingSearchSnapshot> SearchSnapshot(std::wstring_view, std::string_view) const override { return std::nullopt; }
};
}

int main()
{
    Service service;
    const Target target{"widget", "instance"};
    const auto call = [&](std::string_view method, Value extra = Object()) {
        extra.object.emplace("target", TargetJson(target));
        return HandleWidgetSettings(method, extra, target, service, [](auto) { return true; });
    };
    const auto read = call("settings.get");
    Check(service.loads == 0, "reading an open editor preserves its service session");
    Check(read.Find("revision")->string == "7:11", "revision includes generation and service revision");
    Check(Serialize(read).find("do-not-expose") == std::string::npos, "ordinary snapshot never leaks a secret or secret default");
    const auto catalogue = call("settings.catalog");
    Check(catalogue.Find("fields")->array.size() == 2, "all declarative fields appear in catalogue including opaque fields");
    Check(!catalogue.Find("fields")->array[1].Find("default"), "secret default is omitted from catalogue");
    const auto update = [&] (Value changes, std::string revision = "7:11") {
        return call("settings.update", Object({{"ifRevision", String(revision)}, {"changes", std::move(changes)}}));
    };
    Reject([&] { update(Object({{"enabled", Boolean(false)}}), "stale"); }, "stale_settings", "stale edits are rejected before service mutation");
    Check(service.mutations == 0, "stale edit has no side effects");
    Reject([&] { update(Object({{"enabled", Boolean(false)}, {"another", Boolean(true)}})); }, "invalid_params", "non-atomic multi-field edits are rejected before mutation");
    Reject([&] { update(Object({{"enabled", String("false")}})); }, "expectedBoolean", "service type validation is preserved");
    Reject([&] { update(Object({{"unknown", Boolean(false)}})); }, "settingNotFound", "unknown field cannot become arbitrary storage");
    Reject([&] { update(Object({{"apiKey", String("ordinary-secret")}})); }, "wrongValueChannel", "secret cannot use ordinary storage channel");
    const auto changed = update(Object({{"enabled", Boolean(false)}}));
    Check(changed.Find("revision")->string == "7:12" && !service.state.fields[0].currentValue.boolean, "named boolean edit commits through service and advances revision");
    auto reset = call("settings.reset", Object({{"ifRevision", String("7:12")}}));
    Check(service.clears == 0 && service.state.fields[1].opaque.configured, "default reset preserves secret configuration");
    auto secret = call("settings.action", Object({{"ifRevision", String(reset.Find("revision")->string)},
        {"action", String("setSecret")}, {"arguments", Object({{"key", String("apiKey")}, {"value", String("new-private-value")}})}}));
    Check(Serialize(secret).find("new-private-value") == std::string::npos && service.secretReads == 0, "secret write has no plaintext readback");
    const auto cleared = call("settings.action", Object({{"ifRevision", String(secret.Find("revision")->string)},
        {"action", String("clear")}, {"arguments", Object({{"key", String("apiKey")}})}}));
    Check(service.clears == 1 && !service.state.fields[1].opaque.configured, "opaque clearing requires an explicit separate action");
    Reject([&] { call("settings.action", Object({{"ifRevision", String(cleared.Find("revision")->string)}, {"action", String("ReadSecret")}})); },
        "unknown_settings_action", "private service method names are not exposed as actions");
    std::cout << "Widget control settings: " << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
