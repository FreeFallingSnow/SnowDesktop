#pragma once

#include "control_settings.h"
#include <functional>
#include <string>
#include <vector>

namespace snowdesktop::control_settings
{
// Native storage settings are separate from Lua's declarative package schema.
// IDs identify the visible owner and the real child that owns its content view.
struct NativeWidgetConfiguration
{
    std::string title;
    bool showTitle = false;
    bool bottomBarHover = true;
    bool showOnHoverOnly = false;
    bool keepWhenDesktopHidden = false;
    bool privacyMode = false;
    bool singleCellIconMode = false;
    bool scrollContainerMode = false;
    bool largeFolderTitleless = false;
    bool titleBarCollapsed = false;
    bool titleBarExpandOnHover = false;
    bool showSearchBox = false;
    std::string demoIconCategory;
    bool listMode = false;
    int listColumns = 1;
    bool detailShowModified = false;
    bool detailShowType = false;
    bool detailShowSize = false;
    float detailModifiedPosition = 140.0f / 510.0f;
    float detailTypePosition = 300.0f / 510.0f;
    float detailSizePosition = 420.0f / 510.0f;
    bool dateHeaders = false;
    bool showFileCategories = false;
    bool fanPopup = false;
    std::string sortColumn = "none";
    bool sortAscending = true;
    bool autoCollect = false;
    std::vector<std::string> autoCollectCategoryIds;
    std::string activeCategoryId;
    std::string contentCategoryId;
    std::vector<std::string> categoryTabOrder;
};

struct NativeWidgetSettingsState
{
    std::string id;
    std::string type;
    std::string contentTargetId;
    std::string sourceTargetId;
    std::string sortTargetId;
    std::string activeChildId;
    std::string ownershipRevision;
    NativeWidgetConfiguration values;
    NativeWidgetConfiguration defaults;
    std::vector<std::string> editableKeys;
    std::vector<std::string> categoryIds;
    std::vector<std::string> activeCategoryIds;
    std::vector<std::string> contentCategoryIds;
    std::vector<std::string> categoryTabIds;
    std::vector<std::string> childIds;
    std::vector<std::string> demoIconCategories;
    control::Value placement;
    std::string sourceFolderPath;
    std::string customTitle;
    std::string searchText;
    bool editable = false;
    bool searchAvailable = false;
    bool collectAvailable = false;
};

struct NativeWidgetSettingsPatch
{
    NativeWidgetConfiguration values;
    std::vector<std::string> keys;
    bool collectNow = false;
    std::vector<std::string> reorderedChildIds;
};

struct NativeWidgetSettingsContext
{
    std::function<NativeWidgetSettingsState(std::string_view)> capture;
    std::function<bool(const NativeWidgetSettingsState&, const NativeWidgetSettingsPatch&)> apply;
    std::function<bool(std::string_view, const std::string&)> setSearch;
};

control::Value NativeWidgetConfigurationJson(const NativeWidgetConfiguration& values);
std::string NativeWidgetSettingsRevision(const NativeWidgetSettingsState& state);
NativeWidgetSettingsPatch ParseNativeWidgetSettingsPatch(const NativeWidgetSettingsState& state,
    const control::Value& changes);
void ValidateNativeWidgetSettingsPatch(const NativeWidgetSettingsState& state,
    const NativeWidgetSettingsPatch& patch);
control::Value HandleNativeWidgetSettings(std::string_view method, const control::Value& params,
    const Target& target, const NativeWidgetSettingsContext& context);
}
