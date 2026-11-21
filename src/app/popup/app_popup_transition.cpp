#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "common/l10n.h"
#include "diagnostics/diagnostic_log.h"
#include "common/utils.h"
#include "dock/dock_folder_rules.h"
#include "layout/folder_sort_rules.h"
#include "app/dock/persistent_dock_host.h"
#include "app/dock/dock_folder_popup_read.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "app/menus/sort_menu.h"
#include "shell/item_sort_metadata.h"
#include "settings/animation_settings.h"
#include "layout/collection_popup_layout.h"
#include "layout/list_detail_rules.h"
#include "popup_resize_transition.h"
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <utility>

// Collection and Dock-folder popup transitions.

namespace
{
bool SameDockFolderListing(const std::vector<FolderEntry>& displayed,
    const snowdesktop::shell_refresh::FolderSnapshot& snapshot)
{
    if (displayed.size() != snapshot.entries.size()) return false;
    // The popup owns its sort order. A directory read returns enumeration
    // order and may leave icon/type metadata to the existing icon loader.
    std::unordered_map<std::wstring_view, const FolderEntry*> byPath;
    byPath.reserve(displayed.size());
    for (const auto& entry : displayed)
        byPath.emplace(entry.fullPath, &entry);
    for (const auto& fresh : snapshot.entries)
    {
        const auto found = byPath.find(fresh.fullPath);
        if (found == byPath.end()) return false;
        const auto& current = *found->second;
        if (current.name != fresh.name || current.DisplayName() != fresh.DisplayName() ||
            current.isDirectory != fresh.isDirectory || current.fileSize != fresh.fileSize ||
            CompareFileTime(&current.lastWriteTime, &fresh.lastWriteTime) != 0 ||
            (fresh.sysIconIndex >= 0 && current.sysIconIndex != fresh.sysIconIndex) ||
            (!fresh.typeName.empty() && current.typeName != fresh.typeName) ||
            current.iconState == IconState::Loading)
            return false;
        byPath.erase(found);
    }
    return byPath.empty();
}
}

