#include "app.h"

// OLE drag-enter/over/leave/drop session handling.

HRESULT DesktopApp::HandleOleDragEnter(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;

    if (dragDropController_.IsSelfDragActive())
    {
        dragDropController_.MarkSelfDragReturned();
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
        }
        UpdateCollectionPopupDwell(client);
        UpdateCollectionGroupTabDwell(client);
        const bool suppressDesktopWidgetTargets = SuppressDesktopWidgetDragTargets();
        const bool groupedEntryDrag =
            dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries;
        if (!suppressDesktopWidgetTargets && !UpdateDragPageNavigation(client))
        {
            *effect = DROPEFFECT_NONE;
            PresentOleDragInteractionFrame();
            return S_OK;
        }

        // OO hit-test：优先检查集合弹窗（弹窗遮挡的容器不应被穿透命中）
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        const bool popupHit =
            !suppressDesktopWidgetTargets &&
            !groupedEntryDrag &&
            HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion);
        if (!popupHit)
        {
            const DragTargetResolution resolved =
                dragDropController_.ResolveInternalTarget(
                    containers_, client,
                    [&](const Container& candidate) {
                        if (desktopIconsHidden_ &&
                            !IsRetainedContainer(&candidate))
                            return false;
                        return !suppressDesktopWidgetTargets ||
                            (!dynamic_cast<const DesktopGrid*>(&candidate) &&
                             !dynamic_cast<const WidgetContainer*>(&candidate));
                    });
            targetContainer = resolved.container;
            targetSlot = resolved.slot;
            targetRegion = resolved.region;
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (const std::wstring removalHint = GetDockDragOutRemovalHint(client);
            !removalHint.empty())
            hint = removalHint;
        else if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = targetRegion == HitRegion::Blocked
            ? DROPEFFECT_NONE : DROPEFFECT_COPY | DROPEFFECT_MOVE;
        PresentOleDragInteractionFrame();
        return S_OK;
    }

    ExternalDragSummary externalSummary;
    if (dataObject)
    {
        const std::vector<std::wstring> paths =
            GetDropPaths(dataObject);
        externalSummary.fileCount =
            static_cast<int>(paths.size());
        externalSummary.hasShortcut =
            std::any_of(
                paths.begin(), paths.end(),
                [](const std::wstring& path) {
                    return _wcsicmp(
                        PathFindExtensionW(
                            path.c_str()),
                        L".lnk") == 0;
                });
        externalSummary.foldersOnly =
            !paths.empty() &&
            std::all_of(
                paths.begin(), paths.end(),
                [](const std::wstring& path) {
                    return snowdesktop::
                        item_location::
                            ResolveFolderTarget(
                                path).kind !=
                        snowdesktop::
                            item_location::
                                FolderTargetKind::
                                    None;
                });
    }
    else
    {
        externalSummary.fileCount = 1;
    }
    dragDropController_.BeginExternalDrag(
        externalSummary);
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
    {
        PrepareDockBackdropForDragTransition();
        dragSession_.Begin(nullptr, {}, {}, client, client);
    }
    else
        dragSession_.UpdatePoint(client);
    if (!UpdateDragPageNavigation(client))
    {
        *effect = DROPEFFECT_NONE;
        PresentOleDragInteractionFrame();
        return S_OK;
    }

    // OO hit-test for external drop：优先检查集合弹窗
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    if (!HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion))
    {
        const DragTargetResolution resolved =
            dragDropController_.ResolveExternalTarget(
                containers_, client,
                [&](const Container& candidate) {
                    return !desktopIconsHidden_ ||
                        IsRetainedContainer(&candidate);
                });
        targetContainer = resolved.container;
        targetSlot = resolved.slot;
        targetRegion = resolved.region;
    }
    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    int mods = 0;
    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
    if (keyState & MK_ALT)     mods |= MK_ALT;
    if (keyState & MK_SHIFT)   mods |= MK_SHIFT;
    const bool externalDockMapping =
        dynamic_cast<DockContainer*>(targetContainer) &&
        targetRegion != HitRegion::Handoff &&
        targetRegion != HitRegion::Blocked;
    if (externalDockMapping)
        dragSession_.UpdateActionFromMods(
            DropActionToMods(
                snowdesktop::dock_drop_rules::
                    ExternalMappingAction()),
            snowdesktop::dock_drop_rules::
                ExternalMappingAction());
    else
        dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ((desktopIconsHidden_ && !targetContainer) ||
        targetRegion == HitRegion::Blocked)
        ? DROPEFFECT_NONE
        : (externalDockMapping
            ? snowdesktop::dock_drop_rules::
                ChooseExternalMappingEffect(*effect)
            : ChooseDropEffect(keyState, *effect));
    PresentOleDragInteractionFrame();
    return S_OK;
}

