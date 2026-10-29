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

Value Describe()
{
    Value methods = Array();
    const auto add = [&](const char* name, bool mutation, const char* parameters) {
        methods.array.push_back(Object({{"name", String(name)}, {"mutates", Boolean(mutation)},
            {"parameters", String(parameters)}}));
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
    add("desktop.getLayout", false, "{}");
    add("widgets.list", false, "{}");
    add("widgets.get", false, "{id:string}");
    add("widgets.place", true, "{id:string,pageId:string,column:integer,row:integer,columns:integer,rows:integer,ifLayoutRevision:string}");
    add("widgets.open", true, "{id:string}");
    add("widgets.close", true, "{}");
    add("navigation.open", true, "{}");
    add("navigation.close", true, "{}");
    add("settings.open", true, "{page?:string,widgetId?:string}");
    add("dock.show", true, "{}");
    add("dock.hide", true, "{}");
    return Object({{"protocolVersion", Number(ProtocolVersion)}, {"methods", std::move(methods)},
        {"maximumFrameBytes", Number(static_cast<double>(MaximumFrameBytes))},
        {"capabilities", Object({{"elements", Number(1)}, {"privatePointer", Number(1)}, {"privateKeyboard", Number(1)},
            {"demoCursor", Number(1)}, {"monitorCapture", Number(1)}, {"menus", Number(1)},
            {"settingsAutomation", Number(1)}, {"previewRender", Number(1)}, {"monitorRecording", Number(1)},
            {"nativeWidgetControls", Number(1)}, {"demoElementTargets", Number(1)},
            {"nativePopupControls", Number(1)}, {"menuControlValues", Number(1)}, {"settingsPrivateInput", Number(0)},
            {"directSettings", Number(1)}, {"settingsSchema", Number(1)}, {"nativeWidgetSettings", Number(1)}})}});
}

std::string Dispatch(std::string_view request, const Handler& handler) noexcept
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
        const auto description = Describe();
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