namespace snowdesktop::popup_resize_transition
{
namespace
{
RECT Interpolate(RECT from, RECT to, float amount)
{
    const auto edge = [amount](LONG start, LONG end) {
        return static_cast<LONG>(std::lround(std::lerp(static_cast<double>(start),
            static_cast<double>(end), static_cast<double>(amount))));
    };
    return {edge(from.left, to.left), edge(from.top, to.top),
        edge(from.right, to.right), edge(from.bottom, to.bottom)};
}
}

Transition::Transition(Geometry from, Geometry target, double now, double durationScale)
    : from_(from), target_(target)
{
    progress_.SetVisible(true, now, true, durationScale);
}

bool Transition::Advance(double now)
{
    progress_.Advance(now);
    return progress_.IsAnimating();
}

bool Transition::Targets(const Geometry& geometry) const
{
    return EqualRect(&geometry.panel, &target_.panel) && EqualRect(&geometry.content, &target_.content);
}

RECT Transition::Panel(RECT target) const
{
    return EqualRect(&target, &target_.panel)
        ? Interpolate(from_.panel, target, progress_.Amount()) : target;
}

RECT Transition::ReservedPanel(RECT target) const
{
    RECT bounds = target;
    if (EqualRect(&target, &target_.panel)) UnionRect(&bounds, &from_.panel, &target);
    return bounds;
}

RECT Transition::Content(RECT target, RECT layoutPanel) const
{
    if (!Targets({layoutPanel, target})) return target;
    const RECT panel = Panel(layoutPanel);
    // The new page's chrome is already visible. Its insets stay attached to
    // the moving frame even if the old page had a different header.
    const LONG top = panel.top + target.top - layoutPanel.top;
    const LONG left = panel.left + target.left - layoutPanel.left;
    return {left, top,
        std::max(left, panel.right - (layoutPanel.right - target.right)),
        std::max(top, panel.bottom - (layoutPanel.bottom - target.bottom))};
}
}

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
            IsCollectionPopupHostedByFloatingWindow() ? 1 : 0,
            mouseDown_ ? 1 : 0,
            snowdesktop::private_pointer::Capture());
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message);
    }
    if (widgetIndex >= widgets_.size() ||
        (widgets_[widgetIndex].type != DesktopWidgetType::Collection &&
         widgets_[widgetIndex].type != DesktopWidgetType::FileCategories &&
         widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping &&
         !IsGroupWidgetType(widgets_[widgetIndex].type)))
        return;

    CancelCollectionPopupDwell();

    DismissActiveContextMenuForPopupTransition();
    SuspendInlineFolderPreview();

    PersistentDockHost* requestedDockHost = nullptr;
    if (DockContainer* requestedDock =
            GetDockContainerAtPoint(anchorPoint))
    {
        if (DockEntryItem* requestedItem =
                requestedDock->EntryAtPoint(anchorPoint);
            requestedItem &&
            IsLogicalDockEntryType(requestedItem->GetEntryType()) &&
            requestedItem->GetReference() ==
                widgets_[widgetIndex].id)
        {
            requestedDockHost =
                FindPersistentDockHost(requestedDock);
        }
    }
    const bool mappingSource = widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping;
    const bool samePopupSource =
        (mappingSource
            ? dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ == widgets_[widgetIndex].id
            : !dockFolderPopupOpen_ && popupWidgetIndex_ == widgetIndex) &&
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
        ContinueCollectionPopupSwitch();
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

    if (mappingSource)
    {
        OpenDockFolderPopupAt(static_cast<size_t>(-1), anchorPoint, widgetIndex);
        return;
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
            IsLogicalDockEntryType(dockItem->GetEntryType()) &&
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

    if (dockFolderPopupContainer_) dockFolderPopupContainer_->SetSearchFocused(false);
    if (dockFolderPopupOpen_)
        CancelDockFolderPopupIconLoads();
    ResetDockTrayPopup();
    PreserveDockFolderPopupDragSourceForTransition();
    ClearPopupDragTarget();
    dockFolderPopupOpen_ = false;
    SyncFolderChangeNotifications();
    dockFolderPopupAvailable_ = false;
    dockFolderPopupLoading_ = false;
    dockFolderPopupKnownItemCount_ = 0;
    dockFolderPopupContainer_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    dockFolderPopupSourceId_.clear();
    dockFolderPopupMappingWidgetId_.clear();
    ClearDockFolderPopupEntries();
    popupWidgetIndex_ = widgetIndex;
    popupScrollOffset_ = 0;
    ResetCollectionPopupFanScroll();
    popupFanShowAll_ = false;
    popupFanActionFocused_ = false;
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
                dockItem && IsLogicalDockEntryType(dockItem->GetEntryType()) &&
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
    GetCategorizedPopupView();
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widgets_[widgetIndex], popupRect_));
    (void)ResumeFolderPreviewForPopup(widgets_[widgetIndex].id, {});
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
    InvalidateCollectionPopupDesktopScene();
    UpdateFloatingPopupWindowBounds(true);
}

