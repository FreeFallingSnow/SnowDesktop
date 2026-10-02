#include "../app_font.h"
#include "app.h"
#include "../widget_title_layout.h"

// Rename command target selection and editor placement.

void DesktopApp::BeginRenameSelected(
    std::optional<RECT> dockRenameAnchor)
{
    CancelRenameClick();
    if (renameController_.IsActive()) return;
    if (const auto* popup = GetOpenPopupWidget(); popup && UsesCollectionPopupFan(*popup))
        ShowAllCollectionPopupItems();
    renameCommitPending_ = false;

    if (IsCollectionPopupInteractive() &&
        dockFolderPopupOpen_ &&
        std::none_of(widgets_.begin(), widgets_.end(),
            [](const DesktopWidget& widget) { return widget.selected; }))
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
        const bool popupTitleRename = IsCollectionPopupInteractive() &&
            ((!dockFolderPopupOpen_ && popupWidgetIndex_ == selectedWidgetIndex) ||
                (dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ ==
                    widgets_[selectedWidgetIndex].id));
        if (dockRenameAnchor && !popupTitleRename)
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
        bool leftAligned = false;
        float fontSize = ScaleWidgetFontCu(itemFontSizeCu_,
            GetGridCuScaleForBounds(gridPages_, frame));
        int fontWeight = FW_NORMAL;
        if (popupTitleRename)
        {
            // Dock collections have no visible desktop frame. Anchor their
            // editor to the same title rectangle that the popup renders.
            const auto& popupWidget = dockFolderPopupOpen_
                ? dockFolderPopupWidget_ : widgets_[selectedWidgetIndex];
            frame = GetCollectionPopupRect(popupWidget);
            const float popupScale = GetCollectionPopupLayoutMetrics(popupWidget).scale;
            handle = snowdesktop::collection_popup_layout::ResolveTitleRect(
                frame, popupScale, dockFolderPopupOpen_
                    ? GetDockFolderPopupSortButtonRect(frame).left -
                        snowdesktop::collection_popup_layout::ScaleDimension(10, popupScale)
                    : (std::numeric_limits<LONG>::max)());
            if (itemTextFormat_) fontSize = itemTextFormat_->GetFontSize();
            leftAligned = true;
            foundContainer = true;
        }
        else if (IsGroupedCollection(
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
                        InflateRect(&handle, -group->Cu(7.0f), 0);
                        fontSize = group->FontCu(group->GetCategorizedTabFontSize());
                        fontWeight = FW_SEMIBOLD;
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
                        InflateRect(&handle, -group->Cu(7.0f), 0);
                        fontSize = group->FontCu(group->GetCategorizedTabFontSize());
                        fontWeight = FW_SEMIBOLD;
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
                    handle = wc->GetTitleRect();
                    leftAligned = !wc->UsesTopTitleBar();
                    fontSize = wc->FontCu(wc->GetBarHeight() *
                        (wc->UsesTopTitleBar() ? 18.0f / 34.0f : 0.542f));
                    fontWeight = wc->UsesTopTitleBar() ? FW_SEMIBOLD : GetItemFontWeight();
                    foundContainer = true;
                    break;
                }
            }
        }
        if (!foundContainer && widgets_[selectedWidgetIndex].type == DesktopWidgetType::LuaScript)
        {
            frame = GetStandaloneWidgetFrameRect(widgets_[selectedWidgetIndex]);
            handle = GetStandaloneWidgetMoveHandleRect(widgets_[selectedWidgetIndex]);
            const float scale = GetWidgetCellScale(widgets_[selectedWidgetIndex]);
            const float barHeight = CurrentPersonalization().barHeight;
            handle = snowdesktop::widget_title_layout::LuaTitleRect(handle,
                ScaleWidgetCu(4.0f, scale), ScaleWidgetCu(barHeight * 0.083f, scale),
                ScaleWidgetCu(barHeight * 1.17f, scale));
            fontSize = ScaleWidgetFontCu(barHeight * 0.542f, scale);
            fontWeight = GetItemFontWeight();
            leftAligned = true;
        }
        const float renameScale = GetGridCuScaleForBounds(gridPages_, frame);
        const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
        RECT rect = handle;
        InflateRect(&rect, renameMargin, 0);
        RECT screenRect = rect;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

        const DWORD editStyle = snowdesktop::rename_edit_layout::EditStyle(
            leftAligned);
        renameInputWindow_ = CreateWindowExW(
             WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            snowdesktop::text_input::WindowClass(),
            widgets_[selectedWidgetIndex].title.c_str(),
            editStyle,
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
            fontSize))),
            0, 0, 0, fontWeight, FALSE, FALSE, FALSE,
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
        SendMessageW(renameInputWindow_, EM_SETSEL, 0, -1);
        SetFocus(renameInputWindow_);
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
    const bool popupRename =
        !dockFolderPopupOpen_ &&
        popupWidgetIndex_ < widgets_.size() &&
        IsCollectionPopupInteractive();
    bool leftAlignedRename = popupRename &&
        UsesCollectionPopupList(widgets_[popupWidgetIndex_]);
    const DesktopWidget* titleWidget = popupRename ? &widgets_[popupWidgetIndex_] :
        visibilityWidgetIndex < widgets_.size() ? &widgets_[visibilityWidgetIndex] : nullptr;
    RECT textRect = leftAlignedRename
        ? GetCollectionPopupItemTextRect(
            itemBounds)
        : GetItemRenameRect(itemBounds, ResolveItemTitleLines(titleWidget));
    if (!popupRename)
    {
        for (const auto& container : containers_)
        {
            auto* list = dynamic_cast<ScrollingItemWidget*>(container.get());
            if (!list || !list->SingleColumn()) continue;
            for (const auto& slot : list->GetSlots())
            {
                const auto* icon = dynamic_cast<const DesktopIcon*>(slot->GetItem());
                if (icon && icon->GetDesktopItem() == &items_[selectedIndex])
                {
                    textRect = list->GetListItemTextRect(slot->GetBounds());
                    leftAlignedRename = true;
                    break;
                }
            }
            if (leftAlignedRename) break;
        }
    }
    const float renameScale = GetGridCuScaleForBounds(gridPages_, itemBounds);
    const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
    InflateRect(&textRect, renameMargin, 0);
    RECT screenRect = textRect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    const DWORD renameStyle =
        snowdesktop::rename_edit_layout::EditStyle(leftAlignedRename);
    renameInputWindow_ = CreateWindowExW(
         WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        snowdesktop::text_input::WindowClass(),
        items_[selectedIndex].name.c_str(),
        renameStyle,
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
        RenameInitialSelectionEnd(items_[selectedIndex].name, isDirectory));
    SetFocus(renameInputWindow_);
    if (visibilityWidgetIndex < widgets_.size())
    {
        interactionPinnedWidgetId_ =
            widgets_[visibilityWidgetIndex].id;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (popupRename)
    {
        InvalidateCollectionPopupContent();
        InvalidateFloatingPopupWindow(false);
    }
}
