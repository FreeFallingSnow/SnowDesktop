#include "app.h"
#include "../widgets/collection_group_rules.h"

// Widget creation, collection/file-group membership and release operations.

std::wstring DesktopApp::MakeNewWidgetId() const
{
    return L"widget-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
}

void DesktopApp::ConfigureWidgetGridLimits(DesktopWidget& widget) const
{
    widget.minGridSpan = { 1, 1 };
    widget.maxGridSpan = { 0, 0 };

    if (widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup ||
        widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping)
    {
        widget.minGridSpan = { 2, 2 };
    }
    else if (widget.type == DesktopWidgetType::LuaScript && !widget.packageId.empty())
    {
        LuaWidgetManifest manifest = WidgetEngine::GetWidgetManifest(widget.packageId);
        widget.minGridSpan = {
            std::max(1, manifest.minColumns),
            std::max(1, manifest.minRows)
        };
        widget.maxGridSpan = {
            std::max(0, manifest.maxColumns),
            std::max(0, manifest.maxRows)
        };
    }
}

GridSpan DesktopApp::ClampWidgetGridSpan(const DesktopWidget& widget, GridSpan span,
    int availableColumns, int availableRows) const
{
    const int pageMaxColumns = std::max(1, availableColumns);
    const int pageMaxRows = std::max(1, availableRows);
    const int maxColumns = widget.maxGridSpan.columns > 0
        ? std::min(pageMaxColumns, widget.maxGridSpan.columns)
        : pageMaxColumns;
    const int maxRows = widget.maxGridSpan.rows > 0
        ? std::min(pageMaxRows, widget.maxGridSpan.rows)
        : pageMaxRows;
    const int minColumns = std::min(maxColumns, std::max(1, widget.minGridSpan.columns));
    const int minRows = std::min(maxRows, std::max(1, widget.minGridSpan.rows));

    span.columns = std::clamp(span.columns, minColumns, maxColumns);
    span.rows = std::clamp(span.rows, minRows, maxRows);
    return span;
}

/**
 * @brief 将组件添加到网格中，自动查找空闲位置。
 * @param widget 组件对象（移动语义）。
 * @param span 组件跨度。
 */
void DesktopApp::AddWidgetToGrid(DesktopWidget&& widget, GridSpan span)
{
    ConfigureWidgetGridLimits(widget);
    const GridPage* page = GridPageFromScreenPoint(lastContextMenuScreenPoint_);
    GridCell cell;
    if (page)
    {
        cell.pageId = page->id;
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        cell.column = GetGridAxisIndexFromPoint(*page, clientPoint.x, true);
        cell.row = GetGridAxisIndexFromPoint(*page, clientPoint.y, false);
    }
    if (cell.pageId.empty())
    {
        if (const GridPage* firstPage = GetFirstPageGridPage())
            cell = { firstPage->id, 0, 0 };
        if (cell.pageId.empty()) return;
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w) &&
            !(w.type == DesktopWidgetType::Guide &&
                w.gridCell.pageId == cell.pageId))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell freeCell;
    const GridPage* cellPage = FindGridPage(gridPages_, cell.pageId);
    if (cellPage)
        span = ClampWidgetGridSpan(widget, span, cellPage->columns, cellPage->rows);
    bool needSearch = AreGridSlotsMarked(usedSlots, cell, span) ||
        !IsGridAreaValid(cell, span);
    if (!needSearch && cellPage)
    {
        if (cell.column + span.columns > cellPage->columns ||
            cell.row + span.rows > cellPage->rows)
            needSearch = true;
    }
    if (needSearch)
    {
        int startSlot = 0;
        const GridPage* searchPage = FindGridPage(gridPages_, cell.pageId);
        if (searchPage)
            startSlot = cell.column * std::max(1, searchPage->rows) + cell.row;
        if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, startSlot))
        {
            if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, 0))
                TryFindFreeCell(span, usedSlots, freeCell, L"", 0);
        }
        if (freeCell.pageId.empty()) return;
        cell = freeCell;
    }

    widget.gridCell = cell;
    widget.gridSpan = span;
    widgets_.push_back(std::move(widget));
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 在右键菜单位置添加集合组件。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddCollectionWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::Collection;
    w.title = _LW("widget.collection");
    w.showTitle = true;
    w.bottomBarHover = true;
    AddWidgetToGrid(std::move(w), { 1, 1 });
    ShowWidgetAddedHint();
}

