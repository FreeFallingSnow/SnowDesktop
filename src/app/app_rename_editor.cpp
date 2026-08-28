#include "app.h"

// Native rename editor creation.

void DesktopApp::BeginRenameFolderEntry(size_t widgetIndex, size_t memberIndex)
{
    if (renameEdit_ != nullptr ||
        widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return;

    ClearSelection();
    widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
    renameCommitPending_ = false;
    renameController_.BeginFolderEntry(
        widgetIndex, memberIndex);

    RECT rect = GetFolderEntryRenameRect(widgetIndex, memberIndex);
    if (IsRectEmptyRect(rect))
    {
        renameController_.Reset();
        return;
    }
    InflateRect(&rect, 2, 2);
    RECT screenRect = rect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    DWORD style = WS_POPUP | WS_VISIBLE | ES_AUTOVSCROLL;
    style |= widgets_[widgetIndex].listMode ? ES_LEFT : (ES_MULTILINE | ES_CENTER | ES_WANTRETURN);
    renameEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", widgets_[widgetIndex].folderEntries[memberIndex].name.c_str(), style,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);

    if (!renameEdit_)
    {
        renameController_.Reset();
        return;
    }

    if (renameFont_) DeleteObject(renameFont_);
    const float renameScale = GetGridCuScaleForBounds(
        gridPages_, rect);
    renameFont_ = CreateFontW(-std::max(1, static_cast<int>(std::round(
        ScaleWidgetFontCu(itemFontSizeCu_, renameScale)))),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
    SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(renameMargin, renameMargin));
    SetWindowSubclass(renameEdit_, &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(renameEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    SendMessageW(renameEdit_, EM_SETSEL, 0,
        RenameInitialSelectionEnd(
            widgets_[widgetIndex].folderEntries[memberIndex].name,
            widgets_[widgetIndex].folderEntries[memberIndex].isDirectory));
    SetFocus(renameEdit_);
    const size_t visibilityWidgetIndex =
        ResolveRenameVisibilityWidgetIndex(widgetIndex);
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

    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", text.c_str(),
        WS_POPUP | WS_VISIBLE |
            ES_CENTER | ES_AUTOHSCROLL,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left,
        screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!renameEdit_)
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
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_ ? renameFont_
                : GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
    const int editMargin = std::max(
        3, MulDiv(5, static_cast<int>(dpiX), 96));
    SendMessageW(renameEdit_, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(editMargin, editMargin));
    SetWindowSubclass(renameEdit_,
        &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(renameEdit_, HWND_TOPMOST,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left,
        screenRect.bottom - screenRect.top,
        SWP_SHOWWINDOW);
    SendMessageW(renameEdit_, EM_SETSEL,
        0, selectionEnd);
    SetFocus(renameEdit_);
    return true;
}



void DesktopApp::
BeginRenameDockFolderPopupEntry(
    size_t memberIndex)
{
    if (renameEdit_ != nullptr ||
        !dockFolderPopupOpen_ ||
        memberIndex >=
            dockFolderPopupWidget_.
                folderEntries.size())
        return;

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
    RECT itemRect =
        GetCollectionPopupItemRect(
            popup, memberIndex);
    RECT rect =
        GetCollectionPopupItemTextRect(
            itemRect);
    if (IsRectEmptyRect(rect))
    {
        renameController_.Reset();
        return;
    }
    InflateRect(&rect, 2, 2);
    RECT screenRect = rect;
    MapWindowPoints(
        hwnd_, nullptr,
        reinterpret_cast<POINT*>(
            &screenRect), 2);

    DWORD style =
        WS_POPUP | WS_VISIBLE |
        ES_AUTOVSCROLL;
    style |= dockFolderPopupWidget_.listMode
        ? ES_LEFT
        : (ES_MULTILINE | ES_CENTER |
            ES_WANTRETURN);
    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE |
            WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        L"EDIT", entry.name.c_str(),
        style,
        screenRect.left,
        screenRect.top,
        screenRect.right -
            screenRect.left,
        screenRect.bottom -
            screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!renameEdit_)
    {
        renameController_.Reset();
        return;
    }

    if (renameFont_)
        DeleteObject(renameFont_);
    const float renameScale =
        GetGridCuScaleForBounds(gridPages_, itemRect);
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
        L"Segoe UI");
    SendMessageW(
        renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_
                ? renameFont_
                : GetStockObject(
                    DEFAULT_GUI_FONT)),
        TRUE);
    const int renameMargin =
        std::max(
            1, static_cast<int>(
                std::round(
                    6.0f * renameScale)));
    SendMessageW(
        renameEdit_, EM_SETMARGINS,
        EC_LEFTMARGIN |
            EC_RIGHTMARGIN,
        MAKELPARAM(
            renameMargin,
            renameMargin));
    SetWindowSubclass(
        renameEdit_,
        &DesktopApp::
            RenameEditSubclassProc,
        1,
        reinterpret_cast<DWORD_PTR>(
            this));
    SetWindowPos(
        renameEdit_, HWND_TOPMOST,
        screenRect.left,
        screenRect.top,
        screenRect.right -
            screenRect.left,
        screenRect.bottom -
            screenRect.top,
        SWP_SHOWWINDOW);
    SendMessageW(
        renameEdit_, EM_SETSEL, 0,
        RenameInitialSelectionEnd(
            entry.name,
            entry.isDirectory));
    SetFocus(renameEdit_);
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
        if (wParam == VK_RETURN) { app->CommitRename(false); return 0; }
        if (wParam == VK_ESCAPE) { app->CommitRename(true); return 0; }
        break;
    case WM_KILLFOCUS:
        if (!app->renameCommitPending_)
        {
            app->renameCommitPending_ = true;
            if (!PostMessageW(app->hwnd_, kCommitRenameMessage, FALSE, 0))
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
