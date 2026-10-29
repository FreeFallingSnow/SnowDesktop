#include "control_widget_settings.h"
#include "control_settings_values.h"
#include "windows_compat.h"
#include <charconv>
#include <cmath>
#include <limits>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
using namespace widget_runtime;

std::wstring InstanceId(const std::string& value)
{
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!length) throw Error("invalid_params", "Invalid UTF-8 component identity", -32602);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length))
        throw Error("invalid_params", "Invalid UTF-8 component identity", -32602);
    return result;
}

Value Natural(const InteractionValue& value)
{
    using T = InteractionValue::Type;
    switch (value.type)
    {
    case T::Boolean: return Boolean(value.boolean);
    case T::Integer:
        // The service may retain 64-bit values; do not silently round them in
        // JSON's interoperable integer range.
        if (value.integer < -9007199254740991LL || value.integer > 9007199254740991LL)
            return Object({{"integer", String(std::to_string(value.integer))}});
        return Number(static_cast<double>(value.integer));
    case T::Number: return Number(value.number);
    case T::String: return String(value.string);
    case T::Array:
    {
        auto result = Array();
        for (const auto& item : value.array) result.array.push_back(Natural(item));
        return result;
    }
    case T::Object:
    {
        auto result = Object();
        for (const auto& [key, item] : value.object) result.object.emplace(key, Natural(item));
        return result;
    }
    default: return {};
    }
}

InteractionValue FromNatural(const Value& value, unsigned depth = 0)
{
    if (depth > 24) throw Error("invalid_params", "Component setting nesting limit", -32602);
    InteractionValue result;
    if (value.IsBoolean()) { result.type = InteractionValue::Type::Boolean; result.boolean = value.boolean; }
    else if (value.IsString()) { result.type = InteractionValue::Type::String; result.string = value.string; }
    else if (value.IsNumber())
    {
        if (!std::isfinite(value.number) || std::abs(value.number) > 9007199254740991.0)
            throw Error("invalid_params", "Number exceeds the exact JSON range", -32602);
        if (std::trunc(value.number) == value.number)
        { result.type = InteractionValue::Type::Integer; result.integer = static_cast<long long>(value.number); }
        else { result.type = InteractionValue::Type::Number; result.number = value.number; }
    }
    else if (value.IsArray())
    {
        if (value.array.size() > 4096) throw Error("invalid_params", "Too many setting elements", -32602);
        result.type = InteractionValue::Type::Array;
        for (const auto& item : value.array) result.array.push_back(FromNatural(item, depth + 1));
    }
    else if (value.IsObject())
    {
        const auto* integer = value.Find("integer");
        if (integer && value.object.size() == 1 && integer->IsString())
        {
            result.type = InteractionValue::Type::Integer;
            const auto& text = integer->string;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result.integer);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw Error("invalid_params", "Invalid decimal integer", -32602);
        }
        else
        {
            if (value.object.size() > 4096) throw Error("invalid_params", "Too many setting members", -32602);
            result.type = InteractionValue::Type::Object;
            for (const auto& [key, item] : value.object) result.object.emplace(key, FromNatural(item, depth + 1));
        }
    }
    return result;
}

void CheckResult(const WidgetSettingMutationResult& result)
{
    if (result.Succeeded()) return;
    throw Error(result.errorCode.empty() ? "widget_settings_failed" : result.errorCode,
        result.message.empty() ? "Component settings operation failed" : result.message);
}

std::string SnapshotRevision(const WidgetSettingsSnapshot& snapshot)
{
    return std::to_string(snapshot.generation) + ":" + std::to_string(snapshot.revision);
}

Value ReadValues(const WidgetSettingsSnapshot& snapshot, IWidgetSettingsService& service)
{
    auto ordinary = Object(), opaque = Object(), searches = Object();
    for (const auto& field : snapshot.fields)
    {
        if (field.schema.Channel() == WidgetSettingValueChannel::Ordinary)
            ordinary.object.emplace(field.schema.key, Natural(field.currentValue));
        else opaque.object.emplace(field.schema.key, ToValue(field.opaque));
        if (auto search = service.SearchSnapshot(snapshot.widgetId, field.schema.key))
            searches.object.emplace(field.schema.key, ToValue(*search));
    }
    return Object({{"settings", std::move(ordinary)}, {"opaque", std::move(opaque)},
        {"hostAppearance", ToValue(snapshot.hostAppearance)}, {"searches", std::move(searches)}});
}

Value Response(const Target& target, const WidgetSettingsSnapshot& snapshot,
    IWidgetSettingsService& service, std::string_view status = "current", bool persisted = false)
{
    return Object({{"target", TargetJson(target)}, {"revision", String(SnapshotRevision(snapshot))},
        {"values", ReadValues(snapshot, service)}, {"status", String(std::string(status))},
        {"persistence", String(persisted ? "saved" : "unchanged")}, {"effects", Array()}});
}

