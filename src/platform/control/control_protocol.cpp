#include "control_protocol.h"
#include "common/json_string.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <vector>

namespace snowdesktop::control
{
Value Object(std::initializer_list<std::pair<std::string, Value>> fields)
{
    Value value; value.type = Value::Type::Object;
    for (const auto& field : fields) value.object.emplace(field);
    return value;
}
Value Array() { Value value; value.type = Value::Type::Array; return value; }
Value String(std::string text) { Value value; value.type = Value::Type::String; value.string = std::move(text); return value; }
Value Number(double number) { Value value; value.type = Value::Type::Number; value.number = number; return value; }
Value Boolean(bool flag) { Value value; value.type = Value::Type::Boolean; value.boolean = flag; return value; }

std::string Serialize(const Value& value)
{
    switch (value.type)
    {
    case Value::Type::Null: return "null";
    case Value::Type::Boolean: return value.boolean ? "true" : "false";
    case Value::Type::String: return "\"" + json::EscapeString(value.string) + "\"";
    case Value::Type::Number:
    {
        if (!std::isfinite(value.number)) throw Error("invalid_number", "Nonfinite JSON number", -32603);
        char buffer[64]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value.number);
        if (result.ec != std::errc{}) throw Error("invalid_number", "Cannot encode JSON number", -32603);
        return std::string(buffer, result.ptr);
    }
    case Value::Type::Array:
    {
        std::string output = "[";
        for (const auto& entry : value.array) { if (output.size() > 1) output += ','; output += Serialize(entry); }
        return output + ']';
    }
    case Value::Type::Object:
    {
        std::vector<std::string_view> keys;
        for (const auto& [key, entry] : value.object) { (void)entry; keys.push_back(key); }
        std::sort(keys.begin(), keys.end());
        std::string output = "{";
        for (const auto key : keys)
        {
            if (output.size() > 1) output += ',';
            output += Serialize(String(std::string(key))) + ':' + Serialize(value.object.at(std::string(key)));
        }
        return output + '}';
    }
    }
    throw Error("invalid_value", "Unknown JSON value", -32603);
}

Error::Error(std::string value, std::string message, int number)
    : std::runtime_error(std::move(message)), code(std::move(value)), rpcCode(number) {}

void Fields(const Value& params, std::initializer_list<std::string_view> allowed)
{
    if (!params.IsObject()) throw Error("invalid_params", "params must be an object", -32602);
    for (const auto& [key, value] : params.object)
    {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw Error("invalid_params", "Unknown parameter: " + key, -32602);
    }
}
std::string Text(const Value& params, std::string_view key, bool required)
{
    const auto* field = params.Find(key);
    if (!field && !required) return {};
    if (!field || !field->IsString() || field->string.empty() || field->string.size() > 4096 ||
        field->string.find('\0') != std::string::npos)
        throw Error("invalid_params", "Expected nonempty string: " + std::string(key), -32602);
    return field->string;
}
int Integer(const Value& params, std::string_view key, int minimum, int maximum)
{
    const auto* field = params.Find(key);
    if (!field || !field->IsNumber() || !std::isfinite(field->number) ||
        field->number != std::floor(field->number) || field->number < minimum || field->number > maximum)
        throw Error("invalid_params", "Integer outside supported range: " + std::string(key), -32602);
    return static_cast<int>(field->number);
}

