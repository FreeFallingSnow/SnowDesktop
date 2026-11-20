#include "app/app.h"
#include "member_drop_layout.h"

DWORD DesktopApp::CommitInternalDragRelease(POINT point, DWORD keys, DWORD allowed)
{
    ResolveCurrentDragTargetAt(point);
    auto* target = dragSession_.TargetContainer();
    auto* slot = dragSession_.TargetSlot();
    const auto region = dragSession_.TargetRegion();
    const int mods = keys & (MK_CONTROL | MK_SHIFT | MK_ALT);
    const bool removeDockPin = !GetDockDragOutRemovalHint(point).empty();
    const bool canDrop = target && region != HitRegion::None && region != HitRegion::Blocked;
    const bool handoff = canDrop && region == HitRegion::Handoff;
    const auto sources = dragSession_.SourceList();
    const auto items = dragSession_.Items();
    DropPreviewList preview;
    if (canDrop && !removeDockPin && !handoff)
        preview = GetCachedDesktopDropPreview(true, sources, target, slot, region, mods, point);

    const bool submitVisualEnd = !removeDockPin && (handoff || preview.fileBacked);
    if (submitVisualEnd)
        HoldReleasingFileSources(handoff ? DropActionFromClipboardEffect(ChooseDropEffect(keys, allowed)) : preview.action);
    dragSession_.DeactivateForDrop();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    ReleaseCapturePreservingPointerState();
    if (submitVisualEnd) CommitDragVisualEndBeforeShellOperation();

    DWORD effect = DROPEFFECT_NONE;
    bool reload = false;
    bool layout = false;
    bool save = true;
    bool refreshPopup = false;
    if (removeDockPin && (allowed & DROPEFFECT_MOVE))
    {
        layout = RemoveDockDragOutItems(items);
        if (layout) effect = DROPEFFECT_MOVE;
    }
    else if (canDrop && allowed && dragSession_.HasContext() && target == dragSession_.TargetContainer() &&
        region == dragSession_.TargetRegion() && slot == dragSession_.TargetSlot())
    {
        auto* item = slot ? slot->GetItem() : nullptr;
        if (!item && handoff && target == GetDesktopGrid()) item = HitTestIcon(point);
        refreshPopup = IsOpenDockFolderPopupDropTarget(target, item) ||
            (dockFolderPopupOpen_ && sources.origin == dockFolderPopupContainer_.get());
        const ItemDrop drop{items, sources, preview, slot, region, mods, point, allowed};
        if (handoff)
        {
            effect = CommitItemHandoff(item, drop, keys, refreshPopup);
            // The asynchronous file callback refreshes the popup when its contents change.
            refreshPopup = false;
        }
        else
        {
            const bool logical = target->GetSlotSurfaceKind() == snowdesktop::slot_contract::SlotSurfaceKind::LuaLogicalSlot;
            save = !logical;
            reload = target->NeedsShellReloadAfterDrop();
            const bool dockReorder = dynamic_cast<DockContainer*>(target) && dynamic_cast<DockContainer*>(sources.origin);
            const bool desktop = target == GetDesktopGrid();
            const bool handlesLayout = preview.fileBacked || (snowdesktop::member_drop_layout::OnlyReordersMembers(
                sources, preview) && !GetOpenPopupWidget() && !IsGroupedWidget(*preview.targetWidget));
            const auto revision = layoutPassRevision_;
            effect = target->OnItemsDropped(drop).Effect();
            layout = !handlesLayout && !dockReorder && !logical && (!desktop || revision == layoutPassRevision_);
        }
    }

    if (effect && save) RequestLayoutSave();
    if (canDrop || removeDockPin) ClearSelection();
    if (dragSession_.HasContext()) EndDragSession();
    if (effect && refreshPopup && dockFolderPopupOpen_) RefreshDockFolderPopup();
    if (effect && reload)
    {
        RebuildContainersAndItems();
        ReloadItems();
    }
    else if (effect && layout)
    {
        if (!removeDockPin) ApplyPageMapping();
        LayoutItems();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return effect;
}

DWORD DesktopApp::CommitItemHandoff(Item* item, const ItemDrop& drop, DWORD keys, bool refreshPopup)
{
    if (!item) return DROPEFFECT_NONE;
    const auto& items = drop.items;
    const auto& sources = drop.sources;
    const DWORD allowed = drop.allowedEffects;
    POINT point = drop.point;
    const bool dockSource = dynamic_cast<DockContainer*>(sources.origin) != nullptr;
    const bool recycle = IsRecycleDropTarget(item);
    bool explicitFolder = IsOpenDockFolderPopupDropTarget(dragSession_.TargetContainer(), item);
    if (auto* dock = dynamic_cast<DockEntryItem*>(item))
    {
        const auto index = dock->GetEntryIndex();
        explicitFolder = index < dockEntries_.size() && IsFolderDockEntry(dockEntries_[index]);
        if (dockSource && recycle)
        {
            if (!(allowed & DROPEFFECT_MOVE) || !RemoveDockDragOutItems(items)) return DROPEFFECT_NONE;
            LayoutItems();
            return DROPEFFECT_MOVE;
        }
        if (IsLogicalDockEntryType(dock->GetEntryType()))
        {
            const auto receipt = DropItemsIntoDockCollection(drop, dock);
            if (receipt.action)
            {
                LayoutItems();
                if (refreshPopup && dockFolderPopupOpen_) RefreshDockFolderPopup();
            }
            return receipt.Effect();
        }
    }
    if (dockSource && recycle)
        return (allowed & DROPEFFECT_MOVE) && MoveDockItemsToDesktop(items, ResolveDesktopRequestCell(sources, point))
            ? DROPEFFECT_MOVE : DROPEFFECT_NONE;

    const auto originalPath = item->GetPath();
    const auto targetPath = ResolveFileHandoffPath(originalPath);
    if (!targetPath) return DROPEFFECT_NONE;
    if (explicitFolder && (keys & (MK_CONTROL | MK_ALT | MK_SHIFT)) == 0 && (allowed & DROPEFFECT_MOVE))
        keys |= MK_SHIFT;
    const DWORD selected = ChooseDropEffect(keys, allowed);
    if (!selected) return DROPEFFECT_NONE;
    const auto paths = sources.FilePaths();
    const auto refreshFolder = [this, refreshPopup](bool succeeded) {
        if (succeeded && refreshPopup && dockFolderPopupOpen_) RefreshDockFolderPopup();
    };
    const auto finished = [this, refreshFolder](bool succeeded) {
        if (succeeded) RequestShellRefresh();
        refreshFolder(succeeded);
    };
    const DWORD attributes = targetPath->empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(targetPath->c_str());
    if (!paths.empty() && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        MaterializeFilesToFolder(sources, *targetPath, DropActionFromClipboardEffect(selected), finished))
        return selected;

    ClientToScreen(hwnd_, &point);
    const POINTL screen{point.x, point.y};
    if (!paths.empty() && paths.size() == sources.entries.size() && !targetPath->empty() &&
        QueueShellDrop(paths, *targetPath, keys, screen, allowed, refreshFolder, recycle))
        return selected;

    // A namespace item or a rejected queue submission needs the synchronous Shell protocol.
    ComPtr<IDropTarget> target;
    if (auto* icon = dynamic_cast<DesktopIcon*>(item))
    {
        const auto* model = icon->GetDesktopItem();
        if (model && model->childPidl.get() && desktopFolder_)
        {
            PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(model->childPidl.get());
            desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                reinterpret_cast<void**>(target.GetAddressOf()));
        }
    }
    if (!target && !originalPath.empty())
    {
        ComPtr<IShellItem> shellItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(originalPath.c_str(), nullptr, IID_PPV_ARGS(&shellItem))))
            shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target));
    }
    if (!target) return DROPEFFECT_NONE;
    auto data = CreateDataObjectForItems(items);
    if (!data) return DROPEFFECT_NONE;
    DwmFlush();
    DWORD effect = allowed;
    if (FAILED(target->DragEnter(data.Get(), keys, screen, &effect))) return DROPEFFECT_NONE;
    effect = allowed;
    if (FAILED(target->DragOver(keys, screen, &effect)) || !effect)
    {
        target->DragLeave();
        return DROPEFFECT_NONE;
    }
    effect = allowed;
    if (FAILED(target->Drop(data.Get(), keys, screen, &effect))) return DROPEFFECT_NONE;
    finished(true);
    return effect & allowed;
}
