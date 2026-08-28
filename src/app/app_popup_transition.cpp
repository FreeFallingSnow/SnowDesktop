#include "app.h"
#include "../menu_fluent_glyphs.h"

// Collection and Dock-folder popup transitions.

bool DesktopApp::
PreserveDockFolderPopupDragSourceForTransition()
{
    if (!dragSession_.IsActive() ||
        !dockFolderPopupContainer_ ||
        dragSession_.Source() !=
            dockFolderPopupContainer_.get())
        return false;

    DesktopWidget snapshot;
    snapshot.id = dockFolderPopupWidget_.id;
    snapshot.type = DesktopWidgetType::FolderMapping;
    snapshot.title = dockFolderPopupWidget_.title;
    snapshot.customTitle =
        dockFolderPopupWidget_.customTitle;
    snapshot.sourceFolderPath =
        dockFolderPopupWidget_.sourceFolderPath;
    snapshot.gridCell =
        dockFolderPopupWidget_.gridCell;
    snapshot.gridSpan =
        dockFolderPopupWidget_.gridSpan;
    snapshot.bounds =
        dockFolderPopupWidget_.bounds;
    snapshot.cellScale =
        dockFolderPopupWidget_.cellScale;
    snapshot.folderSortMode =
        dockFolderPopupWidget_.folderSortMode;
    snapshot.folderSortAscending =
        dockFolderPopupWidget_.
            folderSortAscending;
    snapshot.itemKeys =
        dockFolderPopupWidget_.itemKeys;

    std::vector<RECT> itemBounds;
    for (Item* sourceItem :
         dragSession_.Items())
    {
        if (!sourceItem ||
            !dynamic_cast<FolderEntryIcon*>(
                sourceItem))
            continue;
        const std::wstring path =
            sourceItem->GetPath();
        auto entry = std::find_if(
            dockFolderPopupWidget_.
                folderEntries.begin(),
            dockFolderPopupWidget_.
                folderEntries.end(),
            [&](const FolderEntry& candidate) {
                return PathsEqualInsensitive(
                    candidate.fullPath, path);
            });
        if (entry ==
            dockFolderPopupWidget_.
                folderEntries.end())
            continue;
        snapshot.folderEntries.push_back(*entry);
        snapshot.folderEntries.back().selected =
            true;
        itemBounds.push_back(
            sourceItem->GetBounds());
    }
    if (snapshot.folderEntries.empty())
    {
        EndDragSession();
        return true;
    }

    dockFolderPopupDragSourceItems_.clear();
    dockFolderPopupDragSourceContainer_.reset();
    dockFolderPopupDragSourceWidget_ =
        std::move(snapshot);
    dockFolderPopupDragSourceContainer_ =
        std::make_unique<FolderMapping>(
            &dockFolderPopupDragSourceWidget_,
            this);

    std::vector<Item*> reboundItems;
    reboundItems.reserve(
        dockFolderPopupDragSourceWidget_.
            folderEntries.size());
    dockFolderPopupDragSourceItems_.reserve(
        dockFolderPopupDragSourceWidget_.
            folderEntries.size());
    for (size_t i = 0;
        i < dockFolderPopupDragSourceWidget_.
            folderEntries.size(); ++i)
    {
        auto item =
            std::make_unique<FolderEntryIcon>(
                &dockFolderPopupDragSourceWidget_.
                    folderEntries[i],
                dockFolderPopupDragSourceContainer_.
                    get(),
                this);
        item->SetBounds(itemBounds[i]);
        reboundItems.push_back(item.get());
        dockFolderPopupDragSourceItems_.
            push_back(std::move(item));
    }

    DragSourceList sourceList =
        BuildDragSourceList(
            reboundItems,
            dockFolderPopupDragSourceContainer_.
                get());
    dragSession_.RebindSource(
        dockFolderPopupDragSourceContainer_.get(),
        std::move(reboundItems),
        std::move(sourceList));
    return true;
}

