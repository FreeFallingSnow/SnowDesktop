#include "ui/render/app_font.h"
#include "app/app.h"

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

    RECT rect = GetFolderEntryRenameRect(widgetIndex, memberIndex);
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

    const size_t owner = ResolveRenameVisibilityWidgetIndex(widgetIndex);
    const bool listMode = owner < widgets_.size() && widgets_[owner].listMode;
    const DWORD style = snowdesktop::rename_edit_layout::EditStyle(listMode, !listMode);
    renameInputWindow_ = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        snowdesktop::text_input::WindowClass(), widgets_[widgetIndex].folderEntries[memberIndex].name.c_str(), style,
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
            widgets_[widgetIndex].folderEntries[memberIndex].name,
            widgets_[widgetIndex].folderEntries[memberIndex].fullPath,
            widgets_[widgetIndex].folderEntries[memberIndex].isDirectory));
    SetFocus(renameInputWindow_);
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
        snowdesktop::text_input::WindowClass(), entry.name.c_str(),
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
            entry.name,
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
