#include "settings/control_font_settings.h"
#include <iostream>
#include <memory>

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
{ if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }
template<class Function> void Reject(std::string_view code, Function function)
{
    try { function(); Check(false, "expected rejection"); }
    catch (const Error& error) { Check(error.code == code, "stable error code"); }
}
struct Controller final : ISettingsController
{
    std::shared_ptr<SettingsSnapshot> state = std::make_shared<SettingsSnapshot>();
    int writes = 0, commits = 0, restarts = 0;
    Controller() { state->generation = 3; state->revision = 4; }
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
    void UpdateGeneral(GeneralSettings general, SettingsUpdateMode mode) override
    {
        Check(mode == SettingsUpdateMode::PreviewAndCommit, "font selection follows normal preview and commit");
        state->values.general = std::move(general); ++state->domainRevisions.general; ++writes;
    }
    void UpdateCategory(CategorySettings, SettingsUpdateMode) override {}
    void UpdateDesktop(DesktopDisplaySettings, SettingsUpdateMode) override {}
    void RequestCommit(SettingsDomain) override {}
    SettingsActionResult FlushPending() override { ++commits; return {}; }
    SettingsActionResult FlushAll() override { return {}; }
    bool RetryPending() override { return false; }
    void PrepareForExternalDataReplacement() override {}
    SettingsActionResult InvokeHostAction(const SettingsHostActions::Request& request) override
    {
        Check(request.action == SettingsHostActions::Action::RestartApplication, "only application restart is dispatched");
        ++restarts; return {};
    }
};

void Run()
{
    Controller controller;
    std::vector<app_fonts::Choice> choices{{{}, L"System", L""}, {{"installed", "Demo Sans"}, L"Demo Sans", L""}};
    app_fonts::Selection active;
    std::filesystem::path importedPath;
    FontSettingsContext context{controller,
        [&] { return choices; },
        [&](const auto& path, auto& imported, auto&) {
            importedPath = path;
            choices.push_back({{"managed-font", "Imported Sans"}, L"Imported Sans", L""});
            imported.push_back(choices.back()); return true;
        }, [&] { return active; }};
    const Target target{"fonts", {}};
    const auto get = [&] { return HandleFontSettings("settings.get", Object({{"target", TargetJson(target)}}), target, context); };
    const auto update = [&](Value selection) {
        return HandleFontSettings("settings.update", Object({{"target", TargetJson(target)}, {"ifRevision", get().object.at("revision")},
            {"changes", Object({{"selection", std::move(selection)}})}}), target, context);
    };
    const auto initial = get();
    Check(initial.object.at("values").object.at("choiceCount").number == 2, "named font choices returned");
    const auto selected = update(Object({{"package", String("installed")}, {"family", String("Demo Sans")}}));
    Check(selected.object.at("restartRequired").boolean && controller.writes == 1 && controller.commits == 1,
        "requested font persists while applied font waits for restart");
    Check(active.package == "system", "adapter does not hot-swap the active font");
    Reject("invalid_params", [&] { update(Object({{"package", String("../escape")}, {"family", String("Unknown")}})); });
    Reject("invalid_params", [&] { update(Object({{"package", String("installed")}, {"family", String("Demo Sans")}, {"path", String("arbitrary")}})); });
    const auto stale = Object({{"target", TargetJson(target)}, {"ifRevision", initial.object.at("revision")},
        {"changes", Object({{"selection", Object({{"package", String("system")}, {"family", String("")}})}})}});
    Reject("stale_settings", [&] { HandleFontSettings("settings.update", stale, target, context); });
    auto keys = Array(); keys.array.push_back(String("selection"));
    HandleFontSettings("settings.reset", Object({{"target", TargetJson(target)}, {"ifRevision", get().object.at("revision")}, {"keys", keys}}), target, context);
    Check(controller.state->values.general.font.package == "system", "reset uses actual system selection default");
    update(Object({{"package", String("system")}, {"family", String("")}}));
    Check(controller.state->values.general.font.family.empty(), "explicit system default accepts an empty family");
    const auto import = [&](std::string path) {
        return HandleFontSettings("settings.action", Object({{"target", TargetJson(target)}, {"ifRevision", get().object.at("revision")},
            {"action", String("import")}, {"arguments", Object({{"path", String(std::move(path))}})}}), target, context);
    };
    Reject("invalid_params", [&] { import("relative.ttf"); });
    const auto imported = import("D:\\Demo Fonts\\Demo.ttf");
    Check(importedPath == std::filesystem::path(L"D:\\Demo Fonts\\Demo.ttf") && imported.object.at("imported").array.size() == 1,
        "font import calls shared service with exact Unicode-capable path");
    Check(controller.writes == 3, "import does not implicitly select a font");
    HandleFontSettings("settings.action", Object({{"target", TargetJson(target)}, {"ifRevision", get().object.at("revision")},
        {"action", String("restart")}, {"arguments", Object()}}), target, context);
    Check(controller.restarts == 1, "restart uses host action instead of process launch bypass");
    auto request = Object({{"target", TargetJson(target)}, {"offset", Number(1)}, {"limit", Number(1)}});
    const auto page = HandleFontSettings("settings.get", request, target, context);
    Check(page.object.at("values").object.at("choices").array.size() == 1 &&
        page.object.at("values").object.at("choiceCount").number == 3, "font catalogue pagination preserves total count");
}
}
int main()
{
    Run();
    if (failures) return 1;
    std::cout << "Font settings adapter checks passed\n";
    return 0;
}