void DesktopApp::
ClearDockFolderPopupDragSourceSnapshot()
{
    dockFolderPopupDragSourceItems_.clear();
    dockFolderPopupDragSourceContainer_.reset();
    dockFolderPopupDragSourceWidget_ =
        DesktopWidget{};
}

void DesktopApp::OpenCollectionPopupAt(size_t widgetIndex,
    POINT anchorPoint, const std::wstring& categoryId,
    bool closingStartedByCurrentPress)
{
    {
        wchar_t message[320]{};
        swprintf_s(
            message,
            L"Popup input trace: collection-open index=%llu generation=%u lua=%d currentCollection=%d mouseDown=%d capture=%p",
            static_cast<unsigned long long>(widgetIndex),
            floatingPopupMouseHookGeneration_,
            luaWidgetPanelRequest_.widgetId.empty() ? 0 : 1,
            GetOpenPopupWidget() ? 1 : 0,
            mouseDown_ ? 1 : 0,
            GetCapture());
        WriteDiagnosticLogEntry(message);
    }
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        return;

    CancelCollectionPopupDwell();

    DismissActiveContextMenuForPopupTransition();

    PersistentDockHost* requestedDockHost = nullptr;
    if (DockContainer* requestedDock =
            GetDockContainerAtPoint(anchorPoint))
    {
        if (DockEntryItem* requestedItem =
                requestedDock->EntryAtPoint(anchorPoint);
            requestedItem &&
            requestedItem->GetEntryType() ==
                DockEntryType::Collection &&
            requestedItem->GetReference() ==
                widgets_[widgetIndex].id)
        {
            requestedDockHost =
                FindPersistentDockHost(requestedDock);
        }
    }
    const bool samePopupSource =
        !dockFolderPopupOpen_ &&
        popupWidgetIndex_ == widgetIndex &&
        collectionPopupDockHost_ == requestedDockHost;
    switch (snowdesktop::popup_animation_rules::
        ResolveExistingSourceAction(
            samePopupSource,
            popupAnimation_.IsInteractive(),
            closingStartedByCurrentPress &&
                popupAnimation_.IsClosing(),
            popupAnimation_.IsClosing()))
    {
    case snowdesktop::popup_animation_rules::
        ExistingSourceAction::CloseExisting:
        pendingCollectionPopupOpen_.reset();
        CloseCollectionPopup();
        return;
    case snowdesktop::popup_animation_rules::
        ExistingSourceAction::KeepClosing:
        return;
    case snowdesktop::popup_animation_rules::
        ExistingSourceAction::OpenAfterExistingCloses:
        pendingCollectionPopupOpen_ =
            PendingCollectionPopupOpenRequest{
                widgets_[widgetIndex].id,
                anchorPoint,
                categoryId,
            };
        return;
    case snowdesktop::popup_animation_rules::
        ExistingSourceAction::ReopenExisting:
        pendingCollectionPopupOpen_.reset();
        AdvanceFloatingPopupContentGeneration();
        StartCollectionPopupAnimation(true);
        InvalidateCollectionPopupAnimation(true);
        return;
    case snowdesktop::popup_animation_rules::
        ExistingSourceAction::OpenAtRequestedAnchor:
    default:
        pendingCollectionPopupOpen_.reset();
        break;
    }

    // The low-level outside-click notification is posted asynchronously.
    // Bind it to the collection content that existed at button-down so the
    // press which requested this popup cannot dismiss the newly published
    // content after its release handler returns.
    AdvanceFloatingPopupContentGeneration();

    if (DockContainer* dock =
            GetDockContainerAtPoint(anchorPoint))
    {
        if (DockEntryItem* dockItem =
                dock->EntryAtPoint(anchorPoint);
            dockItem &&
            dockItem->GetEntryType() ==
                DockEntryType::Collection &&
            dockItem->GetReference() ==
                widgets_[widgetIndex].id)
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

    if (dockFolderPopupOpen_)
        CancelDockFolderPopupIconLoads();
    PreserveDockFolderPopupDragSourceForTransition();
    ClearPopupDragTarget();
    dockFolderPopupOpen_ = false;
    dockFolderPopupAvailable_ = false;
    dockFolderPopupContainer_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    dockFolderPopupSourceId_.clear();
    dockFolderPopupMappingWidgetId_.clear();
    ClearDockFolderPopupEntries();
    popupWidgetIndex_ = widgetIndex;
    popupScrollOffset_ = 0;
    popupHasAnchor_ = anchorPoint.x != LONG_MIN || anchorPoint.y != LONG_MIN;
    popupAnchoredToDock_ = false;
    collectionPopupDockHost_ = nullptr;
    popupAnchorPoint_ = anchorPoint;
    popupCategoryId_ = categoryId;
    popupPageId_ = widgets_[widgetIndex].gridCell.pageId;
    const size_t groupIndex =
            FindCollectionGroupIndexForChild(
                widgets_[widgetIndex].id);
    if (groupIndex < widgets_.size())
    {
        popupPageId_ = widgets_[groupIndex].gridCell.pageId;
    }
    if (DockContainer* dock = GetDockContainerAtPoint(anchorPoint))
    {
        RECT dockBounds = dock->GetInteractiveBounds();
        if (dock->ContainsInteractivePoint(anchorPoint))
        {
            const POINT dockCenter{
                (dockBounds.left + dockBounds.right) / 2,
                (dockBounds.top + dockBounds.bottom) / 2
            };
            const GridPage* dockPage = nullptr;
            for (const auto& page : gridPages_)
            {
                if (PtInRect(&page.bounds, dockCenter))
                {
                    dockPage = &page;
                    break;
                }
            }
            if (!dockPage) dockPage = GetFirstPageGridPage();
            if (dockPage) popupPageId_ = dockPage->id;
            if (DockEntryItem* dockItem = dock->EntryAtPoint(anchorPoint);
                dockItem && dockItem->GetEntryType() == DockEntryType::Collection &&
                dockItem->GetReference() == widgets_[widgetIndex].id)
            {
                RECT itemBounds = dock->GetElementVisualRect(
                    dockItem->GetBounds(), anchorPoint);
                popupDockPosition_ = dockSettings_.position;
                popupAnchoredToDock_ = true;
                collectionPopupDockHost_ =
                    FindPersistentDockHost(dock);
                switch (popupDockPosition_)
                {
                case DockPosition::Top:
                    popupAnchorPoint_ = {
                        (itemBounds.left + itemBounds.right) / 2, itemBounds.bottom };
                    break;
                case DockPosition::Left:
                    popupAnchorPoint_ = {
                        itemBounds.right, (itemBounds.top + itemBounds.bottom) / 2 };
                    break;
                case DockPosition::Right:
                    popupAnchorPoint_ = {
                        itemBounds.left, (itemBounds.top + itemBounds.bottom) / 2 };
                    break;
                case DockPosition::Bottom:
                default:
                    popupAnchorPoint_ = {
                        (itemBounds.left + itemBounds.right) / 2, itemBounds.top };
                    break;
                }
            }
        }
    }
    popupRect_ = GetCollectionPopupRect(widgets_[widgetIndex]);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widgets_[widgetIndex], popupRect_));
    StartCollectionPopupAnimation();
    if (popupAnchoredToDock_)
    {
        PersistentDockHost* host =
            collectionPopupDockHost_;
        if (host &&
            IsPersistentDockHostEffectivelyFloating(*host))
        {
            SelectPersistentDockHost(host);
            UpdateFloatingDockWindowBounds(*host);
            InvalidateFloatingDockWindow(*host, true);
        }
    }
    InvalidateDragStaticScene();
    if (hwnd_ && IsWindow(hwnd_))
    {
        RECT dirty = popupRect_;
        InflateRect(&dirty, 6, 6);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
    UpdateFloatingPopupWindowBounds(true);
}

