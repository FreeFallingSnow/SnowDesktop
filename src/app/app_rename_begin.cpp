#include "app.h"

// Rename command target selection and editor placement.

void DesktopApp::BeginRenameSelected(
    std::optional<RECT> dockRenameAnchor)
{
    if (renameEdit_ != nullptr) return;
    renameCommitPending_ = false;

    if (IsCollectionPopupInteractive() &&
        dockFolderPopupOpen_)
    {
        size_t selectedMember =
            static_cast<size_t>(-1);
        int selectedCount = 0;
        for (size_t i = 0;
            i < dockFolderPopupWidget_.
                folderEntries.size(); ++i)
        {
            if (!dockFolderPopupWidget_.
                    folderEntries[i].
                        selected)
                continue;
            selectedMember = i;
            ++selectedCount;
        }
        if (selectedCount == 1)
        {
            BeginRenameDockFolderPopupEntry(
                selectedMember);
            return;
        }
    }

    int selectedWidgetCount = 0;
    size_t selectedWidgetIndex = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].selected)
        {
            ++selectedWidgetCount;
            selectedWidgetIndex = i;
        }
    }
    if (selectedWidgetCount == 1 && selectedWidgetIndex < widgets_.size())
    {
        if (!CanRenameWidget(widgets_[selectedWidgetIndex])) return;

        size_t visibilityWidgetIndex =
            ResolveRenameVisibilityWidgetIndex(
                selectedWidgetIndex);
        renameController_.BeginWidget(
            selectedWidgetIndex);
        if (dockRenameAnchor)
        {
            if (!BeginDockAnchoredRename(
                    widgets_[selectedWidgetIndex].title,
                    *dockRenameAnchor, -1))
            {
                renameController_.Reset();
            }
            return;
        }

        RECT frame = widgets_[selectedWidgetIndex].bounds;
        RECT handle = frame;
        bool foundContainer = false;
        bool groupedTabRename = false;
        if (IsGroupedCollection(
                widgets_[selectedWidgetIndex]))
        {
            const size_t groupIndex =
                FindCollectionGroupIndexForChild(
                    widgets_[selectedWidgetIndex].id);
            if (groupIndex < widgets_.size())
            {
                for (const auto& c : containers_)
                {
                    auto* group =
                        dynamic_cast<CollectionGroup*>(
                            c.get());
                    if (!group ||
                        group->GetWidgetData() !=
                            &widgets_[groupIndex])
                        continue;
                    frame = group->GetTabRectById(
                        widgets_[selectedWidgetIndex].id);
                    if (!IsRectEmptyRect(frame))
                    {
                        handle = frame;
                        foundContainer = true;
                        groupedTabRename = true;
                    }
                    break;
                }
            }
        }
        else
        {
            const size_t groupIndex =
                FindFileGroupIndexForChild(
                    widgets_[selectedWidgetIndex].id);
            if (groupIndex < widgets_.size())
            {
                for (const auto& c : containers_)
                {
                    auto* group =
                        dynamic_cast<FileGroup*>(
                            c.get());
                    if (!group ||
                        group->GetWidgetData() !=
                            &widgets_[groupIndex])
                        continue;
                    frame = group->GetSourceTabRectById(
                        widgets_[selectedWidgetIndex].id);
                    if (!IsRectEmptyRect(frame))
                    {
                        handle = frame;
                        foundContainer = true;
                        groupedTabRename = true;
                    }
                    break;
                }
            }
        }
        if (!foundContainer)
        {
            for (const auto& c : containers_)
            {
                auto* wc =
                    dynamic_cast<WidgetContainer*>(c.get());
                if (wc && wc->GetWidgetData() ==
                        &widgets_[selectedWidgetIndex])
                {
                    frame = wc->GetFrameRect();
                    handle = wc->GetMoveHandleRect();
                    foundContainer = true;
                    break;
                }
            }
        }
        if (!foundContainer && widgets_[selectedWidgetIndex].type == DesktopWidgetType::LuaScript)
        {
            frame = GetStandaloneWidgetFrameRect(widgets_[selectedWidgetIndex]);
            handle = GetStandaloneWidgetMoveHandleRect(widgets_[selectedWidgetIndex]);
        }
        const int editHeight = groupedTabRename
            ? std::max(
                24, static_cast<int>(
                    handle.bottom - handle.top))
            : std::max(
                40, static_cast<int>(
                    handle.bottom - handle.top) * 2);
        RECT rect = groupedTabRename
            ? MakeRect(
                frame.left + 2, frame.top,
                frame.right - 2, frame.top + editHeight)
            : MakeRect(
                frame.left + 4, handle.top,
                frame.right - 4,
                handle.top + editHeight);
        InflateRect(&rect, 2, 2);
        RECT screenRect = rect;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

        const DWORD editStyle = groupedTabRename
            ? (WS_POPUP | WS_VISIBLE |
                ES_CENTER | ES_AUTOHSCROLL)
            : (WS_POPUP | WS_VISIBLE |
                ES_MULTILINE | ES_CENTER |
                ES_AUTOVSCROLL | ES_WANTRETURN);
        renameEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"EDIT",
            widgets_[selectedWidgetIndex].title.c_str(),
            editStyle,
            screenRect.left, screenRect.top,
            screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
            hwnd_, nullptr, instance_, nullptr);
        if (!renameEdit_)
        {
            renameController_.Reset();
            return;
        }

        if (renameFont_) DeleteObject(renameFont_);
        const float renameScale = GetItemLayoutScale(frame);
        renameFont_ = CreateFontW(-std::max(1, static_cast<int>(std::round(itemFontSize_ * renameScale))),
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
        SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        SetFocus(renameEdit_);
        if (visibilityWidgetIndex < widgets_.size())
        {
            interactionPinnedWidgetId_ =
                widgets_[visibilityWidgetIndex].id;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    size_t folderWidget = static_cast<size_t>(-1);
    size_t folderMember = static_cast<size_t>(-1);
    if (FindSingleSelectedFolderEntry(folderWidget, folderMember))
    {
        BeginRenameFolderEntry(folderWidget, folderMember);
        return;
    }

    int selectedCount = 0;
    int selectedIndex = -1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
    {
        if (items_[i].selected)
        {
            ++selectedCount;
            selectedIndex = i;
        }
    }
    if (selectedCount != 1 || selectedIndex < 0) return;
    if (!items_[selectedIndex].desktopIconClsid.empty()) return;

    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(items_[selectedIndex].absolutePidl.get(), path)) return;
    DWORD fileAttributes = GetFileAttributesW(path);
    bool isDirectory = fileAttributes != INVALID_FILE_ATTRIBUTES &&
        (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    renameController_.BeginDesktopItem(
        static_cast<size_t>(selectedIndex));
    if (dockRenameAnchor)
    {
        if (!BeginDockAnchoredRename(
                items_[selectedIndex].name,
                *dockRenameAnchor,
                RenameInitialSelectionEnd(
                    items_[selectedIndex].name,
                    isDirectory)))
        {
            renameController_.Reset();
        }
        return;
    }
    size_t visibilityWidgetIndex =
        RenameController::InvalidIndex;
    RECT itemBounds = GetVisibleCollectionItemBounds(
        renameController_.Index(),
        &visibilityWidgetIndex);
    if (IsRectEmptyRect(itemBounds))
        itemBounds = items_[selectedIndex].bounds;
    if (IsRectEmptyRect(itemBounds))
    {
        renameController_.Reset();
        return;
    }
    RECT textRect = GetItemTextRect(itemBounds, true);
    InflateRect(&textRect, 2, 2);
    RECT screenRect = textRect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT",
        items_[selectedIndex].name.c_str(),
        WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);

    if (!renameEdit_)
    {
        renameController_.Reset();
        return;
    }

    if (renameFont_) DeleteObject(renameFont_);
    const float renameScale = GetItemLayoutScale(itemBounds);
    renameFont_ = CreateFontW(-std::max(1, static_cast<int>(std::round(itemFontSize_ * renameScale))),
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
        RenameInitialSelectionEnd(items_[selectedIndex].name, isDirectory));
    SetFocus(renameEdit_);
    if (visibilityWidgetIndex < widgets_.size())
    {
        interactionPinnedWidgetId_ =
            widgets_[visibilityWidgetIndex].id;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
