#include "control_runtime_settings.h"

#include "platform/usage_guide.h"

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
Value Names(std::initializer_list<std::string_view> names)
{
    auto value = Array();
    for (const auto name : names) value.array.push_back(String(std::string(name)));
    return value;
}
std::string Utf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw Error("settings_unavailable", "Invalid runtime status text");
    std::string value(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), length, nullptr, nullptr);
    return value;
}
bool BooleanField(const Value& object, const char* key)
{
    const auto* value = object.Find(key);
    if (!value || !value->IsBoolean()) throw Error("invalid_params", std::string(key) + " must be boolean", -32602);
    return value->boolean;
}
void Confirm(const Value& arguments)
{
    const auto* value = arguments.Find("confirmed");
    if (!value || !value->IsBoolean() || !value->boolean)
        throw Error("confirmation_required", "This runtime action requires arguments.confirmed:true");
}
const Value& Arguments(const Value& params)
{
    static const auto empty = Object();
    const auto* arguments = params.Find("arguments");
    if (!arguments) return empty;
    if (!arguments->IsObject()) throw Error("invalid_params", "arguments must be a named object", -32602);
    return *arguments;
}
Value Optional(const std::optional<bool>& value) { return value ? Boolean(*value) : Value{}; }
Value WindowSupport(const window_support::SupportStatus& status)
{
    const char* authorization = "not-registered";
    switch (status.authorization)
    {
    case window_support::AuthorizationState::Registered: authorization = "registered"; break;
    case window_support::AuthorizationState::Invalid: authorization = "invalid"; break;
    case window_support::AuthorizationState::Configuring: authorization = "configuring"; break;
    case window_support::AuthorizationState::AwaitingConsent: authorization = "awaiting-consent"; break;
    default: break;
    }
    const char* provider = "absent";
    switch (status.capabilities.helperState)
    {
    case window_support::ProviderState::Starting: provider = "starting"; break;
    case window_support::ProviderState::Ready: provider = "ready"; break;
    case window_support::ProviderState::Failed: provider = "failed"; break;
    default: break;
    }
    return Object({{"authorization", String(authorization)}, {"hostElevated", Boolean(status.hostElevated)},
        {"error", Number(status.error)}, {"updateAvailable", Boolean(status.updateAvailable)},
        {"inProcessCapabilities", Number(status.capabilities.inProcess)}, {"helperCapabilities", Number(status.capabilities.helper)},
        {"helperState", String(provider)}, {"generation", String(std::to_string(status.capabilities.generation))}});
}
Value Advanced(const winui::GeneralAdvancedFeatureStatus& status)
{
    const char* state = "bridge-unavailable";
    switch (status.state)
    {
    case winui::GeneralAdvancedFeatureState::Unregistered: state = "unregistered"; break;
    case winui::GeneralAdvancedFeatureState::Checking: state = "checking"; break;
    case winui::GeneralAdvancedFeatureState::Registered: state = "registered"; break;
    case winui::GeneralAdvancedFeatureState::RegistrationFailed: state = "registration-failed"; break;
    default: break;
    }
    return Object({{"state", String(state)}, {"bridgeAvailable", Boolean(status.bridgeAvailable)},
        {"registered", Boolean(status.registered)}, {"failure", Number(static_cast<unsigned>(status.failure))},
        {"connectionProblem", Number(static_cast<unsigned>(status.connectionProblem))}, {"errorDetail", String(Utf8(status.errorDetail))},
        {"validUntil", String(std::to_string(status.validUntil))}, {"uiVisible", Boolean(status.cardVisible)},
        {"offerSteamStore", Boolean(status.offerSteamStore)}});
}
Value HostResult(const Target& target, const SettingsActionResult& result)
{
    if (!result.Succeeded()) throw Error(result.status == SettingsActionStatus::Busy ? "busy" : "settings_failed",
        result.message.empty() ? "The host rejected this runtime action" : Utf8(result.message));
    return Object({{"target", TargetJson(target)}, {"status", String("accepted")}});
}
}

bool IsRuntimeSettingsTarget(const Target& target) noexcept { return target.kind == "runtime-services"; }

