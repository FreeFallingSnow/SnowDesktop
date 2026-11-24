#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "common/l10n.h"
#include "diagnostics/diagnostic_log.h"
#include "settings/animation_settings.h"
#include "common/utils.h"
#include "layout/folder_sort_rules.h"
#include "layout/list_detail_rules.h"
#include "app/dock/dock_tray_popup_state.h"
#include "app/dock/persistent_dock_host.h"
#include "diagnostics/performance_trace.h"

// Popup open/close lifecycle and animation.

bool DesktopApp::HasActiveContextMenuSession() const
{
    return snowdesktop::modern_menu::IsActive() ||
        nativeMenuSession_ != nullptr ||
        shellPopupMenuLayerDepth_ > 0 ||
        newMenuContextMenu_.Get() != nullptr ||
        activeContextMenu2_.Get() != nullptr ||
        activeContextMenu3_.Get() != nullptr;
}

void DesktopApp::
DismissActiveContextMenuForPopupTransition()
{
    snowdesktop::modern_menu::DismissActive();
    if (nativeMenuSession_)
    {
        // Keep the source surface in its menu-open state while the independent
        // STA prepares/tracks the popup. An outside action retires that state
        // before its own hit testing, without waiting for the native loop.
        if (nativeMenuPollToken_)
            uiAnimationScheduler_.Cancel(std::exchange(nativeMenuPollToken_, 0));
        nativeMenuSession_.reset();
        snowdesktop::private_pointer::Cursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    if (shellPopupMenuLayerDepth_ > 0 ||
        newMenuContextMenu_.Get() != nullptr ||
        activeContextMenu2_.Get() != nullptr ||
        activeContextMenu3_.Get() != nullptr)
    {
        // The tracker thread owns the native menu window. Remember an early
        // cancellation request until that window exists, then ask its menu
        // loop to unwind without blocking the desktop UI pump.
        shellPopupTrackerCancelRequested_.store(
            true, std::memory_order_release);
        const HWND trackerOwner =
            shellPopupTrackerOwnerHwnd_.load(
                std::memory_order_acquire);
        if (trackerOwner && IsWindow(trackerOwner))
            SendMessageW(trackerOwner, WM_CANCELMODE, 0, 0);
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
    if (!IsPersistentDockHostWindow(
            snowdesktop::private_pointer::WindowAtPoint(screenPoint)))
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
    PersistentDockHost* requestedDockHost =
        FindPersistentDockHost(dock);
    const bool collectionEntry =
        IsLogicalDockEntryType(entry.type);
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
    dockPressedClosedCollectionPopup_ = false;
    if (suppressPointerRelease)
        dockSuppressClickReleaseEntry_ = entryIndex;

    if (collectionEntry)
    {
        if (IsCollectionPopupInteractive() &&
            snowdesktop::floating_dock_rules::
                ShouldCloseCollectionPopup(
                    popupWidgetIndex_,
                    collectionWidgetIndex,
                    collectionPopupDockHost_ ==
                        requestedDockHost))
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
        GetDockFolderPopupEntrySourceId() == sourceId &&
        collectionPopupDockHost_ == requestedDockHost)
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
    size_t entryIndex, POINT anchorPoint, size_t mappingWidgetIndex)
{
    const bool standaloneMapping = mappingWidgetIndex < widgets_.size();
    if (standaloneMapping && widgets_[mappingWidgetIndex].type != DesktopWidgetType::FolderMapping) return;
    if (!standaloneMapping && (entryIndex >= dockEntries_.size() ||
        IsLogicalDockEntryType(dockEntries_[entryIndex].type) ||
        !IsFolderDockEntry(dockEntries_[entryIndex])))
        return;
    DockEntry entry;
    if (standaloneMapping)
    {
        entry.type = DockEntryType::FolderMapping;
        entry.reference = widgets_[mappingWidgetIndex].id;
    }
    else entry = dockEntries_[entryIndex];

    DismissActiveContextMenuForPopupTransition();
    SuspendInlineFolderPreview();

    HMONITOR deferredDockPromotion = nullptr;
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
                anchorScreen, &deferredDockPromotion);
        }
    }

    const std::wstring sourceId =
        std::to_wstring(static_cast<int>(entry.type)) +
        L":" + ToUpperInvariant(entry.reference);
    PersistentDockHost* requestedDockHost = nullptr;
    if (DockContainer* requestedDock =
            GetDockContainerAtPoint(anchorPoint))
    {
        if (DockEntryItem* requestedItem =
                requestedDock->EntryAtPoint(anchorPoint);
            requestedItem &&
            requestedItem->GetEntryIndex() == entryIndex)
        {
            requestedDockHost =
                FindPersistentDockHost(requestedDock);
        }
    }
    const bool sameSource = dockFolderPopupOpen_ &&
        GetDockFolderPopupEntrySourceId() == sourceId &&
        collectionPopupDockHost_ == requestedDockHost;
    if (sameSource && folderPreview_)
    {
        if (popupAnimation_.IsClosing())
        {
            pendingCollectionPopupOpen_.reset();
            StartCollectionPopupAnimation(true);
            UpdateFloatingPopupWindowBounds(true);
        }
        return;
    }
    if (!sameSource && GetOpenPopupWidget() &&
        snowdesktop::popup_animation_rules::OccludesSurface(popupAnimation_))
    {
        pendingCollectionPopupOpen_ = PendingCollectionPopupOpenRequest{
            standaloneMapping ? widgets_[mappingWidgetIndex].id : L"",
            anchorPoint, L"", standaloneMapping ? L"" : sourceId};
        ContinueCollectionPopupSwitch();
        return;
    }
    pendingCollectionPopupOpen_.reset();
    ResetDockTrayPopup();
    PreserveDockFolderPopupDragSourceForTransition();
    ClearPopupDragTarget();
    bool targetPending = false;
    const auto target = ResolveDockFolderTarget(entry, &targetPending);
    const bool reverseClosingAnimation = popupAnimation_.IsClosing() && sameSource;
    const RECT previousBounds = popupRect_;
    ResetCollectionPopupAnimationCache();
    AdvanceFloatingPopupContentGeneration();
    dockFolderPopupOpen_ = true;
    dockFolderPopupAvailable_ = false;
    dockFolderPopupLoading_ = targetPending || !target.path.empty();
    dockFolderPopupSourceId_ = sourceId;
    dockFolderPopupMappingWidgetId_.clear();
    popupWidgetIndex_ = static_cast<size_t>(-1);
    popupScrollOffset_ = 0;
    if (!reverseClosingAnimation)
    {
        ResetCollectionPopupFanScroll();
        popupFanShowAll_ = false;
        popupFanActionFocused_ = false;
    }
    popupHasAnchor_ = true;
    popupAnchoredToDock_ = false;
    collectionPopupDockHost_ = nullptr;
    popupAnchorPoint_ = anchorPoint;
    popupCategoryId_.clear();

    CancelDockFolderPopupIconLoads();
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
    dockFolderPopupWidget_.listMode =
        entry.listMode;
    dockFolderPopupWidget_.listColumns = entry.listColumns;
    dockFolderPopupWidget_.fanPopup = entry.fanPopup;
    dockFolderPopupWidget_.showSearchBox = entry.showSearchBox;
    dockFolderPopupWidget_.showFileCategories = entry.showFileCategories;
    dockFolderPopupWidget_.dateHeaders = entry.dateHeaders;
    dockFolderPopupWidget_.categoryTabOrder = entry.categoryTabOrder;
    dockFolderPopupWidget_.detailShowModified =
        entry.detailShowModified;
    dockFolderPopupWidget_.detailShowType =
        entry.detailShowType;
    dockFolderPopupWidget_.detailShowSize =
        entry.detailShowSize;
    dockFolderPopupWidget_.detailModifiedPosition =
        entry.detailModifiedPosition;
    dockFolderPopupWidget_.detailTypePosition =
        entry.detailTypePosition;
    dockFolderPopupWidget_.detailSizePosition =
        entry.detailSizePosition;
    dockFolderPopupWidget_.showDetails =
        snowdesktop::list_detail_rules::HasMetadataColumns(
            entry.detailShowModified,
            entry.detailShowType,
            entry.detailShowSize);
    dockFolderPopupWidget_.contentSortColumn =
        snowdesktop::list_detail_rules::
            FromLegacyFolderSortMode(
                dockFolderPopupWidget_.folderSortMode);
    dockFolderPopupWidget_.contentSortAscending =
        dockFolderPopupWidget_.folderSortAscending;
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
            dockFolderPopupWidget_.listMode =
                widgets_[widgetIndex].listMode;
            dockFolderPopupWidget_.listColumns = widgets_[widgetIndex].listColumns;
            dockFolderPopupWidget_.showSearchBox = widgets_[widgetIndex].showSearchBox;
            dockFolderPopupWidget_.showFileCategories = widgets_[widgetIndex].showFileCategories;
            dockFolderPopupWidget_.dateHeaders = widgets_[widgetIndex].dateHeaders;
            dockFolderPopupWidget_.categoryTabOrder = widgets_[widgetIndex].categoryTabOrder;
            dockFolderPopupWidget_.activeCategoryId = widgets_[widgetIndex].activeCategoryId;
            dockFolderPopupWidget_.fanPopup = widgets_[widgetIndex].fanPopup;
            dockFolderPopupWidget_.showDetails =
                widgets_[widgetIndex].showDetails;
            dockFolderPopupWidget_.detailShowModified =
                widgets_[widgetIndex].detailShowModified;
            dockFolderPopupWidget_.detailShowType =
                widgets_[widgetIndex].detailShowType;
            dockFolderPopupWidget_.detailShowSize =
                widgets_[widgetIndex].detailShowSize;
            dockFolderPopupWidget_.detailModifiedPosition =
                widgets_[widgetIndex].detailModifiedPosition;
            dockFolderPopupWidget_.detailTypePosition =
                widgets_[widgetIndex].detailTypePosition;
            dockFolderPopupWidget_.detailSizePosition =
                widgets_[widgetIndex].detailSizePosition;
            dockFolderPopupWidget_.contentSortColumn =
                widgets_[widgetIndex].contentSortColumn;
            dockFolderPopupWidget_.contentSortAscending =
                widgets_[widgetIndex].contentSortAscending;
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
    dockFolderPopupKnownItemCount_ = dockFolderPopupWidget_.itemKeys.size();

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
            collectionPopupDockHost_ =
                FindPersistentDockHost(dock);
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
    if (standaloneMapping)
        for (const auto& page : gridPages_)
            if (page.id == widgets_[mappingWidgetIndex].gridCell.pageId) { dockPage = &page; break; }
    if (!dockPage)
        dockPage = GetFirstPageGridPage();
    if (dockPage) popupPageId_ = dockPage->id;

    SyncFolderChangeNotifications();
    // Prepare the model/geometry without revealing a loading frame under the
    // previous timeline. Start the new animation before presenting the host.
    if (!ResumeFolderPreviewForPopup({}, dockFolderPopupMappingWidgetId_))
        RefreshDockFolderPopup(nullptr, false);
    StartCollectionPopupAnimation(
        reverseClosingAnimation, false, deferredDockPromotion);
    PresentDockForCollectionPopup();
    InvalidateCollectionPopupDesktopScene(previousBounds);
    UpdateFloatingPopupWindowBounds(true);
}

