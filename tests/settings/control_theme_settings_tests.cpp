#include "settings/control_theme_settings.h"
#include "settings/control_settings_values.h"
#include "theme/theme_library_settings.h"
#include <fstream>
#include <iostream>

// No category state participates in theme material transactions.
CategorySettings CategorySettings::Defaults() { return {}; }
std::wstring GetDataFilePath(const wchar_t*)
{ throw std::runtime_error("Theme control fixture must use its explicit isolated library path"); }

namespace
{
using namespace snowdesktop;
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
int failures = 0;
void Check(bool value, const char* message)
{ if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
template<class Function> void Reject(std::string_view expected, Function action)
{
    try { action(); Check(false, "expected operation rejection"); }
    catch (const Error& error) { Check(error.code == expected, error.what()); }
}

struct Controller final : ISettingsController
{
    std::shared_ptr<SettingsSnapshot> state = std::make_shared<SettingsSnapshot>();
    int updates = 0, flushes = 0;
    bool failAfterUpdate = false;
    Controller()
    {
        state->initialized = true; state->generation = 1;
        state->values.personalization = PersonalizationSettings::DarkPreset();
    }
    SettingsActionResult Initialize() override { return {}; }
    SettingsActionResult Reload(SettingsReloadPolicy) override { return {}; }
    SettingsActionResult Open(SettingsRoute) override { return {}; }
    SettingsActionResult CloseSession() override { return {}; }
    void SetSnapshotChangedCallback(SnapshotChangedCallback) override {}
    void SetPendingWorkCallback(PendingWorkCallback) override {}
    SnapshotPtr Snapshot() const noexcept override { return std::make_shared<SettingsSnapshot>(*state); }
    std::uint64_t Generation() const noexcept override { return state->generation; }
    bool IsGenerationCurrent(std::uint64_t value) const noexcept override { return value == state->generation; }
    void UpdatePersonalization(PersonalizationSettings value, SettingsUpdateMode) override { state->values.personalization = value; ++updates; }
    void UpdateDock(DockSettings value, SettingsUpdateMode) override { state->values.dock = value; ++updates; }
    void UpdateNavigation(NavigationSettings value, SettingsUpdateMode) override { state->values.navigation = value; ++updates; }
    void UpdateGeneral(GeneralSettings value, SettingsUpdateMode) override { state->values.general = value; ++updates; }
    void UpdateCategory(CategorySettings value, SettingsUpdateMode) override { state->values.category = value; ++updates; }
    void UpdateDesktop(DesktopDisplaySettings value, SettingsUpdateMode) override { state->values.desktop = value; ++updates; }
    void RequestCommit(SettingsDomain) override {}
    SettingsActionResult FlushPending() override { return {}; }
    SettingsActionResult FlushAll() override
    {
        ++flushes;
        SettingsActionResult result;
        if (failAfterUpdate && updates > 0) result.status = SettingsActionStatus::Failed;
        return result;
    }
    bool RetryPending() override { return false; }
    void PrepareForExternalDataReplacement() override {}
    SettingsActionResult InvokeHostAction(const SettingsHostActions::Request&) override { return {}; }
};

struct Fixture
{
    static inline unsigned sequence = 0;
    Controller controller;
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"SnowDesktop.ThemeControl." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++sequence));
    ThemeSettingsContext context{controller, root / L"library.json", {}, {}};
    Target target{"themes", {}};
    Fixture()
    {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Unique test directory already exists");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    Value Get(const Target& query)
    { return HandleThemeSettings("settings.get", Object({{"target", TargetJson(query)}}), query, context); }
    Value Get() { return Get(target); }
    Value Action(std::string action, Value arguments, const Target* query = nullptr)
    {
        const auto& selected = query ? *query : target;
        return HandleThemeSettings("settings.action", Object({{"target", TargetJson(selected)},
            {"ifRevision", Get(selected).object.at("revision")}, {"action", String(std::move(action))},
            {"arguments", std::move(arguments)}}), selected, context);
    }
    themes::Library Library()
    {
        themes::Library result; std::string error;
        Check(themes::Load(context.libraryPath, result, error), "load the real temporary theme library");
        return result;
    }
};

void ReadAndReject()
{
    Fixture fixture;
    const auto first = fixture.Get();
    Check(!std::filesystem::exists(fixture.context.libraryPath), "get does not create a theme library");
    Check(!first.object.at("values").object.at("entries").array.empty(), "built-in themes are discoverable");
    const auto catalog = HandleThemeSettings("settings.catalog", Object({{"target", TargetJson(fixture.target)}}), fixture.target, fixture.context);
    Check(catalog.object.at("actions").array.size() == 7 && catalog.object.at("selectionTargets").array.size() == 12,
        "catalog exposes local management and exact appearance targets");
    fixture.controller.state->generation++;
    Reject("stale_settings", [&] {
        HandleThemeSettings("settings.action", Object({{"target", TargetJson(fixture.target)},
            {"ifRevision", first.object.at("revision")}, {"action", String("detach")},
            {"arguments", Object({{"selectionTarget", String("dock")}})}}), fixture.target, fixture.context);
    });
    const int before = fixture.controller.flushes;
    Reject("invalid_params", [&] { fixture.Action("apply", Object({{"selectionTarget", String("unknown")}, {"id", String("builtin/global/light")}})); });
    Reject("invalid_params", [&] { fixture.Action("apply", Object({{"selectionTarget", String("dock")}, {"id", String("builtin/global/light")}, {"rawRpc", String("escape")}})); });
    Check(fixture.controller.flushes == before, "invalid arguments do not flush settings or mutate files");
    Reject("unsupported_setting", [&] { fixture.Action("publish", Object()); });
    fixture.controller.state->externalReplacementPending = true;
    Reject("settings_unavailable", [&] { fixture.Get(); });
}

void ApplyAndDetach()
{
    Fixture fixture;
    fixture.controller.state->values.general.dockEnabled = false;
    const auto previous = fixture.controller.state->values.personalization;
    fixture.Action("apply", Object({{"selectionTarget", String("dock")}, {"id", String("builtin/global/light")}}));
    auto library = fixture.Library();
    Check(library.references.at("dock").id == "builtin/global/light", "apply commits the real selected reference");
    Check(!fixture.controller.state->values.general.dockEnabled && fixture.controller.state->values.personalization == previous,
        "applying Dock material preserves enablement and unrelated component appearance");
    Check(!fixture.controller.state->values.dock.followComponentAppearance, "Dock apply uses its independent material model");
    const auto applied = fixture.controller.state->values.dock;
    fixture.Action("detach", Object({{"selectionTarget", String("dock")}}));
    library = fixture.Library();
    Check(library.references.at("dock").id.empty() && fixture.controller.state->values.dock == applied,
        "detach preserves actual appearance and its last snapshot");
    Fixture failing;
    failing.controller.failAfterUpdate = true;
    const auto original = failing.controller.state->values.dock;
    Reject("theme_operation_failed", [&] { failing.Action("apply", Object({{"selectionTarget", String("dock")}, {"id", String("builtin/global/light")}})); });
    Check(failing.Library().references.empty() && failing.controller.state->values.dock == original,
        "settings persistence failure leaves references untouched and restores the previous values");
}

void SaveAndTransfer()
{
    Fixture fixture;
    const auto saved = fixture.Action("save-as", Object({{"selectionTarget", String("dock")}, {"name", String("Tools")}}));
    const auto id = saved.object.at("savedId").string;
    Check(id.starts_with("theme/") && fixture.Library().themes.at(id).name == "Tools", "save-as captures a real current source under a new identity");
    const Target selected{"theme", id};
    fixture.Action("apply", Object({{"selectionTarget", String("dock")}}), &selected);
    fixture.controller.state->values.personalization = PersonalizationSettings::LightPreset();
    fixture.Action("update-saved", Object({{"selectionTarget", String("global")}, {"name", String("Tools Light")}, {"scopes", Number(themes::Dock)}}), &selected);
    const auto refreshed = fixture.Library();
    Check(refreshed.references.at("dock").id == id &&
        themes::AppliedAppearanceMatches(refreshed.themes.at(id).appearance, fixture.controller.state->values.dock.customAppearance),
        "explicit updates reapply an unchanged active reference through controller persistence");
    fixture.controller.state->values.dock.customAppearance.widgetAlpha = .73f;
    fixture.Action("update-saved", Object({{"selectionTarget", String("dock")}, {"name", String("Tools Updated")}}), &selected);
    Check(fixture.Library().themes.at(id).name == "Tools Updated" && fixture.Library().themes.at(id).appearance.widgetAlpha == .73f,
        "explicit saved update retains the identity and captures the effective material");
    const auto path = fixture.root / L"tools.snowtheme";
    const auto encodedPath = values_detail::Utf8(path.wstring());
    fixture.Action("export", Object({{"path", String(encodedPath)}}), &selected);
    themes::Package package; std::string error;
    Check(themes::ReadPackage(path, package, error) && package.contains(id), "export writes a valid production theme package");
    fixture.Action("remove", Object(), &selected);
    Check(!fixture.Library().themes.contains(id), "remove uses the existing safe library deletion");
    fixture.Action("import", Object({{"path", String(encodedPath)}}));
    Check(fixture.Library().themes.contains(id), "import validates and restores the complete local package");
    Reject("invalid_params", [&] { fixture.Action("export", Object({{"id", String(id)}, {"path", String("relative.snowtheme")}})); });
    const auto full = fixture.Action("save-as", Object({{"selectionTarget", String("global")}, {"name", String("Full Workspace")}}));
    const auto fullLibrary = fixture.Library();
    const auto& parent = fullLibrary.themes.at(full.object.at("savedId").string);
    Check(themes::FullScope(parent.scopes) && fullLibrary.themes.contains(parent.quickPanel) && fullLibrary.themes.contains(parent.popup),
        "a full global save captures and validates both real child dependencies");
}

void GuardedReferences()
{
    Fixture fixture;
    const auto saved = fixture.Action("save-as", Object({{"selectionTarget", String("global")},
        {"name", String("Component Material")}, {"scopes", Number(themes::Components)}}));
    const auto id = saved.object.at("savedId").string;
    themes::Library written; std::string error;
    Check(themes::Transact(fixture.context.libraryPath, [&](auto& library, auto& detail) {
        return themes::Select(library, "widget/test", id, themes::Kind::Global, themes::Components, detail);
    }, written, error), "seed a valid guarded widget reference through the real library transaction");
    const auto original = written.references.at("widget/test").snapshot;
    const auto oldSnapshot = themes::Resolve(original, id)->appearance;
    fixture.controller.state->values.personalization = PersonalizationSettings::LightPreset();
    const Target source{"theme", id};
    const auto updated = fixture.Action("update-saved", Object({{"selectionTarget", String("global")},
        {"name", String("Component Material Light")}, {"scopes", Number(themes::Components)}}), &source);
    Check(updated.object.at("status").string == "saved-with-pending-targets" &&
        updated.object.at("pendingTargets").array.size() == 1 &&
        updated.object.at("pendingTargets").array.front().string == "widget/test",
        "a guarded widget is explicitly pending instead of silently mutated or reported applied");
    Check(themes::Resolve(fixture.Library().references.at("widget/test").snapshot, id)->appearance == oldSnapshot,
        "a pending widget retains the complete last-successful material snapshot");
    int callbacks = 0;
    fixture.context.applyWidgetTheme = [&](std::string_view widget, const themes::Theme& previous, const themes::Theme& next) {
        ++callbacks;
        Check(widget == "test" && previous.id == id && next.id == id, "callback receives the real instance identity and exact old/new themes");
        themes::Library reentered; std::string detail;
        const bool unlocked = themes::Transact(fixture.context.libraryPath, [](auto&, auto&) { return true; }, reentered, detail);
        Check(unlocked, "guarded widget callback is outside the nonrecursive library writer lock");
        return unlocked;
    };
    const auto applied = fixture.Action("update-saved", Object({{"selectionTarget", String("global")},
        {"name", String("Component Material Applied")}, {"scopes", Number(themes::Components)}}), &source);
    Check(callbacks == 1 && applied.object.at("pendingTargets").array.empty() &&
        fixture.Library().references.at("widget/test").snapshot.at(id).name == "Component Material Applied",
        "a successful guarded widget apply records the matching saved-source version afterwards");
    const auto successfulSnapshot = fixture.Library().references.at("widget/test").snapshot.at(id).name;
    fixture.context.applyWidgetTheme = [&](std::string_view, const themes::Theme&, const themes::Theme&) {
        themes::Library changed; std::string detail;
        return themes::Transact(fixture.context.libraryPath, [&](auto& library, auto&) {
            library.themes.at(id).name = "Concurrent writer"; return true;
        }, changed, detail);
    };
    const auto raced = fixture.Action("update-saved", Object({{"selectionTarget", String("global")},
        {"name", String("Racing Update")}, {"scopes", Number(themes::Components)}}), &source);
    Check(raced.object.at("status").string == "saved-with-pending-targets" &&
        fixture.Library().references.at("widget/test").snapshot.at(id).name == successfulSnapshot,
        "a source changed during widget persistence is not recorded as a successful new reference");
    const auto before = fixture.Library();
    const auto corrupt = fixture.root / L"corrupt.snowtheme";
    { std::ofstream file(corrupt); file << "{invalid-package}"; }
    Reject("theme_operation_failed", [&] { fixture.Action("import", Object({{"path", String(values_detail::Utf8(corrupt.wstring()))}})); });
    std::string beforeError, afterError;
    Check(themes::EncodeLibrary(before, beforeError) == themes::EncodeLibrary(fixture.Library(), afterError),
        "a malformed package does not change any themes or successful references");
}
}

int main()
{
    try { ReadAndReject(); ApplyAndDetach(); SaveAndTransfer(); GuardedReferences(); }
    catch (const std::exception& error) { std::cerr << "Unexpected failure: " << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}
