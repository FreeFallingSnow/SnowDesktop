#include "settings/control_runtime_settings.h"
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
    std::vector<SettingsHostActions::Request> requests;
    std::function<void(const SettingsHostActions::Request&)> observe;
    Controller() { state->generation = 4; state->revision = 2; state->initialized = true; }
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
    { requests.push_back(request); if (observe) observe(request); return {}; }
};
struct OwnerWindow
{
    HWND window = CreateWindowExW(0, L"STATIC", L"Runtime adapter test owner", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ~OwnerWindow() { if (window) DestroyWindow(window); }
};

void Run()
{
    Controller controller;
    OwnerWindow owner;
    Check(owner.window != nullptr, "owned message-only authorization handle created");
    window_support::SupportStatus support;
    winui::GeneralAdvancedFeatureStatus advanced;
    winui::HomeAboutStatusPatch home;
    home.animationDiagnosticsEnabled = false;
    home.temporaryInitializationEnabled = false;
    home.usageGuideExpanded = true;
    int configurations = 0, registrations = 0, resets = 0;
    controller.observe = [&](const auto& request) {
        if (request.action == SettingsHostActions::Action::SetAnimationDiagnostics) home.animationDiagnosticsEnabled = request.boolValue;
        if (request.action == SettingsHostActions::Action::SetUsageGuideExpanded) home.usageGuideExpanded = request.boolValue;
    };
    RuntimeSettingsContext context{controller,
        [&] { return support; },
        [&](bool enabled, HWND window) { Check(enabled && window == owner.window, "native configure receives exact app-owned HWND"); ++configurations; },
        [&] { return advanced; }, [&] { ++registrations; }, [&] { ++resets; return true; },
        [&](std::uint64_t, std::uint64_t) { ++home.revision; home.animationDiagnosticsStatus = L"changing metrics"; return home; },
        [&] { return owner.window; }, [] { return false; }};
    const Target target{"runtime-services", {}};
    const auto get = [&] { return HandleRuntimeSettings("settings.get", Object({{"target", TargetJson(target)}}), target, context); };
    const auto action = [&](std::string name, Value arguments, bool confirmed = true) {
        if (confirmed) arguments.object.emplace("confirmed", Boolean(true));
        return HandleRuntimeSettings("settings.action", Object({{"target", TargetJson(target)}, {"ifRevision", get().object.at("revision")},
            {"action", String(std::move(name))}, {"arguments", std::move(arguments)}}), target, context);
    };
    const auto original = get();
    Check(original.object.at("revision").string == get().object.at("revision").string, "live metric sequence does not invalidate edits");
    Check(!original.object.at("debugUiVisible").boolean, "UI visibility is metadata rather than authorization");
    auto update = Object({{"target", TargetJson(target)}, {"ifRevision", original.object.at("revision")},
        {"changes", Object({{"animationDiagnosticsEnabled", Boolean(true)}})}});
    HandleRuntimeSettings("settings.update", update, target, context);
    Check(home.animationDiagnosticsEnabled.value_or(false), "in-memory diagnostics flag uses host action");
    Reject("stale_settings", [&] { HandleRuntimeSettings("settings.update", update, target, context); });
    Reject("confirmation_required", [&] { action("configure-window-support", Object({{"enabled", Boolean(true)}}), false); });
    const auto configured = action("configure-window-support", Object({{"enabled", Boolean(true)}}));
    Check(configurations == 1 && configured.object.at("status").string == "accepted" &&
        support.authorization == window_support::AuthorizationState::NotRegistered, "configure receipt never pretends system consent was granted");
    support.authorization = window_support::AuthorizationState::AwaitingConsent;
    Reject("busy", [&] { action("configure-window-support", Object({{"enabled", Boolean(true)}})); });
    Reject("feature_unavailable", [&] { action("unlock-advanced-features", Object()); });
    Check(registrations == 0, "missing Steam bridge blocks positive unlock action");
    advanced.bridgeAvailable = true; advanced.state = winui::GeneralAdvancedFeatureState::Unregistered;
    action("unlock-advanced-features", Object());
    Check(registrations == 1 && !advanced.registered, "verification action does not set entitlement state");
    action("reset-advanced-features", Object());
    Check(resets == 1, "explicit reset reaches host-owned reverse action");
    action("set-temporary-initialization", Object({{"enabled", Boolean(true)}}));
    Check(controller.requests.back().action == SettingsHostActions::Action::SetTemporaryGridInitialization,
        "temporary desktop uses original host transition");
    Reject("invalid_params", [&] { action("start-usage-guide-practice", Object({{"topic", String("unknown")}})); });
    action("start-usage-guide-practice", Object({{"topic", String("move")}}));
    Check(controller.requests.back().action == SettingsHostActions::Action::StartUsageGuidePractice &&
        controller.requests.back().value == L"move", "practice validates and forwards stable topic");
    Reject("confirmation_required", [&] { action("trigger-crash-test", Object(), false); });
    action("trigger-crash-test", Object());
    Check(controller.requests.back().action == SettingsHostActions::Action::TriggerCrashTest, "crash test is only recorded by fake host, never executed");
    auto keys = Array(); keys.array.push_back(String("animationDiagnosticsEnabled"));
    HandleRuntimeSettings("settings.reset", Object({{"target", TargetJson(target)}, {"ifRevision", get().object.at("revision")}, {"keys", keys}}), target, context);
    Check(!home.animationDiagnosticsEnabled.value_or(true), "diagnostics reset restores run default");
    Check(!controller.state->sessionActive, "runtime adapter leaves Settings UI route and session untouched");
}
}
int main()
{
    Run();
    if (failures) return 1;
    std::cout << "Runtime settings adapter checks passed\n";
    return 0;
}