/**
 * @brief 在右键菜单位置添加集合组组件。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddCollectionGroupWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget widget;
    widget.id = MakeNewWidgetId();
    widget.type = DesktopWidgetType::CollectionGroup;
    widget.title = _LW("widget.collection_group");
    widget.showTitle = true;
    AddWidgetToGrid(std::move(widget), { 2, 2 });
    ShowWidgetAddedHint();
}

void DesktopApp::AddFileGroupWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget widget;
    widget.id = MakeNewWidgetId();
    widget.type = DesktopWidgetType::FileGroup;
    widget.title = _LW("widget.file_group");
    widget.showTitle = true;
    widget.showFileCategories = true;
    widget.showSearchBox = false;
    widget.listMode = false;
    widget.dateHeaders = false;
    AddWidgetToGrid(std::move(widget), { 2, 2 });
    ShowWidgetAddedHint();
}

size_t DesktopApp::HitTestCollectionGroupIndex(
    POINT point, size_t excludeWidgetIndex) const
{
    for (size_t i = widgets_.size(); i-- > 0;)
    {
        if (i == excludeWidgetIndex ||
            widgets_[i].type != DesktopWidgetType::CollectionGroup)
            continue;
        RECT bounds = widgets_[i].bounds;
        if (!IsRectEmptyRect(bounds) && PtInRect(&bounds, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

size_t DesktopApp::HitTestFileGroupIndex(
    POINT point, size_t excludeWidgetIndex) const
{
    for (size_t i = widgets_.size(); i-- > 0;)
    {
        if (i == excludeWidgetIndex ||
            widgets_[i].type != DesktopWidgetType::FileGroup)
            continue;
        RECT bounds = widgets_[i].bounds;
        if (!IsRectEmptyRect(bounds) && PtInRect(&bounds, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

bool DesktopApp::AddWidgetToFileGroup(
    size_t childIndex, size_t groupIndex, size_t insertIndex)
{
    if (childIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        childIndex == groupIndex ||
        widgets_[groupIndex].type != DesktopWidgetType::FileGroup)
        return false;
    const DesktopWidgetType childType = widgets_[childIndex].type;
    const auto childKind =
        childType == DesktopWidgetType::FileCategories
            ? snowdesktop::collection_group_rules::
                FileGroupChildKind::DesktopFileCategories
            : (childType ==
                    DesktopWidgetType::FolderMapping
                ? snowdesktop::collection_group_rules::
                    FileGroupChildKind::FolderMapping
                : snowdesktop::collection_group_rules::
                    FileGroupChildKind::Collection);
    if (!snowdesktop::collection_group_rules::
            AcceptsFileGroupChild(childKind))
        return false;

    const std::wstring childId = widgets_[childIndex].id;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FileGroup) continue;
        std::erase(widget.childWidgetIds, childId);
        if (widget.activeCategoryId == childId)
            widget.activeCategoryId =
                widget.childWidgetIds.empty()
                    ? L""
                    : widget.childWidgetIds.front();
    }

    DesktopWidget& group = widgets_[groupIndex];
    if (insertIndex == static_cast<size_t>(-1))
        insertIndex = group.childWidgetIds.size();
    insertIndex = std::min(insertIndex, group.childWidgetIds.size());
    group.childWidgetIds.insert(
        group.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(insertIndex),
        childId);
    if (group.activeCategoryId.empty())
        group.activeCategoryId = childId;
    widgets_[childIndex].selected = false;
    group.scrollOffset = 0;
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::MoveFolderMappingsToFileGroup(
    const std::vector<Item*>& sourceItems,
    size_t groupIndex, size_t insertIndex)
{
    if (sourceItems.empty() ||
        groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::FileGroup)
        return false;

    std::vector<std::wstring> movingIds;
    for (Item* source : sourceItems)
    {
        std::wstring id;
        if (auto* dockItem =
                dynamic_cast<DockEntryItem*>(source))
        {
            if (dockItem->GetEntryType() !=
                    DockEntryType::FolderMapping)
                return false;
            id = dockItem->GetReference();
        }
        else if (auto* groupEntry =
                     dynamic_cast<
                         FileGroupEntryItem*>(
                         source))
        {
            id = groupEntry->GetChildWidgetId();
        }
        else if (auto* widget =
                     dynamic_cast<Widget*>(source))
        {
            DesktopWidget* data =
                widget->GetWidgetData();
            if (!data ||
                data->type !=
                    DesktopWidgetType::
                        FolderMapping)
                return false;
            id = data->id;
        }
        else
        {
            return false;
        }

        const size_t childIndex =
            FindWidgetIndexById(id);
        if (childIndex >= widgets_.size() ||
            widgets_[childIndex].type !=
                DesktopWidgetType::FolderMapping)
            return false;
        if (std::find(
                movingIds.begin(),
                movingIds.end(), id) ==
            movingIds.end())
            movingIds.push_back(
                std::move(id));
    }
    if (movingIds.empty())
        return false;

    DesktopWidget& target =
        widgets_[groupIndex];
    const std::wstring previousActive =
        target.activeCategoryId;
    insertIndex = std::min(
        insertIndex,
        target.childWidgetIds.size());
    size_t removedBefore = 0;
    for (const auto& id : movingIds)
    {
        const auto existing =
            std::find(
                target.childWidgetIds.begin(),
                target.childWidgetIds.end(),
                id);
        if (existing !=
                target.childWidgetIds.end() &&
            static_cast<size_t>(
                std::distance(
                    target.childWidgetIds.begin(),
                    existing)) < insertIndex)
            ++removedBefore;
    }

    for (auto& group : widgets_)
    {
        if (group.type !=
                DesktopWidgetType::FileGroup)
            continue;
        for (const auto& id : movingIds)
            std::erase(
                group.childWidgetIds, id);
        group.activeCategoryId =
            snowdesktop::
                collection_group_rules::
                    ResolveActiveItem(
                        group.childWidgetIds,
                        group.activeCategoryId);
    }

    const size_t insertAt =
        std::min(
            insertIndex > removedBefore
                ? insertIndex - removedBefore
                : 0,
            target.childWidgetIds.size());
    target.childWidgetIds.insert(
        target.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(
                insertAt),
        movingIds.begin(), movingIds.end());
    target.activeCategoryId =
        snowdesktop::
            collection_group_rules::
                ResolveActiveItem(
                    target.childWidgetIds,
                    previousActive);

    const std::unordered_set<std::wstring>
        movingSet(
            movingIds.begin(), movingIds.end());
    std::erase_if(
        dockEntries_,
        [&](const DockEntry& entry) {
            return entry.type ==
                    DockEntryType::
                        FolderMapping &&
                movingSet.contains(
                    entry.reference);
        });
    for (const auto& id : movingIds)
    {
        const size_t childIndex =
            FindWidgetIndexById(id);
        if (childIndex < widgets_.size())
            widgets_[childIndex].
                selected = false;
    }

    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::ReleaseWidgetFromFileGroup(
    const std::wstring& childId, GridCell preferredCell)
{
    const size_t childIndex = FindWidgetIndexById(childId);
    const size_t groupIndex = FindFileGroupIndexForChild(childId);
    if (childIndex >= widgets_.size() ||
        groupIndex >= widgets_.size())
        return false;
    const DesktopWidgetType childType = widgets_[childIndex].type;
    if (childType != DesktopWidgetType::FileCategories &&
        childType != DesktopWidgetType::FolderMapping)
        return false;

    DesktopWidget& group = widgets_[groupIndex];
    auto membership = std::find(
        group.childWidgetIds.begin(),
        group.childWidgetIds.end(), childId);
    const size_t membershipIndex =
        membership == group.childWidgetIds.end()
            ? group.childWidgetIds.size()
            : static_cast<size_t>(std::distance(
                group.childWidgetIds.begin(), membership));
    const std::wstring previousActive = group.activeCategoryId;
    std::erase(group.childWidgetIds, childId);
    if (group.activeCategoryId == childId)
        group.activeCategoryId =
            group.childWidgetIds.empty()
                ? L""
                : group.childWidgetIds[
                    std::min(membershipIndex,
                        group.childWidgetIds.size() - 1)];

    DesktopWidget& child = widgets_[childIndex];
    child.selected = false;
    const GridPage* page =
        FindGridPage(gridPages_, preferredCell.pageId);
    if (!page)
    {
        preferredCell = group.gridCell;
        page = FindGridPage(gridPages_, preferredCell.pageId);
    }
    auto restoreMembership = [&]() {
        group.childWidgetIds.insert(
            group.childWidgetIds.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(membershipIndex,
                        group.childWidgetIds.size())),
            childId);
        group.activeCategoryId = previousActive;
    };
    if (!page)
    {
        restoreMembership();
        return false;
    }

    GridSpan span = ClampWidgetGridSpan(
        child, child.gridSpan, page->columns, page->rows);
    preferredCell.column = std::clamp(
        preferredCell.column, 0,
        std::max(0, page->columns - span.columns));
    preferredCell.row = std::clamp(
        preferredCell.row, 0,
        std::max(0, page->rows - span.rows));

    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == childIndex || IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(
            usedSlots, widgets_[i].gridCell,
            widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell landing = preferredCell;
    if (!IsGridAreaValid(landing, span) ||
        AreGridSlotsMarked(usedSlots, landing, span))
    {
        const int startSlot =
            SlotFromCell(gridPages_, preferredCell);
        if (!TryFindFreeCell(
                span, usedSlots, landing,
                preferredCell.pageId, startSlot) &&
            !TryFindFreeCell(
                span, usedSlots, landing, L"", 0))
        {
            restoreMembership();
            return false;
        }
    }

    child.gridCell = landing;
    child.gridSpan = span;
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::AddCollectionToGroup(
    size_t collectionIndex, size_t groupIndex, size_t insertIndex)
{
    if (collectionIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        collectionIndex == groupIndex ||
        widgets_[collectionIndex].type != DesktopWidgetType::Collection ||
        widgets_[groupIndex].type != DesktopWidgetType::CollectionGroup)
        return false;

    const std::wstring collectionId = widgets_[collectionIndex].id;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::CollectionGroup) continue;
        std::erase(widget.childWidgetIds, collectionId);
        if (widget.activeCategoryId == collectionId)
            widget.activeCategoryId =
                widget.childWidgetIds.empty()
                    ? L""
                    : widget.childWidgetIds.front();
    }

    DesktopWidget& group = widgets_[groupIndex];
    if (insertIndex == static_cast<size_t>(-1))
        insertIndex = group.childWidgetIds.size();
    insertIndex = std::min(insertIndex, group.childWidgetIds.size());
    group.childWidgetIds.insert(
        group.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(insertIndex),
        collectionId);
    if (group.activeCategoryId.empty())
        group.activeCategoryId = collectionId;

    std::erase_if(dockEntries_, [&](const DockEntry& entry) {
        return entry.type == DockEntryType::Collection &&
            entry.reference == collectionId;
    });
    if (popupWidgetIndex_ == collectionIndex)
        CloseCollectionPopup();
    widgets_[collectionIndex].selected = false;
    group.scrollOffset = std::clamp(
        group.scrollOffset, 0, INT_MAX);
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::ReleaseCollectionFromGroup(
    const std::wstring& collectionId, GridCell preferredCell)
{
    const size_t collectionIndex = FindWidgetIndexById(collectionId);
    const size_t groupIndex =
        FindCollectionGroupIndexForChild(collectionId);
    if (collectionIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        widgets_[collectionIndex].type != DesktopWidgetType::Collection)
        return false;

    DesktopWidget& group = widgets_[groupIndex];
    auto membership = std::find(
        group.childWidgetIds.begin(),
        group.childWidgetIds.end(), collectionId);
    const size_t membershipIndex =
        membership == group.childWidgetIds.end()
            ? group.childWidgetIds.size()
            : static_cast<size_t>(
                std::distance(group.childWidgetIds.begin(), membership));
    const std::wstring previousActiveCategory =
        group.activeCategoryId;
    std::erase(group.childWidgetIds, collectionId);
    if (group.activeCategoryId == collectionId)
        group.activeCategoryId =
            group.childWidgetIds.empty()
                ? L""
                : group.childWidgetIds[
                    std::min(
                        membershipIndex,
                        group.childWidgetIds.size() - 1)];
    DesktopWidget& collection = widgets_[collectionIndex];
    collection.selected = false;

    const GridPage* page =
        FindGridPage(gridPages_, preferredCell.pageId);
    if (!page)
    {
        preferredCell = group.gridCell;
        page = FindGridPage(gridPages_, preferredCell.pageId);
    }
    if (!page)
    {
        group.childWidgetIds.insert(
            group.childWidgetIds.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(membershipIndex,
                        group.childWidgetIds.size())),
            collectionId);
        group.activeCategoryId = previousActiveCategory;
        return false;
    }

    GridSpan span = ClampWidgetGridSpan(collection, collection.gridSpan,
        page->columns, page->rows);
    preferredCell.column = std::clamp(
        preferredCell.column, 0,
        std::max(0, page->columns - span.columns));
    preferredCell.row = std::clamp(
        preferredCell.row, 0,
        std::max(0, page->rows - span.rows));

    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == collectionIndex || IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(usedSlots,
            widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell landing = preferredCell;
    if (!IsGridAreaValid(landing, span) ||
        AreGridSlotsMarked(usedSlots, landing, span))
    {
        const int startSlot =
            SlotFromCell(gridPages_, preferredCell);
        if (!TryFindFreeCell(span, usedSlots, landing,
            preferredCell.pageId, startSlot) &&
            !TryFindFreeCell(span, usedSlots, landing, L"", 0))
        {
            group.childWidgetIds.insert(
                group.childWidgetIds.begin() +
                    static_cast<std::ptrdiff_t>(
                        std::min(membershipIndex,
                            group.childWidgetIds.size())),
                collectionId);
            group.activeCategoryId =
                previousActiveCategory;
            return false;
        }
    }

    collection.gridCell = landing;
    collection.gridSpan = span;
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void DesktopApp::ReleaseCollectionGroupChildren(size_t groupIndex)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type != DesktopWidgetType::CollectionGroup)
        return;

    DesktopWidget& group = widgets_[groupIndex];
    const std::vector<std::wstring> childIds =
        snowdesktop::collection_group_rules::
            TakeAllForRelease(
                group.childWidgetIds,
                group.activeCategoryId);

    std::unordered_set<std::wstring> childSet(
        childIds.begin(), childIds.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == groupIndex ||
            childSet.contains(widgets_[i].id) ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(usedSlots,
            widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    const std::wstring preferredPage = group.gridCell.pageId;
    int startSlot = SlotFromCell(gridPages_, group.gridCell);
    for (const auto& childId : childIds)
    {
        const size_t childIndex = FindWidgetIndexById(childId);
        if (childIndex >= widgets_.size() ||
            widgets_[childIndex].type != DesktopWidgetType::Collection)
            continue;
        DesktopWidget& child = widgets_[childIndex];
        GridCell landing;
        if (!TryFindFreeCell(child.gridSpan, usedSlots, landing,
            preferredPage, startSlot) &&
            !TryFindFreeCell(child.gridSpan, usedSlots, landing, L"", 0) &&
            !FindDockReturnCell(
                usedSlots, preferredPage, startSlot,
                child.gridSpan, landing))
            continue;
        child.gridCell = landing;
        child.selected = false;
        MarkGridArea(usedSlots, landing, child.gridSpan);
        startSlot = FindGridPage(gridPages_, landing.pageId)
            ? SlotFromCell(gridPages_, landing) + 1
            : 0;
    }
}

void DesktopApp::ReleaseFileGroupChildren(size_t groupIndex)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type != DesktopWidgetType::FileGroup)
        return;

    DesktopWidget& group = widgets_[groupIndex];
    const std::vector<std::wstring> childIds =
        snowdesktop::collection_group_rules::
            TakeAllForRelease(
                group.childWidgetIds,
                group.activeCategoryId);

    std::unordered_set<std::wstring> childSet(
        childIds.begin(), childIds.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == groupIndex ||
            childSet.contains(widgets_[i].id) ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(
            usedSlots, widgets_[i].gridCell,
            widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    const std::wstring preferredPage = group.gridCell.pageId;
    int startSlot = SlotFromCell(gridPages_, group.gridCell);
    for (const auto& childId : childIds)
    {
        const size_t childIndex = FindWidgetIndexById(childId);
        if (childIndex >= widgets_.size())
            continue;
        DesktopWidget& child = widgets_[childIndex];
        if (child.type != DesktopWidgetType::FileCategories &&
            child.type != DesktopWidgetType::FolderMapping)
            continue;
        GridCell landing;
        if (!TryFindFreeCell(
                child.gridSpan, usedSlots, landing,
                preferredPage, startSlot) &&
            !TryFindFreeCell(
                child.gridSpan, usedSlots, landing, L"", 0) &&
            !FindDockReturnCell(
                usedSlots, preferredPage, startSlot,
                child.gridSpan, landing))
            continue;
        child.gridCell = landing;
        child.selected = false;
        MarkGridArea(usedSlots, landing, child.gridSpan);
        startSlot = FindGridPage(
                gridPages_, landing.pageId)
            ? SlotFromCell(gridPages_, landing) + 1
            : 0;
    }
}

void DesktopApp::ReleaseDesktopItemsFromWidget(
    size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size())
        return;

    const DesktopWidget& source = widgets_[widgetIndex];
    if (source.type != DesktopWidgetType::Collection &&
        source.type != DesktopWidgetType::FileCategories)
        return;

    std::vector<std::wstring> claimedKeys;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex) continue;
        for (const auto& key : widgets_[i].itemKeys)
        {
            if (!key.empty())
                claimedKeys.push_back(
                    ToUpperInvariant(key));
        }
    }
    for (const auto& entry : dockEntries_)
    {
        if (entry.type ==
                DockEntryType::DesktopItem &&
            !entry.keepOnDesktop &&
            !entry.reference.empty())
        {
            claimedKeys.push_back(
                ToUpperInvariant(entry.reference));
        }
    }

    std::vector<std::wstring> sourceKeys;
    sourceKeys.reserve(source.itemKeys.size());
    for (const auto& key : source.itemKeys)
        sourceKeys.push_back(
            ToUpperInvariant(key));
    const std::vector<std::wstring> releasedKeys =
        snowdesktop::collection_group_rules::
            ClaimUniqueAllowedItems(
                sourceKeys, claimedKeys,
                [](const std::wstring& key) {
                    return !key.empty();
                });
    if (releasedKeys.empty())
        return;

    std::unordered_set<std::wstring> releasedSet(
        releasedKeys.begin(), releasedKeys.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(
            usedSlots, widgets_[i].gridCell,
            widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() ||
            releasedSet.contains(
                ToUpperInvariant(item.layoutKey)) ||
            IsItemInAnyWidget(item) ||
            item.gridCell.pageId ==
                kDockPageId)
            continue;
        MarkGridArea(
            usedSlots, item.gridCell,
            item.gridSpan);
    }

    GridCell preferredCell = source.gridCell;
    if (source.type ==
            DesktopWidgetType::Collection)
    {
        const size_t groupIndex =
            FindCollectionGroupIndexForChild(
                source.id);
        if (groupIndex < widgets_.size())
            preferredCell =
                widgets_[groupIndex].gridCell;
    }
    else
    {
        const size_t groupIndex =
            FindFileGroupIndexForChild(source.id);
        if (groupIndex < widgets_.size())
            preferredCell =
                widgets_[groupIndex].gridCell;
    }

    auto isKnownPage =
        [&](const std::wstring& pageId) {
            return !pageId.empty() &&
                pageId != kDockPageId &&
                (FindGridPage(
                     gridPages_, pageId) !=
                     nullptr ||
                 (savedPageColumns_.contains(pageId) &&
                  savedPageRows_.contains(pageId)));
        };
    if (!isKnownPage(preferredCell.pageId))
    {
        const GridPage* firstPage =
            GetFirstPageGridPage();
        if (!firstPage) return;
        preferredCell = {
            firstPage->id, 0, 0
        };
    }

    auto slotForCell =
        [&](const GridCell& cell) {
            int rows = 1;
            if (const GridPage* page =
                    FindGridPage(
                        gridPages_, cell.pageId))
                rows = std::max(1, page->rows);
            else if (const auto it =
                         savedPageRows_.find(
                             cell.pageId);
                     it != savedPageRows_.end())
                rows = std::max(1, it->second);
            return std::max(0, cell.column) *
                    rows +
                std::max(0, cell.row);
        };

    std::wstring preferredPage =
        preferredCell.pageId;
    int startSlot = slotForCell(preferredCell);
    for (const auto& key : releasedKeys)
    {
        const size_t itemIndex =
            FindItemIndexByKey(key);
        if (itemIndex >= items_.size())
            continue;

        DesktopItem& item = items_[itemIndex];
        item.gridSpan.columns =
            std::max(1, item.gridSpan.columns);
        item.gridSpan.rows =
            std::max(1, item.gridSpan.rows);

        GridCell landing;
        if (!FindDockReturnCell(
                usedSlots, preferredPage,
                startSlot, item.gridSpan,
                landing))
            continue;

        item.gridCell = landing;
        item.slot = slotForCell(landing);
        item.selected = false;
        MarkGridArea(
            usedSlots, landing, item.gridSpan);
        preferredPage = landing.pageId;
        startSlot = item.slot + 1;
    }
}

/**
 * @brief 在右键菜单位置添加桌面文件分类组件。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddFileCategoryWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::FileCategories;
    w.title = _LW("widget.desktop_files");
    w.showTitle = true;
    AddWidgetToGrid(std::move(w), { 2, 2 });
    ShowWidgetAddedHint();
}

/**
 * @brief 在右键菜单位置添加文件夹映射组件（弹出文件夹选择对话框）。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddFolderMappingWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;

    // Pick source folder via modern file-explorer-style dialog
    std::wstring folderPath;
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileOpenDialog* pfd = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&pfd))))
        {
            pfd->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            pfd->SetTitle(_LW("app.interact.select_folder"));
            if (SUCCEEDED(pfd->Show(hwnd_)))
            {
                IShellItem* psi = nullptr;
                if (SUCCEEDED(pfd->GetResult(&psi)))
                {
                    PWSTR pszPath = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
                    {
                        folderPath = pszPath;
                        CoTaskMemFree(pszPath);
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
        CoUninitialize();
    }
    if (folderPath.empty()) return;
    std::wstring title = folderPath;
    if (!title.empty() && title.back() == L'\\') title.pop_back();
    size_t lastSep = title.find_last_of(L"\\/");
    title = (lastSep != std::wstring::npos) ? title.substr(lastSep + 1) : title;

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::FolderMapping;
    w.title = title;
    w.showTitle = true;
    w.sourceFolderPath = folderPath;
    AddWidgetToGrid(std::move(w), { 2, 2 });

    // Enumerate entries
    size_t idx = widgets_.size() - 1;
    EnumerateFolderMappingEntries(widgets_[idx]);
    RebuildContainersAndItems();
    ShowWidgetAddedHint();
}

/**
 * @brief 在右键菜单位置添加 Lua 脚本组件。
 * @param screenPoint 屏幕坐标点。
 * @param scriptFilename 脚本文件名。
 */
