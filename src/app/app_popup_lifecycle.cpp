#include "app.h"

// Popup open/close lifecycle and animation.

bool DesktopApp::HasActiveContextMenuSession() const
{
    return snowdesktop::modern_menu::IsActive() ||
        shellPopupMenuLayerDepth_ > 0 ||
        newMenuContextMenu_.Get() != nullptr ||
        activeContextMenu2_.Get() != nullptr ||
        activeContextMenu3_.Get() != nullptr;
}

void DesktopApp::
DismissActiveContextMenuForPopupTransition()
{
    snowdesktop::modern_menu::DismissActive();
    if (shellPopupMenuLayerDepth_ > 0 ||
        newMenuContextMenu_.Get() != nullptr ||
        activeContextMenu2_.Get() != nullptr ||
        activeContextMenu3_.Get() != nullptr)
    {
        // EndMenu is safe from the UI thread while TrackPopupMenuEx pumps its
        // nested loop. The RAII layer guard remains responsible for restoring
        // the floating Dock after TrackPopupMenuEx unwinds.
        EndMenu();
    }
}

bool DesktopApp::
TryActivateDockPopupFromMenuPointerPress(
    POINT desktopPoint,
    POINT screenPoint,
    bool suppressPointerRelease)
{
    if (!HasActiveContextMenuSession())
        return false;
    if (!floatingDockHwnd_ ||
        !IsWindow(floatingDockHwnd_) ||
        WindowFromPoint(screenPoint) !=
            floatingDockHwnd_)
    {
        return false;
    }

    DockContainer* dock =
        GetDockContainerAtPoint(desktopPoint);
    if (!dock ||
        !dock->ContainsInteractivePoint(desktopPoint))
        return false;
    DockEntryItem* item =
        dock->EntryAtPoint(desktopPoint);
    if (!item)
        return false;

    const size_t entryIndex = item->GetEntryIndex();
    if (entryIndex >= dockEntries_.size())
        return false;
    const DockEntry& entry = dockEntries_[entryIndex];
    const bool collectionEntry =
        entry.type == DockEntryType::Collection;
    const bool folderEntry = IsFolderDockEntry(entry);
    if (!collectionEntry && !folderEntry)
        return false;

    size_t collectionWidgetIndex =
        static_cast<size_t>(-1);
    if (collectionEntry)
    {
        collectionWidgetIndex =
            FindWidgetIndexById(entry.reference);
        if (collectionWidgetIndex >= widgets_.size())
            return false;
    }

    DismissActiveContextMenuForPopupTransition();
    if (suppressPointerRelease)
        dockSuppressClickReleaseEntry_ = entryIndex;

    if (collectionEntry)
    {
        if (IsCollectionPopupInteractive() &&
            snowdesktop::floating_dock_rules::
                ShouldCloseCollectionPopup(
                    popupWidgetIndex_,
                    collectionWidgetIndex))
        {
            CloseCollectionPopup();
        }
        else
        {
            OpenCollectionPopupAt(
                collectionWidgetIndex,
                desktopPoint);
        }
        return true;
    }

    const std::wstring sourceId =
        std::to_wstring(
            static_cast<int>(entry.type)) +
        L":" + ToUpperInvariant(entry.reference);
    if (IsCollectionPopupInteractive() &&
        dockFolderPopupOpen_ &&
        dockFolderPopupSourceId_ == sourceId)
    {
        CloseCollectionPopup();
    }
    else
    {
        OpenDockFolderPopupAt(
            entryIndex, desktopPoint);
    }
    return true;
}

void DesktopApp::OpenDockFolderPopupAt(
    size_t entryIndex, POINT anchorPoint)
{
    if (entryIndex >= dockEntries_.size() ||
        !IsFolderDockEntry(dockEntries_[entryIndex]))
        return;

    pendingCollectionPopupOpen_.reset();
    DismissActiveContextMenuForPopupTransition();

    if (DockContainer* dock =
            GetDockContainerAtPoint(anchorPoint))
    {
        if (DockEntryItem* dockItem =
                dock->EntryAtPoint(anchorPoint);
            dockItem &&
            dockItem->GetEntryIndex() == entryIndex)
        {
            POINT anchorScreen = anchorPoint;
            if (hwnd_ && IsWindow(hwnd_))
                ClientToScreen(hwnd_, &anchorScreen);
            else
            {
                anchorScreen.x += virtualLeft_;
                anchorScreen.y += virtualTop_;
            }
            EnsureFloatingDockVisibleForAssociatedSurface(
                anchorScreen);
        }
    }

    PreserveDockFolderPopupDragSourceForTransition();
    ClearPopupDragTarget();
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

    ClearDockFolderPopupEntries();
    dockFolderPopupWidget_ = DesktopWidget{};
    dockFolderPopupWidget_.type =
        DesktopWidgetType::FolderMapping;
    dockFolderPopupWidget_.id =
        kDockFolderPopupWidgetId;
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
    UpdateFloatingPopupWindowBounds(true);
}

