#pragma once

#include "widget/packages/widget_script_manifest.h"
#include "widget/tasks/widget_filesystem_handle_store.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace snowdesktop::widget_runtime
{
bool IsRemovedPanelEffectSettingKey(const std::string& key);
bool IsHostStructureSettingKey(const std::string& key);
bool IsHostAppearanceSettingKey(const std::string& key);
std::string LuaReadStorageField(lua_State* L, int tableIndex, const char* key);
void ReadLuaDeclaredSettings(lua_State* L, int widgetTableIndex,
    std::vector<LuaWidgetManifest::Setting>& settings,
    std::vector<LuaWidgetManifest::SettingGroup>& groups,
    std::vector<LuaWidgetManifest::SettingPreset>& presets);
bool IsEntityReferenceSettingType(std::string_view type) noexcept;
bool IsFilesystemHandleSettingType(std::string_view type) noexcept;
std::optional<WidgetFilesystemHandleAccess> FilesystemSettingAccess(
        std::string_view access) noexcept;
std::string FilesystemSettingMetadataKey(std::string_view key);
std::string_view EntityReferenceSettingTypeFilter(
    std::string_view type) noexcept;

bool ValidateWidgetSettings(const LuaWidgetManifest& manifest,
    const std::vector<LuaWidgetManifest::Setting>& settings,
    const std::vector<LuaWidgetManifest::SettingGroup>& groups,
    const std::vector<LuaWidgetManifest::SettingPreset>& presets,
    std::string& error);
}
