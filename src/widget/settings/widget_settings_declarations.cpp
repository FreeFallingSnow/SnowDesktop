#include "widget_settings_declarations.h"

#include "widget_setting_rules.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
bool IsRemovedPanelEffectSettingKey(const std::string& key)
{
    const size_t separator = key.rfind('.');
    const std::string setting = separator == std::string::npos
        ? key : key.substr(separator + 1);
    return setting == "shadowAlpha" || setting == "shadowBlur" ||
        setting == "shadowOffsetY" || setting == "highlightAlpha" ||
        setting == "noiseAlpha";
}

bool IsHostStructureSettingKey(const std::string& key)
{
    return key == "cornerRadius" || key == "barHeight";
}

bool IsHostAppearanceSettingKey(const std::string& key)
{
    return key == "bg" || key == "border" || key == "alpha" ||
        key == "borderAlpha" || key == "borderStyle" ||
        key == "borderWidth" || key == "edgeHighlightEnabled" ||
        key == "edgeHighlightWidth" || key == "edgeHighlightStrength" ||
        key == "gradientEndA" ||
        IsRemovedPanelEffectSettingKey(key) || key == "glassEnabled" ||
        key == "glassBlurRadius" || key == "acrylicEnabled" ||
        key == "followPersonalization" || key == "__edgeLight";
}

static std::string LuaValueToStorageString(lua_State* L, int index)
{
    index = lua_absindex(L, index);
    if (lua_isboolean(L, index))
        return lua_toboolean(L, index) ? "1" : "0";
    if (lua_isinteger(L, index))
        return std::to_string(static_cast<long long>(lua_tointeger(L, index)));
    if (lua_isnumber(L, index))
    {
        std::ostringstream ss;
        ss << lua_tonumber(L, index);
        return ss.str();
    }
    if (lua_isstring(L, index))
        return lua_tostring(L, index);
    return {};
}

std::string LuaReadStorageField(lua_State* L, int tableIndex, const char* key)
{
    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, key);
    std::string value = LuaValueToStorageString(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool LuaReadBoolField(lua_State* L, int tableIndex, const char* key, bool defaultValue = false)
{
    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, key);
    bool value = lua_isnil(L, -1) ? defaultValue : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return value;
}

static std::vector<std::string> ReadLuaStringArray(
    lua_State* L, int tableIndex)
{
    std::vector<std::string> values;
    tableIndex = lua_absindex(L, tableIndex);
    for (int index = 1; index <= 65; ++index)
    {
        lua_rawgeti(L, tableIndex, index);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            break;
        }
        values.push_back(LuaValueToStorageString(L, -1));
        lua_pop(L, 1);
    }
    return values;
}

static std::optional<LuaWidgetManifest::SettingCondition>
ReadLuaSettingConditionField(lua_State* L, int tableIndex,
    const char* field)
{
    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, field);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return std::nullopt;
    }
    LuaWidgetManifest::SettingCondition condition;
    if (!lua_istable(L, -1))
    {
        condition.operation = "__invalid";
        lua_pop(L, 1);
        return condition;
    }
    const int conditionTable = lua_absindex(L, -1);
    condition.key = LuaReadStorageField(L, conditionTable, "key");
    condition.operation =
        LuaReadStorageField(L, conditionTable, "operator");
    lua_getfield(L, conditionTable, "value");
    if (lua_istable(L, -1))
    {
        const int valuesTable = lua_absindex(L, -1);
        for (int index = 1; index <= 65; ++index)
        {
            lua_rawgeti(L, valuesTable, index);
            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                break;
            }
            if (!lua_isboolean(L, -1) && !lua_isnumber(L, -1) &&
                !lua_isstring(L, -1))
                condition.operation = "__invalid";
            condition.values.push_back(
                LuaValueToStorageString(L, -1));
            lua_pop(L, 1);
        }
    }
    else if (!lua_isnil(L, -1))
    {
        if (!lua_isboolean(L, -1) && !lua_isnumber(L, -1) &&
            !lua_isstring(L, -1))
            condition.operation = "__invalid";
        condition.values.push_back(LuaValueToStorageString(L, -1));
    }
    lua_pop(L, 2);
    return condition;
}