void DesktopApp::StartCollectionPopupAnimation(
    bool reverseClosingAnimation)
{
    if (!reverseClosingAnimation)
        popupAnimation_.ResetHidden();
    if (!snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
    {
        popupAnimation_.ShowImmediately();
        ResetCollectionPopupAnimationCache();
        return;
    }
    // The snapshot visual belongs to the shared topmost popup host. Materialize
    // and size that host before preparing the animation cache so the snapshot
    // can be attached to the correct DComp tree instead of falling back to
    // UI-thread frame rendering.
    UpdateFloatingPopupWindowBounds(false);
    PrepareCollectionPopupAnimationCache();
    popupAnimation_.Open(static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::
            MonotonicMilliseconds()));
    if (!StartCollectionPopupCompositionAnimation())
    {
        UpdateCollectionPopupCompositionAnimation();
        EnsureUiAnimationFrame();
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
            RECT dirty = popupRect_;
            if (!IsRectEmptyRect(popupAnimationCacheRect_))
            {
                if (IsRectEmptyRect(dirty))
                    dirty = popupAnimationCacheRect_;
                else
                    UnionRect(
                        &dirty, &dirty,
                        &popupAnimationCacheRect_);
            }
            if (!IsRectEmptyRect(dirty))
            {
                InflateRect(&dirty, 6, 6);
                InvalidateRect(hwnd_, &dirty, FALSE);
            }
        }
        else if (!IsCollectionPopupHostedByFloatingWindow() &&
                 !(popupAnchoredToDock_ &&
                   floatingDockDesktopCopySuppressed_) &&
                 !IsRectEmptyRect(popupRect_))
        {
            RECT dirty = popupRect_;
            InflateRect(&dirty, 4, 4);
            InvalidateRect(
                hwnd_, &dirty, FALSE);
        }
    }
    // The popup owns an independent compact DComp surface. Animation frames
    // therefore never repaint either the desktop foreground or floating Dock.
    UpdateFloatingPopupWindowBounds(false);
    InvalidateFloatingPopupWindow(true);
}

void DesktopApp::FinalizeCloseCollectionPopup()
{
    auto pendingOpen =
        std::move(pendingCollectionPopupOpen_);
    pendingCollectionPopupOpen_.reset();
    RECT dirty = popupRect_;
    if (!IsRectEmptyRect(popupAnimationCacheRect_))
    {
        if (IsRectEmptyRect(dirty))
            dirty = popupAnimationCacheRect_;
        else
            UnionRect(
                &dirty, &dirty,
                &popupAnimationCacheRect_);
    }
    popupAnimation_.ResetHidden();
    ResetCollectionPopupAnimationCache();
    if (popupWidgetIndex_ == static_cast<size_t>(-1) &&
        !dockFolderPopupOpen_)
        return;
    ClearPopupDragTarget();
    popupWidgetIndex_ = static_cast<size_t>(-1);
    dockFolderPopupOpen_ = false;
    dockFolderPopupAvailable_ = false;
    dockFolderPopupSourceId_.clear();
    dockFolderPopupMappingWidgetId_.clear();
    dockFolderPopupContainer_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    ClearDockFolderPopupEntries();
    marqueeDockFolderPopup_ = false;
    popupScrollOffset_ = 0;
    popupHasAnchor_ = false;
    popupAnchoredToDock_ = false;
    popupAnchorPoint_ = {};
    popupPageId_.clear();
    popupCategoryId_.clear();
    popupRect_ = {};
    InvalidateDragStaticScene();
    if (hwnd_ && IsWindow(hwnd_) &&
        !IsRectEmptyRect(dirty))
    {
        InflateRect(&dirty, 6, 6);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
    UpdateFloatingPopupWindowBounds(true);
    if (pendingOpen && hwnd_ && IsWindow(hwnd_))
    {
        const size_t widgetIndex =
            FindWidgetIndexById(pendingOpen->widgetId);
        if (widgetIndex < widgets_.size())
        {
            OpenCollectionPopupAt(
                widgetIndex,
                pendingOpen->anchorPoint,
                pendingOpen->categoryId);
        }
    }
}

void DesktopApp::ClearDockFolderPopupEntries()
{
    // FolderEntry releases its HBITMAP in the destructor, while the shared
    // D2D cache is keyed by that handle value. Remove both raw/beautified GPU
    // entries first so repeated popup lifecycles cannot retain stale bitmaps
    // or alias a newly allocated GDI handle to an old icon.
    for (const auto& entry : dockFolderPopupWidget_.folderEntries)
        EraseD2DIconCacheForBitmap(entry.iconBitmap);
    dockFolderPopupWidget_.folderEntries.clear();
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
    ClearPopupMouseDownItem();
    ClearPopupDragTarget();
    marqueeActive_ = false;
    marqueeDockFolderPopup_ = false;
    dockFolderPopupMarqueeInitialSelection_.clear();

    if (!snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
    {
        FinalizeCloseCollectionPopup();
        return;
    }
    UpdateFloatingPopupWindowBounds(false);
    PrepareCollectionPopupAnimationCache();
    if (popupAnimationOverlay_.active &&
        popupAnimationOverlay_.host ==
            UiCompositionAnimationHost::Desktop)
    {
        ClearDesktopBehindCompositionAnimation(
            popupAnimationCacheRect_);
    }
    popupAnimation_.Close(static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::
            MonotonicMilliseconds()));
    if (popupAnimation_.IsHidden())
    {
        FinalizeCloseCollectionPopup();
        return;
    }
    if (!StartCollectionPopupCompositionAnimation())
    {
        UpdateCollectionPopupCompositionAnimation();
        EnsureUiAnimationFrame();
    }
    InvalidateCollectionPopupAnimation(true);
}
