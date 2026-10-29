#include "settings/control_environment_settings.h"
#include <iostream>
#include <memory>

// This adapter test does not load persisted user settings or create WinUI.
PersonalizationSettings PersonalizationSettings::DarkPreset() { return {}; }
PersonalizationSettings PersonalizationSettings::AcrylicDarkPreset() { return {}; }
CategorySettings CategorySettings::Defaults() { return {}; }

namespace
{
using namespace snowdesktop;
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
int failures = 0;
void Check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
template<class Function> void Reject(std::string_view code, Function function)
{
    try { function(); Check(false, "expected rejection"); }
    catch (const Error& error) { Check(error.code == code, "stable error code"); }
}

struct Controller final : ISettingsController
{
    std::shared_ptr<SettingsSnapshot> state = std::make_shared<SettingsSnapshot>();
    std::vector<SettingsHostActions::Request> requests;
    Controller() { state->generation = 3; state->revision = 7; state->initialized = true; }
    SettingsActionResult Initialize() override { return {}; }
    SettingsActionResult Reload(SettingsReloadPolicy) override { return {}; }
    SettingsActionResult Open(SettingsRoute) override { return {}; }
    SettingsActionResult CloseSession() override { return {}; }
    void SetSnapshotChangedCallback(SnapshotChangedCallback) override {}
    void SetPendingWorkCallback(PendingWorkCallback) override {}
    SnapshotPtr Snapshot() const noexcept override { return state; }
    std::uint64_t Generation() const noexcept override { return state->generation; }
    bool IsGenerationCurrent(std::uint64_t generation) const noexcept override { return generation == state->generation; }
    void UpdatePersonalization(PersonalizationSettings, SettingsUpdateMode) override {}
    void UpdateDock(DockSettings, SettingsUpdateMode) override {}
    void UpdateNavigation(NavigationSettings, SettingsUpdateMode) override {}
    void UpdateGeneral(GeneralSettings, SettingsUpdateMode) override {}
    void UpdateCategory(CategorySettings, SettingsUpdateMode) override {}
    void UpdateDesktop(DesktopDisplaySettings, SettingsUpdateMode) override {}
    void RequestCommit(SettingsDomain) override {}
    SettingsActionResult FlushPending() override { return {}; }
    SettingsActionResult FlushAll() override { return {}; }
    bool RetryPending() override { return false; }
    void PrepareForExternalDataReplacement() override {}
    SettingsActionResult InvokeHostAction(const SettingsHostActions::Request& request) override
    { requests.push_back(request); return {}; }
};

struct Fixture
{
    Controller controller;
    PageLayoutSnapshot pages;
    winui::HomeAboutStatusPatch debug;
    bool debugVisible = true;
    int pageWrites = 0;
    EnvironmentSettingsContext context{controller, {}, {}, {}};
    Fixture()
    {
        pages.revision = 8; pages.editable = true; pages.monitorCount = 1;
        pages.pages = {{L"one", L"Work", 5, 4}, {L"two", L"Play", 8, 6}};
        context.pages.capture = [&] { return pages; };
        context.pages.renamePage = [&](std::uint64_t revision, const std::wstring& id, const std::wstring& name) {
            Check(revision == pages.revision, "native page revision forwarded");
            for (auto& page : pages.pages) if (page.id == id) page.name = name;
            ++pages.revision; ++pageWrites;
            return PageLayoutOperationResult{PageLayoutOperationStatus::Succeeded, {}, pages};
        };
        context.pages.analyzeGrid = [](const std::wstring& id, int columns, int rows) {
            return PageGridChangeImpact{true, id, 5, 4, columns, rows, 1, 0, 0};
        };
        context.pages.applyGrid = [&](std::uint64_t revision, const std::wstring& id, int columns, int rows) {
            Check(revision == pages.revision, "grid keeps original revision");
            for (auto& page : pages.pages) if (page.id == id) { page.columns = columns; page.rows = rows; }
            ++pages.revision; ++pageWrites;
            return PageLayoutOperationResult{PageLayoutOperationStatus::Succeeded, {}, pages};
        };
        context.pages.applyOrder = [&](std::uint64_t revision, const std::vector<std::wstring>& ids) {
            Check(revision == pages.revision, "order native revision forwarded");
            std::vector<PageLayoutEntry> ordered;
            for (const auto& id : ids) for (const auto& page : pages.pages) if (page.id == id) ordered.push_back(page);
            pages.pages = std::move(ordered); ++pages.revision; ++pageWrites;
            return PageLayoutOperationResult{PageLayoutOperationStatus::Succeeded, {}, pages};
        };
        context.pages.addPage = [&](std::uint64_t revision) {
            Check(revision == pages.revision, "create native revision forwarded");
            pages.pages.push_back({L"three", L"", 4, 4});
            ++pages.revision; ++pageWrites;
            return PageLayoutOperationResult{PageLayoutOperationStatus::Succeeded, {}, pages};
        };
        context.pages.analyzeRemoval = [](const std::wstring& id) {
            return PageRemovalImpact{id != L"one", 2, 1, 0};
        };
        context.pages.removePage = [&](std::uint64_t revision, const std::wstring& id) {
            Check(revision == pages.revision, "remove native revision forwarded");
            std::erase_if(pages.pages, [&](const auto& page) { return page.id == id; });
            ++pages.revision; ++pageWrites;
            return PageLayoutOperationResult{PageLayoutOperationStatus::Succeeded, {}, pages};
        };
        debug.debugEnvironments = std::vector<winui::DebugEnvironmentSummary>{{L"default", L"Default"}, {L"env-shots", L"Screenshots"}};
        debug.debugEnvironmentId = L"env-shots"; debug.debugProfileEnabled = true;
        debug.debugDataDirectory = L"D:\\Sandbox\\data"; debug.debugDesktopDirectory = L"D:\\Sandbox\\Desktop";
        context.homeAboutStatus = [&](std::uint64_t, std::uint64_t) { ++debug.revision; return debug; };
        context.debugVisible = [&] { return debugVisible; };
    }
    Value Get(const Target& target)
    { return HandleEnvironmentSettings("settings.get", Object({{"target", TargetJson(target)}}), target, context); }
    Value Mutate(std::string_view method, const Target& target, Value values, std::string action = {}, bool confirmed = false)
    {
        if (confirmed) values.object.emplace("confirmed", Boolean(true));
        auto params = Object({{"target", TargetJson(target)}, {"ifRevision", Get(target).object.at("revision")},
            {method == "settings.action" ? "arguments" : "changes", std::move(values)}});
        if (!action.empty()) params.object.emplace("action", String(std::move(action)));
        return HandleEnvironmentSettings(method, params, target, context);
    }
};

void Pages()
{
    Fixture fixture;
    const Target page{"page", "one"}, all{"pages", {}};
    const auto original = fixture.Get(page);
    const auto renamed = fixture.Mutate("settings.update", page, Object({{"name", String("  Projects  ")}}));
    Check(renamed.object.at("values").object.at("name").string == "Projects", "rename uses native normalization");
    Check(fixture.pageWrites == 1, "one real page callback invoked");
    auto stale = Object({{"target", TargetJson(page)}, {"ifRevision", original.object.at("revision")}, {"changes", Object({{"name", String("stale")}})}});
    Reject("stale_settings", [&] { HandleEnvironmentSettings("settings.update", stale, page, fixture.context); });
    Reject("invalid_params", [&] { fixture.Mutate("settings.update", page, Object({{"columns", Number(0)}})); });
    Reject("invalid_params", [&] { fixture.Mutate("settings.update", page, Object({{"name", String("x")}, {"rows", Number(3)}})); });
    Reject("confirmation_required", [&] { fixture.Mutate("settings.update", page, Object({{"columns", Number(3)}, {"rows", Number(2)}})); });
    fixture.Mutate("settings.action", page, Object({{"columns", Number(3)}, {"rows", Number(2)}}), "set-grid", true);
    Check(fixture.pages.pages[0].columns == 3 && fixture.pages.pages[0].rows == 2, "confirmed grid applied through callback");
    auto order = Array(); order.array = {String("two"), String("two")};
    Reject("invalid_params", [&] { fixture.Mutate("settings.action", all, Object({{"order", order}}), "reorder"); });
    order.array = {String("two"), String("one")};
    fixture.Mutate("settings.action", all, Object({{"order", order}}), "reorder");
    Check(fixture.pages.pages[0].id == L"two", "stable ids reorder host pages");
    Reject("settings_target_not_found", [&] { fixture.Get({"page", "missing"}); });
    Reject("invalid_params", [&] { fixture.Mutate("settings.update", page, Object({{"visible", Boolean(false)}})); });
    auto reset = Object({{"target", TargetJson(page)}, {"ifRevision", fixture.Get(page).object.at("revision")}, {"keys", Array()}});
    reset.object.at("keys").array.push_back(String("name"));
    HandleEnvironmentSettings("settings.reset", reset, page, fixture.context);
    Check(fixture.pages.pages[1].name.empty(), "name reset restores the native default label");
    fixture.Mutate("settings.action", all, Object(), "add");
    Check(fixture.pages.pages.size() == 3, "page create invokes model callback");
    const Target added{"page", "three"};
    const auto impact = fixture.Mutate("settings.action", added, Object(), "analyze-removal");
    Check(impact.object.at("confirmationRequired").boolean && impact.object.at("widgetCount").number == 1,
        "removal analysis preserves actual impact");
    Reject("confirmation_required", [&] { fixture.Mutate("settings.action", added, Object(), "remove"); });
    fixture.Mutate("settings.action", added, Object(), "remove", true);
    Check(fixture.pages.pages.size() == 2, "confirmed removal invokes existing model callback");
    Reject("invalid_params", [&] { fixture.Mutate("settings.action", page, Object(), "remove", true); });
}

void Debug()
{
    Fixture fixture;
    const Target current{"debug-environment", "env-shots"}, other{"debug-environment", "default"}, all{"debug-environments", {}};
    Check(fixture.Get(current).object.at("revision").string == fixture.Get(current).object.at("revision").string,
        "read-only status sequence does not invalidate revision");
    fixture.Mutate("settings.update", current, Object({{"name", String("Studio")}}));
    Check(fixture.controller.requests.back().action == SettingsHostActions::Action::RenameDebugEnvironment &&
        fixture.controller.requests.back().value == L"Studio", "rename routed through controller host action");
    Reject("unsupported_setting", [&] { fixture.Mutate("settings.update", other, Object({{"name", String("wrong")}})); });
    Reject("invalid_params", [&] { fixture.Mutate("settings.update", current, Object({{"enabled", Boolean(false)}})); });
    Reject("confirmation_required", [&] { fixture.Mutate("settings.action", current, Object({{"enabled", Boolean(false)}}), "set-enabled"); });
    fixture.Mutate("settings.action", current, Object({{"enabled", Boolean(false)}}), "set-enabled", true);
    Check(fixture.controller.requests.back().action == SettingsHostActions::Action::SetDebugProfileEnabled &&
        !fixture.controller.requests.back().boolValue, "enable action preserves boolean payload");
    fixture.Mutate("settings.action", current, Object(), "clear", true);
    Check(fixture.controller.requests.back().action == SettingsHostActions::Action::ClearDebugProfile, "clear is explicit and confirmed");
    fixture.Mutate("settings.action", all, Object({{"id", String("default")}}), "select", true);
    Check(fixture.controller.requests.back().action == SettingsHostActions::Action::SelectDebugEnvironment &&
        fixture.controller.requests.back().value == L"default", "select forwards opaque managed id");
    fixture.Mutate("settings.action", all, Object({{"name", String("  Demo Stage  ")}}), "create", true);
    Check(fixture.controller.requests.back().action == SettingsHostActions::Action::CreateDebugEnvironment &&
        fixture.controller.requests.back().value == L"Demo Stage", "create forwards validated name through host");
    fixture.Mutate("settings.action", current, Object({{"path", String("D:\\Demo Files")}}), "set-desktop", true);
    Check(fixture.controller.requests.back().action == SettingsHostActions::Action::SetDebugDesktopDirectory &&
        fixture.controller.requests.back().value == L"D:\\Demo Files", "desktop change uses host path validation entry");
    Reject("settings_target_not_found", [&] { fixture.Mutate("settings.action", all, Object({{"id", String("../escape")}}), "select", true); });
    Reject("invalid_params", [&] { fixture.Mutate("settings.action", all, Object({{"name", String("")}}), "create", true); });
    const auto before = fixture.Get(current);
    ++fixture.controller.state->generation;
    auto stale = Object({{"target", TargetJson(current)}, {"ifRevision", before.object.at("revision")}, {"changes", Object({{"name", String("old session")}})}});
    Reject("stale_settings", [&] { HandleEnvironmentSettings("settings.update", stale, current, fixture.context); });
    fixture.debugVisible = false;
    Check(!fixture.Get(all).object.at("uiVisible").boolean, "hidden debug UI remains available to authorized IPC");
    Check(fixture.controller.requests.size() == 6, "rejected actions caused no hidden host mutations");
}
}

int main()
{
    Pages(); Debug();
    if (failures) return 1;
    std::cout << "Environment settings adapter checks passed\n";
    return 0;
}
