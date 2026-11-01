#include "control_icon_settings.h"
#include "control_settings_values.h"
#include "icons/large_icon_edit_rules.h"
#include <algorithm>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
Value Arguments(const Value& params)
{
    const auto* value = params.Find("arguments");
    if (!value) return Object();
    if (!value->IsObject()) throw Error("invalid_params", "arguments must be a named object", -32602);
    return *value;
}
void Check(bool succeeded, const std::string& error)
{
    if (!succeeded) throw Error(error.empty() ? "icon_settings_failed" : error, "Native icon settings service rejected the operation");
}
Value Reply(const Target& target, const Value& values, const Value& identity,
    std::string_view status = "current", std::string_view persistence = "unchanged")
{
    return Object({{"target", TargetJson(target)}, {"revision", String(Revision(identity))},
        {"values", values}, {"status", String(std::string(status))},
        {"persistence", String(std::string(persistence))}, {"effects", Array()}});
}
void MethodFields(std::string_view method, const Value& params)
{
    if (method == "settings.get" || method == "settings.catalog") Fields(params, {"target"});
    else if (method == "settings.update") Fields(params, {"target", "ifRevision", "changes"});
    else if (method == "settings.reset") Fields(params, {"target", "ifRevision", "keys"});
    else if (method == "settings.action") Fields(params, {"target", "ifRevision", "action", "arguments"});
    else throw Error("method_not_found", "Unknown icon settings method", -32601);
}
Value Actions(std::initializer_list<const char*> names)
{
    auto result = Array();
    for (const auto* name : names) result.array.push_back(Object({{"name", String(name)}, {"requiresRevision", Boolean(true)}}));
    return result;
}
bool ReadOnlyLarge(std::string_view key)
{
    return key == "version" || key == "image" || key == "imageAssetId" || key == "cachedCover" ||
        key == "foregroundImage" || key == "foregroundImageAssetId";
}
Value LargeValue(const LargeIconConfig& config)
{
    auto result = Object();
    VisitLargeIconFields(config, [&](const char* key, const auto& field) { result.object.emplace(key, ToValue(field)); });
    return result;
}
LargeIconConfig Config(const std::string& text)
{
    Value document;
    LargeIconConfig config;
    if (!ParseJson(text, document) || !DecodeLargeIconConfig(document, config))
        throw Error("invalid_large_icon", "Native large-icon configuration could not be decoded");
    return config;
}
void PatchLarge(LargeIconConfig& config, const Value& changes)
{
    if (!changes.IsObject()) throw Error("invalid_params", "changes must be a named object", -32602);
    std::size_t count = 0;
    VisitLargeIconFields(config, [&](const char* key, auto& field) {
        if (const auto* value = changes.Find(key))
        {
            if (ReadOnlyLarge(key)) throw Error("read_only_setting", "Image references must use the owned import/library action");
            ++count; PatchValue(field, *value);
        }
    });
    if (count != changes.object.size()) throw Error("invalid_params", "Unknown large-icon setting", -32602);
    if (!ValidateLargeIconConfig(config)) throw Error("invalid_params", "Large-icon values violate native constraints", -32602);
}
LargeIconSettingsSnapshot ReadLarge(const Target& target, const IconSettingsContext& context)
{
    if (!context.largeIcon) throw Error("settings_unavailable", "Large-icon settings service is unavailable");
    LargeIconSettingsRequest request;
    request.key = values_detail::Wide(target.id);
    if (context.state.largeIcon && context.state.largeIcon->key == request.key && context.state.largeIcon->session)
    {
        request.action = "status"; request.session = context.state.largeIcon->session;
        request.revision = context.state.largeIcon->revision;
    }
    else
    {
        if (context.editingBusy && context.editingBusy())
            throw Error("settings_busy", "Close the visible settings editor before opening a direct large-icon edit session");
        request.action = "read";
    }
    auto snapshot = context.largeIcon(request);
    if (!snapshot.succeeded && snapshot.error == "largeIcon.stale")
    {
        context.state.largeIcon.reset();
        throw Error("stale_settings", "Large-icon editor ownership changed; query again");
    }
    if (snapshot.succeeded) context.state.largeIcon = snapshot;
    return snapshot;
}
}