namespace
{
Value ResultField(const char* type, const char* description)
{
    return Object({{"type", String(type)}, {"description", String(description)}});
}

Value ResultObject(const char* description,
    std::initializer_list<std::pair<std::string, Value>> properties)
{
    auto result = ResultField("object", description);
    result.object.emplace("properties", Object(properties));
    return result;
}

Value ResultArray(const char* description, Value items)
{
    auto result = ResultField("array", description);
    result.object.emplace("items", std::move(items));
    return result;
}

Value DesktopLayoutResultSchema()
{
    const auto bounds = ResultObject(
        "Host layout rectangle in physical pixels relative to the virtual desktop origin. Use ui.snapshot for screen coordinates and visibility.", {
        {"left", ResultField("integer", "X coordinate of the left edge.")},
        {"top", ResultField("integer", "Y coordinate of the top edge.")},
        {"right", ResultField("integer", "X coordinate of the exclusive right edge.")},
        {"bottom", ResultField("integer", "Y coordinate of the exclusive bottom edge.")},
    });
    const auto page = ResultObject("Grid geometry for a desktop page or layout surface.", {
        {"id", ResultField("string", "Page ID referenced by widget.pageId.")},
        {"monitorId", ResultField("string", "Host display identity for the page.")},
        {"bounds", bounds},
        {"columns", ResultField("integer", "Number of grid columns on this page.")},
        {"rows", ResultField("integer", "Number of grid rows on this page.")},
        {"dpiX", ResultField("integer", "Horizontal display DPI used for layout scaling.")},
        {"dpiY", ResultField("integer", "Vertical display DPI used for layout scaling.")},
    });
    const auto widget = ResultObject("Placement and group membership for one widget instance.", {
        {"id", ResultField("string", "Widget instance ID. Pass this to widgets.get or widget-targeted settings queries.")},
        {"type", ResultField("string", "Instance type, such as collection, lua, or luaStack. Count type lua for Lua instances. A luaStack is the container of its child instances.")},
        {"title", ResultField("string", "Current display title. Titles can be shared by different widget instances.")},
        {"pageId", ResultField("string", "Stored placement page. __snowdesktop_dock__ is the Dock surface.")},
        {"column", ResultField("integer", "Stored zero-based starting column for the widget's grid placement.")},
        {"row", ResultField("integer", "Stored zero-based starting row for the widget's grid placement.")},
        {"columns", ResultField("integer", "Horizontal grid span in cells.")},
        {"rows", ResultField("integer", "Vertical grid span in cells.")},
        {"grouped", ResultField("boolean", "True when the widget is a child of another widget. For grouped widgets, the parent owns the visible placement.")},
        {"bounds", bounds},
        {"activeMemberId", ResultField("string", "Selected child widget ID for a Lua stack. Other widget types return an empty string.")},
        {"memberCount", ResultField("integer", "Number of child widget IDs, equal to the length of memberIds. A collection can hold files or shortcuts and still return zero. This result has no total collection item count.")},
        {"memberIds", ResultArray("Child widget instance IDs in stored order. Collection files and shortcuts are outside this list.",
            ResultField("string", "ID of one child widget instance."))},
    });
    const auto revision = ResultField("string", "Opaque layout identity including placement state. Use the latest value as ifLayoutRevision before moving or resizing a widget.");
    const auto layout = ResultObject("Current desktop grid geometry and widget placements.", {
        {"layoutRevision", revision},
        {"pages", ResultArray("Grid pages in the host layout.", page)},
        {"widgets", ResultArray("Widget instances, including group children and Dock widgets.", widget)},
    });
    return Object({
        {"layout", layout},
        {"widget", ResultObject("One widget and its layout identity.", {
            {"widget", widget},
            {"layoutRevision", revision},
        })},
        {"placement", ResultObject("Result of applying an in-memory widget placement.", {
            {"status", ResultField("string", "Applied means the requested placement was accepted by the host.")},
            {"persistence", ResultField("string", "Queued means saving was scheduled after application in memory.")},
            {"layout", layout},
        })},
    });
}

Value SettingsSearchResultSchema()
{
    const auto target = ResultObject("Settings catalog target. Some target kinds need an instance ID.", {
        {"kind", ResultField("string", "Settings target kind.")},
        {"id", ResultField("string", "Concrete instance ID, when provided.")},
    });
    const auto match = ResultObject("One documented field or action argument.", {
        {"target", target},
        {"field", ResultField("string", "Field path relative to the target or action. [] refers to an array element and .* to a map value.")},
        {"description", ResultField("string", "English behavior description from the settings metadata.")},
        {"requiresTargetId", ResultField("boolean", "When present and true, resolve a concrete instance before querying its catalog.")},
        {"action", ResultField("string", "Named settings.action when the field is an action argument. Omitted for direct fields.")},
        {"name", ResultField("string", "Name of the Lua instance, when available.")},
    });
    return ResultObject("One page of matching setting descriptions.", {
        {"query", ResultField("string", "Search text after trimming surrounding whitespace.")},
        {"results", ResultArray("Matches ordered by relevance, with a deterministic tie order.", match)},
        {"total", ResultField("integer", "Number of matching descriptions before pagination.")},
        {"offset", ResultField("integer", "Zero-based starting match for this page.")},
        {"limit", ResultField("integer", "Maximum matches requested for this page.")},
        {"unavailableWidgetCount", ResultField("integer", "Number of Lua instances whose runtime declarations were unavailable.")},
    });
}
}