static LuaWidgetManifest::Setting ReadLuaSettingTable(lua_State* L, int tableIndex)
{
    tableIndex = lua_absindex(L, tableIndex);
    LuaWidgetManifest::Setting setting;
    setting.key = LuaReadStorageField(L, tableIndex, "key");
    setting.label = LuaReadStorageField(L, tableIndex, "label");
    setting.description =
        LuaReadStorageField(L, tableIndex, "description");
    setting.agentDescription =
        LuaReadStorageField(L, tableIndex, "agentDescription");
    setting.group = LuaReadStorageField(L, tableIndex, "group");
    setting.validationMessage =
        LuaReadStorageField(L, tableIndex, "validationMessage");
    setting.dependsOn =
        LuaReadStorageField(L, tableIndex, "dependsOn");
    setting.type = LuaReadStorageField(L, tableIndex, "type");
    if (setting.type == "duration")
    {
        const auto unit = LuaReadStorageField(L, tableIndex, "unit");
        if (!unit.empty()) setting.durationUnit = unit;
        const int scale = snowdesktop::widget_runtime::DurationSettingUnitSeconds(setting.durationUnit);
        setting.maxValue = scale ? snowdesktop::widget_runtime::kMaximumSettingDurationSeconds / scale : 0;
        setting.stepValue = 1;
    }
    setting.defaultValue = LuaReadStorageField(L, tableIndex, "default");
    setting.searchKey = LuaReadStorageField(L, tableIndex, "searchKey");
    setting.binding = LuaReadStorageField(L, tableIndex, "binding");
    setting.access = LuaReadStorageField(L, tableIndex, "access");
    setting.emptyLabel = LuaReadStorageField(L, tableIndex, "emptyLabel");
    setting.noResultsLabel =
        LuaReadStorageField(L, tableIndex, "noResultsLabel");

    lua_getfield(L, tableIndex, "min");
    if (lua_isnumber(L, -1)) setting.minValue = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "max");
    if (lua_isnumber(L, -1)) setting.maxValue = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "step");
    if (lua_isnumber(L, -1)) setting.stepValue = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (setting.type == "duration" && setting.defaultValue.empty())
        setting.defaultValue = std::to_string(setting.minValue);
    if (setting.type == "duration")
        for (const char* property : {"min", "max", "step"})
        {
            lua_getfield(L, tableIndex, property);
            if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TNUMBER)
                setting.stepValue = std::numeric_limits<double>::quiet_NaN();
            lua_pop(L, 1);
        }
    lua_getfield(L, tableIndex, "minLength");
    if (!lua_isnil(L, -1))
    {
        const lua_Integer value = lua_isinteger(L, -1)
            ? lua_tointeger(L, -1)
            : static_cast<lua_Integer>(-2);
        setting.minLength = value >= (std::numeric_limits<int>::min)() &&
            value <= (std::numeric_limits<int>::max)()
            ? static_cast<int>(value) : -2;
    }
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "maxLength");
    if (!lua_isnil(L, -1))
    {
        const lua_Integer value = lua_isinteger(L, -1)
            ? lua_tointeger(L, -1)
            : static_cast<lua_Integer>(-2);
        setting.maxLength = value >= (std::numeric_limits<int>::min)() &&
            value <= (std::numeric_limits<int>::max)()
            ? static_cast<int>(value) : -2;
    }
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "required");
    if (lua_isboolean(L, -1))
        setting.required = lua_toboolean(L, -1) != 0;
    else if (!lua_isnil(L, -1))
        setting.minLength = -2;
    lua_pop(L, 1);
    setting.showWhen = ReadLuaSettingConditionField(
        L, tableIndex, "showWhen");
    setting.enabledWhen = ReadLuaSettingConditionField(
        L, tableIndex, "enabledWhen");

    if (setting.type == "multiSelect")
    {
        lua_getfield(L, tableIndex, "default");
        if (lua_istable(L, -1))
            setting.defaultValues = ReadLuaStringArray(L, -1);
        lua_pop(L, 1);
        setting.defaultValue.clear();
    }

    lua_getfield(L, tableIndex, "options");
    if (lua_istable(L, -1))
        setting.options = ReadLuaStringArray(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "optionLabels");
    if (lua_istable(L, -1))
        setting.optionLabels = ReadLuaStringArray(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "extensions");
    if (lua_istable(L, -1))
        setting.extensions = ReadLuaStringArray(L, -1);
    lua_pop(L, 1);

    if (setting.label.empty()) setting.label = setting.key;
    if (setting.type.empty()) setting.type = "text";
    if (setting.access.empty()) setting.access = "read";
    if (setting.type == "password")
    {
        // Secret defaults would put plaintext in the package or Lua source.
        setting.defaultValue.clear();
        setting.defaultValues.clear();
        setting.options.clear();
        setting.optionLabels.clear();
        setting.extensions.clear();
    }
    else if (setting.type == "appReference" ||
        setting.type == "desktopItemReference" ||
        setting.type == "fileReference" ||
        setting.type == "folderReference")
    {
        // Host-owned references never enter ordinary setting storage.
        setting.defaultValue.clear();
        setting.defaultValues.clear();
        setting.searchKey.clear();
        setting.options.clear();
        setting.optionLabels.clear();
        setting.extensions.clear();
    }
    else if (setting.type == "fileHandle" ||
        setting.type == "folderHandle")
    {
        // Handles are host-owned grants and never ordinary setting values.
        setting.defaultValue.clear();
        setting.defaultValues.clear();
        setting.searchKey.clear();
        setting.binding.clear();
        setting.options.clear();
        setting.optionLabels.clear();
    }
    return setting;
}