/**
 * @brief COM IDropTarget::DragOver 实现
 * @param keyState 键盘修饰键状态
 * @param point 鼠标屏幕坐标
 * @param effect [in/out] 拖放效果
 * @return S_OK 或错误码
 */

HRESULT DesktopApp::HandleOleDragOver(
    DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;

    if (dragDropController_.IsSelfDragActive())
    {
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
        }
        UpdateCollectionPopupDwell(client);
        UpdateCollectionGroupTabDwell(client);
        const bool suppressDesktopWidgetTargets = SuppressDesktopWidgetDragTargets();
        const bool groupedEntryDrag =
            dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries;
        if (!suppressDesktopWidgetTargets && !UpdateDragPageNavigation(client))
        {
            *effect = DROPEFFECT_NONE;
            PresentOleDragInteractionFrame();
            return S_OK;
        }

        // OO hit-test：优先检查集合弹窗（弹窗遮挡的容器不应被穿透命中）
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        const bool popupHit =
            !suppressDesktopWidgetTargets &&
            !groupedEntryDrag &&
            HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion);
        if (!popupHit)
        {
            const DragTargetResolution resolved =
                dragDropController_.ResolveInternalTarget(
                    containers_, client,
                    [&](const Container& candidate) {
                        if (desktopIconsHidden_ &&
                            !IsRetainedContainer(&candidate))
                            return false;
                        return !suppressDesktopWidgetTargets ||
                            (!dynamic_cast<const DesktopGrid*>(&candidate) &&
                             !dynamic_cast<const WidgetContainer*>(&candidate));
                    });
            targetContainer = resolved.container;
            targetSlot = resolved.slot;
            targetRegion = resolved.region;
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (const std::wstring removalHint = GetDockDragOutRemovalHint(client);
            !removalHint.empty())
            hint = removalHint;
        else if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = targetRegion == HitRegion::Blocked
            ? DROPEFFECT_NONE : DROPEFFECT_COPY | DROPEFFECT_MOVE;
        PresentOleDragInteractionFrame();
        return S_OK;
    }

    dragDropController_.ContinueExternalDrag();
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
    {
        PrepareDockBackdropForDragTransition();
        dragSession_.Begin(nullptr, {}, {}, client, client);
    }
    else
        dragSession_.UpdatePoint(client);
    if (!UpdateDragPageNavigation(client))
    {
        *effect = DROPEFFECT_NONE;
        PresentOleDragInteractionFrame();
        return S_OK;
    }

    // OO hit-test for external drop：优先检查集合弹窗
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    if (!HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion))
    {
        const DragTargetResolution resolved =
            dragDropController_.ResolveExternalTarget(
                containers_, client,
                [&](const Container& candidate) {
                    return !desktopIconsHidden_ ||
                        IsRetainedContainer(&candidate);
                });
        targetContainer = resolved.container;
        targetSlot = resolved.slot;
        targetRegion = resolved.region;
    }
    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    int mods = 0;
    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
    if (keyState & MK_ALT)     mods |= MK_ALT;
    if (keyState & MK_SHIFT)   mods |= MK_SHIFT;
    const bool externalDockMapping =
        dynamic_cast<DockContainer*>(targetContainer) &&
        targetRegion != HitRegion::Handoff &&
        targetRegion != HitRegion::Blocked;
    if (externalDockMapping)
        dragSession_.UpdateActionFromMods(
            DropActionToMods(
                snowdesktop::dock_drop_rules::
                    ExternalMappingAction()),
            snowdesktop::dock_drop_rules::
                ExternalMappingAction());
    else
        dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ((desktopIconsHidden_ && !targetContainer) ||
        targetRegion == HitRegion::Blocked)
        ? DROPEFFECT_NONE
        : (externalDockMapping
            ? snowdesktop::dock_drop_rules::
                ChooseExternalMappingEffect(*effect)
            : ChooseDropEffect(keyState, *effect));
    PresentOleDragInteractionFrame();
    return S_OK;
}

/**
 * @brief COM IDropTarget::DragLeave 实现
 * @return S_OK
 */