void DesktopApp::ShowDockFolderPopupSortMenu(
    POINT screenPoint)
{
    if (!dockFolderPopupOpen_) return;

    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = snowdesktop::sort_menu::Create(
        [](const char* key) { return _LW(key); });
    if (!menu)
    {
        ClearMenuIcons();
        return;
    }

    const int mode = snowdesktop::folder_sort_rules::NormalizeMode(
        dockFolderPopupWidget_.folderSortMode);
    for (const auto& row : snowdesktop::sort_menu::kRows)
        if (row.mode == mode)
            CheckMenuItem(menu,
                dockFolderPopupWidget_.folderSortAscending
                    ? row.ascendingCommand : row.descendingCommand,
                MF_BYCOMMAND | MF_CHECKED);

    snowdesktop::sort_menu::SetIcons(menu,
        [this](HMENU target, UINT commandId, const wchar_t* glyph) {
            SetMenuItemIcon(target, commandId, glyph, MenuIconFont::FluentRegular);
        });
    RestoreInteractionInputFocus();
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();

    if (const auto sort = snowdesktop::sort_menu::ResolveCommand(command))
        SortDockFolderPopupContents(sort->mode, sort->ascending);
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

void DesktopApp::RefreshDockFolderPopup(
    const snowdesktop::shell_refresh::FolderSnapshot* snapshot, bool present)
{
    if (shellFileOperationInFlight_ > 0)
    {
        shellDockFolderPopupRefreshPending_ = true;
        return;
    }
    shellDockFolderPopupRefreshPending_ = false;
    if (!dockFolderPopupOpen_) return;
    const bool dockEntryOwnsBinding = !folderPreview_ && dockFolderPopupMappingWidgetId_.empty();
    if (snapshot && folderPreview_ &&
        snowdesktop::shell_refresh::FolderKey(snapshot->path) !=
            snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath))
        return;
    const bool previewDirectoryLoaded = folderPreview_ && snapshot && dockFolderPopupLoading_ &&
        !dockFolderPopupAvailable_;
    if (snapshot && dockEntryOwnsBinding &&
        snowdesktop::shell_refresh::FolderKey(snapshot->path) !=
            snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath))
        return;

    bool targetPending = false;
    // After binding, deliver a directory listing just as a mapped folder does.
    // Only a separate target update can rebind the popup to a different folder.
    if (!snapshot && dockEntryOwnsBinding)
    {
        const auto source = std::find_if(dockEntries_.begin(), dockEntries_.end(), [&](const auto& entry) {
            return std::to_wstring(static_cast<int>(entry.type)) + L":" +
                ToUpperInvariant(entry.reference) == dockFolderPopupSourceId_;
        });
        const auto target = source != dockEntries_.end()
            ? ResolveDockFolderTarget(*source, &targetPending)
            : snowdesktop::item_location::FolderTarget{};
        if (snowdesktop::dock_folder_popup_read::BindTarget(target.path, targetPending,
                dockFolderPopupWidget_.sourceFolderPath,
                dockFolderPopupAvailable_, dockFolderPopupLoading_))
        {
            PreserveDockFolderPopupDragSourceForTransition();
            ClearPopupDragTarget();
            ClearPopupMouseDownItem();
            CancelDockFolderPopupIconLoads();
            ClearDockFolderPopupEntries();
            popupScrollOffset_ = 0;
            SyncFolderChangeNotifications();
        }
        else if (!targetPending && dockFolderPopupAvailable_)
        {
            QueueFolderRead(dockFolderPopupWidget_.sourceFolderPath);
            return;
        }
    }
    const bool refreshVisibleDockFolder = snapshot && snapshot->complete && present &&
        !targetPending && popupAnchoredToDock_ && dockFolderPopupMappingWidgetId_.empty() &&
        dockFolderPopupAvailable_ && !dockFolderPopupLoading_ &&
        dockFolderPopupContainer_ && IsCollectionPopupInteractive() &&
        !popupAnimation_.IsAnimating() && !popupAnimationCompositorDriven_ &&
        floatingPopupHwnd_ && IsWindowVisible(floatingPopupHwnd_);
    if (refreshVisibleDockFolder && pendingLandingCaches_.empty() &&
        (snapshot->error == ERROR_SUCCESS || snapshot->error == ERROR_FILE_NOT_FOUND) &&
        SameDockFolderListing(dockFolderPopupWidget_.folderEntries, *snapshot))
        return; // The file-operation completion and Shell change can report the same listing.

    const RECT previousBounds = popupRect_;
    if (!snapshot)
        CancelDockFolderPopupIconLoads();
    PreserveDockFolderPopupDragSourceForTransition();
    ClearPopupDragTarget();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    if (!snapshot)
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
            dockFolderPopupWidget_.listColumns = source.listColumns;
            dockFolderPopupWidget_.showSearchBox = source.showSearchBox;
            dockFolderPopupWidget_.showFileCategories = source.showFileCategories;
            dockFolderPopupWidget_.dateHeaders = source.dateHeaders;
            dockFolderPopupWidget_.fanPopup = source.fanPopup;
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
    using namespace snowdesktop::popup_resize_transition;
    const auto popupGeometry = [this]() {
        const RECT panel = GetCollectionPopupRect(dockFolderPopupWidget_);
        return Geometry{panel, GetCollectionPopupContentRect(panel)};
    };
    std::optional<Geometry> before;
    if (!folderPreview_ && snapshot && snapshot->complete && snapshot->entries.size() != dockFolderPopupWidget_.folderEntries.size() &&
        present && dockFolderPopupAvailable_ && dockFolderPopupContainer_ &&
        IsCollectionPopupInteractive() && !popupAnimation_.IsAnimating() &&
        !UsesCollectionPopupFan(dockFolderPopupWidget_) && !UsesCollectionPopupList(dockFolderPopupWidget_) &&
        !dockFolderPopupWidget_.dateHeaders && !mouseDown_ && !dragSession_.HasContext() &&
        !dragDropController_.IsTransportActive() &&
        snowdesktop::animation::RuntimePopupEffect() != snowdesktop::animation::NoEffect)
    {
        before = popupGeometry();
        if (const auto& active = dockFolderPopupContainer_->PopupResizeTransition())
        {
            before->content = active->Content(before->content, before->panel);
            before->panel = active->Panel(before->panel);
        }
    }

    const bool accepted = snowdesktop::dock_folder_popup_read::Refresh(
        dockFolderPopupWidget_.sourceFolderPath, snapshot,
        dockFolderPopupAvailable_, dockFolderPopupLoading_,
        [this](const auto& path) { QueueFolderRead(path); },
        [this](const auto& folder) {
            EnumerateFolderMappingEntries(dockFolderPopupWidget_, true, &folder);
            if (!folderPreview_ && dockFolderPopupAvailable_ && !dragSession_.HasContext() &&
                !dragDropController_.IsTransportActive() && ApplyPendingFolderPlacements(
                    dockFolderPopupWidget_, dockFolderPopupMappingWidgetId_, dockFolderPopupSourceId_))
                CommitDockFolderPopupStateToSource();
        }, targetPending);
    if (!accepted) return;
    if (previewDirectoryLoaded)
        popupScrollOffset_ = folderPreview_->pages.back().scroll;
    if (folderPreview_ && snapshot && !dockFolderPopupAvailable_)
        ClearDockFolderPopupEntries();
    if (snapshot && snapshot->complete && dockFolderPopupAvailable_)
        dockFolderPopupKnownItemCount_ = dockFolderPopupWidget_.folderEntries.size();
    if (snapshot)
    {
        // Remember a successfully listed plain Dock folder in memory. Its next
        // open can reserve the real footprint instead of the empty-grid size.
        // Do not persist settings or trigger another folder refresh here.
        if (!folderPreview_ && dockFolderPopupAvailable_ && dockFolderPopupMappingWidgetId_.empty())
        {
            for (auto& entry : dockEntries_)
            {
                const auto sourceId = std::to_wstring(static_cast<int>(entry.type)) +
                    L":" + ToUpperInvariant(entry.reference);
                if (sourceId == dockFolderPopupSourceId_)
                {
                    entry.folderItemKeys = dockFolderPopupWidget_.itemKeys;
                    break;
                }
            }
        }
        const auto message = L"Dock folder popup read: available=" +
            std::to_wstring(dockFolderPopupAvailable_ ? 1 : 0) + L" error=" +
            std::to_wstring(snapshot->error) + L" items=" +
            std::to_wstring(dockFolderPopupWidget_.folderEntries.size()) + L" path=" +
            dockFolderPopupWidget_.sourceFolderPath;
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message.c_str(), DiagnosticLogLevel::Warning);
    }
    if (!dockFolderPopupContainer_)
        dockFolderPopupContainer_ =
            std::make_unique<FolderMapping>(
                &dockFolderPopupWidget_, this);
    dockFolderPopupContainer_->InvalidateFilterCache();
    if (previewDirectoryLoaded && std::exchange(folderPreview_->selectFirstItemOnLoad, false) &&
        folderPreview_->keyboardFocused && dockFolderPopupAvailable_)
    {
        RefreshDockFolderPopupGeometry(false);
        if (!IsFolderPreviewContentPrivate(popupRect_) && GetPopupItemCount(dockFolderPopupWidget_) != 0)
            SelectDockFolderPopupKeyboardItem(0, false);
    }
    if (folderPreview_ && snapshot && folderPreview_->pendingPopupResize)
    {
        const auto from = *folderPreview_->pendingPopupResize;
        folderPreview_->pendingPopupResize.reset();
        if (present) StartFolderPreviewPopupResize(from);
    }
    if (before && dockFolderPopupAvailable_)
    {
        RefreshDockFolderPopupGeometry(false);
        const auto target = popupGeometry();
        const auto& active = dockFolderPopupContainer_->PopupResizeTransition();
        const bool sizeChanged =
            before->panel.right - before->panel.left != target.panel.right - target.panel.left ||
            before->panel.bottom - before->panel.top != target.panel.bottom - target.panel.top;
        if (sizeChanged && (!active || !active->Targets(target)))
        {
            const auto transition = std::make_shared<Transition>(*before, target,
                snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(), snowdesktop::animation::RuntimeDurationScale());
            dockFolderPopupContainer_->SetPopupResizeTransition(transition);
            const auto token = uiAnimationScheduler_.StartAnimation(snowdesktop::UiAnimationSurface::Popup,
                [this, weak = std::weak_ptr<Transition>(transition)](double now) {
                    const auto current = weak.lock();
                    if (!current || !dockFolderPopupOpen_ || !dockFolderPopupContainer_ ||
                        dockFolderPopupContainer_->PopupResizeTransition() != current) return false;
                    const bool keep = IsCollectionPopupInteractive() && !exitRequested_ &&
                        snowdesktop::animation::RuntimePopupEffect() != snowdesktop::animation::NoEffect &&
                        current->Advance(now);
                    if (!keep) dockFolderPopupContainer_->SetPopupResizeTransition({});
                    RequestCollectionPopupResizePresentation(keep);
                    if (!keep && hwnd_)
                        PostMessageW(hwnd_, kBackgroundShellReadyMessage, 0, 0);
                    return keep;
                });
            if (!token) dockFolderPopupContainer_->SetPopupResizeTransition({});
        }
        else if (active && !active->Targets(target))
        {
            dockFolderPopupContainer_->SetPopupResizeTransition({});
        }
    }
    if (refreshVisibleDockFolder && dockFolderPopupAvailable_)
    {
        RefreshDockFolderPopupGeometry(false);
        if (folderPreview_ && folderPreview_->inlineBounds)
            (void)QueueDesktopWidgetComposition(folderPreview_->hostWidgetId);
        InvalidateCollectionPopupContent();
        InvalidateCollectionPopupDesktopScene(previousBounds);
        if (!EqualRect(&previousBounds, &popupRect_))
            UpdateFloatingPopupWindowBounds(false, false);
        InvalidateFloatingPopupWindow(false);
        return;
    }
    // Empty-to-populated listings are content updates of the existing popup.
    // The geometry refresh preserves its current opening or closing timeline.
    RefreshDockFolderPopupGeometry(present);
}

