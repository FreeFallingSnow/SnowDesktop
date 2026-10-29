#include "control_native_widget_settings.h"

#include "layout/list_detail_rules.h"
#include <algorithm>
#include <cmath>
#include <set>
#include "winui/windows_compat.h"

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
struct BooleanField { const char* name; bool NativeWidgetConfiguration::*member; };
constexpr BooleanField kBooleans[]{
    {"showTitle", &NativeWidgetConfiguration::showTitle}, {"bottomBarHover", &NativeWidgetConfiguration::bottomBarHover},
    {"showOnHoverOnly", &NativeWidgetConfiguration::showOnHoverOnly}, {"keepWhenDesktopHidden", &NativeWidgetConfiguration::keepWhenDesktopHidden},
    {"privacyMode", &NativeWidgetConfiguration::privacyMode}, {"singleCellIconMode", &NativeWidgetConfiguration::singleCellIconMode},
    {"scrollContainerMode", &NativeWidgetConfiguration::scrollContainerMode}, {"largeFolderTitleless", &NativeWidgetConfiguration::largeFolderTitleless},
    {"titleBarCollapsed", &NativeWidgetConfiguration::titleBarCollapsed}, {"titleBarExpandOnHover", &NativeWidgetConfiguration::titleBarExpandOnHover},
    {"showSearchBox", &NativeWidgetConfiguration::showSearchBox}, {"listMode", &NativeWidgetConfiguration::listMode},
    {"detailShowModified", &NativeWidgetConfiguration::detailShowModified}, {"detailShowType", &NativeWidgetConfiguration::detailShowType},
    {"detailShowSize", &NativeWidgetConfiguration::detailShowSize}, {"dateHeaders", &NativeWidgetConfiguration::dateHeaders},
    {"showFileCategories", &NativeWidgetConfiguration::showFileCategories}, {"fanPopup", &NativeWidgetConfiguration::fanPopup},
    {"sortAscending", &NativeWidgetConfiguration::sortAscending}, {"autoCollect", &NativeWidgetConfiguration::autoCollect}};
struct PositionField { const char* name; float NativeWidgetConfiguration::*member; };
constexpr PositionField kPositions[]{
    {"detailModifiedPosition", &NativeWidgetConfiguration::detailModifiedPosition},
    {"detailTypePosition", &NativeWidgetConfiguration::detailTypePosition},
    {"detailSizePosition", &NativeWidgetConfiguration::detailSizePosition}};
