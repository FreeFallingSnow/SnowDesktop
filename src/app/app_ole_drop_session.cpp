#include "app.h"
#include "../drop_image_data.h"
#include "../ole_drag_rules.h"
#include "../virtual_file_drop.h"
#include "../widgets/lua_logical_slot.h"

// OLE drag-enter/over/leave/drop session handling.

namespace
{
using DirectoryPathSet = std::unordered_set<std::wstring>;

std::wstring UserDesktopDirectory()
{
    wchar_t desktopPath[MAX_PATH]{};
    if (!SHGetSpecialFolderPathW(
            nullptr, desktopPath,
            CSIDL_DESKTOPDIRECTORY, FALSE))
        return {};
    return TrimTrailingPathSeparators(desktopPath);
}

std::optional<DirectoryPathSet> SnapshotDirectoryPaths(
    const std::wstring& directory)
{
    DirectoryPathSet paths;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error)
        return std::nullopt;

    const std::filesystem::directory_iterator end;
    while (iterator != end)
    {
        paths.insert(ToUpperInvariant(
            iterator->path().lexically_normal().wstring()));
        iterator.increment(error);
        if (error)
            return std::nullopt;
    }
    return paths;
}

std::optional<std::vector<std::wstring>> FindNewDirectoryPaths(
    const std::wstring& directory,
    const DirectoryPathSet& previousPaths)
{
    std::vector<std::wstring> paths;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error)
        return std::nullopt;

    const std::filesystem::directory_iterator end;
    while (iterator != end)
    {
        const std::wstring path =
            iterator->path().lexically_normal().wstring();
        if (!previousPaths.contains(ToUpperInvariant(path)))
            paths.push_back(path);
        iterator.increment(error);
        if (error)
            return std::nullopt;
    }

    std::stable_sort(
        paths.begin(), paths.end(),
        [](const std::wstring& left,
            const std::wstring& right) {
            return _wcsicmp(
                left.c_str(), right.c_str()) < 0;
        });
    return paths;
}

bool IsInternetShortcutPath(const std::wstring& path)
{
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    return extension &&
        (_wcsicmp(extension, L".lnk") == 0 ||
         _wcsicmp(extension, L".url") == 0 ||
         _wcsicmp(extension, L".website") == 0);
}

bool IsInternetShortcutDescriptor(
    const snowdesktop::virtual_file_drop::VirtualFileDescriptor& descriptor)
{
    return IsInternetShortcutPath(snowdesktop::virtual_file_drop::
        SanitizeSuggestedFileName(descriptor.suggestedFileName));
}

std::wstring ReadInternetShortcutTarget(const std::wstring& path)
{
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!extension) return {};
    if (_wcsicmp(extension, L".url") == 0 ||
        _wcsicmp(extension, L".website") == 0)
    {
        std::array<wchar_t, 8192> target{};
        const DWORD length = GetPrivateProfileStringW(
            L"InternetShortcut", L"URL", L"",
            target.data(), static_cast<DWORD>(target.size()),
            path.c_str());
        return std::wstring(target.data(), length);
    }
    if (_wcsicmp(extension, L".lnk") != 0)
        return {};

    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) ||
        !shellLink)
        return {};
    ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) ||
        FAILED(persistFile->Load(path.c_str(), STGM_READ)))
        return {};
    std::array<wchar_t, 8192> target{};
    WIN32_FIND_DATAW findData{};
    if (FAILED(shellLink->GetPath(target.data(),
            static_cast<int>(target.size()), &findData,
            SLGP_RAWPATH)))
        return {};
    return target.data();
}

class StagedDropPathLease final
{
public:
    explicit StagedDropPathLease(
        std::vector<std::wstring> paths)
        : paths_(std::move(paths))
    {
    }

    ~StagedDropPathLease()
    {
        if (keep_) return;
        for (const auto& path : paths_)
            (void)DeleteFileW(path.c_str());
    }

    StagedDropPathLease(const StagedDropPathLease&) = delete;
    StagedDropPathLease& operator=(
        const StagedDropPathLease&) = delete;

    // Dock links and logical-slot references use the staged path as their
    // durable backing file instead of copying it to another destination.
    void Keep() noexcept
    {
        keep_ = true;
    }

private:
    std::vector<std::wstring> paths_;
    bool keep_ = false;
};

}

void DesktopApp::CancelPendingExternalOleDragLeave()
{
    externalOleDragLeavePending_ = false;
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kExternalOleDragLeaveGraceTimerId);
}

void DesktopApp::FinalizePendingExternalOleDragLeave()
{
    if (!externalOleDragLeavePending_)
        return;

    CancelPendingExternalOleDragLeave();
    if (!dragDropController_.IsExternalDragActive())
        return;

    dragDropController_.EndExternalDrag();
    EndDragSession();
    HideDragHintWindow();
    PresentOleDragInteractionFrame();
}

