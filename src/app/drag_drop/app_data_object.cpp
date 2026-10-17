#include "app/app.h"
#include "desktop/folder_mapping_visibility_rules.h"
#include "desktop/desktop_membership_recovery.h"
#include "drag_drop/desktop_landings.h"

// OLE data-object construction and desktop landing bookkeeping.

POINT DesktopApp::GetDragTargetPoint(POINT current) const
{
    // DeactivateForDrop only ends interactive rendering; the adjusted session
    // coordinates remain authoritative until the drop plan has been committed.
    if (dragSession_.HasContext())
    {
        return dragSession_.ResolveTargetPoint(
            { dragGroupOriginX_, dragGroupOriginY_ }, current);
    }
    return {
        dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
        dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
    };
}

GridCell DesktopApp::ResolveDesktopRequestCell(
    const DragSourceList& sourceList, POINT current) const
{
    if (sourceList.UsesPointerDesktopPlacement() || dragSession_.IsPointerAnchored())
        return CellFromPoint(current);
    return CellFromDragOrigin(GetDragTargetPoint(current), current);
}

void DesktopApp::RefreshDragPresentationAnchor()
{
    if (!dragSession_.IsActive())
    {
        dragSession_.ClearPresentationAnchor();
        return;
    }

    Container* target = dragSession_.TargetContainer();
    const HitRegion region = dragSession_.TargetRegion();
    if (target != GetDesktopGrid() ||
        region == HitRegion::None ||
        region == HitRegion::Handoff ||
        region == HitRegion::Blocked)
    {
        dragSession_.ClearPresentationAnchor();
        return;
    }

    const DragSourceList& sourceList = dragSession_.SourceList();
    const POINT current = dragSession_.CurrentPoint();
    dragSession_.UpdatePresentationAnchor(
        ResolveDesktopRequestCell(sourceList, current));
}

/**
 * @brief 为选中的桌面项创建 IDataObject（用于拖拽/剪贴板）。
 * @return COM 数据对象，失败返回 nullptr。
 */
ComPtr<IDataObject> DesktopApp::CreateSelectedDataObject() const
{
    std::vector<PCUITEMID_CHILD> pidls;
    for (const auto& item : items_)
    {
        if (item.selected)
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()));
    }
    if (pidls.empty()) return nullptr;

    ComPtr<IDataObject> dataObject;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IDataObject, nullptr,
        reinterpret_cast<void**>(dataObject.GetAddressOf()));
    if (FAILED(hr)) return nullptr;
    return dataObject;
}

/**
 * @brief 为指定文件路径列表创建文件拖拽数据对象。
 * @param paths 文件路径列表。
 * @return COM 数据对象，失败返回 nullptr。
 */
ComPtr<IDataObject> DesktopApp::CreateFileDropDataObject(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return nullptr;

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& path : paths)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
            pidls.push_back(pidl);
    }

    if (pidls.empty()) return nullptr;

    auto freePidls = [&]() {
        for (auto* pidl : pidls)
            ILFree(pidl);
    };

    ComPtr<IShellFolder> parentFolder;
    PCUITEMID_CHILD unusedChild = nullptr;
    HRESULT hr = SHBindToParent(pidls.front(), IID_IShellFolder,
        reinterpret_cast<void**>(parentFolder.GetAddressOf()), &unusedChild);
    if (FAILED(hr) || !parentFolder)
    {
        freePidls();
        return nullptr;
    }

    std::vector<PCUITEMID_CHILD> children;
    children.reserve(pidls.size());
    for (auto* pidl : pidls)
        children.push_back(ILFindLastID(pidl));

    ComPtr<IDataObject> dataObject;
    hr = parentFolder->GetUIObjectOf(nullptr, static_cast<UINT>(children.size()), children.data(),
        IID_IDataObject, nullptr, reinterpret_cast<void**>(dataObject.GetAddressOf()));
    freePidls();

    if (FAILED(hr)) return nullptr;
    return dataObject;
}

/**
 * @brief 根据源项目列表创建数据对象（桌面图标优先，否则用文件路径）。
 * @param sourceItems 源项目列表。
 * @return COM 数据对象，失败返回 nullptr。
 */
ComPtr<IDataObject> DesktopApp::CreateDataObjectForItems(
    const std::vector<Item*>& sourceItems) const
{
    DropPayload payload = DropPayload::From(sourceItems);
    if (payload.hasDesktopIcons)
    {
        if (ComPtr<IDataObject> desktopObject = CreateSelectedDataObject())
            return desktopObject;
    }
    return CreateFileDropDataObject(payload.filePaths);
}

/**
 * @brief 将选中项目拖拽放置到目标桌面项上（调用 Shell IDropTarget 接口）。
 * @param targetIndex 目标桌面项的索引。
 */
void DesktopApp::DropSelectedItemsOnTarget(int targetIndex)
{
    if (targetIndex < 0 || static_cast<size_t>(targetIndex) >= items_.size()) return;
    auto& targetItem = items_[targetIndex];

    ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
    if (!dataObj) return;

    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(targetItem.childPidl.get());
    ComPtr<IDropTarget> dropTarget;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, 1, &child, IID_IDropTarget, nullptr,
        reinterpret_cast<void**>(dropTarget.GetAddressOf()));
    if (FAILED(hr) || !dropTarget) return;

    POINT screenPt = dragSession_.CurrentPoint();
    ClientToScreen(hwnd_, &screenPt);
    POINTL screenPtL{ screenPt.x, screenPt.y };

    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    hr = dropTarget->DragEnter(dataObj.Get(), MK_LBUTTON, screenPtL, &effect);
    if (SUCCEEDED(hr))
        hr = dropTarget->DragOver(MK_LBUTTON, screenPtL, &effect);
    if (SUCCEEDED(hr))
        hr = dropTarget->Drop(dataObj.Get(), MK_LBUTTON, screenPtL, &effect);
    else
        dropTarget->DragLeave();
}