static LuaWidgetManifest::SettingGroup ReadLuaSettingGroupTable(
    lua_State* L, int tableIndex)
{
    tableIndex = lua_absindex(L, tableIndex);
    LuaWidgetManifest::SettingGroup group;
    group.id = LuaReadStorageField(L, tableIndex, "id");
    group.label = LuaReadStorageField(L, tableIndex, "label");
    group.description =
        LuaReadStorageField(L, tableIndex, "description");
    group.collapsible = LuaReadBoolField(
        L, tableIndex, "collapsible");
    group.defaultExpanded = LuaReadBoolField(
        L, tableIndex, "defaultExpanded", true);
    if (group.label.empty()) group.label = group.id;
    return group;
}

static void ReadLuaSettingGroupsArray(lua_State* L, int arrayIndex,
    std::vector<LuaWidgetManifest::SettingGroup>& out)
{
    arrayIndex = lua_absindex(L, arrayIndex);
    for (int index = 1;; ++index)
    {
        lua_rawgeti(L, arrayIndex, index);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            break;
        }
        if (lua_istable(L, -1))
        {
            auto group = ReadLuaSettingGroupTable(L, -1);
            out.push_back(std::move(group));
        }
        lua_pop(L, 1);
    }
}

static void ReadLuaSettingsArray(lua_State* L, int arrayIndex,
    std::vector<LuaWidgetManifest::Setting>& out)
{
    arrayIndex = lua_absindex(L, arrayIndex);
    for (int i = 1;; ++i)
    {
        lua_rawgeti(L, arrayIndex, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (lua_istable(L, -1))
        {
            LuaWidgetManifest::Setting setting = ReadLuaSettingTable(L, -1);
            if (!setting.key.empty() &&
                !IsHostStructureSettingKey(setting.key) &&
                !IsHostAppearanceSettingKey(setting.key))
                out.push_back(std::move(setting));
        }
        lua_pop(L, 1);
    }
}

static void ReadLuaPresetValues(lua_State* L, int tableIndex,
    LuaWidgetManifest::SettingPreset& preset)
{
    tableIndex = lua_absindex(L, tableIndex);
    lua_pushnil(L);
    while (lua_next(L, tableIndex) != 0)
    {
        if (lua_type(L, -2) == LUA_TSTRING)
        {
            std::string key = lua_tostring(L, -2);
            if (!IsHostStructureSettingKey(key))
            {
                if (lua_istable(L, -1))
                    preset.arrayValues[key] =
                        ReadLuaStringArray(L, -1);
                else
                {
                    std::string value = LuaValueToStorageString(L, -1);
                    if (!value.empty())
                        preset.values[key] = std::move(value);
                }
            }
        }
        lua_pop(L, 1);
    }
}

static LuaWidgetManifest::SettingPreset ReadLuaPresetTable(lua_State* L, int tableIndex)
{
    tableIndex = lua_absindex(L, tableIndex);
    LuaWidgetManifest::SettingPreset preset;
    preset.id = LuaReadStorageField(L, tableIndex, "id");
    preset.label = LuaReadStorageField(L, tableIndex, "label");
    if (preset.label.empty())
        preset.label = LuaReadStorageField(L, tableIndex, "name");
    preset.isDefault = LuaReadBoolField(L, tableIndex, "default") ||
        LuaReadBoolField(L, tableIndex, "isDefault");
    preset.showWhen = ReadLuaSettingConditionField(L, tableIndex, "showWhen");

    lua_getfield(L, tableIndex, "values");
    if (lua_istable(L, -1))
        ReadLuaPresetValues(L, -1, preset);
    lua_pop(L, 1);

    if (preset.id.empty()) preset.id = preset.label;
    if (preset.label.empty()) preset.label = preset.id;
    return preset;
}

static void ReadLuaPresetsArray(lua_State* L, int arrayIndex,
    std::vector<LuaWidgetManifest::SettingPreset>& out)
{
    arrayIndex = lua_absindex(L, arrayIndex);
    for (int i = 1;; ++i)
    {
        lua_rawgeti(L, arrayIndex, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (lua_istable(L, -1))
        {
            LuaWidgetManifest::SettingPreset preset = ReadLuaPresetTable(L, -1);
            if (!preset.id.empty() && !preset.label.empty() &&
                (!preset.values.empty() || !preset.arrayValues.empty()))
                out.push_back(std::move(preset));
        }
        lua_pop(L, 1);
    }
}

void ReadLuaDeclaredSettings(lua_State* L, int widgetTableIndex,
    std::vector<LuaWidgetManifest::Setting>& settings,
    std::vector<LuaWidgetManifest::SettingGroup>& groups,
    std::vector<LuaWidgetManifest::SettingPreset>& presets)
{
    widgetTableIndex = lua_absindex(L, widgetTableIndex);
    lua_getfield(L, widgetTableIndex, "settings");
    if (lua_istable(L, -1))
    {
        int settingsTable = lua_absindex(L, -1);
        lua_getfield(L, settingsTable, "fields");
        if (lua_istable(L, -1))
        {
            ReadLuaSettingsArray(L, -1, settings);
            lua_pop(L, 1);
        }
        else
        {
            lua_pop(L, 1);
            lua_rawgeti(L, settingsTable, 1);
            bool directArray = lua_istable(L, -1);
            lua_pop(L, 1);
            if (directArray)
                ReadLuaSettingsArray(L, settingsTable, settings);
        }

        lua_getfield(L, settingsTable, "groups");
        if (lua_istable(L, -1))
            ReadLuaSettingGroupsArray(L, -1, groups);
        lua_pop(L, 1);

        lua_getfield(L, settingsTable, "presets");
        if (lua_istable(L, -1))
            ReadLuaPresetsArray(L, -1, presets);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, widgetTableIndex, "presets");
    if (lua_istable(L, -1))
        ReadLuaPresetsArray(L, -1, presets);
    lua_pop(L, 1);
}

bool IsEntityReferenceSettingType(std::string_view type) noexcept
{
    return type == "appReference" || type == "desktopItemReference" ||
        type == "fileReference" || type == "folderReference";
}

bool IsFilesystemHandleSettingType(std::string_view type) noexcept
{
    return type == "fileHandle" || type == "folderHandle";
}

std::optional<WidgetFilesystemHandleAccess> FilesystemSettingAccess(
        std::string_view access) noexcept
{
    using Access = snowdesktop::widget_runtime::
        WidgetFilesystemHandleAccess;
    if (access == "read") return Access::Read;
    if (access == "write") return Access::Write;
    if (access == "readWrite") return Access::ReadWrite;
    return std::nullopt;
}

std::string FilesystemSettingMetadataKey(std::string_view key)
{
    return "__host.settingHandle." + std::string(key);
}

static std::string_view EntityReferenceSettingKind(
    std::string_view type) noexcept
{
    if (type == "appReference") return "app.reference";
    if (type == "desktopItemReference") return "desktop.item";
    if (type == "fileReference" || type == "folderReference")
        return "filesystem.reference";
    return {};
}

std::string_view EntityReferenceSettingTypeFilter(
    std::string_view type) noexcept
{
    if (type == "fileReference") return "file";
    if (type == "folderReference") return "folder";
    return {};
}

static bool ValidateEntityReferenceSettings(
    const std::vector<LuaWidgetManifest::Setting>& manifestSettings,
    const std::vector<LuaWidgetManifest::Setting>& scriptSettings,
    const snowdesktop::widget_runtime::LogicalSlotDeclarations& slots,
    std::string& error)
{
    error.clear();
    std::unordered_map<std::string, std::string> bindingOwners;
    const auto validate = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (!IsEntityReferenceSettingType(setting.type)) continue;
            if (setting.binding.empty())
            {
                error = setting.type + " setting '" + setting.key +
                    "' requires a binding";
                return false;
            }
            const std::string_view expectedKind =
                EntityReferenceSettingKind(setting.type);
            if (const auto [owner, inserted] = bindingOwners.emplace(
                    setting.binding, setting.key); !inserted)
            {
                error = "entity reference binding '" + setting.binding +
                    "' is already used by setting '" + owner->second + "'";
                return false;
            }
            const auto declaration = slots.find(setting.binding);
            if (declaration == slots.end() ||
                declaration->second.kind !=
                    snowdesktop::widget_runtime::LogicalSlotKind::Binding ||
                declaration->second.operation != "reference" ||
                declaration->second.replacePolicy != "allow" ||
                declaration->second.accepts.size() != 1 ||
                std::find(declaration->second.accepts.begin(),
                    declaration->second.accepts.end(), expectedKind) ==
                    declaration->second.accepts.end())
            {
                error = setting.type + " setting '" + setting.key +
                    "' must target a replaceable " +
                    std::string(expectedKind) + " binding slot";
                return false;
            }
        }
        return true;
    };
    return validate(manifestSettings) && validate(scriptSettings);
}

