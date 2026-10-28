#include "ui/render/app_font.h"
#include "app/app.h"
#include "ui/render/file_search_result_row.h"

// Native rename editor creation.

void DesktopApp::BeginRenameFolderEntry(size_t widgetIndex, size_t memberIndex)
{
    if (renameController_.IsActive() ||
        widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return;

    ClearSelection();
    widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
    renameCommitPending_ = false;
    renameController_.BeginFolderEntry(
        widgetIndex, memberIndex);

    const size_t owner = ResolveRenameVisibilityWidgetIndex(widgetIndex);
    BeginFolderEntryRenameEditor(widgets_[widgetIndex].folderEntries[memberIndex],
        GetFolderEntryRenameRect(widgetIndex, memberIndex), owner,
        owner < widgets_.size() && widgets_[owner].listMode);
}

void DesktopApp::BeginRenameSearchResult(const std::wstring& path)
{
    if (renameController_.IsActive() || path.empty()) return;
    for (const auto& container : containers_)
    {
        auto* view = dynamic_cast<ScrollingItemWidget*>(container.get());
        if (!view || !view->UsesFolderSearch()) continue;
        const RECT viewport = view->GetContentViewportRect();
        for (const auto& slot : view->GetSlots())
        {
            const auto* item = slot->GetItem();
            const RECT bounds = slot->GetBounds();
            if (!item || !item->IsSelected() || item->GetPath() != path ||
                !RectsIntersect(bounds, viewport)) continue;
            FolderEntry entry;
            entry.fullPath = path;
            entry.name = PathFindFileNameW(path.c_str());
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return;
            entry.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            RECT rect = view->SingleColumn() ? view->GetListItemTextRect(bounds)
                : GetItemRenameRect(bounds, ResolveItemTitleLines(view->GetWidgetData()));
            if (item->IsSearchResult())
            {
                namespace row = snowdesktop::file_search_result_row;
                rect = row::Resolve(bounds, {view->Cu(28.0f), view->Cu(12.0f), view->Cu(10.0f),
                    view->Cu(6.0f), view->Cu(19.0f), view->Cu(5.0f), view->Cu(118.0f),
                    view->Cu(156.0f), view->Cu(8.0f)}, false).name;
            }
            IntersectRect(&rect, &rect, &viewport);
            renameCommitPending_ = false;
            renameController_.BeginSearchResult(path);
            BeginFolderEntryRenameEditor(entry, rect,
                FindWidgetIndexById(view->GetWidgetData()->id), item->IsSearchResult() || view->SingleColumn());
            return;
        }
    }
}

void DesktopApp::BeginFolderEntryRenameEditor(
    const FolderEntry& entry, RECT rect, size_t owner, bool listMode)
{
    if (IsRectEmptyRect(rect))
    {
        renameController_.Reset();
        return;
    }
    const float renameScale = GetGridCuScaleForBounds(gridPages_, rect);
    const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
    InflateRect(&rect, renameMargin, 0);
    RECT screenRect = rect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    const DWORD style = snowdesktop::rename_edit_layout::EditStyle(listMode, !listMode);
    renameInputWindow_ = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        snowdesktop::text_input::WindowClass(), entry.DisplayName().c_str(), style,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);

    if (!renameInputWindow_)
    {
        renameController_.Reset();
        return;
    }

    if (renameFont_) DeleteObject(renameFont_);
    renameFont_ = CreateFontW(-std::max(1, static_cast<int>(std::round(
        ScaleWidgetFontCu(itemFontSizeCu_, renameScale)))),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, snowdesktop::app_fonts::GdiFamily().c_str());
    SendMessageW(renameInputWindow_, WM_SETFONT,
        reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(renameInputWindow_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(renameMargin, renameMargin));
    SetWindowSubclass(renameInputWindow_, &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    snowdesktop::text_input::SetAccessibleName(renameInputWindow_, _LW("app.menu.rename"));
    snowdesktop::text_input::SetPadding(renameInputWindow_,
        static_cast<float>(renameMargin), 4.0f * renameScale);
    renameEditLayout_.Begin(renameInputWindow_);
    SetWindowPos(renameInputWindow_, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SendMessageW(renameInputWindow_, EM_SETSEL, 0,
        RenameInitialSelectionEnd(
            entry.DisplayName(),
            entry.fullPath,
            entry.isDirectory));
    SetFocus(renameInputWindow_);
    const size_t visibilityWidgetIndex = owner;
    if (visibilityWidgetIndex < widgets_.size())
    {
        interactionPinnedWidgetId_ =
            widgets_[visibilityWidgetIndex].id;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool DesktopApp::BeginDockAnchoredRename(
    const std::wstring& text, RECT anchorClient,
    int selectionEnd)
{
    RECT anchorScreen = anchorClient;
    MapWindowPoints(hwnd_, nullptr,
        reinterpret_cast<POINT*>(&anchorScreen), 2);
    const POINT anchorCenter{
        (anchorScreen.left + anchorScreen.right) / 2,
        (anchorScreen.top + anchorScreen.bottom) / 2
    };
    const HMONITOR monitor = MonitorFromPoint(
        anchorCenter, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        monitorInfo.rcWork = {
            virtualLeft_, virtualTop_,
            virtualLeft_ + virtualWidth_,
            virtualTop_ + virtualHeight_
        };
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI,
            &dpiX, &dpiY)))
        dpiX = 96;
    const int desiredWidth =
        std::max(150, MulDiv(180, static_cast<int>(dpiX), 96));
    const int desiredHeight =
        std::max(26, MulDiv(30, static_cast<int>(dpiX), 96));
    const int gap =
        std::max(3, MulDiv(6, static_cast<int>(dpiX), 96));
    const int monitorMargin =
        std::max(3, MulDiv(5, static_cast<int>(dpiX), 96));
    const RECT screenRect =
        snowdesktop::dock_rename_layout::
            CalculateAdjacentEditRect(
                anchorScreen, monitorInfo.rcWork,
                dockSettings_.position,
                desiredWidth, desiredHeight,
                gap, monitorMargin);

    renameInputWindow_ = CreateWindowExW(
         WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        snowdesktop::text_input::WindowClass(), text.c_str(),
        snowdesktop::rename_edit_layout::EditStyle(),
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left,
        screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!renameInputWindow_)
        return false;

    if (renameFont_)
        DeleteObject(renameFont_);
    const int fontHeight = std::max(
        12, MulDiv(
            static_cast<int>(std::round(itemFontSizeCu_)),
            static_cast<int>(dpiX), 96));
    renameFont_ = CreateFontW(
        -fontHeight, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, snowdesktop::app_fonts::GdiFamily().c_str());
    SendMessageW(renameInputWindow_, WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_ ? renameFont_
                : GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
    const int editMargin = std::max(
        3, MulDiv(5, static_cast<int>(dpiX), 96));
    SendMessageW(renameInputWindow_, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(editMargin, editMargin));
    SetWindowSubclass(renameInputWindow_,
        &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    using snowdesktop::rename_edit_layout::HeightAnchor;
    const auto heightAnchor = dockSettings_.position == DockPosition::Bottom
        ? HeightAnchor::Bottom
        : dockSettings_.position == DockPosition::Top
            ? HeightAnchor::Top : HeightAnchor::Center;
    snowdesktop::text_input::SetAccessibleName(renameInputWindow_, _LW("app.menu.rename"));
    renameEditLayout_.Begin(renameInputWindow_, heightAnchor);
    SetWindowPos(renameInputWindow_, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SendMessageW(renameInputWindow_, EM_SETSEL,
        0, selectionEnd);
    SetFocus(renameInputWindow_);
    return true;
}



void DesktopApp::
BeginRenameDockFolderPopupEntry(
    size_t memberIndex)
{
    if (renameController_.IsActive() ||
        !dockFolderPopupOpen_ ||
        memberIndex >=
            dockFolderPopupWidget_.
                folderEntries.size())
        return;

    ShowAllCollectionPopupItems();
    // memberIndex addresses the source folder; geometry addresses the
    // filtered visible order. A hidden entry has no rename anchor.
    const size_t visibleCount = GetPopupItemCount(dockFolderPopupWidget_);
    size_t visibleIndex = 0;
    while (visibleIndex < visibleCount &&
        GetPopupFolderEntryIndex(dockFolderPopupWidget_, visibleIndex) != memberIndex)
        ++visibleIndex;
    if (visibleIndex == visibleCount) return;

    ClearSelection();
    for (auto& entry :
         dockFolderPopupWidget_.folderEntries)
        entry.selected = false;
    FolderEntry& entry =
        dockFolderPopupWidget_.
            folderEntries[memberIndex];
    entry.selected = true;
    renameCommitPending_ = false;
    renameController_.BeginDockFolderEntry(
        memberIndex);

    const RECT popup =
        GetCollectionPopupRect(
            dockFolderPopupWidget_);
    RECT itemRect = GetCollectionPopupItemRect(popup, visibleIndex);
    if (!UsesCollectionPopupFan(dockFolderPopupWidget_))
    {
        const RECT content = GetCollectionPopupContentRect(popup);
        if (itemRect.top < content.top)
            popupScrollOffset_ -= content.top - itemRect.top;
        else if (itemRect.bottom > content.bottom)
            popupScrollOffset_ += itemRect.bottom - content.bottom;
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
            GetCollectionPopupMaxScrollOffset(dockFolderPopupWidget_, popup));
        itemRect = GetCollectionPopupItemRect(popup, visibleIndex);
    }
    RECT rect =
        UsesCollectionPopupList(dockFolderPopupWidget_)
            ? GetCollectionPopupItemTextRect(itemRect)
            : GetItemRenameRect(itemRect, ResolveItemTitleLines(&dockFolderPopupWidget_));
    if (IsRectEmptyRect(rect))
    {
        renameController_.Reset();
        return;
    }
    const float renameScale = GetGridCuScaleForBounds(gridPages_, itemRect);
    const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
    InflateRect(&rect, renameMargin, 0);
    RECT screenRect = rect;
    MapWindowPoints(
        hwnd_, nullptr,
        reinterpret_cast<POINT*>(
            &screenRect), 2);

    const DWORD style = snowdesktop::rename_edit_layout::EditStyle(
        UsesCollectionPopupList(dockFolderPopupWidget_),
        !UsesCollectionPopupList(dockFolderPopupWidget_));
    renameInputWindow_ = CreateWindowExW(
            WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        snowdesktop::text_input::WindowClass(), entry.DisplayName().c_str(),
        style,
        screenRect.left,
        screenRect.top,
        screenRect.right -
            screenRect.left,
        screenRect.bottom -
            screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!renameInputWindow_)
    {
        renameController_.Reset();
        return;
    }

    if (renameFont_)
        DeleteObject(renameFont_);
    renameFont_ = CreateFontW(
        -std::max(
            1, static_cast<int>(
                std::round(
                    ScaleWidgetFontCu(
                        itemFontSizeCu_, renameScale)))),
        0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        snowdesktop::app_fonts::GdiFamily().c_str());
    SendMessageW(
        renameInputWindow_, WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_
                ? renameFont_
                : GetStockObject(
                    DEFAULT_GUI_FONT)),
        TRUE);
    SendMessageW(
        renameInputWindow_, EM_SETMARGINS,
        EC_LEFTMARGIN |
            EC_RIGHTMARGIN,
        MAKELPARAM(
            renameMargin,
            renameMargin));
    SetWindowSubclass(
        renameInputWindow_,
        &DesktopApp::
            RenameEditSubclassProc,
        1,
        reinterpret_cast<DWORD_PTR>(
            this));
    snowdesktop::text_input::SetAccessibleName(renameInputWindow_, _LW("app.menu.rename"));
    snowdesktop::text_input::SetPadding(renameInputWindow_,
        static_cast<float>(renameMargin), 4.0f * renameScale);
    renameEditLayout_.Begin(renameInputWindow_);
    SetWindowPos(renameInputWindow_, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SendMessageW(
        renameInputWindow_, EM_SETSEL, 0,
        RenameInitialSelectionEnd(
            entry.DisplayName(),
            entry.fullPath,
            entry.isDirectory));
    SetFocus(renameInputWindow_);
    InvalidateCollectionPopupContent();
    InvalidateFloatingPopupWindow(false);
}

LRESULT CALLBACK DesktopApp::RenameEditSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message)
    {
    case WM_MOUSEWHEEL:
        if (app->renameController_.BlocksScrolling())
            return 0;
        break;
    case WM_ACTIVATE:
        if (snowdesktop::text_input::HasEditingMenu(hwnd))
            break;
        if (app->renameController_.
                IsQuickNavigationPresentation() &&
            LOWORD(wParam) == WA_INACTIVE)
        {
            const HWND activatedWindow =
                reinterpret_cast<HWND>(lParam);
            const bool remainsInQuickNavigation =
                activatedWindow ==
                    app->quickNavigationHwnd_ ||
                activatedWindow ==
                    app->quickNavigationSearchEdit_ ||
                app->quickNavBackdropCompositor_.
                    IsBackdropWindow(
                        activatedWindow);
            if (!remainsInQuickNavigation)
            {
                app->CommitRename(false);
                app->CloseQuickNavigation();
                return 0;
            }
        }
        break;
    case WM_KEYDOWN:
        if (snowdesktop::text_input::IsComposing(hwnd))
            break;
        if (wParam == VK_RETURN) { app->CommitRename(false); return 0; }
        if (wParam == VK_ESCAPE) { app->CommitRename(true); return 0; }
        break;
    case WM_KILLFOCUS:
        if (snowdesktop::text_input::HasEditingMenu(hwnd))
            return 0;
        snowdesktop::text_input::CompleteComposition(hwnd);
        if (!app->renameCommitPending_)
        {
            app->renameCommitPending_ = true;
            if (!PostMessageW(app->hwnd_, kCommitRenameMessage, FALSE,
                    static_cast<LPARAM>(
                        app->renameController_.SessionId())))
            {
                app->renameCommitPending_ = false;
                app->CommitRename(false);
            }
        }
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

/** @brief 将 Lua 内联编辑框当前内容实时写回小部件存储。 */