void DesktopApp::StartCollectionPopupAnimation(
    bool reverseClosingAnimation, bool fromFan, HMONITOR deferredDockPromotion)
{
    snowdesktop::performance::Scope openingScope("popup", "animation.prepare");
    const auto refreshDockTitles = [&]() {
        SyncDockTitleTooltip();
        InvalidateDockRects();
    };
    const auto presentDeferredDock = [&] {
        refreshDockTitles();
        ShowFloatingDock(deferredDockPromotion, DockRevealSource::AssociatedSurface, true);
    };
    const DesktopWidget* widget = GetOpenPopupWidget();
    // Reversing an expanded panel keeps the effect chosen when it began closing.
    if (!(reverseClosingAnimation && popupFanShowAll_))
    {
        const bool fan = widget && UsesCollectionPopupFan(*widget);
        popupAnimation_.Configure(
            fromFan || snowdesktop::animation::RuntimePopupEffect() == snowdesktop::animation::Fade,
            snowdesktop::animation::RuntimeDurationScale() *
                (fromFan ? 0.7 : (fan ? 2.4 : 1.0)),
            !fan);
    }
    if (!reverseClosingAnimation)
    {
        popupAnimation_.ResetHidden();
        PublishCollectionPopupIconBackdrops({});
        popupFanIconDrawings_.clear();
    }
    if (!(snowdesktop::animation::RuntimePopupEffect() != 0))
    {
        popupAnimation_.ShowImmediately();
        ResetCollectionPopupAnimationCache();
        if (deferredDockPromotion) presentDeferredDock();
        else refreshDockTitles();
        return;
    }
    // Publish the Dock in its target band before the popup acquires its
    // backdrop and snapshot. The popup model and anchor are already ready.
    if (deferredDockPromotion) presentDeferredDock();
    // The snapshot visual belongs to the shared topmost popup host. Materialize
    // and size that host before preparing the animation cache so the snapshot
    // can be attached to the correct DComp tree instead of falling back to
    // UI-thread frame rendering.
    {
        snowdesktop::performance::Scope hostScope("popup", "host.prepare");
        UpdateFloatingPopupWindowBounds(false);
    }
    const double cacheStarted = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    {
        snowdesktop::performance::Scope snapshotScope("popup", "snapshot.prepare");
        PrepareCollectionPopupAnimationCache();
    }
    const double cacheElapsed = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - cacheStarted;
    popupAnimation_.Open(static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::
            MonotonicMilliseconds()));
    if (!StartCollectionPopupCompositionAnimation())
    {
        UpdateCollectionPopupCompositionAnimation();
        EnsureUiAnimationFrame();
    }
    if (!deferredDockPromotion) refreshDockTitles();
    wchar_t message[240]{};
    swprintf_s(message,
        L"Popup animation prepared: driver=%s cacheMs=%.2f folder=%d fan=%d items=%llu",
        popupAnimationCompositorDriven_ ? L"compositor" : L"ui",
        cacheElapsed, dockFolderPopupOpen_ ? 1 : 0,
        widget && UsesCollectionPopupFan(*widget) ? 1 : 0,
        static_cast<unsigned long long>(widget ? GetPopupItemCount(*widget) : 0));
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message);
}