static bool ValidateSettingPresentation(
    const std::vector<LuaWidgetManifest::Setting>& manifestSettings,
    const std::vector<LuaWidgetManifest::Setting>& scriptSettings,
    const std::vector<LuaWidgetManifest::SettingGroup>& manifestGroups,
    const std::vector<LuaWidgetManifest::SettingGroup>& scriptGroups,
    std::string& error)
{
    error.clear();
    if (manifestGroups.size() + scriptGroups.size() > 32)
    {
        error = "settings accept at most 32 groups";
        return false;
    }
    std::unordered_set<std::string> groupIds;
    const auto validateGroups = [&](const auto& groups) {
        for (const auto& group : groups)
        {
            const bool safeId = snowdesktop::widget_runtime::
                IsValidSettingGroupId(group.id);
            if (!safeId || group.label.empty() ||
                group.label.size() > 512 ||
                group.description.size() > 4096)
            {
                error = "setting group declarations are invalid";
                return false;
            }
            if (!groupIds.emplace(group.id).second)
            {
                error = "setting group '" + group.id +
                    "' is declared more than once";
                return false;
            }
        }
        return true;
    };
    if (!validateGroups(manifestGroups) || !validateGroups(scriptGroups))
        return false;

    std::unordered_set<std::string> settingKeys;
    const auto validateSettings = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (!settingKeys.emplace(setting.key).second)
            {
                error = "setting key '" + setting.key +
                    "' is declared more than once";
                return false;
            }
            if (setting.description.size() > 2048)
            {
                error = "setting '" + setting.key +
                    "' description exceeds 2048 bytes";
                return false;
            }
            if (setting.agentDescription.size() > 2048)
            {
                error = "setting '" + setting.key +
                    "' agentDescription exceeds 2048 bytes";
                return false;
            }
            if (!setting.group.empty() && !groupIds.contains(setting.group))
            {
                error = "setting '" + setting.key +
                    "' references undeclared group '" + setting.group + "'";
                return false;
            }
        }
        return true;
    };
    return validateSettings(manifestSettings) &&
        validateSettings(scriptSettings);
}