Value Describe()
{
    Value methods = Array();
    const auto add = [&](const char* name, bool mutation, const char* parameters,
                         const char* resultSchema = nullptr) {
        auto method = Object({{"name", String(name)}, {"mutates", Boolean(mutation)},
            {"parameters", String(parameters)}});
        if (resultSchema)
            method.object.emplace("resultSchema", String(resultSchema));
        methods.array.push_back(std::move(method));
    };
    add("api.describe", false, "{}");
    add("app.getState", false, "{}");
    add("ui.snapshot", false, "{}");
    add("input.begin", true, "{monitorId:string,x?:integer,y?:integer,showCursor?:boolean}");
    add("input.pointer", true, "{sessionId:string,type:string,x?:integer,y?:integer,button?:string,delta?:integer}");
    add("input.keyboard", true, "{sessionId:string,type:string,key?:integer,text?:string}");
    add("input.state", false, "{includeTrace?:boolean}");
    add("input.end", true, "{sessionId:string}");
    add("ui.dismissMenu", true, "{}");
    add("ui.settingsRefresh", false, "{}");
    add("ui.settingsState", false, "{id:string}");
    add("settings.search", false, "{query:string,offset?:integer,limit?:integer}", "settingsSearch");
    add("settings.catalog", false, "{target?:{kind:string,id?:string},offset?:integer,limit?:integer}");
    add("settings.get", false, "{target:{kind:string,id?:string},offset?:integer,limit?:integer}");
    add("settings.update", true, "{target:{kind:string,id?:string},ifRevision:string,changes:object}");
    add("settings.reset", true, "{target:{kind:string,id?:string},ifRevision:string,keys?:string[]}");
    add("settings.action", true, "{target:{kind:string,id?:string},ifRevision:string,action:string,arguments?:object}");
    add("preview.describe", false, "{}");
    add("preview.render", true, "{scene:object}");
    add("preview.state", false, "{id:string}");
    add("preview.cancel", true, "{id:string}");
    add("capture.monitors", false, "{}");
    add("capture.screenshot", true, "{monitorId?:string,includeSystemCursor?:boolean}");
    add("capture.record", true, "{monitorId?:string,includeSystemCursor?:boolean,durationMs:integer,fps:integer}");
    add("capture.state", false, "{id:string}");
    add("capture.cancel", true, "{id:string}");
    add("desktop.getLayout", false, "{}", "layout");
    add("desktop.inspect", false, "{}");
    add("desktop.previewEdits", false, "{ifLayoutRevision:string,addWidgets?:array,moveWidgets?:array,setCollections?:array,setGroups?:array,moveIcons?:array}");
    add("desktop.applyEdits", true, "{ifLayoutRevision:string,addWidgets?:array,moveWidgets?:array,setCollections?:array,setGroups?:array,moveIcons?:array}");
    add("widgets.list", false, "{}", "layout");
    add("widgets.get", false, "{id:string}", "widget");
    add("widgets.agentActions", false, "{id:string}");
    add("widgets.invokeAgentAction", true, "{id:string,action:string,arguments?:object}");
    add("widgets.place", true, "{id:string,pageId:string,column:integer,row:integer,columns:integer,rows:integer,ifLayoutRevision:string}", "placement");
    add("widgets.open", true, "{id:string}");
    add("widgets.close", true, "{}");
    add("navigation.open", true, "{}");
    add("navigation.close", true, "{}");
    add("settings.open", true, "{page?:string,widgetId?:string}");
    add("dock.show", true, "{}");
    add("dock.hide", true, "{}");
    auto resultSchemas = DesktopLayoutResultSchema();
    resultSchemas.object.emplace("settingsSearch", SettingsSearchResultSchema());
    return Object({{"protocolVersion", Number(ProtocolVersion)}, {"methods", std::move(methods)},
        {"resultSchemas", std::move(resultSchemas)},
        {"maximumFrameBytes", Number(static_cast<double>(MaximumFrameBytes))},
        {"guidance", Object({
            {"language", String("en")},
            {"overview", String("SnowDesktop organizes desktop items in pages, grids and storage components. It also provides visual personalization, a Dock, a status bar and Quick Navigation.")},
            {"organize", String("Read desktop.inspect for the native component catalog, layouts, icon entrances, group tabs, file filters and search. Use desktop.previewEdits and desktop.applyEdits to create components, assign group or collection members and place entries in one batch. Check concrete native-widget settings for live controls.")},
            {"interactionStyle", String("Discover layout presets and their effects on Dock, status bar, Windows taskbar and reserved space in the runtime-services settings catalog. Use settings.search for specific navigation, motion or visibility behavior, then inspect the owning settings target.")},
            {"appearance", String("Use the personalization target for common surface defaults, desktop for ordinary icon and title presentation, and the individual component or icon target for its own overrides. An independent theme may take precedence over global settings.")},
            {"settings", String("Use settings.search to find settings by keywords in their field paths and English descriptions. Results identify a target and field, plus an action for action arguments. Resolve targets marked requiresTargetId, then use settings.catalog on the concrete target to check values, constraints, availability and revision. Call settings.catalog without a target to discover domains and live component instances. Refresh the revision before each edit and respect confirmation, restart and availability requirements.")},
            {"components", String("Use native-widget for built-in storage component view and content settings and widget for Lua-authored settings and host appearance. Discover a loaded Lua instance's optional English action declarations with widgets.agentActions. A delivered action ran the component event callback; verify the outcome separately.")},
        })},
        {"capabilities", Object({{"elements", Number(1)}, {"privatePointer", Number(1)}, {"privateKeyboard", Number(1)},
            {"demoCursor", Number(1)}, {"monitorCapture", Number(1)}, {"menus", Number(1)},
            {"settingsAutomation", Number(1)}, {"previewRender", Number(1)}, {"monitorRecording", Number(1)},
            {"nativeWidgetControls", Number(1)}, {"demoElementTargets", Number(1)},
            {"nativePopupControls", Number(1)}, {"menuControlValues", Number(1)}, {"settingsPrivateInput", Number(0)},
            {"directSettings", Number(1)}, {"settingsSchema", Number(1)}, {"nativeWidgetSettings", Number(1)},
            {"settingsGuidance", Number(1)}, {"settingsSearch", Number(1)}, {"widgetAgentActions", Number(1)},
            {"desktopBatchEdits", Number(1)}, {"desktopNativeComponents", Number(1)}, {"desktopComponentCatalog", Number(1)}})}});
}