const Value& Arguments(const Value& params)
{
    static const auto empty = Object();
    const auto* arguments = params.Find("arguments");
    if (!arguments) return empty;
    if (!arguments->IsObject()) throw Error("invalid_params", "arguments must be an object", -32602);
    return *arguments;
}

std::string TextValue(const Value& arguments, std::string_view key)
{
    const auto* value = arguments.Find(key);
    if (!value || !value->IsString() || value->string.size() > 32768 || value->string.find('\0') != std::string::npos)
        throw Error("invalid_params", "Expected bounded text value: " + std::string(key), -32602);
    (void)values_detail::Wide(value->string);
    return value->string;
}

std::uint64_t Decimal(const Value& params, std::string_view key)
{
    const auto text = Text(params, key);
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw Error("invalid_params", "Expected a decimal string identifier", -32602);
    return number;
}
}

Value HandleWidgetSettings(std::string_view method, const Value& params,
    const Target& target, widget_runtime::IWidgetSettingsService& service,
    const std::function<bool(std::wstring_view)>& ensure)
{
    using namespace control;
    using namespace widget_runtime;
    if (target.kind != "widget" || target.id.empty())
        throw Error("invalid_params", "A component instance target is required", -32602);
    const auto widgetId = InstanceId(target.id);
    if (ensure && !ensure(widgetId)) throw Error("widget_not_found", "Component instance is unavailable");
    auto snapshot = service.Snapshot(widgetId);
    if (!snapshot)
    {
        auto loaded = service.Load(widgetId);
        if (!loaded.Succeeded() || !loaded.snapshot)
            throw Error(loaded.errorCode.empty() ? "widget_settings_unavailable" : loaded.errorCode,
                loaded.message.empty() ? "Component settings could not be loaded" : loaded.message);
        snapshot = std::move(loaded.snapshot);
    }
    if (method == "settings.get" || method == "settings.catalog")
    {
        Fields(params, {"target"});
        auto response = Response(target, *snapshot, service);
        if (method == "settings.catalog")
        {
            auto fields = Array(), presets = Array(), actions = Array();
            for (const auto& field : snapshot->fields)
            {
                auto item = Object({{"key", String(field.schema.key)}, {"schema", ToValue(field.schema)},
                    {"visible", Boolean(field.visible)}, {"enabled", Boolean(field.enabled)},
                    {"valid", Boolean(field.valid)}, {"validationError", String(field.validationError)},
                    {"channel", String(field.schema.Channel() == WidgetSettingValueChannel::Ordinary ? "ordinary" :
                        field.schema.Channel() == WidgetSettingValueChannel::Secret ? "secret" :
                        field.schema.Channel() == WidgetSettingValueChannel::FilesystemHandle ? "filesystem-handle" : "entity-reference")},
                    {"readOnly", Boolean(!field.enabled)}, {"resettable", Boolean(field.schema.Channel() == WidgetSettingValueChannel::Ordinary)}});
                if (field.schema.Channel() == WidgetSettingValueChannel::Ordinary)
                    item.object.emplace("default", Natural(field.defaultValue));
                fields.array.push_back(std::move(item));
            }
            for (const auto& preset : snapshot->presets)
                presets.array.push_back(Object({{"id", String(preset.id)}, {"label", String(preset.label)}}));
            const auto addAction = [&](const char* name, Value arguments, const char* effect) {
                actions.array.push_back(Object({{"name", String(name)}, {"arguments", std::move(arguments)},
                    {"effect", String(effect)}, {"requiresRevision", Boolean(true)}}));
            };
            const auto keySchema = Object({{"key", String("declared-setting-key")}});
            addAction("setSecret", Object({{"key", String("secret-setting-key")}, {"value", String("secret-text-not-returned")}}), "save-secret");
            addAction("clear", keySchema, "clear-opaque-configuration");
            addAction("chooseFile", keySchema, "open-owned-filesystem-picker");
            addAction("chooseReference", keySchema, "open-owned-reference-picker");
            addAction("applyPreset", Object({{"id", String("declared-preset-id")}}), "save-atomic-preset");
            addAction("appearance", DescribeValue(WidgetHostAppearancePatch{}), "save-host-appearance");
            addAction("previewAppearance", DescribeValue(WidgetHostAppearancePatch{}), "transient-host-appearance");
            addAction("previewSetting", Object({{"key", String("ordinary-setting-key")}, {"value", String("typed-setting-value")}}), "transient-setting");
            addAction("setSearchQuery", Object({{"key", String("search-setting-key")}, {"query", String("text")}}), "save-search-query");
            addAction("commitPreview", Object(), "save-preview");
            addAction("revertPreview", Object(), "discard-preview");
            addAction("search", Object({{"key", String("search-setting-key")}, {"query", String("text")}, {"maximumResults", Number(8)}}), "asynchronous-search");
            addAction("cancelSearch", keySchema, "cancel-search");
            addAction("commitSearchResult", Object({{"key", String("search-setting-key")}, {"requestId", String("decimal-search-request-id")}, {"resultId", String("owned-result-id")}}), "save-owned-search-result");
            response.object.emplace("fields", std::move(fields));
            response.object.emplace("presets", std::move(presets));
            response.object.emplace("actions", std::move(actions));
            response.object.emplace("updateLimit", Number(1));
            response.object.emplace("availability", String("available"));
        }
        return response;
    }

    RequireRevision(params, SnapshotRevision(*snapshot));
    const auto guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    WidgetSettingMutationResult result;
    bool persistent = true;
    if (method == "settings.update")
    {
        Fields(params, {"target", "ifRevision", "changes"});
        const auto* changes = params.Find("changes");
        if (!changes || !changes->IsObject() || changes->object.size() != 1)
            throw Error("invalid_params", "Update one declared component setting at a time; use a preset for an atomic bundle", -32602);
        const auto& [key, value] = *changes->object.begin();
        result = service.SetOrdinary(guard, key, FromNatural(value));
    }
    else if (method == "settings.reset")
    {
        Fields(params, {"target", "ifRevision", "keys"});
        if (const auto* keys = params.Find("keys"))
        {
            if (!keys->IsArray() || keys->array.size() != 1 || !keys->array.front().IsString())
                throw Error("invalid_params", "Reset one declared key, or omit keys to reset ordinary settings atomically", -32602);
            result = service.ResetField(guard, keys->array.front().string);
        }
        else result = service.Reset(guard);
    }
    else if (method == "settings.action")
    {
        Fields(params, {"target", "ifRevision", "action", "arguments"});
        const auto action = Text(params, "action");
        const auto& arguments = Arguments(params);
        if (action == "setSecret")
        { Fields(arguments, {"key", "value"}); result = service.SetSecret(guard, Text(arguments, "key"), TextValue(arguments, "value")); }
        else if (action == "clear")
        { Fields(arguments, {"key"}); result = service.ClearOpaque(guard, Text(arguments, "key")); }
        else if (action == "chooseFile")
        { Fields(arguments, {"key"}); result = service.ChooseFilesystemHandle(guard, Text(arguments, "key")); }
        else if (action == "chooseReference")
        { Fields(arguments, {"key"}); result = service.OpenEntityReferencePicker(guard, Text(arguments, "key")); }
        else if (action == "applyPreset")
        { Fields(arguments, {"id"}); result = service.ApplyPreset(guard, Text(arguments, "id")); }
        else if (action == "previewSetting")
        {
            Fields(arguments, {"key", "value"});
            const auto* value = arguments.Find("value");
            if (!value) throw Error("invalid_params", "value is required", -32602);
            result = service.PreviewOrdinary(guard, Text(arguments, "key"), FromNatural(*value));
            persistent = false;
        }
        else if (action == "setSearchQuery")
        { Fields(arguments, {"key", "query"}); result = service.SetSearchQuery(guard, Text(arguments, "key"), TextValue(arguments, "query")); }
        else if (action == "appearance" || action == "previewAppearance")
        {
            WidgetHostAppearancePatch patch;
            PatchValue(patch, arguments);
            result = action == "appearance" ? service.UpdateHostAppearance(guard, patch) : service.PreviewHostAppearance(guard, patch);
            persistent = action == "appearance";
        }
        else if (action == "commitPreview" || action == "revertPreview")
        { Fields(arguments, {}); result = action == "commitPreview" ? service.CommitPreview(guard) : service.RevertPreview(guard); persistent = action == "commitPreview"; }
        else if (action == "search")
        {
            Fields(arguments, {"key", "query", "maximumResults"});
            const auto maximum = arguments.Find("maximumResults") ? Integer(arguments, "maximumResults", 1, 64) : 8;
            result = service.StartSearch(guard, Text(arguments, "key"), TextValue(arguments, "query"), static_cast<std::size_t>(maximum));
            persistent = false;
        }
        else if (action == "cancelSearch")
        { Fields(arguments, {"key"}); result = service.CancelSearch(guard, Text(arguments, "key")); persistent = false; }
        else if (action == "commitSearchResult")
        { Fields(arguments, {"key", "requestId", "resultId"}); result = service.CommitSearchResult(guard, Text(arguments, "key"), Decimal(arguments, "requestId"), Text(arguments, "resultId")); }
        else throw Error("unknown_settings_action", "Unknown component settings action", -32602);
    }
    else throw Error("method_not_found", "Unknown settings method", -32601);
    CheckResult(result);
    snapshot = service.Snapshot(widgetId);
    if (!snapshot) throw Error("widget_settings_unavailable", "Component disappeared after the operation");
    const bool started = result.status == WidgetSettingMutationStatus::Started;
    auto response = Response(target, *snapshot, service, started ? "started" : "applied", persistent && !started);
    if (started) response.object["persistence"] = String("pending");
    return response;
}
}