bool IsIconSettingsTarget(const Target& target) noexcept
{
    return target.kind == "icon-appearance" || target.kind == "icon-library" || target.kind == "large-icon";
}

Value HandleIconSettings(std::string_view method, const Value& params,
    const Target& target, const IconSettingsContext& context)
{
    using namespace control;
    MethodFields(method, params);
    const bool read = method == "settings.get" || method == "settings.catalog";
    const bool catalogue = method == "settings.catalog";
    if (target.kind == "icon-appearance")
    {
        if (!context.appearance || target.id.empty()) throw Error("settings_unavailable", "An existing icon appearance target is required");
        IconAppearanceRequest request;
        request.target = values_detail::Wide(target.id);
        auto snapshot = context.appearance(request);
        Check(snapshot.succeeded, snapshot.error);
        auto values = Object({{"settings", ToValue(snapshot.current.settings)}, {"themeId", String(snapshot.current.themeId)},
            {"followGlobalTheme", Boolean(snapshot.current.followGlobalTheme)}});
        const auto identity = Object({{"values", values}, {"session", String(std::to_string(snapshot.session))},
            {"revision", String(std::to_string(snapshot.revision))}, {"target", TargetJson(target)}});
        if (read)
        {
            auto response = Reply(target, values, identity);
            response.object.emplace("available", Boolean(snapshot.available));
            response.object.emplace("choices", ToValue(snapshot.themes));
            if (catalogue)
            {
                response.object.emplace("fields", DescribeValue(snapshot.current.settings));
                response.object.emplace("actions", Actions({"preview", "inherit", "previewInherit", "cancel"}));
            }
            return response;
        }
        RequireRevision(params, Revision(identity));
        if (method == "settings.reset")
        {
            if (params.Find("keys")) throw Error("invalid_params", "Omit keys to restore inherited icon appearance", -32602);
            request.action = "inherit"; request.session = 0;
        }
        else
        {
            if (context.editingBusy && context.editingBusy()) throw Error("settings_busy", "Visible settings editor owns icon appearance");
            auto candidate = snapshot.current.settings;
            std::string themeId = snapshot.current.themeId;
            bool follow = snapshot.current.followGlobalTheme;
            const auto action = method == "settings.update" ? std::string("apply") : Text(params, "action");
            if (method != "settings.update" && method != "settings.action") throw Error("method_not_found", "Unknown settings method", -32601);
            if (action == "apply" || action == "preview")
            {
                const auto* changes = params.Find("changes");
                if (method == "settings.update" && !changes)
                    throw Error("invalid_params", "changes is required", -32602);
                const auto arguments = method == "settings.update" ? *changes : Arguments(params);
                Fields(arguments, {"settings", "themeId", "followGlobalTheme"});
                if (const auto* settings = arguments.Find("settings")) PatchValue(candidate, *settings);
                if (const auto* id = arguments.Find("themeId")) { if (!id->IsString()) throw Error("invalid_params", "themeId must be a string", -32602); themeId = id->string; }
                if (const auto* inherit = arguments.Find("followGlobalTheme")) { if (!inherit->IsBoolean()) throw Error("invalid_params", "followGlobalTheme must be boolean", -32602); follow = inherit->boolean; }
                // Native service validates the complete icon theme and target.
            }
            else if (action != "inherit" && action != "previewInherit" && action != "cancel")
                throw Error("unknown_settings_action", "Unknown icon appearance action", -32602);
            request.action = "begin";
            const auto begun = context.appearance(request); Check(begun.succeeded, begun.error);
            request.session = begun.session; request.revision = begun.revision;
            request.action = action == "preview" ? "previewApply" : action;
            request.settings = std::move(candidate); request.themeId = std::move(themeId); request.followGlobalTheme = follow;
        }
        const auto changed = context.appearance(request); Check(changed.succeeded, changed.error);
        auto response = HandleIconSettings("settings.get", Object({{"target", TargetJson(target)}}), target, context);
        response.object["status"] = String("applied");
        response.object["persistence"] = String(request.action == "previewApply" || request.action == "previewInherit" || request.action == "cancel" ? "unchanged" : "saved");
        return response;
    }
    if (target.kind == "large-icon")
    {
        if (target.id.empty()) throw Error("invalid_params", "An existing large-icon key is required", -32602);
        auto snapshot = ReadLarge(target, context);
        const auto identity = ToValue(snapshot);
        auto response = Reply(target, identity, identity);
        if (!snapshot.config.empty()) response.object["values"] = LargeValue(Config(snapshot.config));
        if (read)
        {
            response.object.emplace("available", Boolean(snapshot.available));
            response.object.emplace("editable", Boolean(snapshot.editable));
            response.object.emplace("error", String(snapshot.error));
            if (catalogue)
            {
                auto fields = Object();
                const auto defaults = snapshot.defaultConfig.empty() ? LargeIconConfig{} : Config(snapshot.defaultConfig);
                VisitLargeIconFields(defaults, [&](const char* key, const auto& field) {
                    auto description = DescribeValue(field); description.object["readOnly"] = Boolean(ReadOnlyLarge(key));
                    fields.object.emplace(key, std::move(description));
                });
                response.object.emplace("fields", std::move(fields));
                response.object.emplace("actions", Actions({"preview", "cancel", "refresh", "import", "library", "restoreNormal"}));
                response.object.emplace("exclusiveEditSession", Boolean(true));
            }
            return response;
        }
        RequireRevision(params, Revision(identity));
        if (method == "settings.action" && Text(params, "action") == "restoreNormal")
        {
            const auto arguments = Arguments(params); Fields(arguments, {});
            if (!context.restoreLargeIcon || !context.restoreLargeIcon(values_detail::Wide(target.id)))
                throw Error("icon_settings_failed", "Native service could not restore the ordinary icon");
            context.state.largeIcon.reset();
            const auto ordinary = Object({{"available", Boolean(false)}, {"ordinary", Boolean(true)}});
            return Reply(target, ordinary, ordinary, "applied", "saved");
        }
        Check(snapshot.succeeded, snapshot.error);
        if (context.editingBusy && context.editingBusy())
            throw Error("settings_busy", "Visible settings editor owns large-icon editing");
        if (!snapshot.editable) throw Error("feature_locked", "Large-icon editing is unavailable for this runtime");
        LargeIconSettingsRequest request;
        request.key = snapshot.key; request.session = snapshot.session; request.revision = snapshot.revision;
        if (method == "settings.update" || method == "settings.reset")
        {
            auto config = Config(snapshot.config);
            Value patch;
            if (method == "settings.reset")
            {
                patch = LargeValue(Config(snapshot.defaultConfig));
                for (auto it = patch.object.begin(); it != patch.object.end();)
                    if (ReadOnlyLarge(it->first)) it = patch.object.erase(it); else ++it;
                if (const auto* keys = params.Find("keys"))
                {
                    if (!keys->IsArray()) throw Error("invalid_params", "keys must be a string array", -32602);
                    auto selected = Object();
                    for (const auto& key : keys->array)
                    {
                        if (!key.IsString() || !patch.Find(key.string)) throw Error("invalid_params", "Unknown or read-only reset key", -32602);
                        selected.object.emplace(key.string, *patch.Find(key.string));
                    }
                    patch = std::move(selected);
                }
            }
            else { const auto* changes = params.Find("changes"); if (!changes) throw Error("invalid_params", "changes is required", -32602); patch = *changes; }
            PatchLarge(config, patch);
            request.action = "commit"; request.config = EncodeLargeIconConfig(config);
            for (const auto& [key, unused] : patch.object) { (void)unused; request.fields.push_back(key); }
        }
        else if (method == "settings.action")
        {
            request.action = Text(params, "action");
            const auto arguments = Arguments(params);
            if (request.action == "preview")
            {
                auto config = Config(snapshot.config); PatchLarge(config, arguments);
                request.config = EncodeLargeIconConfig(config);
                for (const auto& [key, unused] : arguments.object) { (void)unused; request.fields.push_back(key); }
            }
            else if (request.action == "import" || request.action == "library")
            { Fields(arguments, {"path"}); request.path = values_detail::Wide(Text(arguments, "path")); }
            else if (request.action == "cancel" || request.action == "refresh") Fields(arguments, {});
            else throw Error("unknown_settings_action", "Unknown large-icon action", -32602);
        }
        const auto changed = context.largeIcon(request); Check(changed.succeeded, changed.error);
        context.state.largeIcon = changed;
        response = HandleIconSettings("settings.get", Object({{"target", TargetJson(target)}}), target, context);
        response.object["status"] = String(changed.loading ? "started" : "applied");
        response.object["persistence"] = String(changed.loading ? "pending" : request.action == "commit" ? "saved" : "unchanged");
        return response;
    }
    if (target.kind == "icon-library")
    {
        if (!context.library) throw Error("settings_unavailable", "Icon library service is unavailable");
        IconLibraryRequest request;
        const auto targetId = values_detail::Wide(target.id);
        if (context.state.library && context.state.library->target == targetId && context.state.library->session)
            request.target = targetId;
        auto snapshot = context.library(request);
        if (!snapshot.succeeded && !snapshot.busy) Check(false, snapshot.error);
        const auto identity = ToValue(snapshot);
        context.state.library = snapshot;
        if (read)
        {
            auto response = Reply(target, identity, identity);
            if (catalogue)
            {
                response.object.emplace("fields", Object({{"readOnly", Boolean(true)}}));
                response.object.emplace("actions", Actions({"begin", "restore", "apply", "cancel", "refresh", "import", "rename", "delete", "export",
                    "createGroup", "renameGroup", "deleteGroup", "deleteGroupWithAssets", "moveGroup", "copyLocalGroup"}));
            }
            return response;
        }
        RequireRevision(params, Revision(identity));
        if (method != "settings.action") throw Error("action_required", "Icon library changes use an explicitly named action");
        if (snapshot.busy) throw Error("settings_busy", "Icon library has pending work");
        const auto action = Text(params, "action");
        const auto allowed = Actions({"begin", "restore", "apply", "cancel", "refresh", "import", "rename", "delete", "export",
            "createGroup", "renameGroup", "deleteGroup", "deleteGroupWithAssets", "moveGroup", "copyLocalGroup"});
        if (std::none_of(allowed.array.begin(), allowed.array.end(), [&](const auto& item) { return item.Find("name")->string == action; }))
            throw Error("unknown_settings_action", "Unknown local icon library action", -32602);
        auto arguments = Arguments(params);
        for (const auto* reserved : {"action", "target", "session", "revision", "operationId", "preparationId", "rightsConfirmed"})
            if (arguments.Find(reserved)) throw Error("invalid_params", "Reserved icon operation field", -32602);
        if (action == "delete" || action == "deleteGroup" || action == "deleteGroupWithAssets")
        {
            const auto* confirmed = arguments.Find("confirmed");
            if (!confirmed || !confirmed->IsBoolean() || !confirmed->boolean)
                throw Error("confirmation_required", "Confirm this icon deletion with arguments.confirmed:true and the current revision");
            arguments.object.erase("confirmed");
        }
        PatchValue(request, arguments); request.action = action;
        request.target = values_detail::Wide(target.id); request.session = snapshot.session; request.revision = snapshot.revision;
        const auto changed = context.library(request);
        if (!changed.busy) Check(changed.succeeded, changed.error);
        context.state.library = changed;
        return Reply(target, ToValue(changed), ToValue(changed), changed.busy ? "started" : "applied", changed.busy ? "pending" : "saved");
    }
    throw Error("unknown_settings_target", "Unknown icon settings target", -32602);
}
}