RECT DesktopApp::GetCollectionPopupDesktopFootprint(RECT previousBounds) const
{
    const bool floatingSnapshot = IsCollectionPopupHostedByFloatingWindow() &&
        popupAnimationOverlay_.active &&
        popupAnimationOverlay_.host == UiCompositionAnimationHost::FloatingPopup;
    // The independent host owns the snapshot and glass. Keep desktop damage
    // when an older footprint, desktop interaction, or pointer occlusion may change.
    if (floatingSnapshot && IsRectEmptyRect(previousBounds) &&
        !desktopBackdropFullCollectionPending_ && !GetCollectionPopupResizeTransition() &&
        !dragSession_.HasContext() && !dragDropController_.IsTransportActive() &&
        widgetAction_ == WidgetAction::None && !marqueeActive_ &&
        !PtInRect(&popupRect_, lastMousePoint_) &&
        !PtInRect(&floatingPopupCollectionRegion_, lastMousePoint_))
        return {};

    RECT dirty{};
    UnionRect(&dirty, &previousBounds, &popupRect_);
    UnionRect(&dirty, &dirty, &popupAnimationCacheRect_);
    return dirty;
}

void DesktopApp::InvalidateCollectionPopupDesktopScene(RECT previousBounds)
{
    InvalidateDragStaticScene(!IsCollectionPopupHostedByFloatingWindow() ||
        dragSession_.HasContext() || dragDropController_.IsExternalDragActive());
    if (!hwnd_ || !IsWindow(hwnd_)) return;

    RECT dirty = GetCollectionPopupDesktopFootprint(previousBounds);
    if (IsCollectionPopupHostedByFloatingWindow())
    {
        const size_t sourceIndex = dockFolderPopupOpen_
            ? FindWidgetIndexById(dockFolderPopupMappingWidgetId_) : popupWidgetIndex_;
        if (sourceIndex < widgets_.size())
        {
            const RECT sourceFrame = GetStandaloneWidgetFrameRect(widgets_[sourceIndex]);
            UnionRect(&dirty, &dirty, &sourceFrame);
        }
    }
    if (IsRectEmptyRect(dirty)) return;
    InflateRect(&dirty, 6, 6);
    InvalidateRect(hwnd_, &dirty, FALSE);
}