std::string Dispatch(std::string_view request, const Handler& handler,
    const AccessCheck& accessCheck) noexcept
{
    Value id;
    try
    {
        Value root;
        if (request.size() > MaximumFrameBytes || !ParseJson(request, root))
            throw Error("parse_error", "Invalid JSON request", -32700);
        if (!root.IsObject()) throw Error("invalid_request", "Expected a JSON-RPC object", -32600);
        const auto* receivedId = root.Find("id");
        if (!receivedId || (!receivedId->IsString() && !receivedId->IsNumber()) ||
            (receivedId->IsString() && receivedId->string.size() > 128) ||
            (receivedId->IsNumber() && (!std::isfinite(receivedId->number) ||
                receivedId->number != std::floor(receivedId->number) || std::abs(receivedId->number) > 9007199254740991.0)))
            throw Error("invalid_request", "A string or safe integer id is required", -32600);
        id = *receivedId;
        Fields(root, {"jsonrpc", "id", "method", "params"});
        if (Text(root, "jsonrpc") != "2.0") throw Error("invalid_request", "jsonrpc must be 2.0", -32600);
        const auto method = Text(root, "method");
        if (accessCheck && !accessCheck())
            throw Error("agent_operations_disabled",
                "Ask the user to enable Allow agent access in SnowDesktop Settings > Agent Operations.");
        static const auto description = Describe();
        bool known = false;
        for (const auto& entry : description.Find("methods")->array)
            if (entry.Find("name")->string == method) known = true;
        if (!known) throw Error("method_not_found", "Unknown method", -32601);
        const Value empty = Object();
        const auto* params = root.Find("params");
        if (!params) params = &empty;
        if (!params->IsObject()) throw Error("invalid_params", "params must be an object", -32602);
        Value result;
        if (method == "api.describe") { Fields(*params, {}); result = description; }
        else result = handler(method, *params);
        auto response = Serialize(Object({{"jsonrpc", String("2.0")}, {"id", id}, {"result", std::move(result)}}));
        if (response.size() > MaximumFrameBytes) throw Error("result_too_large", "Result exceeds frame limit");
        return response;
    }
    catch (const Error& error)
    {
        return Serialize(Object({{"jsonrpc", String("2.0")}, {"id", id}, {"error", Object({
            {"code", Number(error.rpcCode)}, {"message", String(error.what())},
            {"data", Object({{"code", String(error.code)}})}})}}));
    }
    catch (...)
    {
        return Serialize(Object({{"jsonrpc", String("2.0")}, {"id", id}, {"error", Object({
            {"code", Number(-32603)}, {"message", String("Internal application control error")},
            {"data", Object({{"code", String("internal_error")}})}})}}));
    }
}
}