bool Contains(const std::vector<std::string>& values, std::string_view value)
{ return std::find(values.begin(), values.end(), value) != values.end(); }
Value Strings(const std::vector<std::string>& strings)
{
    auto result = Array();
    for (const auto& string : strings) result.array.push_back(String(string));
    return result;
}
void Invalid(const std::string& message) { throw Error("invalid_params", message, -32602); }
std::string StringValue(const Value& value, std::size_t maximum)
{
    if (!value.IsString() || value.string.size() > maximum || value.string.find('\0') != std::string::npos ||
        (!value.string.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.string.data(), static_cast<int>(value.string.size()), nullptr, 0) <= 0))
        Invalid("Expected bounded UTF-8 text without embedded NUL");
    return value.string;
}
std::vector<std::string> StringArray(const Value& value, const std::vector<std::string>& allowed)
{
    if (!value.IsArray() || value.array.size() > allowed.size()) Invalid("Expected an array of distinct known IDs");
    std::vector<std::string> result;
    for (const auto& item : value.array)
    {
        const auto id = StringValue(item, 4096);
        if (!Contains(allowed, id) || Contains(result, id)) Invalid("Unknown or duplicate native category ID");
        result.push_back(id);
    }
    return result;
}
bool ContentKey(std::string_view key)
{
    return key == "listMode" || key == "listColumns" || key.starts_with("detail") || key == "dateHeaders" ||
        key == "showFileCategories" || key == "sortColumn" || key == "sortAscending" || key == "autoCollect" || key == "autoCollectCategoryIds";
}
Value StateJson(const NativeWidgetSettingsState& state)
{
    return Object({{"id", String(state.id)}, {"type", String(state.type)}, {"contentTargetId", String(state.contentTargetId)},
        {"sourceTargetId", String(state.sourceTargetId)}, {"sortTargetId", String(state.sortTargetId)}, {"activeChildId", String(state.activeChildId)},
        {"ownershipRevision", String(state.ownershipRevision)}, {"values", NativeWidgetConfigurationJson(state.values)},
        {"defaults", NativeWidgetConfigurationJson(state.defaults)},
        {"customTitle", String(state.customTitle)}, {"editableKeys", Strings(state.editableKeys)},
        {"categoryIds", Strings(state.categoryIds)}, {"activeCategoryIds", Strings(state.activeCategoryIds)},
        {"contentCategoryIds", Strings(state.contentCategoryIds)}, {"childIds", Strings(state.childIds)},
        {"categoryTabIds", Strings(state.categoryTabIds)},
        {"sourceFolderPath", String(state.sourceFolderPath)}, {"placement", state.placement}, {"searchText", String(state.searchText)}});
}
Value Read(const Target& target, const NativeWidgetSettingsState& state, bool catalog)
{
    auto result = StateJson(state);
    result.object.erase("ownershipRevision");
    result.object.emplace("target", TargetJson(target));
    result.object.emplace("revision", String(NativeWidgetSettingsRevision(state)));
    result.object.emplace("editable", Boolean(state.editable));
    result.object.emplace("searchAvailable", Boolean(state.searchAvailable));
    if (!catalog) return result;
    auto fields = Object();
    const auto values = NativeWidgetConfigurationJson(state.values);
    const auto defaults = NativeWidgetConfigurationJson(state.defaults);
    for (const auto& [key, value] : values.object)
    {
        auto field = Object({{"type", String(value.IsBoolean() ? "boolean" : value.IsNumber() ? "number" : value.IsArray() ? "array" : "string")},
            {"available", Boolean(Contains(state.editableKeys, key))}, {"default", defaults.object.at(key)},
            {"ownerId", String(key == "autoCollect" || key == "autoCollectCategoryIds" || key == "contentCategoryId" || key == "categoryTabOrder" ? state.sourceTargetId :
                key == "sortColumn" || key == "sortAscending" ? state.sortTargetId : ContentKey(key) ? state.contentTargetId : state.id)}});
        if (key == "listColumns") { field.object.emplace("minimum", Number(1)); field.object.emplace("maximum", Number(4)); field.object.insert_or_assign("type", String("integer")); }
        if (key.starts_with("detail") && key.ends_with("Position"))
        { field.object.emplace("minimum", Number(0.05)); field.object.emplace("maximum", Number(0.95)); field.object.emplace("minimumVisibleGap", Number(0.04)); }
        if (key == "sortColumn") field.object.emplace("enum", Strings({"none", "name", "modified", "type", "size"}));
        if (key == "showTitle") field.object.emplace("description", String("Persisted native title visibility; top-title-bar layout may provide its own title chrome"));
        if (key == "bottomBarHover") field.object.emplace("description", String("Show the native bottom action bar on hover where that presentation is used"));
        if (key == "title") field.object.emplace("resetDescription", String("Clears the custom title and restores the native type/source title"));
        fields.object.emplace(key, std::move(field));
    }
    result.object.emplace("fields", std::move(fields));
    result.object.emplace("demoIconCategories", Strings(state.demoIconCategories));
    result.object.emplace("readOnly", Strings({"id", "type", "contentTargetId", "sourceTargetId", "sortTargetId", "activeChildId", "childIds", "sourceFolderPath", "placement", "customTitle", "searchText"}));
    auto actions = Array();
    if (state.collectAvailable) actions.array.push_back(String("collect-now"));
    if (state.searchAvailable) actions.array.push_back(String("set-search"));
    if (!state.childIds.empty()) actions.array.push_back(String("reorder-tabs"));
    result.object.emplace("actions", std::move(actions));
    result.object.emplace("constraints", Strings({"Group activeCategoryId changes cannot be combined with content/source edits; read the new revision first",
        "listColumns above one disables metadata columns; explicitly enabling metadata requires one column",
        "Placement and membership use the existing widgets/place and native grouping operations",
        "set-search changes only the live view; its query is not persisted"}));
    return result;
}
}

