#pragma once

#include "common/types.h"
#include "core/widget.h"
#include "data/folder_preview_view_state.h"
#include "layout/collection_popup_types.h"
#include "app/popup/popup_resize_transition.h"

#include <algorithm>
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
    std::wstring viewKey;
    std::wstring sourceWidgetId;
    std::wstring hostWidgetId;
    std::vector<ViewPage> pages;
    std::optional<RECT> inlineBounds;
    RECT inlineCollapseButton{};
    RECT inlineResizeHandle{};
    RECT inlineTitleBar{};
    RECT inlineTitle{};
    RECT inlineExplorerButton{};
    LONG inlineRowTop = 0;
    LONG inlineContentBottom = 0;
    float inlineScale = 1.0f;
    bool keyboardFocused = true;
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

    bool IsTemporary() const { return viewKey.empty(); }

    void SetInlineGeometry(const WidgetContainer& host)
    {
        inlineBounds = host.GetFrameRect();
        inlineScale = host.GetCellScale();
        inlineCollapseButton = host.GetCollapseButtonRect();
        inlineResizeHandle = host.GetResizeHandleRect();
        inlineTitleBar = host.GetMoveHandleRect();
        inlineTitle = host.GetTitleRect();
        inlineRowTop = host.UsesTopTitleBar() ? inlineTitleBar.bottom : inlineBounds->top;
        inlineContentBottom = host.UsesTopTitleBar() ? inlineBounds->bottom : inlineTitleBar.top;
        if (!IsRectEmpty(&inlineResizeHandle))
            inlineContentBottom = std::min(inlineContentBottom, inlineResizeHandle.top);
        inlineExplorerButton = {};
        for (const auto& control : host.GetControlRegions())
            if (control.hit == WidgetHit::OpenFolderBtn)
            {
                inlineExplorerButton = control.bounds;
                break;
            }
    }
};

// Inline views keep their own directory model while another surface owns input.
// The native FolderMapping is bound to the projected popup model when drawn.
struct InlinePreview
{
    std::optional<Session> session;
    FolderPopupHome model;
    std::wstring pageId;
    RECT bounds{};
    int scroll = 0;
    bool open = false;
};
}
