#include "settings/control_package_settings.h"
#include <iostream>

using namespace snowdesktop;
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
using namespace snowdesktop::winui;
namespace
{
int failures = 0;
void Check(bool condition, const char* message) { if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
template<class F> void Reject(F call, std::string_view code, const char* message)
{ try { call(); Check(false, message); } catch (const Error& error) { Check(error.code == code, message); } }
class Backend final : public IWidgetsPageBackend
{
public:
    std::shared_ptr<WidgetsPageSnapshot> state = std::make_shared<WidgetsPageSnapshot>();
    WidgetsPageRequest last;
    int invokes = 0;
    Backend()
    {
        state->generation = 4; state->revision = 9;
        InstalledWidgetPackageSnapshot package;
        package.packageId = L"package-id"; package.name = L"Test Component";
        package.version = L"1.2.0"; package.sourceId = L"local";
        package.permissionScopeFingerprint = L"exact-review";
        WidgetPermissionSnapshot permission; permission.id = L"system.status";
        package.permissions.push_back(permission);
        package.declaredNetworkDomains.push_back(L"example.invalid");
        package.enabled = true; package.canEnable = true;
        package.canUseDevelopmentOverride = true;
        state->installed.push_back(package); state->developerOverridesVisible = true;
    }
    void SetSnapshotChangedCallback(WidgetsPageBackendOptions::SnapshotChangedCallback) override {}
    bool Activate(std::uint64_t generation, bool) override { state->generation = generation; return true; }
    void Deactivate() noexcept override {}
    bool Refresh() override { return true; }
    bool Invoke(std::uint64_t generation, WidgetsPageRequest request) override
    {
        if (generation != state->generation) return false;
        ++invokes; last = std::move(request);
        if (last.command == WidgetsPageCommand::SetPackageEnabled) state->installed.front().enabled = last.enabled;
        if (last.command == WidgetsPageCommand::SetDevelopmentOverride) state->installed.front().developmentOverrideActive = last.enabled;
        if (last.command == WidgetsPageCommand::SetAgentSkillTargetSelection) state->agentSkillTargetMask = last.agentSkillTargetMask;
        ++state->revision; return true;
    }
    std::shared_ptr<const WidgetsPageSnapshot> Snapshot() const noexcept override { return state; }
    std::uint64_t Generation() const noexcept override { return state->generation; }
    bool IsGenerationCurrent(std::uint64_t generation) const noexcept override { return generation == state->generation; }
    void Close() noexcept override {}
};
}
int main()
{
    Backend backend;
    PackageSettingsContext context{backend, 4};
    const Target target{"package", "package-id"};
    const auto get = [&](const Target& id) { return HandlePackageSettings("settings.get", Object({{"target", TargetJson(id)}}), id, context); };
    auto read = get(target);
    const auto params = [&](const Target& id, const Value& value) { return Object({{"target", TargetJson(id)}, {"ifRevision", *value.Find("revision")}}); };
    auto invalid = params(target, read); invalid.object.emplace("changes", Object({{"enabled", String("false")}}));
    Reject([&] { HandlePackageSettings("settings.update", invalid, target, context); }, "invalid_params", "invalid boolean type is rejected before backend mutation");
    auto update = params(target, read); update.object.emplace("changes", Object({{"enabled", Boolean(false)}}));
    auto changed = HandlePackageSettings("settings.update", update, target, context);
    Check(backend.invokes == 1 && !backend.state->installed.front().enabled, "package enabled preference reaches the existing backend");
    Reject([&] { HandlePackageSettings("settings.update", update, target, context); }, "stale_settings", "package identity revision protects concurrent changes");
    auto multiple = params(target, changed); multiple.object.emplace("changes", Object({{"enabled", Boolean(true)}, {"developmentOverrideActive", Boolean(true)}}));
    Reject([&] { HandlePackageSettings("settings.update", multiple, target, context); }, "invalid_params", "non-atomic preference bundle is rejected");
    auto permission = params(target, changed); permission.object.emplace("action", String("permissions"));
    permission.object.emplace("arguments", Object({{"decision", String("grant")}, {"permissions", Array()}, {"networkDomains", Array()}, {"confirmed", Boolean(true)}}));
    permission.object.at("arguments").object.at("permissions").array.push_back(String("not-declared"));
    Reject([&] { HandlePackageSettings("settings.action", permission, target, context); }, "invalid_params", "undeclared permission cannot be granted");
    permission.object.at("arguments").object.at("permissions").array[0] = String("system.status");
    auto granted = HandlePackageSettings("settings.action", permission, target, context);
    Check(backend.last.version == L"1.2.0" && backend.last.sourceId == L"local" && backend.last.scopeFingerprint == L"exact-review",
        "permission decision binds current package version, source and scope");
    Check(backend.last.grantedPermissions == std::vector<std::wstring>{L"system.status"}, "declared grant reaches native permission flow");
    auto denied = params(target, granted); denied.object.emplace("action", String("permissions")); denied.object.emplace("arguments", Object({{"decision", String("deny")}}));
    auto revoked = HandlePackageSettings("settings.action", denied, target, context);
    Check(backend.last.permissionState == WidgetPackagePermissionState::Denied && backend.last.grantedPermissions.empty(), "revocation has no implicit grants");
    auto publish = params(target, revoked); publish.object.emplace("action", String("PublishDevelopmentPackage"));
    Reject([&] { HandlePackageSettings("settings.action", publish, target, context); }, "unknown_settings_action", "private package management enum is not publicly invocable");
    const Target skills{"agent-skills", {}};
    auto skillState = get(skills);
    auto selection = params(skills, skillState); selection.object.emplace("changes", Object({{"selectedMask", Number(3)}}));
    HandlePackageSettings("settings.update", selection, skills, context);
    Check(backend.last.command == WidgetsPageCommand::SetAgentSkillTargetSelection && backend.state->agentSkillTargetMask == 3, "Agent Skill target preference is separate from file installation");
    auto currentSkills = get(skills);
    auto install = params(skills, currentSkills); install.object.emplace("action", String("apply-selection")); install.object.emplace("arguments", Object({{"confirmed", Boolean(false)}}));
    Reject([&] { HandlePackageSettings("settings.action", install, skills, context); }, "confirmation_required", "writing user skill files requires explicit action confirmation");
    const Target missing{"package", "gone"};
    Reject([&] { get(missing); }, "settings_target_not_found", "missing dynamic package identity fails explicitly");
    std::cout << "Package control settings: " << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
