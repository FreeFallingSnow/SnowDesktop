#include "settings/control_global_settings.h"
#include "settings/control_settings_values.h"
#include <iostream>
#include <stdexcept>

namespace
{
using namespace snowdesktop;
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Store final : SettingsStore
{
    SettingsValues values;
    unsigned writes = 0;
    bool failDock = false;
    Store() { values.category = CategorySettings::Defaults(); }
    SettingsActionResult Load(SettingsValues& output) override { output = values; return SettingsActionResult::Success(); }
    SettingsActionResult SavePersonalization(const PersonalizationSettings& value) override
    { ++writes; values.personalization = value; return SettingsActionResult::Success(SettingsDomain::Personalization); }
    SettingsActionResult SaveDock(const DockSettings& value) override
    { ++writes; if (failDock) return SettingsActionResult::Failure(L"Test persistence failure", SettingsDomain::Dock); values.dock = value; return SettingsActionResult::Success(SettingsDomain::Dock); }
    SettingsActionResult SaveNavigation(const NavigationSettings& value) override
    { ++writes; values.navigation = value; return SettingsActionResult::Success(SettingsDomain::Navigation); }
    SettingsActionResult SaveGeneral(const GeneralSettings& value) override
    { ++writes; values.general = value; return SettingsActionResult::Success(SettingsDomain::General); }
    SettingsActionResult SaveCategory(const CategorySettings& value) override
    { ++writes; values.category = value; return SettingsActionResult::Success(SettingsDomain::Category); }
};
struct Actions final : SettingsHostActions
{
    unsigned previews = 0, commits = 0, invocations = 0;
    Request last;
    SettingsActionResult OnSettingsPreview(const SettingsSnapshot&, SettingsDomain domains) override
    { ++previews; return SettingsActionResult::Success(domains); }
    SettingsActionResult OnSettingsCommitted(const SettingsSnapshot&, SettingsDomain domains) override
    { ++commits; return SettingsActionResult::Success(domains); }
    SettingsActionResult OnSettingsRouteChanged(const SettingsRoute&) override { return SettingsActionResult::Success(); }
    SettingsActionResult Invoke(const Request& request) override { ++invocations; last = request; return SettingsActionResult::Success(); }
};
struct Fixture
{
    std::shared_ptr<Store> store = std::make_shared<Store>();
    Actions actions;
    SettingsController controller{store, &actions};
    GlobalSettingsContext context{controller, [](std::string_view language) { return language == "system" || language == "en-US"; },
        [](const app_fonts::Selection& selection) { return selection.package == "system"; }};
    Fixture() { Check(controller.Initialize().Succeeded(), "initialize the real controller"); }
    Value Get(const Target& target)
    { return HandleGlobalSettings("settings.get", Object({{"target", TargetJson(target)}}), target, context); }
    Value Update(const Target& target, Value changes)
    {
        const auto revision = Get(target).object.at("revision");
        return HandleGlobalSettings("settings.update", Object({{"target", TargetJson(target)}, {"ifRevision", revision}, {"changes", std::move(changes)}}), target, context);
    }
};
template<class F> void Reject(F function, std::string_view expected)
{
    bool rejected = false;
    try { function(); } catch (const Error& error) { rejected = error.code == expected; }
    Check(rejected, "settings request must fail with its expected stable error code");
}
}

