#include "control_environment_settings.h"

#include "data/debug_environment_catalog.h"
#include "layout/page_management_rules.h"
#include <algorithm>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;

std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw Error("settings_unavailable", "The host returned invalid Unicode");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Wide(std::string_view value)
{
    if (value.empty()) return {};
    if (value.size() > 32768 || value.find('\0') != std::string_view::npos)
        throw Error("invalid_params", "Invalid text length or embedded NUL", -32602);
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw Error("invalid_params", "Text must be valid UTF-8", -32602);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

Value Strings(std::initializer_list<std::string_view> values)
{
    auto result = Array();
    for (const auto value : values) result.array.push_back(String(std::string(value)));
    return result;
}

bool Flag(const Value& params, std::string_view key)
{
    const auto* value = params.Find(key);
    if (!value || !value->IsBoolean()) throw Error("invalid_params", std::string(key) + " must be boolean", -32602);
    return value->boolean;
}

void Confirm(const Value& params)
{
    const auto* value = params.Find("confirmed");
    if (!value || !value->IsBoolean() || !value->boolean)
        throw Error("confirmation_required", "Review the action and send confirmed:true with the current revision");
}

const Value& Changes(const Value& params)
{
    const auto* values = params.Find("changes");
    if (!values || !values->IsObject() || values->object.empty())
        throw Error("invalid_params", "changes must be a nonempty named object", -32602);
    return *values;
}

const Value& Arguments(const Value& params)
{
    static const auto empty = Object();
    const auto* values = params.Find("arguments");
    if (!values) return empty;
    if (!values->IsObject()) throw Error("invalid_params", "Action values must be a named object", -32602);
    return *values;
}

Value Page(const PageLayoutEntry& page)
{
    const char* role = page.role == PageLayoutRole::FixedMonitor ? "fixed-monitor" :
        page.role == PageLayoutRole::LastMonitorDefault ? "last-monitor-default" : "overflow";
    return Object({{"id", String(Utf8(page.id))}, {"name", String(Utf8(page.name))},
        {"columns", Number(page.columns)}, {"rows", Number(page.rows)},
        {"itemCount", Number(static_cast<double>(page.itemCount))},
        {"widgetCount", Number(static_cast<double>(page.widgetCount))},
        {"guideCount", Number(static_cast<double>(page.guideCount))},
        {"role", String(role)}, {"monitorOrdinal", Number(static_cast<double>(page.monitorOrdinal))},
        {"visible", Boolean(page.visible)}, {"activeOnLastMonitor", Boolean(page.activeOnLastMonitor)}});
}

Value PageState(const PageLayoutSnapshot& snapshot)
{
    auto pages = Array();
    for (const auto& page : snapshot.pages) pages.array.push_back(Page(page));
    return Object({{"pages", std::move(pages)}, {"monitorCount", Number(static_cast<double>(snapshot.monitorCount))},
        {"editable", Boolean(snapshot.editable)}, {"hostRevision", String(std::to_string(snapshot.revision))}});
}

const PageLayoutEntry& FindPage(const PageLayoutSnapshot& snapshot, const Target& target)
{
    if (target.id.empty()) throw Error("invalid_params", "page target requires an id", -32602);
    const auto id = Wide(target.id);
    const auto found = std::find_if(snapshot.pages.begin(), snapshot.pages.end(),
        [&](const auto& page) { return page.id == id; });
    if (found == snapshot.pages.end()) throw Error("settings_target_not_found", "Page no longer exists");
    return *found;
}

Value Envelope(const Target& target, Value values, std::string revision)
{
    return Object({{"target", TargetJson(target)}, {"revision", String(std::move(revision))}, {"values", std::move(values)}});
}

Value PageResult(const Target& target, const PageLayoutOperationResult& result)
{
    if (!result.Succeeded())
        throw Error(result.status == PageLayoutOperationStatus::Stale ? "stale_settings" :
            result.status == PageLayoutOperationStatus::Invalid ? "invalid_params" : "settings_failed",
            result.message.empty() ? "The page operation was rejected by the host" : Utf8(result.message));
    auto state = PageState(result.snapshot);
    auto values = target.kind == "page" ? Page(FindPage(result.snapshot, target)) : state;
    auto output = Envelope(target, std::move(values), Revision(state));
    output.object.emplace("status", String("applied"));
    return output;
}

Value Pages(std::string_view method, const Value& params, const Target& target, const EnvironmentSettingsContext& context)
{
    if (!context.pages.capture) throw Error("settings_unavailable", "Page layout service is unavailable");
    if (target.kind == "pages" && !target.id.empty()) throw Error("invalid_params", "pages target has no id", -32602);
    const auto snapshot = context.pages.capture();
    const auto state = PageState(snapshot);
    const auto revision = Revision(state);
    const PageLayoutEntry* page = target.kind == "page" ? &FindPage(snapshot, target) : nullptr;
    if (method == "settings.catalog")
    {
        Fields(params, {"target"});
        auto output = Envelope(target, page ? Page(*page) : state, revision);
        output.object.emplace("fields", page ? Object({
            {"name", Object({{"type", String("string")}, {"maximumLength", Number(64)}, {"resetValue", String("")}})},
            {"columns", Object({{"type", String("integer")}, {"minimum", Number(1)}, {"maximum", Number(50)}})},
            {"rows", Object({{"type", String("integer")}, {"minimum", Number(1)}, {"maximum", Number(50)}})}}) : Object());
        output.object.emplace("actions", page ? Strings({"analyze-grid", "set-grid", "analyze-removal", "remove"}) : Strings({"add", "reorder"}));
        output.object.emplace("readOnly", page ? Strings({"id", "itemCount", "widgetCount", "guideCount", "role", "monitorOrdinal", "visible", "activeOnLastMonitor"}) : Strings({"pages", "monitorCount", "editable", "hostRevision"}));
        return output;
    }
    if (method == "settings.get")
    {
        Fields(params, {"target"});
        return Envelope(target, page ? Page(*page) : state, revision);
    }
    if (!snapshot.editable) throw Error("settings_unavailable", "Page layout cannot be edited now");
    RequireRevision(params, revision);
    if (method == "settings.update" || method == "settings.reset")
    {
        if (!page) throw Error("unsupported_setting", "Update a specific page; page ordering uses an action");
        Value resetValues;
        const Value* changes = nullptr;
        if (method == "settings.reset")
        {
            Fields(params, {"target", "ifRevision", "keys"});
            const auto* fields = params.Find("keys");
            if (!fields || !fields->IsArray() || fields->array.size() != 1 || !fields->array[0].IsString() || fields->array[0].string != "name")
                throw Error("unsupported_setting", "Only the page name has a default; request keys:[\"name\"]");
            resetValues = Object({{"name", String("")}});
            changes = &resetValues;
        }
        else
        {
            Fields(params, {"target", "ifRevision", "changes"});
            changes = &Changes(params);
        }
        Fields(*changes, {"name", "columns", "rows"});
        if (changes->Find("name"))
        {
            if (changes->object.size() != 1) throw Error("invalid_params", "Rename and grid changes are separate transactions", -32602);
            const auto* nameValue = changes->Find("name");
            if (!nameValue->IsString()) throw Error("invalid_params", "Page name must be a string", -32602);
            const auto name = page_management::NormalizeName(Wide(nameValue->string));
            if (!name) throw Error("invalid_params", "Invalid page name", -32602);
            if (!context.pages.renamePage) throw Error("settings_unavailable", "Page rename is unavailable");
            return PageResult(target, context.pages.renamePage(snapshot.revision, page->id, *name));
        }
        const int columns = changes->Find("columns") ? Integer(*changes, "columns", 1, 50) : page->columns;
        const int rows = changes->Find("rows") ? Integer(*changes, "rows", 1, 50) : page->rows;
        if (!context.pages.analyzeGrid || !context.pages.applyGrid) throw Error("settings_unavailable", "Page grid service is unavailable");
        const auto impact = context.pages.analyzeGrid(page->id, columns, rows);
        if (!impact.valid) throw Error("invalid_params", "The host rejected these grid dimensions", -32602);
        if (impact.RequiresConfirmation())
            throw Error("confirmation_required", "Review analyze-grid, then use set-grid with arguments.confirmed:true");
        return PageResult(target, context.pages.applyGrid(snapshot.revision, page->id, columns, rows));
    }
    if (method != "settings.action") throw Error("method_not_found", "Unknown environment settings method", -32601);
    Fields(params, {"target", "ifRevision", "action", "arguments"});
    const auto action = Text(params, "action");
    const auto* arguments = &Arguments(params);
    if (!page && action == "add")
    {
        Fields(*arguments, {});
        if (!context.pages.addPage) throw Error("settings_unavailable", "Page creation is unavailable");
        return PageResult(target, context.pages.addPage(snapshot.revision));
    }
    if (!page && action == "reorder")
    {
        Fields(*arguments, {"order"});
        const auto* order = arguments->Find("order");
        if (!order || !order->IsArray() || order->array.size() != snapshot.pages.size()) throw Error("invalid_params", "order must contain each page once", -32602);
        std::vector<std::wstring> requested, current;
        for (const auto& item : order->array)
        {
            if (!item.IsString()) throw Error("invalid_params", "Page ids must be strings", -32602);
            requested.push_back(Wide(item.string));
        }
        for (const auto& item : snapshot.pages) current.push_back(item.id);
        if (!IsValidPageOrder(current, requested)) throw Error("invalid_params", "order must contain each current page once", -32602);
        if (!context.pages.applyOrder) throw Error("settings_unavailable", "Page reorder is unavailable");
        return PageResult(target, context.pages.applyOrder(snapshot.revision, requested));
    }
    if (page && (action == "analyze-grid" || action == "set-grid"))
    {
        Fields(*arguments, {"columns", "rows", "confirmed"});
        const int columns = Integer(*arguments, "columns", 1, 50), rows = Integer(*arguments, "rows", 1, 50);
        if (!context.pages.analyzeGrid) throw Error("settings_unavailable", "Page grid analysis is unavailable");
        const auto impact = context.pages.analyzeGrid(page->id, columns, rows);
        if (action == "set-grid")
        {
            if (!impact.valid) throw Error("invalid_params", "The host rejected these grid dimensions", -32602);
            if (impact.RequiresConfirmation()) Confirm(*arguments);
            if (!context.pages.applyGrid) throw Error("settings_unavailable", "Page grid editing is unavailable");
            return PageResult(target, context.pages.applyGrid(snapshot.revision, page->id, columns, rows));
        }
        return Object({{"target", TargetJson(target)}, {"revision", String(revision)}, {"valid", Boolean(impact.valid)},
            {"confirmationRequired", Boolean(impact.RequiresConfirmation())},
            {"displacedItemCount", Number(static_cast<double>(impact.displacedItemCount))},
            {"displacedWidgetCount", Number(static_cast<double>(impact.displacedWidgetCount))},
            {"resizedWidgetCount", Number(static_cast<double>(impact.resizedWidgetCount))}});
    }
    if (page && (action == "analyze-removal" || action == "remove"))
    {
        Fields(*arguments, {"confirmed"});
        if (!context.pages.analyzeRemoval) throw Error("settings_unavailable", "Page removal analysis is unavailable");
        const auto impact = context.pages.analyzeRemoval(page->id);
        if (action == "analyze-removal") return Object({{"target", TargetJson(target)}, {"revision", String(revision)},
            {"valid", Boolean(impact.valid)}, {"confirmationRequired", Boolean(impact.RequiresConfirmation())},
            {"itemCount", Number(static_cast<double>(impact.itemCount))}, {"widgetCount", Number(static_cast<double>(impact.widgetCount))},
            {"addedPageCount", Number(static_cast<double>(impact.addedPageCount))}});
        if (!impact.valid) throw Error("invalid_params", "This page cannot be removed", -32602);
        if (impact.RequiresConfirmation()) Confirm(*arguments);
        if (!context.pages.removePage) throw Error("settings_unavailable", "Page removal is unavailable");
        return PageResult({"pages", {}}, context.pages.removePage(snapshot.revision, page->id));
    }
    throw Error("unsupported_action", "Unknown action for this page target");
}

Value DebugState(const winui::HomeAboutStatusPatch& status)
{
    if (!status.debugEnvironments || !status.debugEnvironmentId || !status.debugProfileEnabled)
        throw Error("settings_unavailable", "Debug environment state is unavailable");
    auto environments = Array();
    for (const auto& environment : *status.debugEnvironments)
        environments.array.push_back(Object({{"id", String(Utf8(environment.id))}, {"name", String(Utf8(environment.name))}}));
    return Object({{"environments", std::move(environments)}, {"selectedId", String(Utf8(*status.debugEnvironmentId))},
        {"enabled", Boolean(*status.debugProfileEnabled)},
        {"dataDirectory", String(Utf8(status.debugDataDirectory.value_or(L"")))},
        {"desktopDirectory", String(Utf8(status.debugDesktopDirectory.value_or(L"")))}});
}

Value Debug(std::string_view method, const Value& params, const Target& target, const EnvironmentSettingsContext& context)
{
    const auto snapshot = context.controller.Snapshot();
    if (!snapshot || !snapshot->initialized || snapshot->externalReplacementPending || !context.homeAboutStatus)
        throw Error("settings_unavailable", "An initialized settings controller is required");
    const auto status = context.homeAboutStatus(snapshot->generation, snapshot->revision);
    auto state = DebugState(status);
    state.object.emplace("generation", String(std::to_string(snapshot->generation)));
    const auto revision = Revision(state);
    const bool single = target.kind == "debug-environment";
    const auto id = Wide(target.id);
    const winui::DebugEnvironmentSummary* environment = nullptr;
    if (single)
    {
        const auto found = std::find_if(status.debugEnvironments->begin(), status.debugEnvironments->end(), [&](const auto& item) { return item.id == id; });
        if (found == status.debugEnvironments->end()) throw Error("settings_target_not_found", "Debug environment no longer exists");
        environment = &*found;
    }
    else if (!target.id.empty()) throw Error("invalid_params", "debug-environments target has no id", -32602);
    auto values = single ? Object({{"id", String(target.id)}, {"name", String(Utf8(environment->name))},
        {"selected", Boolean(id == *status.debugEnvironmentId)}}) : state;
    if (single && id == *status.debugEnvironmentId)
    {
        values.object.emplace("enabled", state.object.at("enabled"));
        values.object.emplace("desktopDirectory", state.object.at("desktopDirectory"));
        values.object.emplace("dataDirectory", state.object.at("dataDirectory"));
    }
    if (method == "settings.catalog" || method == "settings.get")
    {
        Fields(params, {"target"});
        auto output = Envelope(target, std::move(values), revision);
        output.object.emplace("uiVisible", Boolean(context.debugVisible && context.debugVisible()));
        if (method == "settings.catalog")
        {
            output.object.emplace("fields", single ? Object({{"name", Object({{"type", String("string")},
                {"maximumLength", Number(static_cast<double>(debug_profile::MaximumEnvironmentNameLength))},
                {"editable", Boolean(id == *status.debugEnvironmentId)}})}}) : Object());
            output.object.emplace("actions", single ? (id == *status.debugEnvironmentId ?
                Strings({"set-enabled", "set-desktop", "clear"}) : Array()) : Strings({"create", "select"}));
            output.object.emplace("readOnly", Strings({"id", "selected", "enabled", "desktopDirectory", "dataDirectory", "environments", "selectedId", "generation"}));
            output.object.emplace("restartMayBeRequired", Boolean(true));
        }
        return output;
    }
    RequireRevision(params, revision);
    SettingsHostActions::Request request;
    using Action = SettingsHostActions::Action;
    if (method == "settings.update")
    {
        Fields(params, {"target", "ifRevision", "changes"});
        const auto& changes = Changes(params);
        Fields(changes, {"name"});
        if (!single || id != *status.debugEnvironmentId) throw Error("unsupported_setting", "Only the selected environment can be renamed; select explicitly first");
        const auto name = debug_profile::TrimEnvironmentName(Wide(Text(changes, "name")));
        if (!debug_profile::ValidEnvironmentName(name)) throw Error("invalid_params", "Invalid debug environment name", -32602);
        request.action = Action::RenameDebugEnvironment;
        request.value = name;
    }
    else if (method == "settings.action")
    {
        Fields(params, {"target", "ifRevision", "action", "arguments"});
        const auto action = Text(params, "action");
        const auto& arguments = Arguments(params);
        if (!single && action == "create")
        {
            Fields(arguments, {"name", "confirmed"});
            const auto name = debug_profile::TrimEnvironmentName(Wide(Text(arguments, "name")));
            if (!debug_profile::ValidEnvironmentName(name)) throw Error("invalid_params", "Invalid debug environment name", -32602);
            if (status.debugEnvironments->size() >= debug_profile::MaximumEnvironments) throw Error("settings_limit", "The debug environment limit was reached");
            Confirm(arguments);
            request.action = Action::CreateDebugEnvironment;
            request.value = name;
        }
        else if (!single && action == "select")
        {
            Fields(arguments, {"id", "confirmed"});
            const auto selected = Wide(Text(arguments, "id"));
            if (std::none_of(status.debugEnvironments->begin(), status.debugEnvironments->end(), [&](const auto& item) { return item.id == selected; }))
                throw Error("settings_target_not_found", "Debug environment no longer exists");
            Confirm(arguments);
            request.action = Action::SelectDebugEnvironment;
            request.value = selected;
        }
        else if (single && id == *status.debugEnvironmentId && action == "set-enabled")
        {
            Fields(arguments, {"enabled", "confirmed"}); Confirm(arguments);
            request.action = Action::SetDebugProfileEnabled;
            request.boolValue = Flag(arguments, "enabled");
        }
        else if (single && id == *status.debugEnvironmentId && action == "set-desktop")
        {
            Fields(arguments, {"path", "confirmed"}); Confirm(arguments);
            request.action = Action::SetDebugDesktopDirectory;
            request.value = Wide(Text(arguments, "path"));
            if (request.value.empty()) throw Error("invalid_params", "A desktop directory is required", -32602);
        }
        else if (single && id == *status.debugEnvironmentId && action == "clear")
        {
            Fields(arguments, {"confirmed"}); Confirm(arguments); request.action = Action::ClearDebugProfile;
        }
        else throw Error("unsupported_action", "Unknown action or unselected debug environment; selection must be explicit");
    }
    else if (method == "settings.reset") throw Error("unsupported_setting", "Environment reset is the explicit confirmed clear action");
    else throw Error("method_not_found", "Unknown environment settings method", -32601);
    const auto result = context.controller.InvokeHostAction(request);
    if (!result.Succeeded()) throw Error(result.status == SettingsActionStatus::Busy ? "busy" : "settings_failed",
        result.message.empty() ? "The host rejected the debug environment operation" : Utf8(result.message));
    return Object({{"target", TargetJson(target)}, {"status", String("accepted")},
        {"restartMayBeRequired", Boolean(request.action != Action::RenameDebugEnvironment)},
        {"completion", String("Query runtime state after the host action; accepted does not assert restart completion")}});
}
}

bool IsEnvironmentSettingsTarget(const Target& target) noexcept
{
    return target.kind == "pages" || target.kind == "page" || target.kind == "debug-environments" || target.kind == "debug-environment";
}

Value HandleEnvironmentSettings(std::string_view method, const Value& params,
    const Target& target, const EnvironmentSettingsContext& context)
{
    if (target.kind == "pages" || target.kind == "page") return Pages(method, params, target, context);
    if (target.kind == "debug-environments" || target.kind == "debug-environment") return Debug(method, params, target, context);
    throw control::Error("settings_target_not_found", "Unknown environment settings target");
}
}
