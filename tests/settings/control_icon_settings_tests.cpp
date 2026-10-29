#include "settings/control_icon_settings.h"
#include "settings/control_settings_values.h"
#include <iostream>

using namespace snowdesktop;
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
namespace
{
int failures = 0;
void Check(bool condition, const char* message) { if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
template<class F> void Reject(F call, std::string_view code, const char* message)
{ try { call(); Check(false, message); } catch (const Error& error) { Check(error.code == code, message); } }
Value Params(const Target& target, const Value& response)
{ return Object({{"target", TargetJson(target)}, {"ifRevision", *response.Find("revision")}}); }
}
int main()
{
    IconSettingsState state;
    bool busy = false, restored = false;
    int commits = 0, reads = 0, imports = 0;
    LargeIconSettingsSnapshot large;
    large.key = L"desktop-item"; large.available = large.editable = large.succeeded = true;
    large.session = 5; large.revision = 8;
    LargeIconConfig config;
    large.config = large.defaultConfig = EncodeLargeIconConfig(config);
    IconAppearanceSnapshot appearance;
    appearance.available = appearance.succeeded = true; appearance.session = 2; appearance.revision = 3;
    appearance.current.target = L"widget:instance";
    IconLibrarySnapshot library;
    library.succeeded = true; library.revision = 9;
    IconSettingsContext context{
        [&](IconAppearanceRequest request) {
            if (request.action == "begin") ++appearance.session;
            else if (request.action == "apply") { appearance.current.settings = request.settings; ++appearance.revision; }
            else if (request.action == "inherit") { appearance.independent = false; ++appearance.revision; }
            return appearance;
        },
        [&](IconLibraryRequest request) {
            if (request.action == "begin") { library.target = request.target; ++library.session; }
            if (request.action == "import") { ++imports; ++library.revision; }
            return library;
        },
        [&](LargeIconSettingsRequest request) {
            if (request.action == "read") ++reads;
            if (request.action == "commit") { ++commits; large.config = request.config; ++large.revision; }
            return large;
        }, state, [&] { return busy; }, [&](std::wstring_view id) { restored = id == L"desktop-item"; return restored; }};
    const Target largeTarget{"large-icon", "desktop-item"};
    auto read = HandleIconSettings("settings.get", Object({{"target", TargetJson(largeTarget)}}), largeTarget, context);
    Check(reads == 1, "first query acquires the native large-icon edit session once");
    auto again = HandleIconSettings("settings.get", Object({{"target", TargetJson(largeTarget)}}), largeTarget, context);
    Check(reads == 1 && read.Find("revision")->string == again.Find("revision")->string, "read polling preserves native session/revision");
    auto change = Params(largeTarget, read); change.object.emplace("changes", Object({{"opacity", Number(.4)}}));
    auto changed = HandleIconSettings("settings.update", change, largeTarget, context);
    Check(commits == 1 && changed.Find("values")->Find("opacity")->number == .4, "typed named patch uses existing native commit");
    Reject([&] { HandleIconSettings("settings.update", change, largeTarget, context); }, "stale_settings", "stale large-icon write is rejected");
    auto invalid = Params(largeTarget, changed); invalid.object.emplace("changes", Object({{"opacity", Number(2)}}));
    Reject([&] { HandleIconSettings("settings.update", invalid, largeTarget, context); }, "invalid_params", "native large-icon ranges are enforced before commit");
    invalid.object["changes"] = Object({{"image", String("external.png")}});
    Reject([&] { HandleIconSettings("settings.update", invalid, largeTarget, context); }, "read_only_setting", "owned image sources cannot be replaced by raw field writes");
    Check(commits == 1, "rejected patches did not reach commit");
    busy = true;
    auto busyParams = Params(largeTarget, changed); busyParams.object.emplace("changes", Object({{"opacity", Number(.5)}}));
    Reject([&] { HandleIconSettings("settings.update", busyParams, largeTarget, context); }, "settings_busy", "visible editor prevents replacing its pending state");
    // Reverse operations remain available even when normal editing is locked.
    large.editable = large.succeeded = false; large.error = "largeIcon.locked";
    auto locked = HandleIconSettings("settings.get", Object({{"target", TargetJson(largeTarget)}}), largeTarget, context);
    auto restore = Params(largeTarget, locked); restore.object.emplace("action", String("restoreNormal"));
    auto ordinary = HandleIconSettings("settings.action", restore, largeTarget, context);
    Check(restored && ordinary.Find("values")->Find("ordinary")->boolean, "restore ordinary icon works without unlock");
    busy = false;
    const Target appearanceTarget{"icon-appearance", "widget:instance"};
    auto style = HandleIconSettings("settings.get", Object({{"target", TargetJson(appearanceTarget)}}), appearanceTarget, context);
    auto missing = Params(appearanceTarget, style);
    Reject([&] { HandleIconSettings("settings.update", missing, appearanceTarget, context); }, "invalid_params", "missing appearance changes is rejected safely");
    auto unknown = Params(appearanceTarget, style); unknown.object.emplace("changes", Object({{"unknown", Number(2)}}));
    Reject([&] { HandleIconSettings("settings.update", unknown, appearanceTarget, context); }, "invalid_params", "unknown appearance fields are not private RPC routing");
    const Target libraryTarget{"icon-library", "control:start"};
    auto libraryRead = HandleIconSettings("settings.get", Object({{"target", TargetJson(libraryTarget)}}), libraryTarget, context);
    auto publish = Params(libraryTarget, libraryRead); publish.object.emplace("action", String("publishPrepared"));
    Reject([&] { HandleIconSettings("settings.action", publish, libraryTarget, context); }, "unknown_settings_action", "local settings API does not expose a network publication action");
    auto begin = Params(libraryTarget, libraryRead); begin.object.emplace("action", String("begin"));
    auto begun = HandleIconSettings("settings.action", begin, libraryTarget, context);
    Check(library.session == 1 && library.target == L"control:start", "library target uses an explicit owned edit session");
    auto remove = Params(libraryTarget, begun); remove.object.emplace("action", String("delete"));
    Reject([&] { HandleIconSettings("settings.action", remove, libraryTarget, context); }, "confirmation_required", "asset deletion requires confirmation bound to the revision");
    Check(imports == 0, "read-only and rejected library requests cause no import");
    std::cout << "Icon control settings: " << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