/**
 * @brief 根据布局键查找项目索引。
 * @param key 项目布局键。
 * @return 项目索引，未找到返回 -1。
 */
size_t DesktopApp::FindItemIndexByKey(const std::wstring& key) const
{
    std::wstring normalized = ToUpperInvariant(key);
    auto it = itemIndexByKeyCache_.find(normalized);
    if (it != itemIndexByKeyCache_.end() && it->second < items_.size())
        return it->second;
    return static_cast<size_t>(-1);
}

void DesktopApp::RefreshDesktopItemIndexCache()
{
    itemIndexByKeyCache_.clear();
    itemIndexByKeyCache_.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (!items_[i].layoutKey.empty())
            itemIndexByKeyCache_.emplace(ToUpperInvariant(items_[i].layoutKey), i);
    }
}

void DesktopApp::RefreshCollectedKeysCache()
{
    collectedKeysCache_.clear();
    for (const auto& widget : widgets_)
    {
        if (widget.type == DesktopWidgetType::FolderMapping) continue;
        for (const auto& key : widget.itemKeys)
            if (!key.empty())
                collectedKeysCache_.insert(ToUpperInvariant(key));
    }
    for (const auto& entry : dockEntries_)
    {
        if (entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            !entry.reference.empty())
            collectedKeysCache_.insert(ToUpperInvariant(entry.reference));
    }
    snowdesktop::folder_mapping_visibility::HideMappedSources(
        items_, widgets_, collectedKeysCache_,
        [](const std::wstring& key) { return ToUpperInvariant(key); });
}

/**
 * @brief 从所有组件中移除指定桌面键。
 * @param keys 要移除的布局键列表。
 */
void DesktopApp::RemoveDesktopKeysFromWidgets(const std::vector<std::wstring>& keys)
{
    if (keys.empty()) return;

    for (auto& widget : widgets_)
        snowdesktop::desktop_membership_recovery::Forget(widget, keys);
    RefreshCollectedKeysCache();
}

/**
 * @brief 快照当前所有桌面项的布局键。
 * @return 布局键的集合。
 */
std::unordered_set<std::wstring> DesktopApp::SnapshotDesktopKeys() const
{
    std::unordered_set<std::wstring> keys;
    for (const auto& item : items_)
        if (!item.layoutKey.empty())
            keys.insert(ToUpperInvariant(item.layoutKey));
    return keys;
}

/**
 * @brief 构建桌面放置列表，为拖拽源中的每个条目分配网格位置。
 * @param sourceList 拖拽源列表。
 * @param targetCell 目标网格单元格。
 * @param internalMove 是否为内部移动。
 * @return 放置操作列表。
 */
std::vector<DropLanding> DesktopApp::BuildDesktopLandings(
    const DragSourceList& sourceList, GridCell targetCell, bool internalMove) const
{
    std::vector<DropLanding> landings;
    if (sourceList.Empty()) return landings;

    if (targetCell.pageId.empty())
    {
        const GridPage* firstPage = GetFirstPageGridPage();
        if (firstPage) targetCell.pageId = firstPage->id;
    }
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return landings;

    bool desktopToDesktopMove = internalMove &&
        (sourceList.hasDesktopIcons || sourceList.HasDesktopWidgetIcons()) &&
        !containers_.empty() && sourceList.origin == containers_.front().get();
    if (desktopToDesktopMove)
    {
        std::vector<PendingGridMove> moves = BuildSelectedMove(targetCell);
        if (moves.empty()) return landings;

        for (const auto& entry : sourceList.entries)
        {
            auto it = std::find_if(moves.begin(), moves.end(),
                [&](const PendingGridMove& move) {
                    return entry.desktopWidgetIcon
                        ? move.widget && move.index == FindWidgetIndexById(entry.widgetId)
                        : !move.widget && move.index == entry.desktopIndex;
                });
            if (it == moves.end()) continue;
            DropLanding landing;
            landing.kind = DropLandingKind::DesktopCell;
            landing.sourceIndex = entry.sourceIndex;
            landing.cell = it->cell;
            landing.span = entry.originalSpan;
            landings.push_back(landing);
        }
        return landings;
    }

    snowdesktop::desktop_landings::Services services;
    services.grouped = [this](const auto& widget) { return IsGroupedWidget(widget); };
    services.owned = [this](const auto& item) { return IsItemInAnyWidget(item); };
    services.normalize = ToUpperInvariant;
    services.findFree = [this](auto span, auto& used, auto& cell, const auto& preferred, int slot) {
        return TryFindFreeCell(span, used, cell, preferred, slot);
    };
    services.generateId = [this] { return GeneratePageId(); };
    return snowdesktop::desktop_landings::Build(sourceList, std::move(targetCell), internalMove, *page,
        widgets_, items_, savedPageColumns_, savedPageRows_, services);
}

/**
 * @brief 根据源项目和所属容器构建拖拽源列表。
 * @param sourceItems 源项目指针列表。
 * @param origin 来源容器。
 * @return 拖拽源列表。
 */