void DesktopApp::AddLuaWidgetAt(POINT screenPoint, const std::wstring& packageId)
{
    if (packageId.empty()) return;
    lastContextMenuScreenPoint_ = screenPoint;

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::LuaScript;
    w.title = WidgetEngine::GetWidgetDisplayName(packageId);
    if (w.title.empty())
    {
        w.title = packageId;
    }
    w.packageId = packageId;
    w.bottomBarHover = true;
    if (widgetEngine_)
    {
        widgetEngine_->EnsureWidgetLoaded(w.id, packageId);
        w.showTitle = widgetEngine_->ReadBoolFlag(packageId, "showTitle", false);
        w.bottomBarHover = widgetEngine_->ReadBoolFlag(packageId, "bottomBarHover", true);
    }
    int defaultColumns = 1;
    int defaultRows = 1;
    WidgetEngine::GetWidgetDefaultSpan(packageId, defaultColumns, defaultRows);
    AddWidgetToGrid(std::move(w), { defaultColumns, defaultRows });
    ShowWidgetAddedHint();
}

/**
 * @brief 将组件放置到指定网格位置，并重新安置被挤占的桌面项。
 * @param widgetIndex 组件索引。
 * @param targetCell 目标单元格。
 * @param targetSpan 目标跨度。
 * @param isMove 是否为移动操作（而非缩放）。
 */
