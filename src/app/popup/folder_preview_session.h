#pragma once

#include "common/types.h"
#include "core/widget.h"
#include "layout/collection_popup_types.h"
#include "app/popup/popup_resize_transition.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::folder_preview
{
struct NavigationRow;
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

struct Breadcrumb
{
    RECT bounds;
    std::wstring path;
    std::wstring label;
    std::vector<std::wstring> hiddenParents;
};

struct Session
{
    struct Page
    {
        std::wstring path;
        int scroll = 0;
    };
    std::wstring sourceWidgetId;
    std::wstring hostWidgetId;
    std::vector<Page> pages;
    std::optional<RECT> inlineBounds;
    RECT inlineCollapseButton{};
    RECT inlineResizeHandle{};
    RECT inlineTitleBar{};
    RECT inlineExplorerButton{};
    LONG inlineRowTop = 0;
    LONG inlineContentBottom = 0;
    bool inlineListMode = false;
    bool inlineDateHeaders = false;
    std::optional<FolderPopupHome> folderHome;
    std::optional<popup_resize_transition::Geometry> pendingPopupResize;
    RECT homeBounds{};
    int homeScroll = 0;
    collection_popup_layout::FanScrollState homeFanScroll;
    bool homeFanShowAll = false;
    bool homeFanActionFocused = false;
    std::vector<std::wstring> selectedDesktopKeys;
    std::vector<std::pair<std::wstring, std::wstring>> selectedFolderPaths;
};
}