Value HandleRuntimeSettings(std::string_view method, const Value& params,
    const Target& target, const RuntimeSettingsContext& context)
{
    if (target.kind != "runtime-services" || !target.id.empty()) throw Error("settings_target_not_found", "Unknown runtime settings target");
    const auto snapshot = context.controller.Snapshot();
    if (!snapshot) throw Error("settings_unavailable", "Settings controller is unavailable");
    const auto status = context.homeAboutStatus ? context.homeAboutStatus(snapshot->generation, snapshot->revision) : winui::HomeAboutStatusPatch{};
    const auto support = context.windowSupportStatus ? context.windowSupportStatus() : window_support::SupportStatus{};
    const auto advanced = context.advancedFeatureStatus ? context.advancedFeatureStatus() : winui::GeneralAdvancedFeatureStatus{};
    auto state = Object({{"animationDiagnosticsEnabled", Optional(status.animationDiagnosticsEnabled)},
        {"temporaryInitializationEnabled", Optional(status.temporaryInitializationEnabled)}, {"usageGuideExpanded", Optional(status.usageGuideExpanded)},
        {"windowSupport", context.windowSupportStatus ? WindowSupport(support) : Value{}},
        {"advancedFeatures", context.advancedFeatureStatus ? Advanced(advanced) : Value{}},
        {"generation", String(std::to_string(snapshot->generation))}});
    const auto revision = Revision(state);
    if (method == "settings.get" || method == "settings.catalog")
    {
        Fields(params, {"target"});
        auto values = state;
        values.object.emplace("animationDiagnosticsStatus", status.animationDiagnosticsStatus ? String(Utf8(*status.animationDiagnosticsStatus)) : Value{});
        auto result = Object({{"target", TargetJson(target)}, {"revision", String(revision)}, {"values", std::move(values)},
            {"debugUiVisible", Boolean(context.debugVisible && context.debugVisible())}});
        if (method == "settings.catalog")
        {
            result.object.emplace("fields", Object({
                {"animationDiagnosticsEnabled", Object({{"type", String("boolean")}, {"default", Boolean(false)}, {"available", Boolean(status.animationDiagnosticsEnabled.has_value())}})},
                {"usageGuideExpanded", Object({{"type", String("boolean")}, {"default", Boolean(true)}, {"available", Boolean(status.usageGuideExpanded.has_value())}})}}));
            result.object.emplace("maximumChanges", Number(1));
            result.object.emplace("readOnly", Names({"temporaryInitializationEnabled", "windowSupport", "advancedFeatures", "generation", "animationDiagnosticsStatus"}));
            auto actions = Array();
            if (context.configureWindowSupport && context.windowSupportStatus) actions.array.push_back(String("configure-window-support"));
            if (context.registerAdvancedFeatures && context.advancedFeatureStatus && advanced.bridgeAvailable) actions.array.push_back(String("unlock-advanced-features"));
            if (context.resetAdvancedFeatures) actions.array.push_back(String("reset-advanced-features"));
            if (status.temporaryInitializationEnabled) actions.array.push_back(String("set-temporary-initialization"));
            actions.array.push_back(String("start-usage-guide-practice"));
            actions.array.push_back(String("trigger-crash-test"));
            result.object.emplace("actions", std::move(actions));
            auto practices = Array();
            for (const auto& lesson : usage_guide::kLessons) if (lesson.practice) practices.array.push_back(String(lesson.key));
            result.object.emplace("practiceTopics", std::move(practices));
        }
        return result;
    }
    if (snapshot->externalReplacementPending) throw Error("settings_unavailable", "Data replacement is pending");
    RequireRevision(params, revision);
    using Action = SettingsHostActions::Action;
    SettingsHostActions::Request request;
    if (method == "settings.update" || method == "settings.reset")
    {
        std::string key;
        bool enabled = false;
        if (method == "settings.update")
        {
            Fields(params, {"target", "ifRevision", "changes"});
            const auto* changes = params.Find("changes");
            if (!changes || !changes->IsObject() || changes->object.size() != 1)
                throw Error("invalid_params", "Runtime flags are individual host transactions; edit one field at a time", -32602);
            Fields(*changes, {"animationDiagnosticsEnabled", "usageGuideExpanded"});
            key = changes->object.begin()->first;
            enabled = BooleanField(*changes, key.c_str());
        }
        else
        {
            Fields(params, {"target", "ifRevision", "keys"});
            const auto* keys = params.Find("keys");
            if (!keys || !keys->IsArray() || keys->array.size() != 1 || !keys->array[0].IsString())
                throw Error("invalid_params", "Reset one runtime flag at a time", -32602);
            key = keys->array[0].string;
            enabled = key == "usageGuideExpanded";
        }
        if (key == "animationDiagnosticsEnabled" && status.animationDiagnosticsEnabled)
            request.action = Action::SetAnimationDiagnostics;
        else if (key == "usageGuideExpanded" && status.usageGuideExpanded)
            request.action = Action::SetUsageGuideExpanded;
        else throw Error("unsupported_setting", "Runtime flag is unknown or unavailable");
        request.boolValue = enabled;
        return HostResult(target, context.controller.InvokeHostAction(request));
    }
    if (method != "settings.action") throw Error("method_not_found", "Unknown runtime settings method", -32601);
    Fields(params, {"target", "ifRevision", "action", "arguments"});
    const auto action = Text(params, "action");
    const auto& arguments = Arguments(params);
    if (action == "configure-window-support")
    {
        Fields(arguments, {"enabled", "confirmed"}); Confirm(arguments);
        if (!context.windowSupportStatus || !context.configureWindowSupport || !context.ownerWindow)
            throw Error("settings_unavailable", "Window support configuration is unavailable");
        if (window_support::AuthorizationPending(support.authorization)) throw Error("busy", "Window support authorization is already pending");
        const bool enabled = BooleanField(arguments, "enabled");
        const auto owner = context.ownerWindow();
        if (!owner || !IsWindow(owner)) throw Error("settings_unavailable", "The application authorization owner is unavailable");
        context.configureWindowSupport(enabled, owner);
        return Object({{"target", TargetJson(target)}, {"status", String("accepted")},
            {"completion", String("System consent and capability availability must be queried after the native authorization flow")}});
    }
    if (action == "unlock-advanced-features")
    {
        Fields(arguments, {"confirmed"}); Confirm(arguments);
        if (!context.advancedFeatureStatus || !advanced.bridgeAvailable || !context.registerAdvancedFeatures)
            throw Error("feature_unavailable", "A compatible Steam bridge is required for advanced feature verification");
        if (advanced.state == winui::GeneralAdvancedFeatureState::Checking) throw Error("busy", "Steam verification is already pending");
        context.registerAdvancedFeatures();
        return Object({{"target", TargetJson(target)}, {"status", String("accepted")},
            {"completion", String("Query advancedFeatures; acceptance does not imply ownership or unlock success")}});
    }
    if (action == "reset-advanced-features")
    {
        Fields(arguments, {"confirmed"}); Confirm(arguments);
        if (!context.resetAdvancedFeatures) throw Error("settings_unavailable", "Advanced feature reset is unavailable");
        if (!context.resetAdvancedFeatures()) throw Error("settings_failed", "Advanced feature reset failed");
        return Object({{"target", TargetJson(target)}, {"status", String("applied")}});
    }
    if (action == "set-temporary-initialization")
    {
        Fields(arguments, {"enabled", "confirmed"}); Confirm(arguments);
        if (!status.temporaryInitializationEnabled) throw Error("settings_unavailable", "Temporary initialization state is unavailable");
        request.action = Action::SetTemporaryGridInitialization;
        request.boolValue = BooleanField(arguments, "enabled");
    }
    else if (action == "start-usage-guide-practice")
    {
        Fields(arguments, {"topic", "confirmed"}); Confirm(arguments);
        const auto topic = Text(arguments, "topic");
        const auto parsed = usage_guide::ParseTopic(topic);
        const auto* lesson = parsed ? usage_guide::Find(*parsed) : nullptr;
        if (!lesson || !lesson->practice) throw Error("invalid_params", "Unknown or non-practice usage-guide topic", -32602);
        request.action = Action::StartUsageGuidePractice;
        request.value.assign(topic.begin(), topic.end());
    }
    else if (action == "trigger-crash-test")
    {
        Fields(arguments, {"confirmed"}); Confirm(arguments);
        request.action = Action::TriggerCrashTest;
    }
    else throw Error("unsupported_action", "Unknown runtime settings action");
    return HostResult(target, context.controller.InvokeHostAction(request));
}
}