static bool ValidateSettingConditionsAndConstraints(
    const std::vector<LuaWidgetManifest::Setting>& manifestSettings,
    const std::vector<LuaWidgetManifest::Setting>& scriptSettings,
    const std::vector<LuaWidgetManifest::SettingPreset>& scriptPresets,
    std::string& error)
{
    error.clear();
    if (manifestSettings.size() + scriptSettings.size() > 256)
    {
        error = "settings accept at most 256 fields";
        return false;
    }
    std::unordered_map<std::string, const LuaWidgetManifest::Setting*>
        declarations;
    for (const auto& setting : manifestSettings)
        declarations.emplace(setting.key, &setting);
    for (const auto& setting : scriptSettings)
        declarations.emplace(setting.key, &setting);

    std::unordered_map<std::string, std::vector<std::string>> edges;
    const auto validateCondition = [&](const std::string& ownerId,
        const LuaWidgetManifest::SettingCondition& condition, bool isField) {
        const std::string owner = (isField ? "setting '" : "preset '") + ownerId + "'";
        if (condition.key.empty() || (isField && condition.key == ownerId) ||
            !snowdesktop::widget_runtime::IsValidSettingCondition(
                condition.operation, condition.values.size()))
        {
            error = owner + " has an invalid condition";
            return false;
        }
        const auto source = declarations.find(condition.key);
        if (source == declarations.end())
        {
            error = owner + " condition references undeclared setting '" +
                condition.key + "'";
            return false;
        }
        const auto& sourceSetting = *source->second;
        const bool existenceOnly = condition.operation == "set" ||
            condition.operation == "unset" ||
            condition.operation == "truthy" ||
            condition.operation == "falsy";
        if (!existenceOnly &&
            (sourceSetting.type == "password" ||
                IsFilesystemHandleSettingType(sourceSetting.type) ||
                IsEntityReferenceSettingType(sourceSetting.type)))
        {
            error = owner + " may only test presence/truthiness of host-managed setting '" +
                condition.key + "'";
            return false;
        }
        const bool contains = condition.operation == "contains" ||
            condition.operation == "notContains";
        if (contains != (sourceSetting.type == "multiSelect"))
        {
            if (contains || (!existenceOnly &&
                    sourceSetting.type == "multiSelect"))
            {
                error = owner + " uses an incompatible condition for '" +
                    condition.key + "'";
                return false;
            }
        }
        if ((sourceSetting.type == "select" ||
                sourceSetting.type == "multiSelect") && !existenceOnly)
        {
            for (const auto& expected : condition.values)
            {
                if (std::find(sourceSetting.options.begin(),
                        sourceSetting.options.end(), expected) ==
                    sourceSetting.options.end())
                {
                    error = owner + " condition uses an undeclared option from '" +
                        condition.key + "'";
                    return false;
                }
            }
        }
        if (sourceSetting.type == "bool" && !existenceOnly)
        {
            for (const auto& expected : condition.values)
            {
                if (expected != "0" && expected != "1")
                {
                    error = owner + " boolean condition values must be booleans";
                    return false;
                }
            }
        }
        if ((sourceSetting.type == "range" || sourceSetting.type == "duration") && !existenceOnly)
        {
            for (const auto& expected : condition.values)
            {
                double number = 0.0;
                if (!snowdesktop::widget_runtime::ParseFiniteSettingNumber(
                        expected, number) || number < sourceSetting.minValue ||
                    number > sourceSetting.maxValue ||
                    (sourceSetting.type == "duration" &&
                        !snowdesktop::widget_runtime::IsValidDurationSettingValue(number,
                            sourceSetting.durationUnit, sourceSetting.minValue, sourceSetting.maxValue, sourceSetting.stepValue)))
                {
                    error = owner + " numeric condition is outside the source bounds";
                    return false;
                }
            }
        }
        if (isField) edges[ownerId].push_back(condition.key);
        return true;
    };

    for (const auto& [key, settingPtr] : declarations)
    {
        (void)key;
        const auto& setting = *settingPtr;
        const bool hasConstraints = setting.required ||
            setting.minLength != -1 || setting.maxLength != -1;
        const bool textType = setting.type == "text" ||
            setting.type == "url" || setting.type == "date" ||
            setting.type == "time";
        if (setting.minLength < -1 || setting.maxLength < -1 ||
            setting.minLength > 2048 || setting.maxLength > 2048 ||
            (setting.minLength >= 0 && setting.maxLength >= 0 &&
                setting.minLength > setting.maxLength) ||
            ((!textType) &&
                (setting.minLength != -1 || setting.maxLength != -1)) ||
            setting.validationMessage.size() > 2048 ||
            (hasConstraints && setting.validationMessage.empty()))
        {
            error = "setting '" + setting.key +
                "' has invalid validation constraints";
            return false;
        }
        if (!setting.dependsOn.empty() && setting.enabledWhen)
        {
            error = "setting '" + setting.key +
                "' cannot declare both dependsOn and enabledWhen";
            return false;
        }
        if (setting.showWhen &&
            !validateCondition(setting.key, *setting.showWhen, true)) return false;
        if (setting.enabledWhen &&
            !validateCondition(setting.key, *setting.enabledWhen, true)) return false;
        if (!setting.dependsOn.empty())
        {
            LuaWidgetManifest::SettingCondition dependency;
            dependency.key = setting.dependsOn;
            dependency.operation = "truthy";
            if (!validateCondition(setting.key, dependency, true)) return false;
        }
    }
    for (const auto& preset : scriptPresets)
        if (preset.showWhen &&
            !validateCondition(preset.id, *preset.showWhen, false)) return false;

    std::unordered_map<std::string, int> state;
    const std::function<bool(const std::string&)> visit =
        [&](const std::string& key) {
            int& current = state[key];
            if (current == 2) return true;
            if (current == 1)
            {
                error = "setting conditions contain a dependency cycle at '" +
                    key + "'";
                return false;
            }
            current = 1;
            if (const auto found = edges.find(key); found != edges.end())
                for (const auto& dependency : found->second)
                    if (!visit(dependency)) return false;
            current = 2;
            return true;
        };
    for (const auto& [key, _] : declarations)
        if (!visit(key)) return false;
    return true;
}