HRESULT DesktopApp::HandleOleDragEnter(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;
    CancelPendingExternalOleDragLeave();

    if (dragDropController_.IsSelfDragActive())
    {
        dragDropController_.MarkSelfDragReturned();
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            // Stay entirely under OLE ownership until DoDragDrop unwinds.
            // Showing the native ghost here leaves both the Shell cursor and
            // our preview active, and its top-level HWND can also become the
            // next stale OLE hit target.
            dragSession_.SetVisualVisible(false);
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(
                keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
            dragSession_.UpdateTarget(
                nullptr, nullptr, HitRegion::None);
        }
        ResetDockHandoffDwell();
        UpdateCollectionPopupDwell(client);
        CancelCollectionGroupTabDwell();
        HideDragHintWindow();
        *effect = DROPEFFECT_NONE;
        PresentOleDragInteractionFrame();
        return S_OK;
    }

    ExternalDragSummary externalSummary;
    if (dataObject)
    {
        const bool delayedFileDrop = snowdesktop::virtual_file_drop::
            UsesAsyncMode(dataObject);
        const std::vector<std::wstring> paths = delayedFileDrop
            ? std::vector<std::wstring>{}
            : GetDropPaths(dataObject);
        const auto virtualFiles = delayedFileDrop
            ? std::vector<snowdesktop::virtual_file_drop::
                VirtualFileDescriptor>{}
            : snowdesktop::virtual_file_drop::ReadDescriptors(dataObject);
        externalSummary.fileCount =
            static_cast<int>(!paths.empty()
                ? paths.size()
                : !virtualFiles.empty()
                    ? virtualFiles.size()
                    : delayedFileDrop ? 1 : 0);
        externalSummary.hasShortcut =
            std::any_of(
                paths.begin(), paths.end(),
                [](const std::wstring& path) {
                    return _wcsicmp(
                        PathFindExtensionW(
                            path.c_str()),
                        L".lnk") == 0;
                }) ||
            std::any_of(
                virtualFiles.begin(), virtualFiles.end(),
                [](const auto& file) {
                    return _wcsicmp(
                        PathFindExtensionW(
                            file.suggestedFileName.c_str()),
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
        HideDragHintWindow();
        PresentOleDragInteractionFrame();
        return S_OK;
    }
    UpdateCollectionPopupDwell(client);

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
    const bool externalLogicalReference =
        dynamic_cast<LuaLogicalSlotContainer*>(targetContainer) != nullptr &&
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
        : (externalLogicalReference
            ? DROPEFFECT_COPY
            : externalDockMapping
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
    CancelPendingExternalOleDragLeave();

    if (dragDropController_.IsSelfDragActive())
    {
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            // DragEnter requested a native hand-back. Keep this callback
            // cheap and keep both custom feedback HWNDs hidden while OLE is
            // still deciding whether to leave its nested loop.
            dragSession_.SetVisualVisible(false);
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(
                keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
            dragSession_.UpdateTarget(
                nullptr, nullptr, HitRegion::None);
        }
        UpdateCollectionPopupDwell(client);
        HideDragHintWindow();
        *effect = DROPEFFECT_NONE;
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
        HideDragHintWindow();
        PresentOleDragInteractionFrame();
        return S_OK;
    }
    UpdateCollectionPopupDwell(client);

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
    const bool externalLogicalReference =
        dynamic_cast<LuaLogicalSlotContainer*>(targetContainer) != nullptr &&
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
        : (externalLogicalReference
            ? DROPEFFECT_COPY
            : externalDockMapping
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
    SetPageNavHotEdgeHover(0);
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    if (dragDropController_.IsSelfDragActive())
    {
        POINT hoverPoint{};
        const bool returningToDesktopSurface =
            dragDropController_.SelfDragNativeResumeRequested() &&
            TryGetDesktopHoverPointFromCursor(hoverPoint);
        if (!dragDropController_.SelfDragNativeResumeRequested())
            dragDropController_.ClearSelfDragReturned();
        ResetDockHandoffDwell();
        if (!returningToDesktopSurface)
            CancelCollectionPopupDwell();
        CancelCollectionGroupTabDwell();
        dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        dragSession_.SetVisualVisible(false);
        HideDragHintWindow();
        PresentOleDragInteractionFrame();
        return S_OK;
    }

    POINT hoverPoint{};
    if (dragDropController_.IsExternalDragActive() &&
        TryGetDesktopHoverPointFromCursor(hoverPoint) &&
        hwnd_ && IsWindow(hwnd_))
    {
        externalOleDragLeavePending_ =
            SetTimer(
                hwnd_, kExternalOleDragLeaveGraceTimerId,
                kExternalOleDragLeaveGraceMs, nullptr) != 0;
        if (externalOleDragLeavePending_)
            return S_OK;
    }

    externalOleDragLeavePending_ = true;
    FinalizePendingExternalOleDragLeave();
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
    CancelPendingExternalOleDragLeave();
    HideDragHintWindow();
    SetPageNavHotEdgeHover(0);
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;

    POINT clientPoint = ScreenPointToClient(point);
    ResolveCurrentDragTargetAt(clientPoint);

    if (dragSession_.TargetRegion() == HitRegion::Blocked)
    {
        if (dragDropController_.IsSelfDragActive())
        {
            dragDropController_.MarkSelfDragReturned();
            // The DoDragDrop owner ends SelfOle only after this callback has
            // returned, keeping nested reload/model guards active meanwhile.
        }
        else
        {
            dragDropController_.EndExternalDrag();
        }
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        ReleaseCapturePreservingPointerState();
        *effect = DROPEFFECT_NONE;
        EndDragSession();
        return S_OK;
    }

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
        // SelfOle is owned by the surrounding DoDragDrop call. Retain that
        // transport through every synchronous Drop callback and unwind it at
        // the single outer call site after the Shell has released the stack.
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        ReleaseCapturePreservingPointerState();
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
                MoveDockItemsToDesktop(
                    dragSession_.Items(),
                    ResolveDesktopRequestCell(
                        dragSession_.SourceList(), clientPoint));
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

        if (auto* logicalSlot =
                dynamic_cast<LuaLogicalSlotContainer*>(
                    dragSession_.TargetContainer()))
        {
            const bool committed = logicalSlot->CommitItems(
                dragSession_.Items(), dragSession_.TargetSlot(),
                dragSession_.TargetRegion());
            ClearSelection();
            EndDragSession();
            InvalidateRect(hwnd_, nullptr, FALSE);
            *effect = committed ? DROPEFFECT_COPY : DROPEFFECT_NONE;
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

    const bool sourceUsesAsyncMode = dataObject &&
        snowdesktop::virtual_file_drop::UsesAsyncMode(dataObject);
    std::vector<std::wstring> dropPaths =
        dataObject && !sourceUsesAsyncMode
            ? GetDropPaths(dataObject) : std::vector<std::wstring>();
    bool forceCopyDrop = false;
    std::unique_ptr<StagedDropPathLease> stagedDropPathLease;
    const auto adoptStagedDropPaths =
        [&dropPaths, &stagedDropPathLease](
            std::vector<std::wstring> paths) {
            if (paths.empty()) return false;
            try
            {
                auto lease = std::make_unique<StagedDropPathLease>(
                    paths);
                dropPaths = std::move(paths);
                stagedDropPathLease = std::move(lease);
                return true;
            }
            catch (...)
            {
                for (const auto& path : paths)
                    (void)DeleteFileW(path.c_str());
                return false;
            }
        };
    const DropReferenceSnapshot dropReferenceSnapshot =
        dataObject && dropPaths.empty() && !sourceUsesAsyncMode
            ? ReadDropReferenceSnapshot(dataObject)
            : DropReferenceSnapshot{};
    const bool fileUrlReference = std::any_of(
        dropReferenceSnapshot.candidates.begin(),
        dropReferenceSnapshot.candidates.end(),
        [](const auto& candidate) {
            return candidate.kind ==
                snowdesktop::drop_text_rules::Kind::FileUrl;
        });
    const std::vector<std::wstring> localFileUrlPaths =
        fileUrlReference
            ? TryExtractLocalFileUrlFromDataObject(
                dropReferenceSnapshot)
            : std::vector<std::wstring>{};
    if (!localFileUrlPaths.empty() &&
        ((*effect & DROPEFFECT_COPY) != 0))
    {
        dropPaths = localFileUrlPaths;
        forceCopyDrop = true;
        *effect = DROPEFFECT_COPY;
    }

    if (dragSession_.TargetRegion() == HitRegion::Handoff && dataObject)
    {
        // ── Handoff on item (desktop OR widget member) ──
        Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
        auto* targetDesktopIcon =
            dynamic_cast<DesktopIcon*>(targetItem);
        DesktopItem* targetDesktopItem = targetDesktopIcon
            ? targetDesktopIcon->GetDesktopItem() : nullptr;
        const bool recycleBinTarget = targetDesktopItem &&
            _wcsicmp(
                targetDesktopItem->desktopIconClsid.c_str(),
                kDesktopIconClsidRecycleBin) == 0;
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
        if (recycleBinTarget && !dropPaths.empty())
        {
            const bool queued = QueueShellFileOperation(
                snowdesktop::CreateRecycleBinDeleteRequest(
                    std::move(dropPaths)),
                [this](bool succeeded) {
                    if (!succeeded)
                    {
                        MessageBeep(MB_ICONWARNING);
                        return;
                    }
                    ReloadItems(false);
                    CheckRecycleBinStatus();
                });
            *effect = queued
                ? DROPEFFECT_MOVE
                : DROPEFFECT_NONE;
            EndDragSession();
            return S_OK;
        }
        if (dropPaths.empty() && !fileUrlReference &&
            !targetPath.empty() &&
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
            if (!sourceUsesAsyncMode &&
                (*effect & DROPEFFECT_COPY) != 0)
            {
                dropPaths = localFileUrlPaths;
                if (dropPaths.empty())
                {
                    (void)adoptStagedDropPaths(
                        TryGetNonFileDropPaths(
                            dataObject,
                            dropReferenceSnapshot));
                }
                if (!dropPaths.empty())
                {
                    forceCopyDrop = true;
                    *effect = DROPEFFECT_COPY;
                }
            }
        }
        if (!dropPaths.empty() &&
            targetAttributes != INVALID_FILE_ATTRIBUTES &&
            (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            const DWORD selectedEffect = forceCopyDrop
                ? DROPEFFECT_COPY
                : ChooseDropEffect(keyState, *effect);
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
        if (!fileUrlReference && !targetPath.empty() &&
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

        if (dt && !fileUrlReference && !sourceUsesAsyncMode)
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
        if (dropPaths.empty() && !fileUrlReference &&
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
        if (dropPaths.empty() && !sourceUsesAsyncMode &&
            ((*effect & DROPEFFECT_COPY) != 0))
        {
            dropPaths = localFileUrlPaths;
            if (dropPaths.empty())
            {
                (void)adoptStagedDropPaths(
                    TryGetNonFileDropPaths(
                        dataObject,
                        dropReferenceSnapshot));
            }
            if (!dropPaths.empty())
            {
                forceCopyDrop = true;
                *effect = DROPEFFECT_COPY;
            }
        }
        if (!dropPaths.empty() &&
            !dockFolderPopupWidget_.sourceFolderPath.empty())
        {
            const DWORD selectedEffect = forceCopyDrop
                ? DROPEFFECT_COPY
                : ChooseDropEffect(keyState, *effect);
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
        if (folderDropTarget && !fileUrlReference &&
            !sourceUsesAsyncMode)
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

    // Browsers and desktop clients can advertise the resource bytes through
    // standard virtual-file, image, or inline-data formats. Consume those
    // bounded payloads before handing the source to Shell: async sources may
    // stop serving their IDataObject content as soon as EndOperation runs.
    Container* delayedFileTarget =
        dragSession_.TargetContainer();
    const GridCell delayedFileTargetCell =
        CellFromPoint(clientPoint);
    const bool bareDesktopTarget =
        (delayedFileTarget == GetDesktopGrid() ||
         (!delayedFileTarget &&
          dragSession_.TargetRegion() == HitRegion::None)) &&
        !delayedFileTargetCell.pageId.empty() &&
        dragSession_.TargetRegion() != HitRegion::Handoff &&
        dragSession_.TargetRegion() != HitRegion::Blocked;
    const bool canCopyDrop = ((*effect & DROPEFFECT_COPY) != 0);
    const std::vector<std::wstring> bareDesktopUrls =
        dropPaths.empty() && dataObject && bareDesktopTarget
            ? ExtractDropUrls(dropReferenceSnapshot)
            : std::vector<std::wstring>{};
    const std::wstring bareDesktopUrl = bareDesktopUrls.empty()
        ? std::wstring{} : bareDesktopUrls.front();
    const auto delayedFileDescriptors =
        dropPaths.empty() && dataObject && bareDesktopTarget &&
            !sourceUsesAsyncMode
        ? snowdesktop::virtual_file_drop::ReadDescriptors(dataObject)
        : std::vector<snowdesktop::virtual_file_drop::
            VirtualFileDescriptor>{};
    const bool preferVirtualFilePayload = std::any_of(
        delayedFileDescriptors.begin(), delayedFileDescriptors.end(),
        [](const auto& descriptor) {
            return !IsInternetShortcutDescriptor(descriptor);
        });
    const bool offersImageData =
        dropPaths.empty() && dataObject && bareDesktopTarget &&
        canCopyDrop && !sourceUsesAsyncMode &&
        snowdesktop::drop_image_data::OffersImageData(dataObject);
    if (dropPaths.empty() && dataObject && bareDesktopTarget &&
        canCopyDrop && preferVirtualFilePayload &&
        !sourceUsesAsyncMode)
    {
        bool allVirtualFilesMaterialized = false;
        auto stagedVirtualPaths =
            TryMaterializeVirtualFilesFromDataObject(
            dataObject, delayedFileDescriptors,
            &allVirtualFilesMaterialized);
        if (!allVirtualFilesMaterialized)
        {
            for (const auto& stagedPath : stagedVirtualPaths)
                (void)DeleteFileW(stagedPath.c_str());
            stagedVirtualPaths.clear();
            MessageBeep(MB_ICONWARNING);
        }
        else
        {
            (void)adoptStagedDropPaths(
                std::move(stagedVirtualPaths));
        }
        if (!dropPaths.empty())
        {
            forceCopyDrop = true;
            *effect = DROPEFFECT_COPY;
        }
    }
    if (dropPaths.empty() && offersImageData)
    {
        (void)adoptStagedDropPaths(
            TryExtractImageFromDataObject(dataObject));
        if (!dropPaths.empty())
        {
            forceCopyDrop = true;
            *effect = DROPEFFECT_COPY;
        }
    }
    if (dropPaths.empty() && dataObject && bareDesktopTarget &&
        canCopyDrop)
    {
        (void)adoptStagedDropPaths(
            TryExtractDataUrlFromDataObject(
                dropReferenceSnapshot));
        if (!dropPaths.empty())
        {
            forceCopyDrop = true;
            *effect = DROPEFFECT_COPY;
        }
    }
    if (dropPaths.empty() && dataObject && !fileUrlReference &&
        bareDesktopTarget &&
        canCopyDrop &&
        sourceUsesAsyncMode)
    {
        const std::wstring desktopDirectory =
            UserDesktopDirectory();
        if (!desktopDirectory.empty())
        {
            const auto previousDirectoryPaths =
                SnapshotDirectoryPaths(desktopDirectory);
            const auto existingDesktopKeys =
                SnapshotDesktopKeys();
            struct AsyncDropPreflightState final
            {
                mutable std::mutex mutex;
                DropReferenceSnapshot snapshot;
                std::vector<snowdesktop::virtual_file_drop::
                    VirtualFileDescriptor> descriptors;
                std::vector<std::wstring> contentPaths;
                bool deleteContentPaths = false;
                bool handledByPreflight = false;

                ~AsyncDropPreflightState()
                {
                    std::vector<std::wstring> pathsToDelete;
                    {
                        std::lock_guard lock(mutex);
                        if (deleteContentPaths)
                            pathsToDelete.swap(contentPaths);
                    }
                    for (const auto& path : pathsToDelete)
                        (void)DeleteFileW(path.c_str());
                }

                void Store(DropReferenceSnapshot newSnapshot,
                    std::vector<snowdesktop::virtual_file_drop::
                        VirtualFileDescriptor> newDescriptors,
                    std::vector<std::wstring> newContentPaths,
                    bool shouldDeleteContentPaths,
                    bool newHandledByPreflight)
                {
                    std::lock_guard lock(mutex);
                    snapshot = std::move(newSnapshot);
                    descriptors = std::move(newDescriptors);
                    contentPaths = std::move(newContentPaths);
                    deleteContentPaths = shouldDeleteContentPaths;
                    handledByPreflight = newHandledByPreflight;
                }

                DropReferenceSnapshot Snapshot() const
                {
                    std::lock_guard lock(mutex);
                    return snapshot;
                }

                std::vector<snowdesktop::virtual_file_drop::
                    VirtualFileDescriptor> Descriptors() const
                {
                    std::lock_guard lock(mutex);
                    return descriptors;
                }

                std::vector<std::wstring> ContentPaths() const
                {
                    std::lock_guard lock(mutex);
                    return contentPaths;
                }

                bool HandledByPreflight() const
                {
                    std::lock_guard lock(mutex);
                    return handledByPreflight;
                }

            };
            auto preflightState =
                std::make_shared<AsyncDropPreflightState>();
            std::function<bool(IDataObject*)> dataObjectPreflight =
                [preflightState](IDataObject* workerDataObject) {
                    DropReferenceSnapshot workerSnapshot =
                        ReadDropReferenceSnapshot(workerDataObject);
                    const bool privateResource = std::any_of(
                        workerSnapshot.candidates.begin(),
                        workerSnapshot.candidates.end(),
                        [](const auto& candidate) {
                            return snowdesktop::drop_text_rules::
                                IsPrivateHierarchicalResource(candidate);
                        });
                    const bool fileResource = std::any_of(
                        workerSnapshot.candidates.begin(),
                        workerSnapshot.candidates.end(),
                        [](const auto& candidate) {
                            return candidate.kind == snowdesktop::
                                drop_text_rules::Kind::FileUrl;
                        });
                    auto workerDescriptors =
                        snowdesktop::virtual_file_drop::ReadDescriptors(
                            workerDataObject);
                    const bool offersStandardVirtualFile = std::any_of(
                        workerDescriptors.begin(), workerDescriptors.end(),
                        [](const auto& descriptor) {
                            return !IsInternetShortcutDescriptor(
                                descriptor);
                        });

                    std::vector<std::wstring> contentPaths;
                    bool deleteContentPaths = false;
                    if (fileResource)
                    {
                        contentPaths =
                            TryExtractLocalFileUrlFromDataObject(
                                workerSnapshot);
                        deleteContentPaths = false;
                    }
                    // When no real virtual file is offered, prefer image
                    // bytes that the source already placed on the data
                    // object.  This runs on the Shell STA worker after
                    // StartOperation, so producer-backed streams can be read
                    // here without blocking the immediate UI-thread probe.
                    // A valid file: reference was resolved first so the
                    // original local file is never re-encoded as PNG.
                    if (contentPaths.empty() &&
                        (privateResource ||
                         !offersStandardVirtualFile))
                    {
                        contentPaths = TryExtractImageFromDataObject(
                            workerDataObject, true);
                        deleteContentPaths = !contentPaths.empty();
                    }
                    if (contentPaths.empty())
                    {
                        contentPaths = TryExtractDataUrlFromDataObject(
                            workerSnapshot);
                        deleteContentPaths = !contentPaths.empty();
                    }
                    const bool handled = !contentPaths.empty();
                    const bool handledByPreflight =
                        !offersStandardVirtualFile &&
                        (handled || privateResource || fileResource);
                    preflightState->Store(
                        std::move(workerSnapshot),
                        std::move(workerDescriptors),
                        std::move(contentPaths),
                        deleteContentPaths,
                        handledByPreflight);
                    // A private marker without a standard virtual file must
                    // never be delegated to Shell as a fake .url/.txt file.
                    // If no standard bytes were exposed, consume the drop
                    // without manufacturing a link the OS cannot resolve.
                    return handledByPreflight;
                };

            auto completed = [
                this, desktopDirectory,
                previousDirectoryPaths,
                existingDesktopKeys, delayedFileTargetCell,
                bareDesktopUrl, bareDesktopUrls,
                preflightState](bool succeeded) mutable {
                const DropReferenceSnapshot workerSnapshot =
                    preflightState->Snapshot();
                const auto descriptors =
                    preflightState->Descriptors();
                const std::vector<std::wstring> workerContentPaths =
                    preflightState->ContentPaths();
                const bool handledByPreflight =
                    preflightState->HandledByPreflight();
                std::optional<std::vector<std::wstring>> newPaths;
                if (previousDirectoryPaths)
                {
                    newPaths = FindNewDirectoryPaths(
                        desktopDirectory,
                        *previousDirectoryPaths);
                }
                const size_t expectedFileCount = std::max({
                    size_t{1}, descriptors.size(),
                    workerContentPaths.size(),
                    newPaths ? newPaths->size() : size_t{0}});
                const DropPreviewList requestedPreview =
                    BuildExternalDesktopPreviewList(
                        delayedFileTargetCell,
                        expectedFileCount);
                std::vector<std::wstring> privateResourceTargets;
                for (const auto& candidate : workerSnapshot.candidates)
                {
                    if (snowdesktop::drop_text_rules::
                            IsPrivateHierarchicalResource(candidate) &&
                        std::find(privateResourceTargets.begin(),
                            privateResourceTargets.end(),
                            candidate.value) ==
                            privateResourceTargets.end())
                    {
                        privateResourceTargets.push_back(
                            candidate.value);
                    }
                }
                const bool privateResource =
                    !privateResourceTargets.empty();
                const bool fileResource = std::any_of(
                    workerSnapshot.candidates.begin(),
                    workerSnapshot.candidates.end(),
                    [](const auto& candidate) {
                        return candidate.kind == snowdesktop::
                            drop_text_rules::Kind::FileUrl;
                    });
                std::vector<std::wstring> resolvedBareDesktopUrls =
                    bareDesktopUrls;
                for (auto& url : ExtractDropUrls(workerSnapshot))
                {
                    if (std::find(resolvedBareDesktopUrls.begin(),
                            resolvedBareDesktopUrls.end(), url) ==
                        resolvedBareDesktopUrls.end())
                    {
                        resolvedBareDesktopUrls.push_back(
                            std::move(url));
                    }
                }
                const std::wstring resolvedBareDesktopUrl =
                    resolvedBareDesktopUrls.empty()
                        ? bareDesktopUrl
                        : resolvedBareDesktopUrls.front();
                bool shouldTryUrlFallback = false;
                std::vector<UrlDropReplacementShortcut>
                    replacementShortcuts;
                std::wstring replacementShortcutTarget;
                bool replacementShortcutTargetIsPrivate = false;
                if (newPaths)
                {
                    shouldTryUrlFallback =
                        newPaths->empty();
                    if (!newPaths->empty())
                    {
                        const bool onlyInternetShortcuts =
                            std::all_of(
                                newPaths->begin(), newPaths->end(),
                                IsInternetShortcutPath);
                        if (onlyInternetShortcuts)
                        {
                            const auto allShortcutsTarget =
                                [&newPaths](const std::wstring& target) {
                                    return !target.empty() && std::all_of(
                                        newPaths->begin(), newPaths->end(),
                                        [&target](const auto& path) {
                                            return ReadInternetShortcutTarget(
                                                path) == target;
                                        });
                                };
                            if (allShortcutsTarget(
                                    resolvedBareDesktopUrl))
                            {
                                replacementShortcutTarget =
                                    resolvedBareDesktopUrl;
                            }
                            else
                            {
                                const auto privateTarget = std::find_if(
                                    privateResourceTargets.begin(),
                                    privateResourceTargets.end(),
                                    allShortcutsTarget);
                                if (privateTarget !=
                                    privateResourceTargets.end())
                                {
                                    replacementShortcutTarget =
                                        *privateTarget;
                                    replacementShortcutTargetIsPrivate =
                                        true;
                                }
                            }
                        }
                        if (!replacementShortcutTarget.empty())
                        {
                            shouldTryUrlFallback = true;
                            replacementShortcuts.reserve(
                                newPaths->size());
                            for (const auto& path : *newPaths)
                            {
                                UrlDropReplacementShortcut replacement;
                                replacement.path = path;
                                HANDLE file = CreateFileW(
                                    path.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL |
                                        FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
                                BY_HANDLE_FILE_INFORMATION information{};
                                if (file != INVALID_HANDLE_VALUE)
                                {
                                    if (GetFileInformationByHandle(
                                            file, &information) &&
                                        (information.dwFileAttributes &
                                            (FILE_ATTRIBUTE_DIRECTORY |
                                             FILE_ATTRIBUTE_REPARSE_POINT)) == 0)
                                    {
                                        replacement.volumeSerialNumber =
                                            information.dwVolumeSerialNumber;
                                        replacement.fileIndexHigh =
                                            information.nFileIndexHigh;
                                        replacement.fileIndexLow =
                                            information.nFileIndexLow;
                                        replacement.identityValid = true;
                                    }
                                    CloseHandle(file);
                                }
                                replacementShortcuts.push_back(
                                    std::move(replacement));
                            }
                            if (std::any_of(
                                    replacementShortcuts.begin(),
                                    replacementShortcuts.end(),
                                    [](const auto& replacement) {
                                        return !replacement.identityValid;
                                    }))
                            {
                                shouldTryUrlFallback = false;
                                replacementShortcuts.clear();
                                replacementShortcutTarget.clear();
                                replacementShortcutTargetIsPrivate = false;
                            }
                        }
                        std::vector<std::optional<std::wstring>>
                            pathsBySource(expectedFileCount);
                        std::vector<bool> pathUsed(
                            newPaths->size(), false);
                        if (descriptors.empty())
                        {
                            // Without descriptors there is no stable way to
                            // distinguish multiple Shell outputs from files
                            // concurrently created on the real desktop.
                            if (newPaths->size() == 1)
                                pathsBySource[0] =
                                    newPaths->front();
                        }
                        else
                        {
                            const auto assignUniqueMatchingPath =
                                [&](size_t sourceIndex,
                                    bool exactNameOnly) {
                                if (pathsBySource[sourceIndex])
                                    return;
                                size_t matchingPath =
                                    newPaths->size();
                                size_t matchCount = 0;
                                for (size_t index = 0;
                                    index < newPaths->size();
                                    ++index)
                                {
                                    if (pathUsed[index] ||
                                        !(exactNameOnly
                                            ? _wcsicmp(
                                                FileNameFromPath(
                                                    (*newPaths)[index]).
                                                    c_str(),
                                                descriptors[sourceIndex].
                                                    suggestedFileName.
                                                    c_str()) == 0
                                            : MatchPendingName(
                                                FileNameFromPath(
                                                    (*newPaths)[index]),
                                                descriptors[sourceIndex].
                                                    suggestedFileName)))
                                        continue;
                                    matchingPath = index;
                                    ++matchCount;
                                }
                                if (matchCount != 1)
                                    return;
                                pathUsed[matchingPath] = true;
                                pathsBySource[sourceIndex] =
                                    (*newPaths)[matchingPath];
                            };
                            // Reserve every exact descriptor name first.
                            // Only the remaining sources may use the fuzzy
                            // localized/Shell collision suffix rules; this
                            // prevents a legitimate "a (2).txt" descriptor
                            // from making the separate "a.txt" ambiguous.
                            for (size_t sourceIndex = 0;
                                sourceIndex < descriptors.size() &&
                                sourceIndex < pathsBySource.size();
                                ++sourceIndex)
                            {
                                assignUniqueMatchingPath(
                                    sourceIndex, true);
                            }
                            for (size_t sourceIndex = 0;
                                sourceIndex < descriptors.size() &&
                                sourceIndex < pathsBySource.size();
                                ++sourceIndex)
                            {
                                assignUniqueMatchingPath(
                                    sourceIndex, false);
                            }
                        }

                        DragSourceList sourceList;
                        sourceList.hasExternalFiles = true;
                        std::unordered_map<size_t,
                            std::wstring>
                                createdPathsBySource;
                        for (size_t sourceIndex = 0;
                            sourceIndex < pathsBySource.size();
                            ++sourceIndex)
                        {
                            const auto& path =
                                pathsBySource[sourceIndex];
                            if (!path)
                                continue;
                            DragSourceEntry entry;
                            entry.kind =
                                DropSourceKind::ExternalFile;
                            entry.sourceIndex = sourceIndex;
                            entry.filePath = *path;
                            entry.displayName =
                                FileNameFromPath(*path);
                            entry.originalSpan = {1, 1};
                            createdPathsBySource.emplace(
                                entry.sourceIndex, *path);
                            sourceList.entries.push_back(
                                std::move(entry));
                        }
                        if (!sourceList.entries.empty() &&
                            !requestedPreview.Empty())
                        {
                            StorePendingLandingCache(
                                sourceList,
                                requestedPreview,
                                existingDesktopKeys,
                                &createdPathsBySource);
                        }
                    }
                }
                bool contentFallbackExecuted = false;
                if (!workerContentPaths.empty() &&
                    (handledByPreflight || shouldTryUrlFallback))
                {
                    DragSourceList fallbackSources;
                    fallbackSources.hasExternalFiles = true;
                    for (size_t index = 0;
                        index < workerContentPaths.size(); ++index)
                    {
                        DragSourceEntry entry;
                        entry.kind = DropSourceKind::ExternalFile;
                        entry.sourceIndex = index;
                        entry.filePath =
                            workerContentPaths[index];
                        entry.displayName = FileNameFromPath(
                            workerContentPaths[index]);
                        entry.originalSpan = {1, 1};
                        fallbackSources.entries.push_back(
                            std::move(entry));
                    }
                    auto contentCompletion = [
                        this, preflightState,
                        replacementShortcuts,
                        replacementShortcutTarget,
                        replacementShortcutTargetIsPrivate,
                        requestedPreview, existingDesktopKeys,
                        workerContentPaths](bool copySucceeded) mutable {
                        if (!copySucceeded)
                        {
                            if (replacementShortcutTargetIsPrivate &&
                                RemoveMatchingUrlDropShortcuts(
                                    replacementShortcuts,
                                    replacementShortcutTarget))
                            {
                                MessageBeep(MB_ICONWARNING);
                                ReloadItems(false);
                            }
                            return;
                        }

                        if (replacementShortcuts.empty() ||
                            !RemoveMatchingUrlDropShortcuts(
                                replacementShortcuts,
                                replacementShortcutTarget))
                            return;

                        DragSourceList placementSources;
                        placementSources.hasExternalFiles = true;
                        for (size_t index = 0;
                            index < workerContentPaths.size(); ++index)
                        {
                            DragSourceEntry entry;
                            entry.kind = DropSourceKind::ExternalFile;
                            entry.sourceIndex = index;
                            entry.filePath = workerContentPaths[index];
                            entry.displayName = FileNameFromPath(
                                workerContentPaths[index]);
                            entry.originalSpan = {1, 1};
                            placementSources.entries.push_back(
                                std::move(entry));
                        }
                        StorePendingLandingCache(
                            placementSources, requestedPreview,
                            existingDesktopKeys, nullptr);
                        ReloadItems(false);
                    };
                    contentFallbackExecuted = ExecuteDropPipeline(
                        fallbackSources, requestedPreview,
                        std::move(contentCompletion), false);
                }
                bool removedUnusablePrivateShortcut = false;
                if (!contentFallbackExecuted &&
                    replacementShortcutTargetIsPrivate &&
                    !replacementShortcuts.empty())
                {
                    removedUnusablePrivateShortcut =
                        RemoveMatchingUrlDropShortcuts(
                            replacementShortcuts,
                            replacementShortcutTarget);
                    if (removedUnusablePrivateShortcut)
                        replacementShortcuts.clear();
                }
                bool urlFallbackQueued = false;
                if (!contentFallbackExecuted &&
                    (handledByPreflight || shouldTryUrlFallback) &&
                    expectedFileCount == 1 &&
                    !resolvedBareDesktopUrl.empty() &&
                    replacementShortcuts.size() <= 1 &&
                    (!replacementShortcutTargetIsPrivate ||
                        replacementShortcuts.empty()))
                {
                    std::vector<std::wstring> fallbackUrls =
                        replacementShortcuts.empty()
                            ? resolvedBareDesktopUrls
                            : std::vector<std::wstring>{
                                resolvedBareDesktopUrl};
                    urlFallbackQueued = QueueUrlDropDownload(
                        std::move(fallbackUrls),
                        requestedPreview,
                        std::move(replacementShortcuts));
                }
                if (!urlFallbackQueued && !contentFallbackExecuted)
                {
                    if (removedUnusablePrivateShortcut ||
                        (replacementShortcutTargetIsPrivate &&
                         !replacementShortcuts.empty()) ||
                        (succeeded && privateResource &&
                         workerContentPaths.empty() &&
                         resolvedBareDesktopUrls.empty()) ||
                        (succeeded && fileResource &&
                         workerContentPaths.empty() &&
                         resolvedBareDesktopUrls.empty()))
                        MessageBeep(MB_ICONWARNING);
                    ReloadItems(false);
                }
            };

            const bool shellDropQueued = QueueAsyncShellDrop(
                    dataObject, desktopDirectory,
                    keyState, point,
                    DROPEFFECT_COPY,
                    std::move(completed),
                    std::move(dataObjectPreflight));
            if (shellDropQueued)
            {
                *effect = DROPEFFECT_COPY;
                EndDragSession();
                return S_OK;
            }
        }
    }

    // Some browsers expose a dragged network resource only as a URL. Resolve
    // its response off-thread so extensionless images and documents are saved
    // while actual HTML pages still materialize as URL shortcuts.
    const DWORD urlDropEffect = (*effect & DROPEFFECT_COPY) != 0
        ? DROPEFFECT_COPY
        : ((*effect & DROPEFFECT_LINK) != 0
            ? DROPEFFECT_LINK : DROPEFFECT_NONE);
    if (dropPaths.empty() && dataObject && bareDesktopTarget &&
        delayedFileDescriptors.size() <= 1 &&
        urlDropEffect != DROPEFFECT_NONE)
    {
        if (!bareDesktopUrl.empty())
        {
            DropPreviewList requestedPreview =
                BuildExternalDesktopPreviewList(
                    delayedFileTargetCell, 1);
            if (QueueUrlDropDownload(
                    bareDesktopUrls,
                    std::move(requestedPreview)))
            {
                *effect = urlDropEffect;
                EndDragSession();
                return S_OK;
            }
        }
    }

    if (dropPaths.empty() && dataObject && bareDesktopTarget &&
        !bareDesktopUrl.empty() && canCopyDrop)
    {
        (void)adoptStagedDropPaths(
            TryExtractUrlFromDataObject(
                dropReferenceSnapshot));
        if (!dropPaths.empty())
        {
            forceCopyDrop = true;
            *effect = DROPEFFECT_COPY;
        }
    }

    if (dropPaths.empty() && dataObject && canCopyDrop &&
        !sourceUsesAsyncMode &&
        (!bareDesktopTarget || bareDesktopUrl.empty()) &&
        (!bareDesktopTarget || delayedFileDescriptors.size() <= 1))
    {
        dropPaths = localFileUrlPaths;
        if (dropPaths.empty())
        {
            (void)adoptStagedDropPaths(
                TryGetNonFileDropPaths(
                    dataObject,
                    dropReferenceSnapshot));
        }
        if (!dropPaths.empty())
        {
            forceCopyDrop = true;
            *effect = DROPEFFECT_COPY;
        }
    }

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

        if (auto* logicalSlot =
                dynamic_cast<LuaLogicalSlotContainer*>(target))
        {
            const bool committed = logicalSlot->CommitItems(
                sourceItems,
                dragSession_.TargetContainer()
                    ? dragSession_.TargetSlot() : nullptr,
                targetRegion);
            if (committed && stagedDropPathLease)
                stagedDropPathLease->Keep();
            EndDragSession();
            InvalidateRect(hwnd_, nullptr, FALSE);
            *effect = committed ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

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
                if (stagedDropPathLease)
                    stagedDropPathLease->Keep();
                EndDragSession();
                InvalidateRect(hwnd_, nullptr, FALSE);
                *effect = mappingEffect;
                return S_OK;
            }
        }

        DropPreviewList preview = BuildDropPreviewList(sourceList, target,
            dragSession_.TargetContainer() ? dragSession_.TargetSlot() : nullptr, targetRegion, mods, clientPoint);
        if (forceCopyDrop)
            preview.action = DropAction::Copy;
        const bool dockFolderPopupTarget =
            IsOpenDockFolderPopupDropTarget(
                target,
                dragSession_.TargetSlot()
                    ? dragSession_.TargetSlot()->GetItem()
                    : nullptr);
        if (dockFolderPopupTarget && !forceCopyDrop)
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
    POINT desktopPoint{};
    const bool pointerOnDesktopSurface =
        dragDropController_.IsSelfDragActive() &&
        dragDropController_.SelfDragReturned() &&
        TryGetNativeDragResumePointFromCursor(desktopPoint);
    return dragDropController_.QueryContinueSelfDrag(
        escapePressed != FALSE,
        (keyState & MK_LBUTTON) != 0,
        pointerOnDesktopSurface);
}

/**
 * @brief COM IDropSource::GiveFeedback 实现
 * @return DRAGDROP_S_USEDEFAULTCURSORS（使用默认光标）
 */
HRESULT DesktopApp::HandleOleGiveFeedback(DWORD)
{
    if (dragDropController_.IsSelfDragActive() &&
        dragDropController_.SelfDragReturned())
    {
        // DragEnter has selected the native hand-back path. Remove OLE's
        // effect-overlay cursor immediately; the custom ghost is still kept
        // hidden until DoDragDrop has returned.
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return S_OK;
    }
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

/**
 * @brief 从数据对象中提取文件路径列表
 * @param dataObject COM 数据对象
 * @return 文件路径列表
 */