void RunControlGlobalSettingsTests()
{
    Fixture fixture;
    const Target appearance{"personalization", {}}, global{"global", {}}, general{"general", {}}, desktop{"desktop", {}}, dock{"dock", {}};
    const auto catalog = HandleGlobalSettings("settings.catalog", Object({{"target", TargetJson(global)}}), global, fixture.context);
    const auto& domains = catalog.object.at("fields").object.at("properties").object;
    Check(domains.size() == 6, "global catalog includes every native stored and desktop domain");
    Check(domains.at("personalization").Find("properties")->Find("scrollableTitleBarOnTop") != nullptr,
        "the previously inaccessible native title-bar setting is discoverable");
    Check(domains.at("general").Find("properties")->Find("autoStartEnabled")->Find("readOnly")->boolean,
        "Windows-owned auto-start is a projection with a separate host action");
    const auto initial = fixture.Get(appearance);
    const auto changed = fixture.Update(appearance, Object({{"scrollableTitleBarOnTop", Boolean(true)}}));
    Check(changed.Find("status")->string == "applied" && fixture.store->values.personalization.scrollableTitleBarOnTop &&
        fixture.actions.previews == 1 && fixture.actions.commits == 1 && fixture.store->writes == 1,
        "direct title-bar update uses actual controller preview, commit and persistence");
    Reject([&] { HandleGlobalSettings("settings.update", Object({{"target", TargetJson(appearance)},
        {"ifRevision", initial.object.at("revision")}, {"changes", Object({{"scrollableTitleBarOnTop", Boolean(false)}})}}), appearance, fixture.context); }, "stale_settings");
    const auto writes = fixture.store->writes;
    Reject([&] { fixture.Update(appearance, Object({{"widgetAlpha", Number(2)}})); }, "invalid_params");
    Reject([&] { fixture.Update(appearance, Object({{"scrollableTitleBarOnTop", Boolean(false)}, {"unknown", Boolean(true)}})); }, "invalid_params");
    Reject([&] { fixture.Update(appearance, Object({{"widgetAlpha", String("invalid")}})); }, "invalid_params");
    Check(fixture.store->writes == writes && fixture.controller.Snapshot()->values.personalization.scrollableTitleBarOnTop,
        "invalid later fields cannot partially mutate or persist the earlier field");
    Reject([&] { fixture.Update(desktop, Object({{"iconBeautify", Object({{"shape", Number(4)}})}})); }, "invalid_params");
    Reject([&] { fixture.Update(desktop, Object({{"itemIconSizeScale", Number(8)}})); }, "invalid_params");
    Reject([&] { fixture.Update(general, Object({{"animationFrameLimit", Number(45)}})); }, "invalid_params");
    Reject([&] { fixture.Update(general, Object({{"language", String("missing-locale")}})); }, "invalid_params");
    Reject([&] { fixture.Update(general, Object({{"font", Object({{"package", String("missing-font")}})}})); }, "invalid_params");
    Reject([&] { fixture.Update(general, Object({{"autoStartEnabled", Boolean(true)}})); }, "read_only");
    Reject([&] { fixture.Update(desktop, Object({{"dockEnabled", Boolean(true)}})); }, "read_only");
    fixture.Update(general, Object({{"language", String("en-US")}, {"font", Object({{"family", String("")}})}}));
    Check(std::string(fixture.store->values.general.language) == "en-US", "bounded char-array language is named UTF-8 text");
    const auto beforeReset = fixture.Get(appearance);
    auto keys = Array(); keys.array.push_back(String("scrollableTitleBarOnTop"));
    HandleGlobalSettings("settings.reset", Object({{"target", TargetJson(appearance)}, {"ifRevision", beforeReset.object.at("revision")},
        {"keys", std::move(keys)}}), appearance, fixture.context);
    Check(!fixture.store->values.personalization.scrollableTitleBarOnTop, "field reset uses actual model default through the controller");
    const auto state = fixture.Get(general);
    HandleGlobalSettings("settings.action", Object({{"target", TargetJson(general)}, {"ifRevision", state.object.at("revision")},
        {"action", String("set-auto-start")}, {"arguments", Object({{"enabled", Boolean(true)}})}}), general, fixture.context);
    Check(fixture.actions.invocations == 1 && fixture.actions.last.action == SettingsHostActions::Action::SetAutoStartEnabled && fixture.actions.last.boolValue,
        "auto-start mutation follows the application-owned system action");
    auto draft = fixture.controller.Snapshot()->values.general;
    draft.doubleClickHideDesktop = true;
    fixture.controller.UpdateGeneral(draft, SettingsUpdateMode::Draft);
    Reject([&] { fixture.Update(appearance, Object({{"widgetAlpha", Number(.5)}})); }, "busy");
    Check(fixture.controller.FlushAll().Succeeded(), "finish the isolated existing draft");
    fixture.store->failDock = true;
    const auto failed = fixture.Update(dock, Object({{"windowEffect", Number(2)}}));
    Check(failed.Find("status")->string == "failed" && failed.Find("persistence")->string == "incomplete" &&
        failed.Find("failedDomains")->number != 0, "store failure is reported without claiming durable success");
    std::cout << "Direct global settings preserve typed validation, revision guards, host actions and actual controller persistence\n";
}