HRESULT DesktopApp::HandleOleDragLeave()
{
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    if (dragDropController_.IsSelfDragActive())
    {
        ResetDockHandoffDwell();
        popupDwellController_.Reset();
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        collectionGroupTabDwellWidgetIndex_ =
            static_cast<size_t>(-1);
        collectionGroupTabDwellId_.clear();
        collectionGroupTabDwellTick_ = 0;
        KillTimer(
            hwnd_, kCollectionGroupTabDwellTimerId);
        dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        HideDragHintWindow();
        PresentOleDragInteractionFrame();
        return S_OK;
    }
    dragDropController_.EndExternalDrag();
    EndDragSession();
    HideDragHintWindow();
    PresentOleDragInteractionFrame();
    return S_OK;
}

/**
 * @brief COM IDropTarget::Drop 实现 — 处理拖放完成事件
 * @param dataObject 拖放数据对象
 * @param keyState 键盘修饰键状态
 * @param point 鼠标屏幕坐标
 * @param effect [in/out] 拖放效果
 */

HRESULT DesktopApp::HandleOleDrop(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;
    HideDragHintWindow();
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;

    if (dragSession_.TargetRegion() == HitRegion::Blocked)
    {
        dragDropController_.EndExternalDrag();
        *effect = DROPEFFECT_NONE;
        EndDragSession();
        return S_OK;
    }

    POINT clientPoint = ScreenPointToClient(point);

    if (dragDropController_.IsSelfDragActive())
    {
        const bool dockFolderPopupSource =
            dockFolderPopupOpen_ &&
            dragSession_.Source() ==
                dockFolderPopupContainer_.get();
        const bool dockFolderPopupTarget =
            IsOpenDockFolderPopupDropTarget(
                dragSession_.TargetContainer(),
                dragSession_.TargetSlot()
                    ? dragSession_.TargetSlot()->GetItem()
                    : nullptr);
        auto refreshDockFolderPopup =
            [&]() {
                if ((dockFolderPopupSource ||
                     dockFolderPopupTarget) &&
                    dockFolderPopupOpen_)
                    RefreshDockFolderPopup();
            };
        dragDropController_.MarkSelfDragReturned();
        dragDropController_.EndSelfDrag();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        ReleaseCapture();
        int dropPreviewMods = 0;
        if (keyState & MK_CONTROL) dropPreviewMods |= MK_CONTROL;
        if (keyState & MK_ALT)     dropPreviewMods |= MK_ALT;
        if (keyState & MK_SHIFT)   dropPreviewMods |= MK_SHIFT;
        bool commitVisualBeforeDrop =
            dragSession_.TargetRegion() == HitRegion::Handoff;
        if (!commitVisualBeforeDrop &&
            dragSession_.TargetContainer())
        {
            const DropPreviewList dropPreview = BuildDropPreviewList(
                dragSession_.SourceList(),
                dragSession_.TargetContainer(),
                dragSession_.TargetSlot(),
                dragSession_.TargetRegion(),
                dropPreviewMods,
                clientPoint);
            commitVisualBeforeDrop = dropPreview.fileBacked;
        }
        dragSession_.DeactivateForDrop();
        if (commitVisualBeforeDrop)
            CommitDragVisualEndBeforeShellOperation();

        if (!GetDockDragOutRemovalHint(clientPoint).empty())
        {
            const bool removed = RemoveDockDragOutItems(dragSession_.Items());
            ClearSelection();
            EndDragSession();
            if (removed)
            {
                SaveLayoutSlots();
                RebuildContainersAndItems();
                LayoutItems();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            *effect = DROPEFFECT_MOVE;
            return S_OK;
        }

        if (dragSession_.TargetRegion() == HitRegion::Handoff)
        {
            // ── Shell handoff via IShellFolder::IDropTarget ────
            Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
            if (auto* dockTarget = dynamic_cast<DockEntryItem*>(targetItem))
            {
                if (dockTarget->GetEntryType() == DockEntryType::Collection)
                {
                    const bool executed = DropItemsIntoDockCollection(
                        dragSession_.Items(), dragSession_.Source(), dockTarget,
                        dropPreviewMods);
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    if (executed)
                    {
                        RebuildContainersAndItems();
                        LayoutItems();
                        refreshDockFolderPopup();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    *effect = executed ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
                    return S_OK;
                }
            }
            auto* targetDesktopIcon = dynamic_cast<DesktopIcon*>(targetItem);
            DesktopItem* targetDesktopItem = targetDesktopIcon
                ? targetDesktopIcon->GetDesktopItem() : nullptr;
            if (dynamic_cast<DockContainer*>(dragSession_.Source()) && targetDesktopItem &&
                _wcsicmp(targetDesktopItem->desktopIconClsid.c_str(),
                    kDesktopIconClsidRecycleBin) == 0)
            {
                MoveDockItemsToDesktop(dragSession_.Items(), CellFromPointForDrag(clientPoint));
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                *effect = DROPEFFECT_MOVE;
                return S_OK;
            }
            const std::vector<std::wstring> sourcePaths =
                dragSession_.SourceList().FilePaths();
            const bool fullyPathBackedSource =
                !sourcePaths.empty() &&
                sourcePaths.size() ==
                    dragSession_.SourceList().entries.size();
            const std::wstring targetPath =
                targetItem ? targetItem->GetPath() : L"";
            const DWORD targetAttributes = targetPath.empty()
                ? INVALID_FILE_ATTRIBUTES
                : GetFileAttributesW(targetPath.c_str());
            if (!sourcePaths.empty() &&
                targetAttributes != INVALID_FILE_ATTRIBUTES &&
                (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                DWORD shellKeyState = keyState;
                const DWORD selectedEffect = ChooseDropEffect(
                    shellKeyState,
                    DROPEFFECT_COPY |
                        DROPEFFECT_MOVE |
                        DROPEFFECT_LINK);
                const DropAction action =
                    selectedEffect == DROPEFFECT_LINK
                        ? DropAction::Link
                        : selectedEffect == DROPEFFECT_COPY
                            ? DropAction::Copy
                            : DropAction::Move;
                DragSourceList fileSources =
                    dragSession_.SourceList();
                auto finished = [this,
                    dockFolderPopupSource,
                    dockFolderPopupTarget](bool succeeded) {
                    if (!succeeded)
                        return;
                    ReloadItems(false);
                    if ((dockFolderPopupSource ||
                         dockFolderPopupTarget) &&
                        dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                };
                if (MaterializeFilesToFolder(
                        fileSources, targetPath, action,
                        std::move(finished)))
                {
                    ClearSelection();
                    EndDragSession();
                    *effect = selectedEffect;
                    return S_OK;
                }
            }
            if (fullyPathBackedSource && !targetPath.empty() &&
                QueueShellDrop(
                    sourcePaths,
                    targetPath,
                    keyState,
                    point,
                    DROPEFFECT_COPY | DROPEFFECT_MOVE |
                        DROPEFFECT_LINK,
                    [this,
                     dockFolderPopupSource,
                     dockFolderPopupTarget](bool succeeded) {
                        if (!succeeded)
                            return;
                        if ((dockFolderPopupSource ||
                             dockFolderPopupTarget) &&
                            dockFolderPopupOpen_)
                            RefreshDockFolderPopup();
                    }))
            {
                ClearSelection();
                EndDragSession();
                *effect = ChooseDropEffect(
                    keyState,
                    DROPEFFECT_COPY | DROPEFFECT_MOVE |
                        DROPEFFECT_LINK);
                return S_OK;
            }
            // This source does not support the Shell async-data protocol.
            // Ensure the already-submitted drag-end frame reaches DWM before
            // entering its unavoidable synchronous IDropTarget fallback.
            DwmFlush();
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj && targetItem)
            {
                ComPtr<IDropTarget> dt;
                bool explicitFolderTarget =
                    dockFolderPopupTarget;
                if (auto* dockTarget =
                        dynamic_cast<DockEntryItem*>(
                            targetItem))
                {
                    const size_t entryIndex =
                        dockTarget->GetEntryIndex();
                    explicitFolderTarget =
                        entryIndex <
                            dockEntries_.size() &&
                        IsFolderDockEntry(
                            dockEntries_[entryIndex]);
                }
                if (auto* icon = dynamic_cast<DesktopIcon*>(targetItem))
                {
                    DesktopItem* di = icon->GetDesktopItem();
                    if (di && di->childPidl.get())
                    {
                        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(di->childPidl.get());
                        desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                            reinterpret_cast<void**>(dt.GetAddressOf()));
                    }
                }
                if (!dt && !targetItem->GetPath().empty())
                {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                        nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                    {
                        shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt));
                    }
                }
                if (dt)
                {
                    POINTL spl{ point.x, point.y };
                    DWORD shellKeyState = keyState;
                    if (explicitFolderTarget &&
                        (shellKeyState &
                            (MK_CONTROL |
                             MK_ALT |
                             MK_SHIFT)) == 0)
                        shellKeyState |= MK_SHIFT;
                    DWORD le = DROPEFFECT_COPY |
                        DROPEFFECT_MOVE |
                        DROPEFFECT_LINK;
                    dt->DragEnter(
                        dataObj.Get(),
                        shellKeyState, spl, &le);
                    dt->DragOver(
                        shellKeyState, spl, &le);
                    dt->Drop(
                        dataObj.Get(),
                        shellKeyState, spl, &le);
                }
            }
            ClearSelection();
            EndDragSession();
            ReloadItems();
            refreshDockFolderPopup();
            *effect = DROPEFFECT_MOVE;
            return S_OK;
        }

        // ── OO dispatch ────────────────────────────────────
        if (dragSession_.TargetContainer())
        {
            Container* targetContainer = dragSession_.TargetContainer();
            bool needsReload = targetContainer->NeedsShellReloadAfterDrop();
            targetContainer->OnItemsDropped(dragSession_.Items(), dragSession_.Source(),
                dragSession_.TargetSlot(), dragSession_.TargetRegion(),
                dropPreviewMods);

            SaveLayoutSlots();
            ClearSelection();
            EndDragSession();
            if (needsReload)
            {
                RebuildContainersAndItems();
                ReloadItems();
            }
            else
            {
                ApplyPageMapping();
                RebuildContainersAndItems();
                LayoutItems();
            }
            refreshDockFolderPopup();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        *effect = DROPEFFECT_MOVE;
        EndDragSession();
        return S_OK;
    }

    // ── External drop ──────────────────────────────────────────
    DockContainer* externalDropDock =
        dynamic_cast<DockContainer*>(
            dragSession_.TargetContainer());
    const bool externalDockMappingTarget =
        externalDropDock &&
        dragSession_.TargetRegion() != HitRegion::Handoff &&
        dragSession_.TargetRegion() != HitRegion::Blocked;
    // Resolve the insertion boundary while the external summary still owns
    // the folder-only classification used by the Dock's split ranges.
    const size_t externalDockInsertIndex =
        externalDockMappingTarget
            ? externalDropDock->GetDropInsertIndex(
                dragSession_.TargetSlot(),
                dragSession_.TargetRegion())
            : 0;
    dragDropController_.EndExternalDrag();
    if (desktopIconsHidden_ &&
        !IsRetainedContainer(
            dragSession_.TargetContainer()))
    {
        *effect = DROPEFFECT_NONE;
        EndDragSession();
        return S_OK;
    }
    dragSession_.DeactivateForDrop();
    CommitDragVisualEndBeforeShellOperation();

    std::vector<std::wstring> dropPaths = dataObject
        ? GetDropPaths(dataObject) : std::vector<std::wstring>();

    if (dragSession_.TargetRegion() == HitRegion::Handoff && dataObject)
    {
        // ── Handoff on item (desktop OR widget member) ──
        Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
        const bool dockFolderPopupTarget =
            IsOpenDockFolderPopupDropTarget(
                dragSession_.TargetContainer(),
                targetItem);
        const auto refreshTargetPopup =
            [this, dockFolderPopupTarget](bool succeeded) {
                if (succeeded && dockFolderPopupTarget &&
                    dockFolderPopupOpen_)
                    RefreshDockFolderPopup();
            };
        const std::wstring targetPath =
            targetItem ? targetItem->GetPath() : L"";
        const DWORD targetAttributes = targetPath.empty()
            ? INVALID_FILE_ATTRIBUTES
            : GetFileAttributesW(targetPath.c_str());
        if (dropPaths.empty() && !targetPath.empty() &&
            QueueAsyncShellDrop(
                dataObject,
                targetPath,
                keyState,
                point,
                *effect,
                refreshTargetPopup))
        {
            *effect = ChooseDropEffect(keyState, *effect);
            EndDragSession();
            return S_OK;
        }
        if (dropPaths.empty() &&
            targetAttributes != INVALID_FILE_ATTRIBUTES &&
            (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            dropPaths = TryGetNonFileDropPaths(dataObject);
        }
        if (!dropPaths.empty() &&
            targetAttributes != INVALID_FILE_ATTRIBUTES &&
            (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            const DWORD selectedEffect = ChooseDropEffect(
                keyState, *effect);
            if (selectedEffect == DROPEFFECT_NONE)
            {
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }
            const DropAction action =
                selectedEffect == DROPEFFECT_LINK
                    ? DropAction::Link
                    : selectedEffect == DROPEFFECT_COPY
                        ? DropAction::Copy
                        : DropAction::Move;
            DragSourceList fileSources;
            fileSources.hasExternalFiles = true;
            for (const auto& path : dropPaths)
            {
                DragSourceEntry entry;
                entry.kind = DropSourceKind::ExternalFile;
                entry.sourceIndex = fileSources.entries.size();
                entry.filePath = path;
                entry.displayName = FileNameFromPath(path);
                fileSources.entries.push_back(std::move(entry));
            }
            auto finished = [this,
                dockFolderPopupTarget](bool succeeded) {
                if (!succeeded)
                    return;
                ReloadItems(false);
                if (dockFolderPopupTarget && dockFolderPopupOpen_)
                    RefreshDockFolderPopup();
            };
            FileOperationCompletion asyncCompletion;
            const bool sourceSupportsAsync =
                PrepareOleAsyncFileOperation(
                    dataObject,
                    action == DropAction::Move
                        ? DROPEFFECT_NONE : selectedEffect,
                    finished, asyncCompletion);
            if (!sourceSupportsAsync)
                DwmFlush();
            const bool handled = MaterializeFilesToFolder(
                fileSources, targetPath, action,
                sourceSupportsAsync
                    ? std::move(asyncCompletion)
                    : std::move(finished),
                !sourceSupportsAsync);
            if (!handled)
            {
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }
            *effect = !sourceSupportsAsync &&
                    action == DropAction::Move
                ? DROPEFFECT_NONE : selectedEffect;
            EndDragSession();
            return S_OK;
        }
        if (!targetPath.empty() &&
            QueueAsyncShellDrop(
                dataObject,
                targetPath,
                keyState,
                point,
                *effect,
                refreshTargetPopup))
        {
            *effect = ChooseDropEffect(keyState, *effect);
            EndDragSession();
            return S_OK;
        }
        // Sources without IDataObjectAsyncCapability must remain synchronous
        // so the returned effect is the operation that actually occurred.
        DwmFlush();
        ComPtr<IDropTarget> dt;
        if (targetItem)
        {
            if (auto* icon = dynamic_cast<DesktopIcon*>(targetItem))
            {
                DesktopItem* di = icon->GetDesktopItem();
                if (di && di->childPidl.get())
                {
                    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(di->childPidl.get());
                    desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                        reinterpret_cast<void**>(dt.GetAddressOf()));
                }
            }
            if (!dt && !targetItem->GetPath().empty())
            {
                ComPtr<IShellItem> shellItem;
                if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                    nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                {
                    shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt));
                }
            }
        }

        if (dt)
        {
            DWORD le = *effect;
            POINTL spl{ point.x, point.y };
            dt->DragEnter(dataObject, keyState, spl, &le);
            dt->DragOver(keyState, spl, &le);
            dt->Drop(dataObject, keyState, spl, &le);
            *effect = le;
            EndDragSession();
            if (dockFolderPopupTarget)
                RefreshDockFolderPopup();
            return S_OK;
        }
    }

    if (dataObject &&
        dockFolderPopupOpen_ &&
        dragSession_.TargetContainer() ==
            dockFolderPopupContainer_.get() &&
        dragSession_.TargetRegion() !=
            HitRegion::Blocked)
    {
        const size_t popupInsertIndex =
            dockFolderPopupContainer_->GetDropInsertIndex(
                dragSession_.TargetSlot(),
                dragSession_.TargetRegion());
        const PendingFolderPlacement popupPlacement =
            BuildPendingFolderPlacement(
                dockFolderPopupWidget_,
                popupInsertIndex);
        if (dropPaths.empty() &&
            !dockFolderPopupWidget_.sourceFolderPath.empty() &&
            QueueAsyncShellDrop(
                dataObject,
                dockFolderPopupWidget_.sourceFolderPath,
                keyState,
                point,
                *effect,
                [this, popupPlacement](bool succeeded) mutable {
                    if (!succeeded)
                        return;
                    ActivatePendingFolderPlacement(
                        std::move(popupPlacement));
                    if (dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                }))
        {
            *effect = ChooseDropEffect(keyState, *effect);
            EndDragSession();
            return S_OK;
        }
        if (dropPaths.empty())
            dropPaths = TryGetNonFileDropPaths(dataObject);
        if (!dropPaths.empty() &&
            !dockFolderPopupWidget_.sourceFolderPath.empty())
        {
            const DWORD selectedEffect = ChooseDropEffect(
                keyState, *effect);
            if (selectedEffect == DROPEFFECT_NONE)
            {
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }
            const DropAction action =
                selectedEffect == DROPEFFECT_LINK
                    ? DropAction::Link
                    : selectedEffect == DROPEFFECT_COPY
                        ? DropAction::Copy
                        : DropAction::Move;
            DragSourceList fileSources;
            fileSources.hasExternalFiles = true;
            for (const auto& path : dropPaths)
            {
                DragSourceEntry entry;
                entry.kind = DropSourceKind::ExternalFile;
                entry.sourceIndex = fileSources.entries.size();
                entry.filePath = path;
                entry.displayName = FileNameFromPath(path);
                fileSources.entries.push_back(std::move(entry));
            }
            PendingFolderPlacement folderPlacement =
                BuildPendingFolderPlacement(
                    dockFolderPopupWidget_,
                    popupInsertIndex,
                    &fileSources);
            auto finished = [this,
                folderPlacement = std::move(folderPlacement)](
                    bool succeeded) mutable {
                if (!succeeded)
                    return;
                ActivatePendingFolderPlacement(
                    std::move(folderPlacement));
                ReloadItems(false);
                if (dockFolderPopupOpen_)
                    RefreshDockFolderPopup();
            };
            FileOperationCompletion asyncCompletion;
            const bool sourceSupportsAsync =
                PrepareOleAsyncFileOperation(
                    dataObject,
                    action == DropAction::Move
                        ? DROPEFFECT_NONE : selectedEffect,
                    finished, asyncCompletion);
            if (!sourceSupportsAsync)
                DwmFlush();
            const bool handled = MaterializeFilesToFolder(
                fileSources,
                dockFolderPopupWidget_.sourceFolderPath,
                action,
                sourceSupportsAsync
                    ? std::move(asyncCompletion)
                    : std::move(finished),
                !sourceSupportsAsync);
            if (!handled)
            {
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }
            *effect = !sourceSupportsAsync &&
                    action == DropAction::Move
                ? DROPEFFECT_NONE : selectedEffect;
            EndDragSession();
            return S_OK;
        }
        if (!dropPaths.empty() &&
            !dockFolderPopupWidget_.sourceFolderPath.empty() &&
            QueueAsyncShellDrop(
                dataObject,
                dockFolderPopupWidget_.sourceFolderPath,
                keyState,
                point,
                *effect,
                [this, popupPlacement](bool succeeded) mutable {
                    if (!succeeded)
                        return;
                    ActivatePendingFolderPlacement(
                        std::move(popupPlacement));
                    if (dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                }))
        {
            *effect = ChooseDropEffect(keyState, *effect);
            EndDragSession();
            return S_OK;
        }
        ComPtr<IShellItem> folderItem;
        ComPtr<IDropTarget> folderDropTarget;
        if (dockFolderPopupAvailable_ &&
            SUCCEEDED(SHCreateItemFromParsingName(
                dockFolderPopupWidget_.
                    sourceFolderPath.c_str(),
                nullptr,
                IID_PPV_ARGS(&folderItem))) &&
            folderItem)
        {
            folderItem->BindToHandler(
                nullptr, BHID_SFUIObject,
                IID_PPV_ARGS(&folderDropTarget));
        }
        if (folderDropTarget)
        {
            DwmFlush();
            DWORD shellEffect = *effect;
            POINTL screenPoint{
                point.x, point.y };
            if (SUCCEEDED(
                    folderDropTarget->DragEnter(
                        dataObject, keyState,
                        screenPoint,
                        &shellEffect)))
            {
                folderDropTarget->DragOver(
                    keyState, screenPoint,
                    &shellEffect);
                const HRESULT dropResult = folderDropTarget->Drop(
                    dataObject, keyState,
                    screenPoint,
                    &shellEffect);
                *effect = shellEffect;
                EndDragSession();
                if (SUCCEEDED(dropResult) &&
                    shellEffect != DROPEFFECT_NONE)
                {
                    ActivatePendingFolderPlacement(
                        popupPlacement);
                }
                RefreshDockFolderPopup();
                return S_OK;
            }
        }
    }

    if (dropPaths.empty() && dataObject)
        dropPaths = TryGetNonFileDropPaths(dataObject);

    if (dataObject && !dropPaths.empty())
    {
        std::vector<std::unique_ptr<ExternalFileItem>> externalItems;
        std::vector<Item*> sourceItems;
        for (const auto& path : dropPaths)
        {
            auto item = std::make_unique<ExternalFileItem>(path);
            sourceItems.push_back(item.get());
            externalItems.push_back(std::move(item));
        }

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        DragSourceList sourceList = BuildDragSourceList(sourceItems, nullptr);
        Container* target = dragSession_.TargetContainer() ? dragSession_.TargetContainer() : GetDesktopGrid();
        HitRegion targetRegion = dragSession_.TargetRegion() != HitRegion::None ? dragSession_.TargetRegion() : HitRegion::Empty;

        if (auto* dock = dynamic_cast<DockContainer*>(target);
            dock && externalDockMappingTarget &&
            dock == externalDropDock)
        {
            if (!dock->HasCapacity(sourceItems.size()))
            {
                MessageBeep(MB_ICONWARNING);
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }

            const DWORD mappingEffect =
                snowdesktop::dock_drop_rules::
                    ChooseExternalMappingEffect(*effect);
            if (mappingEffect == DROPEFFECT_NONE)
            {
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }

            DropPreviewList desktopPreview = BuildDropPreviewList(sourceList, GetDesktopGrid(),
                nullptr, HitRegion::Empty, mods, clientPoint);
            desktopPreview.action =
                snowdesktop::dock_drop_rules::
                    ExternalMappingAction();
            desktopPreview.pinMaterializedItemsToDock = true;
            desktopPreview.dockInsertIndex =
                externalDockInsertIndex;
            FileOperationCompletion asyncCompletion;
            const bool sourceSupportsAsync =
                PrepareOleAsyncFileOperation(
                    dataObject, mappingEffect,
                    {}, asyncCompletion);
            if (!sourceSupportsAsync)
                DwmFlush();
            bool executed = ExecuteDropPipeline(
                sourceList,
                desktopPreview,
                sourceSupportsAsync
                    ? std::move(asyncCompletion)
                    : FileOperationCompletion{},
                !sourceSupportsAsync);
            if (executed)
            {
                EndDragSession();
                InvalidateRect(hwnd_, nullptr, FALSE);
                *effect = mappingEffect;
                return S_OK;
            }
        }

        DropPreviewList preview = BuildDropPreviewList(sourceList, target,
            dragSession_.TargetContainer() ? dragSession_.TargetSlot() : nullptr, targetRegion, mods, clientPoint);
        const bool dockFolderPopupTarget =
            IsOpenDockFolderPopupDropTarget(
                target,
                dragSession_.TargetSlot()
                    ? dragSession_.TargetSlot()->GetItem()
                    : nullptr);
        if (dockFolderPopupTarget)
        {
            if ((*effect & DROPEFFECT_MOVE) != 0)
                preview.action = DropAction::Move;
            else if ((*effect & DROPEFFECT_LINK) != 0)
                preview.action = DropAction::Link;
            else
                preview.action = DropAction::Copy;
        }
        const DWORD performedEffect =
            preview.action == DropAction::Move
                ? DROPEFFECT_MOVE
                : preview.action == DropAction::Link
                    ? DROPEFFECT_LINK : DROPEFFECT_COPY;
        FileOperationCompletion asyncCompletion;
        const bool sourceSupportsAsync =
            PrepareOleAsyncFileOperation(
                dataObject,
                preview.action == DropAction::Move
                    ? DROPEFFECT_NONE : performedEffect,
                {}, asyncCompletion);
        if (!sourceSupportsAsync)
            DwmFlush();
        bool executed = ExecuteDropPipeline(
            sourceList, preview,
            sourceSupportsAsync
                ? std::move(asyncCompletion)
                : FileOperationCompletion{},
            !sourceSupportsAsync);
        if (executed)
        {
            if (dockFolderPopupTarget)
                RefreshDockFolderPopup();
            SaveLayoutSlots();
            EndDragSession();
            RebuildContainersAndItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            *effect = !sourceSupportsAsync &&
                    preview.action == DropAction::Move
                ? DROPEFFECT_NONE : performedEffect;
            return S_OK;
        }
    }

    *effect = DROPEFFECT_NONE;
    EndDragSession();
    return S_OK;
}

/**
 * @brief COM IDropSource::QueryContinueDrag 实现
 * @param escapePressed 是否按下了 Escape
 * @param keyState 键盘修饰键状态
 * @return DRAGDROP_S_CANCEL、DRAGDROP_S_DROP 或 S_OK
 */

HRESULT DesktopApp::HandleOleQueryContinueDrag(
    BOOL escapePressed, DWORD keyState)
{
    if (escapePressed) return DRAGDROP_S_CANCEL;
    if ((keyState & (MK_LBUTTON | MK_RBUTTON)) == 0) return DRAGDROP_S_DROP;
    return S_OK;
}

/**
 * @brief COM IDropSource::GiveFeedback 实现
 * @return DRAGDROP_S_USEDEFAULTCURSORS（使用默认光标）
 */
HRESULT DesktopApp::HandleOleGiveFeedback(DWORD)
{
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

/**
 * @brief 从数据对象中提取文件路径列表
 * @param dataObject COM 数据对象
 * @return 文件路径列表
 */