void DesktopApp::InvalidateCollectionPopupAnimation(
    bool invalidateStaticScene)
{
    if (invalidateStaticScene)
        InvalidateCollectionPopupDesktopScene();
    else if (hwnd_ && IsWindow(hwnd_) && !IsCollectionPopupHostedByFloatingWindow() &&
        !(popupAnchoredToDock_ && persistentDockHostOwnsVisual_) &&
        !IsRectEmptyRect(popupRect_))
    {
        RECT dirty = popupRect_;
        InflateRect(&dirty, 4, 4);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
    // The popup owns an independent compact DComp surface. Animation frames
    // therefore never repaint either the desktop foreground or floating Dock.
    UpdateFloatingPopupWindowBounds(false);
    InvalidateFloatingPopupWindow(true);
}

void DesktopApp::FinalizeCloseCollectionPopup()
{
    collectionPopupKeyboardReturn_ = {};
    PublishCollectionPopupIconBackdrops({});
    const size_t sourceIndex = dockFolderPopupOpen_
        ? FindWidgetIndexById(dockFolderPopupMappingWidgetId_) : popupWidgetIndex_;
    const RECT sourceFrame = IsCollectionPopupHostedByFloatingWindow() && sourceIndex < widgets_.size()
        ? GetStandaloneWidgetFrameRect(widgets_[sourceIndex]) : RECT{};
    const RECT previewBounds = folderPreview_ ? popupRect_ : RECT{};
    const bool temporaryPreview = folderPreview_ && folderPreview_->IsTemporary();
    DiscardFolderPreview();
    const bool collectAllDesktopBackdrops = !IsCollectionPopupHostedByFloatingWindow() ||
        dragSession_.HasContext() || dragDropController_.IsExternalDragActive();
    if (listTitleHoverPopup_) ClearListTitleHover();
    auto* searchView = GetCategorizedPopupView();
    if (!searchView) searchView = dockFolderPopupContainer_.get();
    if (searchView)
    {
        searchView->EndCategoryTabDrag(false);
        searchView->EndSearchPointerSelection();
        searchView->SetSearchFocused(false);
        searchView->SetPopupFrame(nullptr);
    }
    // A date-grouped folder can have projected its live view even when a
    // later menu action hides every categorized control before closing.
    const auto* closingWidget = GetOpenPopupWidget();
    for (const auto& container : containers_)
        if (auto* view = dynamic_cast<WidgetContainer*>(container.get());
            view && view->GetWidgetData() == closingWidget && view->IsPopupHosted())
            view->SetPopupFrame(nullptr);
    if (dockFolderPopupContainer_) dockFolderPopupContainer_->SetPopupFrame(nullptr);
    auto pendingOpen =
        std::move(pendingCollectionPopupOpen_);
    pendingCollectionPopupOpen_.reset();
    RECT dirty = GetCollectionPopupDesktopFootprint(previewBounds);
    UnionRect(&dirty, &dirty, &sourceFrame);
    popupAnimation_.ResetHidden();
    ResetCollectionPopupAnimationCache();
    if (popupWidgetIndex_ == static_cast<size_t>(-1) &&
        !dockFolderPopupOpen_ && !dockTrayPopup_ && !pendingOpen && !temporaryPreview)
    {
        if (!IsRectEmptyRect(previewBounds)) InvalidateCollectionPopupDesktopScene(previewBounds);
        ResumeInlineFolderPreview();
        return;
    }
    ResetDockTrayPopup();
    if (dockFolderPopupOpen_)
        CancelDockFolderPopupIconLoads();
    ClearPopupDragTarget();
    popupWidgetIndex_ = static_cast<size_t>(-1);
    dockFolderPopupOpen_ = false;
    SyncFolderChangeNotifications();
    dockFolderPopupAvailable_ = false;
    dockFolderPopupLoading_ = false;
    dockFolderPopupKnownItemCount_ = 0;
    dockFolderPopupSourceId_.clear();
    dockFolderPopupMappingWidgetId_.clear();
    dockFolderPopupContainer_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    ClearDockFolderPopupEntries();
    dockFolderPopupWidget_ = {};
    marqueeDockFolderPopup_ = false;
    popupScrollOffset_ = 0;
    ResetCollectionPopupFanScroll();
    popupFanShowAll_ = false;
    popupFanActionFocused_ = false;
    popupHasAnchor_ = false;
    popupAnchoredToDock_ = false;
    collectionPopupDockHost_ = nullptr;
    popupAnchorPoint_ = {};
    popupPageId_.clear();
    popupCategoryId_.clear();
    popupRect_ = {};
    InvalidateDockRects();
    InvalidateDragStaticScene(collectAllDesktopBackdrops);
    if (hwnd_ && IsWindow(hwnd_) &&
        !IsRectEmptyRect(dirty))
    {
        InflateRect(&dirty, 6, 6);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
    UpdateFloatingPopupWindowBounds(true);
    ApplyFloatingDockLayerPolicy();
    if (pendingOpen && hwnd_ && IsWindow(hwnd_))
    {
        if (!pendingOpen->dockSourceId.empty())
        {
            for (size_t index = 0; index < dockEntries_.size(); ++index)
            {
                const auto& entry = dockEntries_[index];
                const auto sourceId = std::to_wstring(static_cast<int>(entry.type)) +
                    L":" + ToUpperInvariant(entry.reference);
                if (sourceId == pendingOpen->dockSourceId)
                {
                    OpenDockFolderPopupAt(index, pendingOpen->anchorPoint);
                    break;
                }
            }
            return;
        }
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
    else ResumeInlineFolderPreview();
}

void DesktopApp::ClearDockFolderPopupEntries()
{
    if (dockFolderPopupContainer_) dockFolderPopupContainer_->SetPopupResizeTransition({});
    // FolderEntry releases its HBITMAP in the destructor, while the shared
    // D2D cache is keyed by that handle value. Remove both raw/beautified GPU
    // entries first so repeated popup lifecycles cannot retain stale bitmaps
    // or alias a newly allocated GDI handle to an old icon.
    for (const auto& entry : dockFolderPopupWidget_.folderEntries)
        EraseD2DIconCacheForBitmap(entry.iconBitmap);
    dockFolderPopupWidget_.folderEntries.clear();
}

void DesktopApp::CloseCollectionPopup(
    bool clearSelection, bool returnKeyboardFocus)
{
    auto restore = std::exchange(collectionPopupKeyboardReturn_, {});
    if (returnKeyboardFocus) pendingCollectionPopupOpen_.reset();
    CancelPopupHover(true);
    BeginCollectionPopupClose(clearSelection);
    if (returnKeyboardFocus && restore) restore();
}

void DesktopApp::ContinueCollectionPopupSwitch()
{
    // Keep the outgoing model, anchor and snapshot until its close completes.
    // With effects disabled the finalizer may consume the request synchronously.
    if (popupAnimation_.IsInteractive()) BeginCollectionPopupClose(true);
    if (!pendingCollectionPopupOpen_) return;
    // Switching keeps the normal close trajectory and its original deadline.
    popupAnimation_.Advance(static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
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
}

void DesktopApp::BeginCollectionPopupClose(bool clearSelection)
{
    if (!IsCollectionPopupHostedByFloatingWindow()) return;
    collectionPopupKeyboardReturn_ = {};
    CancelRenameClick();
    folderReadWork_.Cancel(snowdesktop::folder_preview::kResolveWorkKey);
    if (popupAnimation_.IsClosing())
        return;

    PreserveFloatingPopupDragCapture();
    if (dockFolderPopupOpen_)
        CancelDockFolderPopupIconLoads();
    PreserveDockFolderPopupDragSourceForTransition();
    if (clearSelection && !dragSession_.HasContext())
    {
        const bool desktopCollection = !popupAnchoredToDock_ && !dockFolderPopupOpen_ && !dockTrayPopup_ &&
            popupWidgetIndex_ < widgets_.size() && widgets_[popupWidgetIndex_].type == DesktopWidgetType::Collection &&
            !folderPreview_ && !dragDropController_.IsTransportActive() &&
            widgetAction_ == WidgetAction::None && !marqueeActive_ && !GetCollectionPopupResizeTransition();
        if (desktopCollection)
        {
            // The closing snapshot captures the cleared popup. Invalidate the
            // old selection's desktop and Dock projections before that handoff.
            UpdateWidgetControlSelection(nullptr, [this] { ClearSelection(); }, false);
        }
        else if (ClearSelection())
            desktopBackdropFullCollectionPending_ = true;
        for (auto& entry :
             dockFolderPopupWidget_.folderEntries)
            entry.selected = false;
    }
    ClearPopupMouseDownItem();
    ClearPopupDragTarget();
    marqueeActive_ = false;
    marqueeDockFolderPopup_ = false;
    dockFolderPopupMarqueeInitialSelection_.clear();

    const auto effect = snowdesktop::animation::RuntimePopupEffect();
    if (effect == 0)
    {
        FinalizeCloseCollectionPopup();
        return;
    }
    const bool stableFloatingContent = popupAnimation_.IsInteractive() && !popupAnimation_.IsAnimating() &&
        !GetCollectionPopupResizeTransition() && !(folderPreview_ && folderPreview_->inlineBounds) &&
        !IsLuaPanelHostedByFloatingWindow() && !IsLuaStackIndicatorVisible() &&
        !dragSession_.HasContext() && !dragDropController_.IsExternalDragActive();
    UpdateFloatingPopupWindowBounds(false);
    PrepareCollectionPopupAnimationCache();
    if (dockFolderPopupContainer_) dockFolderPopupContainer_->SetPopupResizeTransition({});
    if (popupAnimationOverlay_.active &&
        popupAnimationOverlay_.host ==
            UiCompositionAnimationHost::Desktop)
    {
        ClearDesktopBehindCompositionAnimation(
            popupAnimationCacheRect_);
    }
    const auto now = static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds());
    if (popupFanShowAll_)
    {
        // The short fan-to-panel fade is only for the view change. A close
        // during that fade keeps its current opacity and full panel size.
        popupAnimation_.Advance(now);
        const bool fadingIn = popupAnimation_.IsAnimating() &&
            popupAnimation_.GetVisual().opacity < 1.0f;
        popupAnimation_.Configure(fadingIn || effect == snowdesktop::animation::Fade,
            snowdesktop::animation::RuntimeDurationScale(), true);
    }
    popupAnimation_.Close(now);
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
    else if (stableFloatingContent &&
        popupAnimationOverlay_.host == UiCompositionAnimationHost::FloatingPopup)
    {
        // Starting the native track already paints and commits the floating
        // surface. The source on the desktop still needs its own refresh.
        InvalidateCollectionPopupDesktopScene();
        SyncLuaWidgetTooltip();
        return;
    }
    InvalidateCollectionPopupAnimation(true);
}