void DesktopApp::RefreshDockFolderPopupGeometry(bool present)
{
    const RECT previousBounds = popupRect_;
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
    if (!present) return;
    if (folderPreview_ && folderPreview_->inlineBounds)
    {
        (void)QueueDesktopWidgetComposition(folderPreview_->hostWidgetId);
        return;
    }
    InvalidateCollectionPopupContent();
    if (popupAnimationCompositorDriven_)
        return; // The queued paint updates pixels; geometry settles at completion.
    InvalidateCollectionPopupDesktopScene(previousBounds);
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

    const RECT previousBounds = popupRect_;
    popupRect_ =
        GetCollectionPopupRect(
            widgets_[popupWidgetIndex_]);
    popupScrollOffset_ = std::clamp(
        popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(
            widgets_[popupWidgetIndex_],
            popupRect_));
    InvalidateCollectionPopupDesktopScene(previousBounds);
    UpdateFloatingPopupWindowBounds(true);
}

void DesktopApp::
CommitDockFolderPopupStateToSource()
{
    if (!dockFolderPopupOpen_ || folderPreview_)
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
            source.listColumns = dockFolderPopupWidget_.listColumns;
            source.showSearchBox = dockFolderPopupWidget_.showSearchBox;
            source.showFileCategories = dockFolderPopupWidget_.showFileCategories;
            source.dateHeaders = dockFolderPopupWidget_.dateHeaders;
            source.categoryTabOrder = dockFolderPopupWidget_.categoryTabOrder;
            source.activeCategoryId = dockFolderPopupWidget_.activeCategoryId;
            source.fanPopup = dockFolderPopupWidget_.fanPopup;
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
        entry.listColumns = dockFolderPopupWidget_.listColumns;
        entry.showSearchBox = dockFolderPopupWidget_.showSearchBox;
        entry.showFileCategories = dockFolderPopupWidget_.showFileCategories;
        entry.dateHeaders = dockFolderPopupWidget_.dateHeaders;
        entry.categoryTabOrder = dockFolderPopupWidget_.categoryTabOrder;
        entry.fanPopup = dockFolderPopupWidget_.fanPopup;
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
    if (folderPreview_)
    {
        if (!dockFolderPopupAvailable_) return;
        const auto key = snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath);
        if (!folderPreviewSort_.Set(key, {mode, ascending}))
        {
            MessageBeep(MB_ICONWARNING);
            return;
        }
    }

    const size_t sourceIndex =
        folderPreview_ || dockFolderPopupMappingWidgetId_.empty()
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
        snowdesktop::item_sort_metadata::PrepareFolder(dockFolderPopupWidget_.folderEntries, mode);
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
        if (folderPreview_) SyncFolderPreviewSortToOtherViews();
        else CommitDockFolderPopupStateToSource();
    }

    popupScrollOffset_ = 0;
    ResetCollectionPopupFanScroll();
    RefreshDockFolderPopupGeometry();
}