static bool ValidateOrdinaryDeclarativeSettings(
    const std::vector<LuaWidgetManifest::Setting>& manifestSettings,
    const std::vector<LuaWidgetManifest::Setting>& scriptSettings,
    const std::vector<LuaWidgetManifest::SettingPreset>& manifestPresets,
    const std::vector<LuaWidgetManifest::SettingPreset>& scriptPresets,
    std::string& error)
{
    using snowdesktop::widget_runtime::IsValidDateSettingValue;
    using snowdesktop::widget_runtime::IsValidMultiSelectSettingValue;
    using snowdesktop::widget_runtime::IsValidTimeSettingValue;
    using snowdesktop::widget_runtime::IsValidUrlSettingValue;
    using snowdesktop::widget_runtime::ParseFiniteSettingNumber;

    error.clear();
    std::unordered_map<std::string, const LuaWidgetManifest::Setting*>
        typedSettings;
    const auto validateSettings = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            const bool typed = setting.type == "url" ||
                setting.type == "date" || setting.type == "time" ||
                setting.type == "range" || setting.type == "duration" || setting.type == "multiSelect" ||
                IsFilesystemHandleSettingType(setting.type);
            if (!typed) continue;
            if (!typedSettings.emplace(setting.key, &setting).second)
            {
                error = "typed setting key '" + setting.key +
                    "' is declared more than once";
                return false;
            }
            if (setting.type != "multiSelect" &&
                !setting.defaultValues.empty())
            {
                error = setting.type + " setting '" + setting.key +
                    "' does not accept an array default";
                return false;
            }
            if (IsFilesystemHandleSettingType(setting.type))
            {
                std::vector<std::string> normalizedExtensions;
                if (!snowdesktop::widget_runtime::
                        IsValidFilesystemSettingAccess(setting.access) ||
                    (setting.type == "folderHandle" &&
                        !setting.extensions.empty()) ||
                    (setting.type == "fileHandle" &&
                        !snowdesktop::widget_runtime::
                            NormalizeFilesystemSettingExtensions(
                                setting.extensions,
                                normalizedExtensions)))
                {
                    error = setting.type + " setting '" + setting.key +
                        "' has invalid access or extensions";
                    return false;
                }
                continue;
            }
            if (setting.type == "url" &&
                !IsValidUrlSettingValue(setting.defaultValue))
            {
                error = "url setting '" + setting.key +
                    "' has an invalid HTTP(S) default";
                return false;
            }
            if (setting.type == "date" &&
                !IsValidDateSettingValue(setting.defaultValue))
            {
                error = "date setting '" + setting.key +
                    "' must use YYYY-MM-DD";
                return false;
            }
            if (setting.type == "time" &&
                !IsValidTimeSettingValue(setting.defaultValue))
            {
                error = "time setting '" + setting.key +
                    "' must use HH:MM";
                return false;
            }
            if (setting.type == "range")
            {
                if (!std::isfinite(setting.minValue) ||
                    !std::isfinite(setting.maxValue) ||
                    !std::isfinite(setting.stepValue) ||
                    setting.minValue > setting.maxValue ||
                    setting.stepValue <= 0.0)
                {
                    error = "range setting '" + setting.key +
                        "' requires finite min <= max and step > 0";
                    return false;
                }
                if (!setting.defaultValue.empty())
                {
                    double value = 0.0;
                    if (!ParseFiniteSettingNumber(
                            setting.defaultValue, value) ||
                        value < setting.minValue || value > setting.maxValue)
                    {
                        error = "range setting '" + setting.key +
                            "' has an out-of-range default";
                        return false;
                    }
                }
            }
            if (setting.type == "duration")
            {
                double number = 0;
                if (!ParseFiniteSettingNumber(setting.defaultValue, number) ||
                    !snowdesktop::widget_runtime::IsValidDurationSettingValue(number,
                        setting.durationUnit, setting.minValue, setting.maxValue, setting.stepValue))
                {
                    error = "duration setting '" + setting.key + "' requires an integer default, valid unit, bounds and whole-unit step";
                    return false;
                }
            }
            if (setting.type == "multiSelect" &&
                (!setting.defaultValue.empty() ||
                    !IsValidMultiSelectSettingValue(
                    setting.options, setting.defaultValues) ||
                    (!setting.optionLabels.empty() &&
                        setting.optionLabels.size() !=
                            setting.options.size())))
            {
                error = "multiSelect setting '" + setting.key +
                    "' requires 1-64 unique options, matching labels, and valid defaults";
                return false;
            }
        }
        return true;
    };
    if (!validateSettings(manifestSettings) ||
        !validateSettings(scriptSettings))
        return false;

    const auto validatePresets = [&](const auto& presets) {
        for (const auto& preset : presets)
        {
            for (const auto& [key, value] : preset.values)
            {
                const auto declared = typedSettings.find(key);
                if (declared == typedSettings.end()) continue;
                const auto& setting = *declared->second;
                if (IsFilesystemHandleSettingType(setting.type))
                {
                    error = "preset '" + preset.id +
                        "' cannot assign host-managed setting '" + key + "'";
                    return false;
                }
                if (setting.type == "multiSelect")
                {
                    error = "preset '" + preset.id +
                        "' must use an array for multiSelect setting '" + key + "'";
                    return false;
                }
                if ((setting.type == "url" &&
                        !IsValidUrlSettingValue(value)) ||
                    (setting.type == "date" &&
                        !IsValidDateSettingValue(value)) ||
                    (setting.type == "time" &&
                        !IsValidTimeSettingValue(value)))
                {
                    error = "preset '" + preset.id +
                        "' has an invalid value for setting '" + key + "'";
                    return false;
                }
                if (setting.type == "duration")
                {
                    double number = 0;
                    if (!ParseFiniteSettingNumber(value, number) ||
                        !snowdesktop::widget_runtime::IsValidDurationSettingValue(number,
                            setting.durationUnit, setting.minValue, setting.maxValue, setting.stepValue))
                    {
                        error = "preset '" + preset.id + "' has an invalid duration for setting '" + key + "'";
                        return false;
                    }
                }
                if (setting.type == "range")
                {
                    double number = 0.0;
                    if (!ParseFiniteSettingNumber(value, number) ||
                        number < setting.minValue ||
                        number > setting.maxValue)
                    {
                        error = "preset '" + preset.id +
                            "' has an out-of-range value for setting '" + key + "'";
                        return false;
                    }
                }
            }
            for (const auto& [key, values] : preset.arrayValues)
            {
                const auto declared = typedSettings.find(key);
                if (declared == typedSettings.end() ||
                    declared->second->type != "multiSelect" ||
                    !IsValidMultiSelectSettingValue(
                        declared->second->options, values))
                {
                    error = "preset '" + preset.id +
                        "' has an invalid array value for setting '" + key + "'";
                    return false;
                }
            }
        }
        return true;
    };
    return validatePresets(manifestPresets) &&
        validatePresets(scriptPresets);
}

bool ValidateWidgetSettings(const LuaWidgetManifest& manifest,
    const std::vector<LuaWidgetManifest::Setting>& settings,
    const std::vector<LuaWidgetManifest::SettingGroup>& groups,
    const std::vector<LuaWidgetManifest::SettingPreset>& presets,
    std::string& error)
{
    return ValidateEntityReferenceSettings(manifest.settings, settings,
            manifest.logicalSlots, error) &&
        ValidateSettingPresentation(manifest.settings, settings,
            manifest.settingGroups, groups, error) &&
        ValidateSettingConditionsAndConstraints(
            manifest.settings, settings, presets, error) &&
        ValidateOrdinaryDeclarativeSettings(manifest.settings, settings,
            manifest.presets, presets, error);
}
}
