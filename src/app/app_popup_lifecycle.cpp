#include "app.h"

// Popup open/close lifecycle and animation.

void DesktopApp::OpenDockFolderPopupAt(
    size_t entryIndex, POINT anchorPoint)
{
    if (entryIndex >= dockEntries_.size() ||
        !IsFolderDockEntry(dockEntries_[entryIndex]))
        return;

    PreserveDockFolderPopupDragSourceForTransition();
    const DockEntry entry = dockEntries_[entryIndex];
    const auto target = ResolveDockFolderTarget(entry);
    const std::wstring sourceId =
        std::to_wstring(static_cast<int>(entry.type)) +
        L":" + ToUpperInvariant(entry.reference);
    const bool reverseClosingAnimation =
        popupAnimation_.IsClosing() &&
        dockFolderPopupOpen_ &&
        dockFolderPopupSourceId_ == sourceId;
    dockFolderPopupOpen_ = true;
    dockFolderPopupAvailable_ = target.available;
    dockFolderPopupSourceId_ = sourceId;
    dockFolderPopupMappingWidgetId_.clear();
    popupWidgetIndex_ = static_cast<size_t>(-1);
    popupScrollOffset_ = 0;
    popupHasAnchor_ = true;
    popupAnchoredToDock_ = false;
    popupAnchorPoint_ = anchorPoint;
    popupCategoryId_.clear();

    dockFolderPopupWidget_ = DesktopWidget{};
    dockFolderPopupWidget_.type =
        DesktopWidgetType::FolderMapping;
    dockFolderPopupWidget_.id =
        L"__dock_folder_popup__";
    dockFolderPopupWidget_.sourceFolderPath =
        target.path;
    dockFolderPopupWidget_.folderSortMode =
        snowdesktop::folder_sort_rules::
            NormalizeMode(
                entry.folderSortMode);
    dockFolderPopupWidget_.
        folderSortAscending =
            entry.folderSortAscending;
    dockFolderPopupWidget_.itemKeys =
        entry.folderItemKeys;
    dockFolderPopupWidget_.gridCell =
        { kDockPageId, 0, 0 };
    if (entry.type == DockEntryType::FolderMapping)
    {
        const size_t widgetIndex =
            FindWidgetIndexById(entry.reference);
        if (widgetIndex < widgets_.size())
        {
            dockFolderPopupMappingWidgetId_ =
                widgets_[widgetIndex].id;
            dockFolderPopupWidget_.title =
                widgets_[widgetIndex].title;
            dockFolderPopupWidget_.
                folderSortMode =
                    widgets_[widgetIndex].
                        folderSortMode;
            dockFolderPopupWidget_.
                folderSortAscending =
                    widgets_[widgetIndex].
                        folderSortAscending;
            dockFolderPopupWidget_.itemKeys =
                widgets_[widgetIndex].
                    itemKeys;
        }
    }
    else
    {
        const size_t itemIndex =
            FindItemIndexByKey(entry.reference);
        if (itemIndex < items_.size())
            dockFolderPopupWidget_.title =
                items_[itemIndex].name;
    }
    if (dockFolderPopupWidget_.title.empty())
        dockFolderPopupWidget_.title =
            _LW("widget.folder_mapping");

    const GridPage* dockPage = nullptr;
    if (DockContainer* dock =
            GetDockContainerAtPoint(anchorPoint))
    {
        const RECT dockBounds =
            dock->GetInteractiveBounds();
        const POINT dockCenter{
            (dockBounds.left + dockBounds.right) / 2,
            (dockBounds.top + dockBounds.bottom) / 2
        };
        for (const auto& page : gridPages_)
        {
            if (PtInRect(&page.bounds, dockCenter))
            {
                dockPage = &page;
                break;
            }
        }
        if (DockEntryItem* dockItem =
                dock->EntryAtPoint(anchorPoint);
            dockItem &&
            dockItem->GetEntryIndex() == entryIndex)
        {
            const RECT itemBounds =
                dock->GetElementVisualRect(
                    dockItem->GetBounds(), anchorPoint);
            popupDockPosition_ = dockSettings_.position;
            popupAnchoredToDock_ = true;
            switch (popupDockPosition_)
            {
            case DockPosition::Top:
                popupAnchorPoint_ = {
                    (itemBounds.left + itemBounds.right) / 2,
                    itemBounds.bottom };
                break;
            case DockPosition::Left:
                popupAnchorPoint_ = {
                    itemBounds.right,
                    (itemBounds.top + itemBounds.bottom) / 2 };
                break;
            case DockPosition::Right:
                popupAnchorPoint_ = {
                    itemBounds.left,
                    (itemBounds.top + itemBounds.bottom) / 2 };
                break;
            case DockPosition::Bottom:
            default:
                popupAnchorPoint_ = {
                    (itemBounds.left + itemBounds.right) / 2,
                    itemBounds.top };
                break;
            }
        }
    }
    if (!dockPage)
        dockPage = GetFirstPageGridPage();
    if (dockPage) popupPageId_ = dockPage->id;

    RefreshDockFolderPopup();
    popupRect_ =
        GetCollectionPopupRect(dockFolderPopupWidget_);
    dockFolderPopupWidget_.bounds = popupRect_;
    popupScrollOffset_ = std::clamp(
        popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(
            dockFolderPopupWidget_, popupRect_));
    StartCollectionPopupAnimation(
        reverseClosingAnimation);
    if (floatingDockVisible_)
    {
        floatingDockContainer_ =
            GetDockContainerAtPoint(anchorPoint);
        if (!floatingDockContainer_)
            floatingDockContainer_ =
                SelectFloatingDockContainerForMonitor(
                    floatingDockMonitor_);
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
    }
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::StartCollectionPopupAnimation(
    bool reverseClosingAnimation)
{
    if (!reverseClosingAnimation)
        popupAnimation_.ResetHidden();
    PrepareCollectionPopupAnimationCache();
    popupAnimation_.Open(GetTickCount64());
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(
            hwnd_,
            kCollectionPopupAnimationTimerId,
            snowdesktop::popup_animation_rules::
                kFrameIntervalMs,
            nullptr);
    }
}



