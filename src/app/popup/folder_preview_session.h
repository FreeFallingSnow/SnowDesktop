#pragma once

#include "common/types.h"
#include "core/widget.h"
#include "layout/collection_popup_types.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::folder_preview
{
inline constexpr wchar_t kResolveWorkKey[] = L"folder-preview:resolve";

inline bool Supports(DesktopWidgetType type)
{
    return type == DesktopWidgetType::FileCategories || type == DesktopWidgetType::FolderMapping;
}

struct FolderPopupHome
{
    DesktopWidget data;
    std::unique_ptr<FolderMapping> view;
    std::wstring dockSourceId;
    std::wstring mappingId;
    size_t knownCount = 0;
    bool available = false;
    bool loading = false;
};

struct Session
{
    std::wstring sourceWidgetId;
    std::wstring hostWidgetId;
    std::vector<std::wstring> paths;
    std::optional<RECT> inlineBounds;
    std::optional<FolderPopupHome> folderHome;
    RECT homeBounds{};
    bool homeWasFan = false;
    int homeScroll = 0;
    collection_popup_layout::FanScrollState homeFanScroll;
    bool homeFanShowAll = false;
    bool homeFanActionFocused = false;
    std::vector<std::wstring> selectedDesktopKeys;
    std::vector<std::pair<std::wstring, std::wstring>> selectedFolderPaths;
};
}