control::Value NativeWidgetConfigurationJson(const NativeWidgetConfiguration& values)
{
    auto result = control::Object();
    for (const auto& field : kBooleans) result.object.emplace(field.name, control::Boolean(values.*field.member));
    for (const auto& field : kPositions) result.object.emplace(field.name, control::Number(values.*field.member));
    result.object.emplace("title", control::String(values.title));
    result.object.emplace("demoIconCategory", control::String(values.demoIconCategory));
    result.object.emplace("listColumns", control::Number(values.listColumns));
    result.object.emplace("sortColumn", control::String(values.sortColumn));
    result.object.emplace("autoCollectCategoryIds", Strings(values.autoCollectCategoryIds));
    result.object.emplace("activeCategoryId", control::String(values.activeCategoryId));
    result.object.emplace("contentCategoryId", control::String(values.contentCategoryId));
    result.object.emplace("categoryTabOrder", Strings(values.categoryTabOrder));
    return result;
}
std::string NativeWidgetSettingsRevision(const NativeWidgetSettingsState& state) { return Revision(StateJson(state)); }

NativeWidgetSettingsPatch ParseNativeWidgetSettingsPatch(const NativeWidgetSettingsState& state, const control::Value& changes)
{
    if (!changes.IsObject() || changes.object.empty()) Invalid("changes must be a nonempty named object");
    NativeWidgetSettingsPatch patch{state.values};
    for (const auto& [key, value] : changes.object)
    {
        if (!Contains(state.editableKeys, key)) throw Error("unsupported_setting", "Native setting is unknown or not applicable: " + key);
        patch.keys.push_back(key);
        const auto boolean = std::find_if(std::begin(kBooleans), std::end(kBooleans), [&](const auto& field) { return key == field.name; });
        if (boolean != std::end(kBooleans))
        {
            if (!value.IsBoolean()) Invalid(key + " must be boolean");
            patch.values.*boolean->member = value.boolean;
            continue;
        }
        const auto position = std::find_if(std::begin(kPositions), std::end(kPositions), [&](const auto& field) { return key == field.name; });
        if (position != std::end(kPositions))
        {
            if (!value.IsNumber() || !std::isfinite(value.number) || value.number < 0.05 || value.number > 0.95) Invalid(key + " must be between 0.05 and 0.95");
            patch.values.*position->member = static_cast<float>(value.number);
        }
        else if (key == "title") patch.values.title = StringValue(value, 4096);
        else if (key == "listColumns")
        {
            if (!value.IsNumber() || value.number != std::floor(value.number) || value.number < 1 || value.number > 4) Invalid("listColumns must be an integer from one to four");
            patch.values.listColumns = static_cast<int>(value.number);
        }
        else if (key == "sortColumn")
        {
            patch.values.sortColumn = StringValue(value, 16);
            if (!Contains({"none", "name", "modified", "type", "size"}, patch.values.sortColumn)) Invalid("Unknown native sort column");
        }
        else if (key == "demoIconCategory")
        {
            patch.values.demoIconCategory = StringValue(value, 128);
            if (!patch.values.demoIconCategory.empty() && !Contains(state.demoIconCategories, patch.values.demoIconCategory)) Invalid("Unknown demo icon category");
        }
        else if (key == "activeCategoryId")
        {
            patch.values.activeCategoryId = StringValue(value, 4096);
            if (!patch.values.activeCategoryId.empty() && !Contains(state.activeCategoryIds, patch.values.activeCategoryId)) Invalid("Unknown active native category or child");
        }
        else if (key == "contentCategoryId")
        {
            patch.values.contentCategoryId = StringValue(value, 4096);
            if (!patch.values.contentCategoryId.empty() && !Contains(state.contentCategoryIds, patch.values.contentCategoryId)) Invalid("Unknown inner file category");
        }
        else if (key == "autoCollectCategoryIds") patch.values.autoCollectCategoryIds = StringArray(value, state.categoryIds);
        else if (key == "categoryTabOrder") patch.values.categoryTabOrder = StringArray(value, state.categoryTabIds);
    }
    if (Contains(patch.keys, "activeCategoryId") && (state.type == "collection-group" || state.type == "file-group") &&
        std::any_of(patch.keys.begin(), patch.keys.end(), [](const auto& key) { return ContentKey(key) || key == "contentCategoryId" || key == "categoryTabOrder"; }))
        Invalid("Select the group child first, then read its revision before editing content or source settings");
    if (Contains(patch.keys, "listColumns") && patch.values.listColumns > 1)
    {
        for (const auto* key : {"detailShowModified", "detailShowType", "detailShowSize"})
            if (const auto* value = changes.Find(key); value && value->boolean) Invalid("Metadata columns require listColumns:1");
        patch.values.detailShowModified = patch.values.detailShowType = patch.values.detailShowSize = false;
    }
    else if (patch.values.detailShowModified || patch.values.detailShowType || patch.values.detailShowSize) patch.values.listColumns = 1;
    // Enabling a native metadata column also makes its saved divider usable,
    // as the right-click handler did before this shared transaction existed.
    // Explicit divider requests remain strict and are never silently clamped.
    if (!changes.Find("detailModifiedPosition") && !changes.Find("detailTypePosition") && !changes.Find("detailSizePosition") &&
        ((patch.values.detailShowModified && !state.values.detailShowModified) ||
         (patch.values.detailShowType && !state.values.detailShowType) || (patch.values.detailShowSize && !state.values.detailShowSize)))
    {
        const auto positions = list_detail_rules::NormalizePositions(patch.values.detailShowModified, patch.values.detailShowType,
            patch.values.detailShowSize, {patch.values.detailModifiedPosition, patch.values.detailTypePosition, patch.values.detailSizePosition});
        if (positions.modified != patch.values.detailModifiedPosition) { patch.values.detailModifiedPosition = positions.modified; patch.keys.push_back("detailModifiedPosition"); }
        if (positions.type != patch.values.detailTypePosition) { patch.values.detailTypePosition = positions.type; patch.keys.push_back("detailTypePosition"); }
        if (positions.size != patch.values.detailSizePosition) { patch.values.detailSizePosition = positions.size; patch.keys.push_back("detailSizePosition"); }
    }
    const auto& v = patch.values;
    std::vector<float> dividers;
    if (v.detailShowModified) dividers.push_back(v.detailModifiedPosition);
    if (v.detailShowType) dividers.push_back(v.detailTypePosition);
    if (v.detailShowSize) dividers.push_back(v.detailSizePosition);
    for (std::size_t i = 1; i < dividers.size(); ++i)
        if (dividers[i] - dividers[i - 1] < list_detail_rules::kMinimumDividerGap - 0.000001f)
            Invalid("Visible native metadata dividers must keep their order and minimum gap");
    return patch;
}
void ValidateNativeWidgetSettingsPatch(const NativeWidgetSettingsState& state, const NativeWidgetSettingsPatch& patch)
{
    if (patch.collectNow && !state.collectAvailable) throw control::Error("unsupported_action", "The effective source cannot collect desktop files");
    if (!patch.reorderedChildIds.empty())
    {
        auto before = state.childIds, after = patch.reorderedChildIds;
        std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
        if (before != after || std::adjacent_find(after.begin(), after.end()) != after.end()) Invalid("Group tab order must be an exact permutation of existing children");
    }
    if (patch.keys.empty()) { if (!patch.collectNow && patch.reorderedChildIds.empty()) Invalid("A native transaction requires a setting or action"); return; }
    auto changes = control::Object();
    const auto values = NativeWidgetConfigurationJson(patch.values);
    for (const auto& key : patch.keys)
    {
        const auto* value = values.Find(key);
        if (!value) Invalid("Unknown native setting key");
        if (!changes.object.emplace(key, *value).second) Invalid("Duplicate native setting key");
    }
    (void)ParseNativeWidgetSettingsPatch(state, changes);
}
control::Value HandleNativeWidgetSettings(std::string_view method, const control::Value& params,
    const Target& target, const NativeWidgetSettingsContext& context)
{
    using namespace control;
    if (target.kind != "native-widget" || target.id.empty() || !context.capture) throw Error("invalid_target", "Expected an existing native widget ID");
    const auto state = context.capture(target.id);
    if (method == "settings.get" || method == "settings.catalog")
    { Fields(params, {"target"}); return Read(target, state, method == "settings.catalog"); }
    if (!state.editable || !context.apply) throw Error("busy", "The native desktop model is not editable");
    RequireRevision(params, NativeWidgetSettingsRevision(state));
    NativeWidgetSettingsPatch patch;
    if (method == "settings.update")
    {
        Fields(params, {"target", "ifRevision", "changes"});
        const auto* changes = params.Find("changes");
        if (!changes) Invalid("changes is required");
        patch = ParseNativeWidgetSettingsPatch(state, *changes);
    }
    else if (method == "settings.reset")
    {
        Fields(params, {"target", "ifRevision", "keys"});
        const auto* keys = params.Find("keys");
        if (!keys || !keys->IsArray() || keys->array.empty()) Invalid("keys must be a nonempty array");
        auto changes = Object();
        const auto defaults = NativeWidgetConfigurationJson(state.defaults);
        for (const auto& key : keys->array)
        {
            if (!key.IsString() || !Contains(state.editableKeys, key.string) || changes.Find(key.string)) Invalid("Reset keys must be distinct applicable settings");
            changes.object.emplace(key.string, key.string == "title" ? String("") : defaults.object.at(key.string));
        }
        patch = ParseNativeWidgetSettingsPatch(state, changes);
    }
    else if (method == "settings.action")
    {
        Fields(params, {"target", "ifRevision", "action", "arguments"});
        const auto name = Text(params, "action");
        const auto* arguments = params.Find("arguments");
        if (!arguments || !arguments->IsObject()) Invalid("arguments must be a named object");
        if (name == "set-search")
        {
            Fields(*arguments, {"query"});
            const auto* query = arguments->Find("query");
            if (!query) Invalid("query is required");
            const auto text = StringValue(*query, 4096);
            if (!state.searchAvailable || !context.setSearch || !context.setSearch(target.id, text)) throw Error("settings_unavailable", "The native search view is unavailable");
            auto result = Read(target, context.capture(target.id), false);
            result.object.emplace("status", String("applied")); result.object.emplace("transient", Boolean(true));
            result.object.emplace("persistence", String("unchanged")); return result;
        }
        if (name == "reorder-tabs")
        {
            Fields(*arguments, {"order"});
            const auto* order = arguments->Find("order");
            if (!order || state.childIds.empty()) Invalid("An existing native group and order are required");
            patch.values = state.values; patch.reorderedChildIds = StringArray(*order, state.childIds);
            if (patch.reorderedChildIds.size() != state.childIds.size()) Invalid("Every existing group child must occur exactly once");
            ValidateNativeWidgetSettingsPatch(state, patch);
        }
        else
        {
            if (name != "collect-now") throw Error("unsupported_action", "Unknown native widget action");
            Fields(*arguments, {"confirmed"});
            const auto* confirmed = arguments->Find("confirmed");
            if (!confirmed || !confirmed->IsBoolean() || !confirmed->boolean) throw Error("confirmation_required", "collect-now requires arguments.confirmed:true");
            patch.values = state.values; patch.collectNow = true;
            ValidateNativeWidgetSettingsPatch(state, patch);
        }
    }
    else throw Error("method_not_found", "Unknown native settings method", -32601);
    if (!context.apply(state, patch)) throw Error("settings_failed", "The native configuration could not be saved; the previous model was restored");
    auto result = Read(target, context.capture(target.id), false);
    result.object.emplace("status", String("applied")); result.object.emplace("persistence", String("saved")); return result;
}
}