void DesktopApp::InvalidateCollectionPopupAnimation(
    bool invalidateStaticScene)
{
    if (invalidateStaticScene)
        InvalidateDragStaticScene();
    if (hwnd_ && IsWindow(hwnd_))
    {
        if (invalidateStaticScene)
        {
            InvalidateRect(
                hwnd_, nullptr, FALSE);
        }
        else if (!(popupAnchoredToDock_ &&
                   floatingDockDesktopCopySuppressed_) &&
                 !IsRectEmptyRect(popupRect_))
        {
            RECT dirty = popupRect_;
            InflateRect(&dirty, 4, 4);
            InvalidateRect(
                hwnd_, &dirty, FALSE);
        }
    }
    // Animation frames may be coalesced when the UI thread is busy. Forcing
    // UpdateWindow here would make every timer tick synchronously redraw the
    // complete floating Dock surface and is the main source of frame stalls.
    InvalidateFloatingDockWindow(
        invalidateStaticScene);
}

void DesktopApp::FinalizeCloseCollectionPopup()
{
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(
            hwnd_,
            kCollectionPopupAnimationTimerId);
    popupAnimation_.ResetHidden();
    ResetCollectionPopupAnimationCache();
    if (popupWidgetIndex_ == static_cast<size_t>(-1) &&
        !dockFolderPopupOpen_)
        return;
    popupWidgetIndex_ = static_cast<size_t>(-1);
    dockFolderPopupOpen_ = false;
    dockFolderPopupAvailable_ = false;
    dockFolderPopupSourceId_.clear();
    dockFolderPopupMappingWidgetId_.clear();
    dockFolderPopupContainer_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    dockFolderPopupWidget_.folderEntries.clear();
    marqueeDockFolderPopup_ = false;
    popupScrollOffset_ = 0;
    popupHasAnchor_ = false;
    popupAnchoredToDock_ = false;
    popupAnchorPoint_ = {};
    popupPageId_.clear();
    popupCategoryId_.clear();
    popupRect_ = {};
    if (floatingDockVisible_)
    {
        floatingDockContainer_ =
            SelectFloatingDockContainerForMonitor(
                floatingDockMonitor_);
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
    }
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::CloseCollectionPopup(
    bool clearSelection)
{
    if (popupWidgetIndex_ == static_cast<size_t>(-1) &&
        !dockFolderPopupOpen_)
        return;
    if (popupAnimation_.IsClosing())
        return;

    PreserveDockFolderPopupDragSourceForTransition();
    if (clearSelection)
    {
        ClearSelection();
        for (auto& entry :
             dockFolderPopupWidget_.folderEntries)
            entry.selected = false;
    }
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    marqueeActive_ = false;
    marqueeDockFolderPopup_ = false;
    dockFolderPopupMarqueeInitialSelection_.clear();

    PrepareCollectionPopupAnimationCache();
    popupAnimation_.Close(GetTickCount64());
    if (popupAnimation_.IsHidden())
    {
        FinalizeCloseCollectionPopup();
        return;
    }
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(
            hwnd_,
            kCollectionPopupAnimationTimerId,
            snowdesktop::popup_animation_rules::
                kFrameIntervalMs,
            nullptr);
    }
    InvalidateCollectionPopupAnimation(true);
}