void DesktopApp::ShowDockFolderPopupSortMenu(
    POINT screenPoint)
{
    if (!dockFolderPopupOpen_) return;

    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    HMENU nameMenu = CreatePopupMenu();
    HMENU typeMenu = CreatePopupMenu();
    HMENU dateMenu = CreatePopupMenu();
    if (!menu || !nameMenu ||
        !typeMenu || !dateMenu)
    {
        if (menu) DestroyMenu(menu);
        if (nameMenu) DestroyMenu(nameMenu);
        if (typeMenu) DestroyMenu(typeMenu);
        if (dateMenu) DestroyMenu(dateMenu);
        return;
    }

    auto appendDirectionMenu = [](
        HMENU parent, HMENU child,
        UINT ascendingCommand,
        UINT descendingCommand,
        const wchar_t* label) {
        AppendMenuW(
            child, MF_STRING,
            ascendingCommand,
            _LW("app.menu.sort_asc"));
        AppendMenuW(
            child, MF_STRING,
            descendingCommand,
            _LW("app.menu.sort_desc"));
        AppendMenuW(
            parent, MF_POPUP,
            reinterpret_cast<UINT_PTR>(
                child),
            label);
    };
    appendDirectionMenu(
        menu, nameMenu,
        kContextWidgetSortByName,
        kContextWidgetSortByNameDesc,
        _LW("app.menu.sort_name"));
    appendDirectionMenu(
        menu, typeMenu,
        kContextWidgetSortByType,
        kContextWidgetSortByTypeDesc,
        _LW("app.menu.sort_type"));
    appendDirectionMenu(
        menu, dateMenu,
        kContextWidgetSortByDate,
        kContextWidgetSortByDateDesc,
        _LW("app.interact.sort_date"));

    const int mode =
        snowdesktop::folder_sort_rules::
            NormalizeMode(
                dockFolderPopupWidget_.
                    folderSortMode);
    HMENU checkedMenu = nullptr;
    UINT checkedCommand = 0;
    if (mode ==
        snowdesktop::folder_sort_rules::kName)
    {
        checkedMenu = nameMenu;
        checkedCommand =
            dockFolderPopupWidget_.
                    folderSortAscending
                ? kContextWidgetSortByName
                : kContextWidgetSortByNameDesc;
    }
    else if (mode ==
        snowdesktop::folder_sort_rules::kType)
    {
        checkedMenu = typeMenu;
        checkedCommand =
            dockFolderPopupWidget_.
                    folderSortAscending
                ? kContextWidgetSortByType
                : kContextWidgetSortByTypeDesc;
    }
    else if (mode ==
        snowdesktop::folder_sort_rules::
            kModified)
    {
        checkedMenu = dateMenu;
        checkedCommand =
            dockFolderPopupWidget_.
                    folderSortAscending
                ? kContextWidgetSortByDate
                : kContextWidgetSortByDateDesc;
    }
    if (checkedMenu && checkedCommand != 0)
        CheckMenuItem(
            checkedMenu, checkedCommand,
            MF_BYCOMMAND | MF_CHECKED);

    SetMenuItemIcon(
        menu,
        reinterpret_cast<UINT_PTR>(
            nameMenu),
        snowdesktop::menu_fluent_glyphs::kSortName,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(nameMenu, kContextWidgetSortByName,
        snowdesktop::menu_fluent_glyphs::kSortNameAscending,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(nameMenu, kContextWidgetSortByNameDesc,
        snowdesktop::menu_fluent_glyphs::kSortNameDescending,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(
        menu,
        reinterpret_cast<UINT_PTR>(
            typeMenu),
        snowdesktop::menu_fluent_glyphs::kSortType,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(typeMenu, kContextWidgetSortByType,
        snowdesktop::menu_fluent_glyphs::kSortTypeAscending,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(typeMenu, kContextWidgetSortByTypeDesc,
        snowdesktop::menu_fluent_glyphs::kSortTypeDescending,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(
        menu,
        reinterpret_cast<UINT_PTR>(
            dateMenu),
        snowdesktop::menu_fluent_glyphs::kSortDate,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(dateMenu, kContextWidgetSortByDate,
        snowdesktop::menu_fluent_glyphs::kSortDateAscending,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(dateMenu, kContextWidgetSortByDateDesc,
        snowdesktop::menu_fluent_glyphs::kSortDateDescending,
        MenuIconFont::FluentRegular);
    SetForegroundWindow(hwnd_);
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();

    switch (command)
    {
    case kContextWidgetSortByName:
        SortDockFolderPopupContents(
            snowdesktop::folder_sort_rules::
                kName, true);
        break;
    case kContextWidgetSortByNameDesc:
        SortDockFolderPopupContents(
            snowdesktop::folder_sort_rules::
                kName, false);
        break;
    case kContextWidgetSortByType:
        SortDockFolderPopupContents(
            snowdesktop::folder_sort_rules::
                kType, true);
        break;
    case kContextWidgetSortByTypeDesc:
        SortDockFolderPopupContents(
            snowdesktop::folder_sort_rules::
                kType, false);
        break;
    case kContextWidgetSortByDate:
        SortDockFolderPopupContents(
            snowdesktop::folder_sort_rules::
                kModified, true);
        break;
    case kContextWidgetSortByDateDesc:
        SortDockFolderPopupContents(
            snowdesktop::folder_sort_rules::
                kModified, false);
        break;
    default:
        break;
    }
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
}



bool DesktopApp::IsOpenDockFolderPopupDropTarget(
    const Container* targetContainer,
    const Item* targetItem) const
{
    const bool targetContainerIsPopup =
        targetContainer &&
        targetContainer == dockFolderPopupContainer_.get();
    const bool targetItemContainerIsPopup =
        targetItem &&
        targetItem->GetContainer() ==
            dockFolderPopupContainer_.get();

    bool targetMatchesDockEntry = false;
    if (const auto* dockTarget =
            dynamic_cast<const DockEntryItem*>(targetItem))
    {
        const size_t entryIndex =
            dockTarget->GetEntryIndex();
        if (entryIndex < dockEntries_.size() &&
            IsFolderDockEntry(dockEntries_[entryIndex]))
        {
            const DockEntry& entry =
                dockEntries_[entryIndex];
            const std::wstring sourceId =
                std::to_wstring(
                    static_cast<int>(entry.type)) +
                L":" +
                ToUpperInvariant(entry.reference);
            targetMatchesDockEntry =
                sourceId == dockFolderPopupSourceId_;
        }
    }

    const std::wstring targetPath =
        targetItem ? targetItem->GetPath() : L"";
    const bool targetMatchesFolderPath =
        !targetPath.empty() &&
        !dockFolderPopupWidget_.sourceFolderPath.empty() &&
        PathsEqualInsensitive(
            targetPath,
            dockFolderPopupWidget_.sourceFolderPath);

    return snowdesktop::dock_folder_rules::
        OpenPopupNeedsRefreshAfterDrop(
            dockFolderPopupOpen_,
            targetContainerIsPopup,
            targetItemContainerIsPopup,
            targetMatchesDockEntry,
            targetMatchesFolderPath);
}

void DesktopApp::RefreshDockFolderPopup()
{
    if (shellFileOperationInFlight_ > 0)
    {
        shellDockFolderPopupRefreshPending_ = true;
        return;
    }
    shellDockFolderPopupRefreshPending_ = false;
    if (!dockFolderPopupOpen_) return;
    CancelDockFolderPopupIconLoads();
    PreserveDockFolderPopupDragSourceForTransition();
    ClearPopupDragTarget();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    for (auto& entry : dockFolderPopupWidget_.folderEntries)
        entry.selected = false;
    if (!dockFolderPopupMappingWidgetId_.empty())
    {
        const size_t widgetIndex =
            FindWidgetIndexById(
                dockFolderPopupMappingWidgetId_);
        if (widgetIndex < widgets_.size() &&
            widgets_[widgetIndex].type ==
                DesktopWidgetType::FolderMapping)
        {
            const DesktopWidget& source =
                widgets_[widgetIndex];
            dockFolderPopupWidget_.
                sourceFolderPath =
                    source.sourceFolderPath;
            dockFolderPopupWidget_.
                folderSortMode =
                    source.folderSortMode;
            dockFolderPopupWidget_.
                folderSortAscending =
                    source.folderSortAscending;
            dockFolderPopupWidget_.itemKeys =
                source.itemKeys;
            dockFolderPopupWidget_.listMode =
                source.listMode;
            dockFolderPopupWidget_.showDetails =
                source.showDetails;
            dockFolderPopupWidget_.detailShowModified =
                source.detailShowModified;
            dockFolderPopupWidget_.detailShowType =
                source.detailShowType;
            dockFolderPopupWidget_.detailShowSize =
                source.detailShowSize;
            dockFolderPopupWidget_.detailModifiedPosition =
                source.detailModifiedPosition;
            dockFolderPopupWidget_.detailTypePosition =
                source.detailTypePosition;
            dockFolderPopupWidget_.detailSizePosition =
                source.detailSizePosition;
            dockFolderPopupWidget_.contentSortColumn =
                source.contentSortColumn;
            dockFolderPopupWidget_.contentSortAscending =
                source.contentSortAscending;
        }
        else
        {
            dockFolderPopupMappingWidgetId_.
                clear();
        }
    }
    if (dockFolderPopupAvailable_)
    {
        EnumerateFolderMappingEntries(
            dockFolderPopupWidget_, true);
        if (ApplyPendingFolderPlacements(
                dockFolderPopupWidget_,
                dockFolderPopupMappingWidgetId_,
                dockFolderPopupSourceId_))
        {
            CommitDockFolderPopupStateToSource();
        }
    }
    else
        ClearDockFolderPopupEntries();
    dockFolderPopupContainer_ =
        std::make_unique<FolderMapping>(
            &dockFolderPopupWidget_, this);
    dockFolderPopupContainer_->InvalidateFilterCache();
    RefreshDockFolderPopupGeometry();
}

void DesktopApp::RefreshDockFolderPopupGeometry()
{
    popupRect_ =
        GetCollectionPopupRect(
            dockFolderPopupWidget_);
    dockFolderPopupWidget_.bounds =
        popupRect_;
    popupScrollOffset_ = std::clamp(
        popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(
            dockFolderPopupWidget_,
            popupRect_));
    InvalidateDragStaticScene();
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
    UpdateFloatingPopupWindowBounds(true);
}

void DesktopApp::RefreshOpenCollectionPopupGeometry()
{
    if (!GetOpenPopupWidget())
        return;

    if (dockFolderPopupOpen_)
    {
        RefreshDockFolderPopupGeometry();
        return;
    }

    if (popupWidgetIndex_ >= widgets_.size())
        return;

    popupRect_ =
        GetCollectionPopupRect(
            widgets_[popupWidgetIndex_]);
    popupScrollOffset_ = std::clamp(
        popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(
            widgets_[popupWidgetIndex_],
            popupRect_));
    InvalidateDragStaticScene();
    UpdateFloatingPopupWindowBounds(true);
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::
CommitDockFolderPopupStateToSource()
{
    if (!dockFolderPopupOpen_)
        return;

    if (!dockFolderPopupMappingWidgetId_.empty())
    {
        const size_t sourceIndex =
            FindWidgetIndexById(
                dockFolderPopupMappingWidgetId_);
        if (sourceIndex < widgets_.size() &&
            widgets_[sourceIndex].type ==
                DesktopWidgetType::FolderMapping)
        {
            DesktopWidget& source =
                widgets_[sourceIndex];
            source.folderSortMode =
                snowdesktop::folder_sort_rules::
                    NormalizeMode(
                        dockFolderPopupWidget_.
                            folderSortMode);
            source.folderSortAscending =
                dockFolderPopupWidget_.
                    folderSortAscending;
            source.itemKeys =
                dockFolderPopupWidget_.itemKeys;
            source.listMode =
                dockFolderPopupWidget_.listMode;
            source.showDetails =
                dockFolderPopupWidget_.showDetails;
            source.detailShowModified =
                dockFolderPopupWidget_.detailShowModified;
            source.detailShowType =
                dockFolderPopupWidget_.detailShowType;
            source.detailShowSize =
                dockFolderPopupWidget_.detailShowSize;
            source.detailModifiedPosition =
                dockFolderPopupWidget_.detailModifiedPosition;
            source.detailTypePosition =
                dockFolderPopupWidget_.detailTypePosition;
            source.detailSizePosition =
                dockFolderPopupWidget_.detailSizePosition;
            source.contentSortColumn =
                dockFolderPopupWidget_.contentSortColumn;
            source.contentSortAscending =
                dockFolderPopupWidget_.contentSortAscending;
            RefreshFolderMappingWidget(
                sourceIndex);
            SaveLayoutSlots();
            return;
        }
        dockFolderPopupMappingWidgetId_.clear();
    }

    for (DockEntry& entry : dockEntries_)
    {
        const std::wstring sourceId =
            std::to_wstring(
                static_cast<int>(entry.type)) +
            L":" +
            ToUpperInvariant(entry.reference);
        if (sourceId !=
                dockFolderPopupSourceId_ ||
            !IsFolderDockEntry(entry))
            continue;

        entry.folderSortMode =
            snowdesktop::folder_sort_rules::
                NormalizeMode(
                    dockFolderPopupWidget_.
                        folderSortMode);
        entry.folderSortAscending =
            dockFolderPopupWidget_.
                folderSortAscending;
        entry.folderItemKeys =
            dockFolderPopupWidget_.itemKeys;
        entry.listMode =
            dockFolderPopupWidget_.listMode;
        entry.detailShowModified =
            dockFolderPopupWidget_.detailShowModified;
        entry.detailShowType =
            dockFolderPopupWidget_.detailShowType;
        entry.detailShowSize =
            dockFolderPopupWidget_.detailShowSize;
        entry.detailModifiedPosition =
            dockFolderPopupWidget_.detailModifiedPosition;
        entry.detailTypePosition =
            dockFolderPopupWidget_.detailTypePosition;
        entry.detailSizePosition =
            dockFolderPopupWidget_.detailSizePosition;
        SaveLayoutSlots();
        return;
    }
}







void DesktopApp::SortDockFolderPopupContents(
    int mode, bool ascending)
{
    if (!dockFolderPopupOpen_) return;
    PreserveDockFolderPopupDragSourceForTransition();
    mode =
        snowdesktop::folder_sort_rules::
            NormalizeMode(mode);
    if (mode ==
        snowdesktop::folder_sort_rules::kManual)
        return;

    const size_t sourceIndex =
        dockFolderPopupMappingWidgetId_.empty()
            ? static_cast<size_t>(-1)
            : FindWidgetIndexById(
                dockFolderPopupMappingWidgetId_);
    if (sourceIndex < widgets_.size() &&
        widgets_[sourceIndex].type ==
            DesktopWidgetType::FolderMapping)
    {
        // The mapping widget is the single source of truth, regardless of
        // whether it is hosted on the desktop, in a FileGroup or in Dock.
        SortWidgetContents(
            sourceIndex, mode, ascending);
        RefreshDockFolderPopup();
    }
    else
    {
        dockFolderPopupMappingWidgetId_.clear();
        dockFolderPopupWidget_.folderSortMode =
            mode;
        dockFolderPopupWidget_.
            folderSortAscending = ascending;
        dockFolderPopupWidget_.contentSortColumn =
            snowdesktop::list_detail_rules::
                FromLegacyFolderSortMode(mode);
        dockFolderPopupWidget_.contentSortAscending =
            ascending;
        snowdesktop::folder_sort_rules::
            StableSort(
                dockFolderPopupWidget_.
                    folderEntries,
                mode, ascending);
        snowdesktop::folder_sort_rules::
            RewriteOrderKeys(
                dockFolderPopupWidget_.
                    folderEntries,
                dockFolderPopupWidget_.
                    itemKeys);
        if (dockFolderPopupContainer_)
            dockFolderPopupContainer_->
                InvalidateFilterCache();
        CommitDockFolderPopupStateToSource();
    }

    popupScrollOffset_ = 0;
    RefreshDockFolderPopupGeometry();
}
