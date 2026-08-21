#include "app.h"
#include "../widget_item_layout.h"

// Keyboard drag hints, Shell verbs and desktop-grid navigation.

void DesktopApp::RefreshDragHintFromKeyboard()
{
    if (!dragSession_.IsActive() &&
        !dragDropController_.IsTransportActive()) return;

    int mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(
            mods,
            dragDropController_.IsExternalDragActive()
                ? DropAction::Copy : DropAction::Move);

    std::wstring hint = GetDockDragOutRemovalHint(dragSession_.CurrentPoint());
    if (hint.empty() && dragSession_.TargetContainer() &&
        dragSession_.TargetRegion() != HitRegion::None)
    {
        hint = dragSession_.TargetContainer()->GetDragHint(dragSession_.TargetSlot(),
            dragSession_.TargetRegion(), dragSession_.Items(), dragSession_.Source(), mods);
    }

    if (dragDropController_.IsTransportActive())
    {
        POINT screenPoint = dragSession_.CurrentPoint();
        ClientToScreen(hwnd_, &screenPoint);
        ShowDragHintWindowScreen(screenPoint, hint);
        OnPaint();
        InvalidateFloatingDockWindow(true);
    }
    else
    {
        ShowDragHintWindow(dragSession_.CurrentPoint(), hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

/**
 * @brief 对选中的桌面项调用 Shell 动词（如 "copy"）
 * @param verb Shell 动词字符串
 */
void DesktopApp::InvokeSelectedShellVerb(const char* verb)
{
    std::vector<PCUITEMID_CHILD> pidls;
    for (const auto& item : items_)
    {
        if (item.selected && item.childPidl.value)
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.value));
    }
    if (pidls.empty()) return;

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IContextMenu, nullptr,
        reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(hr) || !contextMenu) return;

    CMINVOKECOMMANDINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = ShellDialogOwnerHwnd();
    info.lpVerb = verb;
    info.nShow = SW_SHOWNORMAL;
    if (SafeInvokeCommand(contextMenu.Get(), &info))
        ReloadItems();
}

/**
 * @brief 在桌面网格上按 2D 空间导航选中项
 * @param arrowKey 方向键虚拟键码
 *
 * 收集当前可见页面上所有可导航目标（桌面图标 + 组件），
 * 按 gridCell 的 column/row 进行上下左右空间移动。
 * 没有任何选中时，从首列首行开始纵向搜索第一个目标。
 */
void DesktopApp::NavigateDesktopGrid(WPARAM arrowKey)
{
    if (items_.empty() && widgets_.empty()) return;

    // 构建当前可见页面 ID 集合
    std::unordered_set<std::wstring> visiblePageIds;
    for (const auto& gp : gridPages_)
        if (!gp.id.empty())
            visiblePageIds.insert(gp.id);
    const bool hasVisiblePages = !visiblePageIds.empty();

    struct Target { bool isWidget; size_t index; int column; int row; int colSpan; int rowSpan; std::wstring pageId; };
    std::vector<Target> targets;

    // 收集未收纳的桌面项（有名称、有边界、在可见页面上）
    for (size_t i = 0; i < items_.size(); ++i)
    {
        const auto& item = items_[i];
        if (item.name.empty()) continue;
        if (IsRectEmptyRect(item.bounds)) continue;
        if (collectedKeysCache_.contains(ToUpperInvariant(item.layoutKey))) continue;
        if (hasVisiblePages && !visiblePageIds.contains(item.gridCell.pageId)) continue;
        targets.push_back({ false, i,
            item.gridCell.column, item.gridCell.row,
            item.gridSpan.columns, item.gridSpan.rows,
            item.gridCell.pageId });
    }

    // 收集当前可见页面上的组件
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& w = widgets_[i];
        if (IsGroupedWidget(w)) continue;
        if (hasVisiblePages && !visiblePageIds.contains(w.gridCell.pageId)) continue;
        targets.push_back({ true, i,
            w.gridCell.column, w.gridCell.row,
            w.gridSpan.columns, w.gridSpan.rows,
            w.gridCell.pageId });
    }

    if (targets.empty()) return;

    // 按列、行排序（纵向搜索：逐列从上到下）
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) {
        if (a.column != b.column) return a.column < b.column;
        return a.row < b.row;
    });

    // 查找当前选中目标
    int currentIndex = -1;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        const auto& t = targets[i];
        if (t.isWidget ? widgets_[t.index].selected : items_[t.index].selected)
        {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    if (currentIndex < 0)
    {
        // 无选中：选中鼠标所在页的第一个目标，不执行方向移动
        ClearSelection();
        // 确定鼠标所在的可见页面
        std::wstring mousePageId;
        {
            POINT screenPt{};
            GetCursorPos(&screenPt);
            for (const auto& gp : gridPages_)
            {
                if (!gp.id.empty() && PtInRect(&gp.workArea, screenPt))
                {
                    mousePageId = gp.id;
                    break;
                }
            }
        }
        // 在目标列表中找该页的第一个
        size_t firstIdx = 0;
        if (!mousePageId.empty())
        {
            bool foundOnPage = false;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                if (targets[i].pageId == mousePageId)
                {
                    firstIdx = i;
                    foundOnPage = true;
                    break;
                }
            }
            if (!foundOnPage)
                firstIdx = 0;   // 该页无目标，回退到首个
        }
        const auto& first = targets[firstIdx];
        if (first.isWidget)
            widgets_[first.index].selected = true;
        else
            items_[first.index].selected = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const Target& current = targets[static_cast<size_t>(currentIndex)];
    int nextIndex = -1;

    // 辅助：两个区间 [a, a+lenA) 与 [b, b+lenB) 是否相交
    auto spansOverlap = [](int a, int lenA, int b, int lenB) {
        return a < b + lenB && b < a + lenA;
    };

    switch (arrowKey)
    {
    case VK_UP:
    {
        int bestRow = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.column, current.colSpan, t.column, t.colSpan) &&
                t.row < current.row && t.row > bestRow)
            {
                bestRow = t.row;
                bestIdx = static_cast<int>(i);
            }
        }
        nextIndex = bestIdx;
        break;
    }
    case VK_DOWN:
    {
        int bestRow = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.column, current.colSpan, t.column, t.colSpan) &&
                t.row > current.row)
            {
                if (bestRow < 0 || t.row < bestRow)
                {
                    bestRow = t.row;
                    bestIdx = static_cast<int>(i);
                }
            }
        }
        nextIndex = bestIdx;
        break;
    }
    case VK_LEFT:
    {
        int bestCol = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.row, current.rowSpan, t.row, t.rowSpan) &&
                t.column < current.column && t.column > bestCol)
            {
                bestCol = t.column;
                bestIdx = static_cast<int>(i);
            }
        }
        if (bestIdx < 0)
        {
            int bestRow = -1;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                const auto& t = targets[i];
                if (t.pageId == current.pageId && t.row < current.row && t.row > bestRow)
                    bestRow = t.row;
            }
            if (bestRow >= 0)
            {
                bestCol = -1;
                for (size_t i = 0; i < targets.size(); ++i)
                {
                    const auto& t = targets[i];
                    if (t.pageId == current.pageId && t.row == bestRow &&
                        (bestCol < 0 || t.column > bestCol))
                    {
                        bestCol = t.column;
                        bestIdx = static_cast<int>(i);
                    }
                }
            }
        }
        nextIndex = bestIdx;
        break;
    }
    case VK_RIGHT:
    {
        int bestCol = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.row, current.rowSpan, t.row, t.rowSpan) &&
                t.column > current.column)
            {
                if (bestCol < 0 || t.column < bestCol)
                {
                    bestCol = t.column;
                    bestIdx = static_cast<int>(i);
                }
            }
        }
        if (bestIdx < 0)
        {
            int bestRow = -1;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                const auto& t = targets[i];
                if (t.pageId == current.pageId &&
                    t.row > current.row && (bestRow < 0 || t.row < bestRow))
                    bestRow = t.row;
            }
            if (bestRow >= 0)
            {
                bestCol = -1;
                for (size_t i = 0; i < targets.size(); ++i)
                {
                    const auto& t = targets[i];
                    if (t.pageId == current.pageId && t.row == bestRow &&
                        (bestCol < 0 || t.column < bestCol))
                    {
                        bestCol = t.column;
                        bestIdx = static_cast<int>(i);
                    }
                }
            }
        }
        nextIndex = bestIdx;
        break;
    }
    default:
        return;
    }

    if (nextIndex < 0) return;   // 该方向无有效目标

    ClearSelection();
    const auto& next = targets[static_cast<size_t>(nextIndex)];
    if (next.isWidget)
        widgets_[next.index].selected = true;
    else
        items_[next.index].selected = true;

    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 滚动组件以确保指定索引的成员项可见
 * @param widgetIndex 组件索引
 * @param memberIndex 成员索引
 *
 * 计算目标成员在组件内容区域中的近似纵向位置，
 * 如果超出当前滚动视口则调整 scrollOffset。
 */

void DesktopApp::ScrollWidgetToMember(size_t widgetIndex, int memberIndex)
{
    if (widgetIndex >= widgets_.size() || memberIndex < 0) return;
    auto& widget = widgets_[widgetIndex];

    WidgetContainer* wc = nullptr;
    for (auto& c : containers_)
    {
        auto* w = dynamic_cast<WidgetContainer*>(c.get());
        if (w && w->GetWidgetData() == &widget) { wc = w; break; }
    }
    if (!wc) return;

    int maxScroll = wc->GetMaxScrollOffset();
    if (maxScroll <= 0) return;

    auto applyScroll = [&](int scroll) {
        scroll = std::clamp(scroll, 0, maxScroll);
        if (scroll == widget.scrollOffset) return;
        widget.scrollOffset = scroll;
        if (auto* group = dynamic_cast<FileGroup*>(wc))
            group->InvalidateHostedView();
        else
            wc->InvalidateSlots();
    };

    // Drawing, hit testing and keyboard reveal all ask the component for the
    // same local-track rectangle. This also accounts for search/date headers
    // and for FileGroup-hosted source geometry.
    const RECT viewport = wc->GetContentViewportRect();
    const RECT target = wc->GetMemberLayoutRect(
        static_cast<size_t>(memberIndex));
    if (!IsRectEmptyRect(target) && !IsRectEmptyRect(viewport))
    {
        applyScroll(snowdesktop::widget_item_layout::
            ScrollOffsetToReveal(viewport, target,
                widget.scrollOffset, maxScroll));
        return;
    }

    // Fallback for non-item surfaces that cannot provide member geometry.
    int columns = std::max(1, widget.gridSpan.columns);
    bool listMode = ((widget.type == DesktopWidgetType::CollectionGroup ||
                      widget.type == DesktopWidgetType::FileGroup) &&
                     (widget.listMode || widget.dateHeaders)) ||
                    (widget.type == DesktopWidgetType::FileCategories && (widget.listMode || widget.dateHeaders)) ||
                    (widget.type == DesktopWidgetType::FolderMapping && (widget.listMode || widget.dateHeaders)) ||
                    (widget.type == DesktopWidgetType::Collection && widget.listMode);

    size_t memberCount = (widget.type == DesktopWidgetType::FolderMapping)
        ? widget.folderEntries.size()
        : widget.itemKeys.size();
    if (widget.type == DesktopWidgetType::FileCategories)
    {
        auto* fc = dynamic_cast<FileCategories*>(wc);
        if (fc) memberCount = fc->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        auto* mapping = dynamic_cast<FolderMapping*>(wc);
        if (mapping) memberCount = mapping->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        auto* group = dynamic_cast<FileGroup*>(wc);
        if (group) memberCount = group->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        auto* group = dynamic_cast<CollectionGroup*>(wc);
        if (group) memberCount = group->GetSlotCount();
    }
    if (memberCount <= 1) return;

    int totalRows = listMode
        ? static_cast<int>(memberCount)
        : (static_cast<int>(memberCount) + columns - 1) / columns;
    int row = listMode ? memberIndex : memberIndex / columns;

    int scroll = (totalRows <= 1) ? 0
        : static_cast<int>((static_cast<int64_t>(row) * maxScroll) / (totalRows - 1));

    applyScroll(scroll);
}

/**
 * @brief 在组件内部导航成员项
 * @param arrowKey 方向键虚拟键码
 *
 * 根据组件类型（Collection、FileCategories、FolderMapping）的列数布局，
 * 在组件成员项之间进行上下左右 2D 导航。list 模式的 FileCategories 使用线性上下移动。
 */
void DesktopApp::NavigateWidgetMembers(WPARAM arrowKey)
{
    keyboardNavVisualFocus_ = true;
    if (keyboardNavWidgetIndex_ >= widgets_.size()) return;
    const size_t navigationWidgetIndex =
        keyboardNavWidgetIndex_;
    auto& widget = widgets_[keyboardNavWidgetIndex_];

    if (widget.type == DesktopWidgetType::FileGroup)
    {
        FileGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<FileGroup*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                group = candidate;
                break;
            }
        }
        if (!group) return;
        const auto& visibleSourceIds =
            group->GetVisibleSourceIds();
        const std::vector<std::wstring> sourceIds(
            visibleSourceIds.begin(),
            visibleSourceIds.end());
        if (sourceIds.empty()) return;
        const size_t groupIndex =
            keyboardNavWidgetIndex_;
        auto activeSourceIt = std::find(
            sourceIds.begin(), sourceIds.end(),
            group->GetActiveSourceId());
        const size_t activeSource =
            activeSourceIt == sourceIds.end()
                ? 0
                : static_cast<size_t>(std::distance(
                    sourceIds.begin(), activeSourceIt));

        const bool searchAvailable =
            !IsRectEmptyRect(group->GetSearchBoxRect());
        auto focusSearch = [&]() {
            if (!searchAvailable) return;
            group->SetSearchFocused(false);
            ClearSelection();
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ = -1;
            keyboardNavSearchBox_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        auto focusSource = [&](size_t index,
            bool activate) {
            if (index >= sourceIds.size()) return;
            group->SetSearchFocused(false);
            ClearSelection();
            if (activate)
            {
                widget.activeCategoryId =
                    sourceIds[index];
                widget.scrollOffset = 0;
                group->InvalidateHostedView();
            }
            const size_t childIndex =
                FindWidgetIndexById(sourceIds[index]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = true;
            keyboardNavFileGroupCategoryTabs_ = false;
            group->EnsureSourceTabVisible(index);
            if (activate) SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        const bool showCategoryRow =
            widget.showFileCategories &&
            !(widget.showSearchBox &&
              !group->GetSearchText().empty());
        const std::vector<std::wstring> categoryIds =
            showCategoryRow
                ? group->GetHostedVisibleCategoryIds()
                : std::vector<std::wstring>{};
        auto* activeContainer =
            group->GetActiveSourceContainer();
        DesktopWidget* activeData = activeContainer
            ? activeContainer->GetWidgetData() : nullptr;
        auto activeCategoryIt = activeData
            ? std::find(categoryIds.begin(),
                categoryIds.end(),
                activeData->activeCategoryId)
            : categoryIds.end();
        const size_t activeCategory =
            activeCategoryIt == categoryIds.end()
                ? 0
                : static_cast<size_t>(std::distance(
                    categoryIds.begin(), activeCategoryIt));

        auto focusCategory = [&](size_t index,
            bool activate) {
            if (!activeData ||
                index >= categoryIds.size())
                return;
            group->SetSearchFocused(false);
            ClearSelection();
            if (activate)
            {
                activeData->activeCategoryId =
                    categoryIds[index];
                widget.scrollOffset = 0;
                group->InvalidateHostedView();
            }
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = true;
            group->EnsureCategoryTabVisible(index);
            if (activate) SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        const bool groupSearching =
            group->IsGroupSearchActive();
        const auto itemKeys = groupSearching
            ? std::vector<std::wstring>{}
            : group->GetHostedVisibleItemKeys();
        const auto folderIndices = groupSearching
            ? std::vector<size_t>{}
            : group->GetHostedVisibleFolderIndices();
        const size_t itemCount = groupSearching
            ? group->GetSlotCount()
            : (!itemKeys.empty()
                ? itemKeys.size()
                : folderIndices.size());
        auto selectItem = [&](size_t index) {
            if (index >= itemCount) return;
            group->SetSearchFocused(false);
            ClearSelection();
            if (groupSearching)
            {
                Item* item =
                    group->GetMemberItem(index);
                if (!item) return;
                item->SetSelected(true);
            }
            else if (!itemKeys.empty())
            {
                const size_t itemIndex =
                    FindItemIndexByKey(itemKeys[index]);
                if (itemIndex >= items_.size()) return;
                items_[itemIndex].selected = true;
            }
            else if (activeData)
            {
                const size_t entryIndex =
                    folderIndices[index];
                if (entryIndex >=
                    activeData->folderEntries.size())
                    return;
                activeData->folderEntries[
                    entryIndex].selected = true;
            }
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            ScrollWidgetToMember(groupIndex,
                static_cast<int>(index));
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        if (keyboardNavSearchBox_)
        {
            if (arrowKey == VK_DOWN)
            {
                if (!groupSearching)
                    focusSource(activeSource, false);
                else if (itemCount > 0)
                    selectItem(0);
            }
            return;
        }

        if (keyboardNavCollectionGroupTabs_)
        {
            size_t current =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        sourceIds.size() - 1)
                    : activeSource;
            if (arrowKey == VK_LEFT && current > 0)
                focusSource(current - 1, true);
            else if (arrowKey == VK_RIGHT &&
                     current + 1 < sourceIds.size())
                focusSource(current + 1, true);
            else if (arrowKey == VK_DOWN)
            {
                if (!categoryIds.empty())
                    focusCategory(activeCategory, false);
                else if (itemCount > 0)
                    selectItem(0);
            }
            else if (arrowKey == VK_UP)
                focusSearch();
            return;
        }
        if (keyboardNavFileGroupCategoryTabs_)
        {
            if (categoryIds.empty())
            {
                focusSource(activeSource, false);
                return;
            }
            size_t current =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        categoryIds.size() - 1)
                    : activeCategory;
            if (arrowKey == VK_LEFT && current > 0)
                focusCategory(current - 1, true);
            else if (arrowKey == VK_RIGHT &&
                     current + 1 < categoryIds.size())
                focusCategory(current + 1, true);
            else if (arrowKey == VK_UP)
                focusSource(activeSource, false);
            else if (arrowKey == VK_DOWN &&
                     itemCount > 0)
                selectItem(0);
            return;
        }

        if (itemCount == 0)
        {
            if (groupSearching)
                return;
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else
                focusSource(activeSource, false);
            return;
        }
        size_t current =
            keyboardNavMemberIndex_ >= 0
                ? std::min(
                    static_cast<size_t>(
                        keyboardNavMemberIndex_),
                    itemCount - 1)
                : 0;
        const int columns =
            (widget.listMode || widget.dateHeaders)
                ? 1
                : std::max(1, widget.gridSpan.columns);
        if (arrowKey == VK_UP &&
            current < static_cast<size_t>(columns))
        {
            if (groupSearching)
            {
                focusSearch();
                return;
            }
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else
                focusSource(activeSource, false);
            return;
        }
        int next = static_cast<int>(current);
        if (arrowKey == VK_UP) next -= columns;
        else if (arrowKey == VK_DOWN) next += columns;
        else if (arrowKey == VK_LEFT) --next;
        else if (arrowKey == VK_RIGHT) ++next;
        else return;
        if (next >= 0 &&
            static_cast<size_t>(next) < itemCount)
            selectItem(static_cast<size_t>(next));
        return;
    }

    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        CollectionGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<CollectionGroup*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                group = candidate;
                break;
            }
        }
        if (!group) return;

        const auto& visibleChildIds =
            group->GetVisibleCollectionIds();
        const std::vector<std::wstring> childIds(
            visibleChildIds.begin(),
            visibleChildIds.end());
        const size_t groupWidgetIndex =
            keyboardNavWidgetIndex_;
        auto activeIt = std::find(
            childIds.begin(), childIds.end(),
            group->GetActiveCollectionId());
        size_t activeTab = activeIt == childIds.end()
            ? 0
            : static_cast<size_t>(
                std::distance(childIds.begin(), activeIt));

        const bool searchAvailable =
            !IsRectEmptyRect(group->GetSearchBoxRect());
        auto focusSearch = [&]() {
            if (!searchAvailable) return;
            group->SetSearchFocused(false);
            ClearSelection();
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupWidgetIndex;
            keyboardNavMemberIndex_ = -1;
            keyboardNavSearchBox_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        auto focusTab = [&](size_t tabIndex,
            bool activate) {
            if (tabIndex >= childIds.size()) return;
            group->SetSearchFocused(false);
            ClearSelection();
            if (activate)
            {
                widget.activeCategoryId =
                    childIds[tabIndex];
                widget.scrollOffset = 0;
                group->InvalidateFilterCache();
            }
            const size_t childIndex =
                FindWidgetIndexById(childIds[tabIndex]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ =
                groupWidgetIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(tabIndex);
            keyboardNavCollectionGroupTabs_ = true;
            group->EnsureTabVisible(tabIndex);
            if (activate)
                SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        if (keyboardNavSearchBox_)
        {
            if (arrowKey == VK_DOWN &&
                !childIds.empty())
                focusTab(activeTab, false);
            return;
        }

        if (keyboardNavCollectionGroupTabs_)
        {
            if (childIds.empty()) return;
            size_t currentTab =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        childIds.size() - 1)
                    : activeTab;
            if (arrowKey == VK_LEFT)
            {
                if (currentTab == 0) return;
                focusTab(currentTab - 1, true);
            }
            else if (arrowKey == VK_RIGHT)
            {
                if (currentTab + 1 >=
                    childIds.size())
                    return;
                focusTab(currentTab + 1, true);
            }
            else if (arrowKey == VK_DOWN)
            {
                const auto& keys =
                    group->GetVisibleItemKeys();
                if (keys.empty()) return;
                const size_t itemIndex =
                    FindItemIndexByKey(keys.front());
                if (itemIndex >= items_.size()) return;
                ClearSelection();
                items_[itemIndex].selected = true;
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ =
                    groupWidgetIndex;
                keyboardNavMemberIndex_ = 0;
                keyboardNavCollectionGroupTabs_ =
                    false;
                ScrollWidgetToMember(
                    keyboardNavWidgetIndex_, 0);
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
            }
            else if (arrowKey == VK_UP)
                focusSearch();
            return;
        }

        if (arrowKey == VK_UP && !childIds.empty())
        {
            const auto& keys =
                group->GetVisibleItemKeys();
            int current = keyboardNavMemberIndex_;
            for (size_t i = 0; i < keys.size(); ++i)
            {
                const size_t itemIndex =
                    FindItemIndexByKey(keys[i]);
                if (itemIndex < items_.size() &&
                    items_[itemIndex].selected)
                {
                    current = static_cast<int>(i);
                    break;
                }
            }
            const int columns = widget.listMode
                ? 1
                : std::max(
                    1, widget.gridSpan.columns);
            if (current >= 0 && current < columns)
            {
                focusTab(activeTab, false);
                return;
            }
        }
    }

    size_t memberCount = 0;
    int columns = 1;
    bool isListMode = false;

    switch (widget.type)
    {
    case DesktopWidgetType::CollectionGroup:
        memberCount = 0;
        columns = std::max(1, widget.gridSpan.columns);
        if (widget.listMode)
        {
            columns = 1;
            isListMode = true;
        }
        break;
    case DesktopWidgetType::Collection:
    case DesktopWidgetType::FileCategories:
        memberCount = widget.itemKeys.size();
        columns = std::max(1, widget.gridSpan.columns);
        if (widget.listMode || widget.dateHeaders)
        {
            columns = 1;
            isListMode = true;
        }
        break;
    case DesktopWidgetType::FolderMapping:
        memberCount = widget.folderEntries.size();
        columns = std::max(1, widget.gridSpan.columns);
        if (widget.listMode || widget.dateHeaders)
        {
            columns = 1;
            isListMode = true;
        }
        break;
    default:
        return;   // LuaScript、Guide 无内部导航
    }

    // Collection 弹窗打开时，按弹窗实际列数进行 2D 导航
    if (widget.type == DesktopWidgetType::Collection &&
        popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
        popupWidgetIndex_ < widgets_.size())
    {
        int popupCols = GetCollectionPopupColumnCount(popupRect_);
        if (popupCols > 0 && !isListMode)
            columns = popupCols;
    }

    // FileCategories：获取当前可见项目键列表（受搜索/分类标签页过滤）
    std::vector<std::wstring> visibleKeys;
    std::vector<size_t> visibleFolderIndices;
    std::vector<std::wstring> categoryIds;
    ScrollingItemWidget* categorizedWidget = nullptr;
    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        for (auto& c : containers_)
        {
            auto* group =
                dynamic_cast<CollectionGroup*>(c.get());
            if (group && group->GetWidgetData() == &widget)
            {
                const auto& keys =
                    group->GetVisibleItemKeys();
                visibleKeys.assign(keys.begin(), keys.end());
                break;
            }
        }
        memberCount = visibleKeys.size();
        if (memberCount == 0) return;
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widget)
            {
                auto* fc = dynamic_cast<FileCategories*>(wc);
                if (fc)
                {
                    categorizedWidget = fc;
                    const auto& rk = fc->GetSearchResultKeys();
                    visibleKeys.assign(rk.begin(), rk.end());
                    if (widget.showFileCategories &&
                        (!widget.showSearchBox ||
                            !fc->IsSearchActive()))
                    {
                        const auto& ids =
                            fc->CachedVisibleCategoryIds();
                        categoryIds.assign(
                            ids.begin(), ids.end());
                    }
                }
                break;
            }
        }
        memberCount = visibleKeys.size();
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        for (auto& c : containers_)
        {
            auto* mapping = dynamic_cast<FolderMapping*>(c.get());
            if (mapping && mapping->GetWidgetData() == &widget)
            {
                categorizedWidget = mapping;
                const auto& indices = mapping->GetVisibleEntryIndices();
                visibleFolderIndices.assign(indices.begin(), indices.end());
                if (widget.showFileCategories)
                {
                    const auto& ids =
                        mapping->GetVisibleCategoryIds();
                    categoryIds.assign(
                        ids.begin(), ids.end());
                }
                break;
            }
        }
        memberCount = visibleFolderIndices.size();
    }
    else if (memberCount == 0)
    {
        return;
    }

    const bool searchAvailable =
        categorizedWidget &&
        !IsRectEmptyRect(
            categorizedWidget->GetSearchBoxRect());
    auto focusSearch = [&]() {
        if (!searchAvailable) return;
        categorizedWidget->SetSearchFocused(false);
        ClearSelection();
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = navigationWidgetIndex;
        keyboardNavMemberIndex_ = -1;
        keyboardNavSearchBox_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    auto activeCategoryIt = std::find(
        categoryIds.begin(), categoryIds.end(),
        widget.activeCategoryId);
    const size_t activeCategory =
        activeCategoryIt == categoryIds.end()
            ? 0
            : static_cast<size_t>(std::distance(
                categoryIds.begin(), activeCategoryIt));
    auto focusCategory = [&](size_t index,
        bool activate) {
        if (!categorizedWidget ||
            index >= categoryIds.size())
            return;
        categorizedWidget->SetSearchFocused(false);
        ClearSelection();
        if (activate)
        {
            widget.activeCategoryId = categoryIds[index];
            widget.scrollOffset = 0;
            if (auto* mapping =
                    dynamic_cast<FolderMapping*>(
                        categorizedWidget))
                mapping->InvalidateFilterCache();
            else
                categorizedWidget->InvalidateSlots();
        }
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = navigationWidgetIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(index);
        keyboardNavFileGroupCategoryTabs_ = true;
        categorizedWidget->EnsureCategoryTabVisible(index);
        if (activate) SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    auto selectVisibleItem = [&](size_t index) {
        if (index >= memberCount) return;
        size_t selectedItemIndex =
            static_cast<size_t>(-1);
        size_t selectedEntryIndex =
            static_cast<size_t>(-1);
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            selectedEntryIndex =
                visibleFolderIndices[index];
            if (selectedEntryIndex >=
                widget.folderEntries.size())
                return;
        }
        else
        {
            selectedItemIndex =
                FindItemIndexByKey(visibleKeys[index]);
            if (selectedItemIndex >= items_.size())
                return;
        }
        if (categorizedWidget)
            categorizedWidget->SetSearchFocused(false);
        ClearSelection();
        if (selectedEntryIndex <
            widget.folderEntries.size())
            widget.folderEntries[
                selectedEntryIndex].selected = true;
        else
            items_[selectedItemIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = navigationWidgetIndex;
        keyboardNavMemberIndex_ =
            widget.type == DesktopWidgetType::FolderMapping
                ? static_cast<int>(selectedEntryIndex)
                : static_cast<int>(index);
        ScrollWidgetToMember(
            keyboardNavWidgetIndex_,
            static_cast<int>(index));
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    if (keyboardNavSearchBox_)
    {
        if (arrowKey == VK_DOWN)
        {
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else if (memberCount > 0)
                selectVisibleItem(0);
        }
        return;
    }
    if (keyboardNavFileGroupCategoryTabs_)
    {
        if (categoryIds.empty())
        {
            if (searchAvailable)
                focusSearch();
            return;
        }
        size_t current = keyboardNavMemberIndex_ >= 0
            ? std::min(
                static_cast<size_t>(keyboardNavMemberIndex_),
                categoryIds.size() - 1)
            : activeCategory;
        if (arrowKey == VK_LEFT && current > 0)
            focusCategory(current - 1, true);
        else if (arrowKey == VK_RIGHT &&
                 current + 1 < categoryIds.size())
            focusCategory(current + 1, true);
        else if (arrowKey == VK_UP)
            focusSearch();
        else if (arrowKey == VK_DOWN &&
                 memberCount > 0)
            selectVisibleItem(0);
        return;
    }

    if (memberCount == 0)
    {
        if (!categoryIds.empty())
            focusCategory(activeCategory, false);
        else
            focusSearch();
        return;
    }

    int currentIdx = keyboardNavMemberIndex_;
    if (currentIdx < 0) currentIdx = 0;

    // FileCategories：基于当前可见选中项确定导航起点
    if (!visibleKeys.empty())
    {
        int foundIdx = -1;
        for (size_t i = 0; i < visibleKeys.size(); ++i)
        {
            size_t itemIdx = FindItemIndexByKey(visibleKeys[i]);
            if (itemIdx != static_cast<size_t>(-1) && items_[itemIdx].selected)
            {
                foundIdx = static_cast<int>(i);
                break;
            }
        }
        if (foundIdx >= 0)
            currentIdx = foundIdx;
        else
            currentIdx = 0;
    }
    else if (!visibleFolderIndices.empty())
    {
        int foundIdx = -1;
        for (size_t i = 0; i < visibleFolderIndices.size(); ++i)
        {
            size_t entryIndex = visibleFolderIndices[i];
            if (entryIndex < widget.folderEntries.size() &&
                widget.folderEntries[entryIndex].selected)
            {
                foundIdx = static_cast<int>(i);
                break;
            }
        }
        currentIdx = foundIdx >= 0 ? foundIdx : 0;
    }

    int currentCol = currentIdx % columns;
    int currentRow = currentIdx / columns;
    int totalRows = static_cast<int>((memberCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));

    int nextRow = currentRow;
    int nextCol = currentCol;

    if (arrowKey == VK_UP && currentRow == 0 &&
        (!categoryIds.empty() || searchAvailable))
    {
        if (!categoryIds.empty())
            focusCategory(activeCategory, false);
        else
            focusSearch();
        return;
    }

    switch (arrowKey)
    {
    case VK_UP:
        nextRow = currentRow - 1;
        if (nextRow < 0) return;
        break;
    case VK_DOWN:
        nextRow = currentRow + 1;
        if (nextRow >= totalRows) return;
        break;
    case VK_LEFT:
        if (isListMode)
        {
            if (currentIdx <= 0) return;
        }
        else
        {
            nextCol = currentCol - 1;
            if (nextCol < 0)
            {
                // 换行：跳到上一行最后一列
                nextRow = currentRow - 1;
                if (nextRow < 0) return;
                nextCol = columns - 1;
            }
        }
        break;
    case VK_RIGHT:
        if (isListMode)
        {
            if (static_cast<size_t>(currentIdx) + 1 >= memberCount) return;
        }
        else
        {
            nextCol = currentCol + 1;
            if (nextCol >= columns)
            {
                // 换行：跳到下一行第一列
                nextRow = currentRow + 1;
                if (nextRow >= totalRows) return;
                nextCol = 0;
            }
        }
        break;
    default:
        return;
    }

    int nextIdx;
    if (isListMode)
    {
        if (arrowKey == VK_UP || arrowKey == VK_LEFT)
            nextIdx = currentIdx - 1;
        else
            nextIdx = currentIdx + 1;
        if (nextIdx < 0 || static_cast<size_t>(nextIdx) >= memberCount) return;
    }
    else
    {
        nextIdx = nextRow * columns + nextCol;
        if (nextIdx < 0 || static_cast<size_t>(nextIdx) >= static_cast<int>(memberCount)) return;
    }

    if (nextIdx == currentIdx) return;

    // 取消旧成员选中
    if (currentIdx >= 0)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            if (static_cast<size_t>(currentIdx) <
                visibleFolderIndices.size())
            {
                size_t entryIndex =
                    visibleFolderIndices[static_cast<size_t>(currentIdx)];
                if (entryIndex < widget.folderEntries.size())
                    widget.folderEntries[entryIndex].selected = false;
            }
        }
        else if (!visibleKeys.empty())
        {
            // 清除所有可见项选中状态，防止多选残留
            for (const auto& key : visibleKeys)
            {
                size_t itemIdx = FindItemIndexByKey(key);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
        else
        {
            const auto& keys = widget.itemKeys;
            if (static_cast<size_t>(currentIdx) < keys.size())
            {
                size_t itemIdx = FindItemIndexByKey(keys[static_cast<size_t>(currentIdx)]);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
    }

    // 选中新成员
    if (widget.type == DesktopWidgetType::FolderMapping)
    {
        size_t entryIndex =
            visibleFolderIndices[static_cast<size_t>(nextIdx)];
        if (entryIndex >= widget.folderEntries.size()) return;
        widget.folderEntries[entryIndex].selected = true;
    }
    else if (!visibleKeys.empty())
    {
        size_t itemIdx = FindItemIndexByKey(visibleKeys[static_cast<size_t>(nextIdx)]);
        if (itemIdx != static_cast<size_t>(-1))
            items_[itemIdx].selected = true;
    }
    else
    {
        const auto& keys = widget.itemKeys;
        size_t itemIdx = FindItemIndexByKey(keys[static_cast<size_t>(nextIdx)]);
        if (itemIdx != static_cast<size_t>(-1))
            items_[itemIdx].selected = true;
    }

    keyboardNavMemberIndex_ =
        widget.type == DesktopWidgetType::FolderMapping
            ? static_cast<int>(
                visibleFolderIndices[static_cast<size_t>(nextIdx)])
            : nextIdx;

    // 确保选中的成员项可见
    ScrollWidgetToMember(keyboardNavWidgetIndex_, nextIdx);

    // Collection 大文件夹模式：超界自动弹窗 / 退回内联区域自动关闭
    if (widget.type == DesktopWidgetType::Collection && !widget.scrollContainerMode)
    {
        int cols = std::max(1, widget.gridSpan.columns);
        int rows = std::max(1, widget.gridSpan.rows);
        size_t inlineCap = (cols <= 1 && rows <= 1) ? 4
                           : static_cast<size_t>(cols * rows - 1);
        if (static_cast<size_t>(nextIdx) >= inlineCap)
        {
            if (popupWidgetIndex_ != keyboardNavWidgetIndex_)
            {
                POINT popupAnchor{
                    widget.bounds.right - 1,
                    widget.bounds.bottom - 1,
                };
                for (const auto& container : containers_)
                {
                    auto* collection =
                        dynamic_cast<Collection*>(container.get());
                    if (!collection ||
                        collection->GetWidgetData() != &widget)
                        continue;
                    const RECT allButton =
                        collection->GetAllButtonRect();
                    if (!IsRectEmptyRect(allButton))
                    {
                        popupAnchor = {
                            allButton.left +
                                (allButton.right - allButton.left) / 2,
                            allButton.top +
                                (allButton.bottom - allButton.top) / 2,
                        };
                    }
                    break;
                }
                OpenCollectionPopupAt(keyboardNavWidgetIndex_,
                    popupAnchor);
            }
        }
        else if (popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
            (cols > 1 || rows > 1))   // 紧凑模式保持弹窗常开
        {
            // 退回内联区域时保留刚导航选中的成员项，但仍播放关闭动画。
            CloseCollectionPopup(false);
        }
    }

    // 弹窗滚动跟随（若弹窗仍打开）
    if (popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
        popupWidgetIndex_ < widgets_.size())
    {
        RECT rPopup = popupRect_;
        int popupCols = GetCollectionPopupColumnCount(rPopup);
        int popupRow = nextIdx / std::max(1, popupCols);
        int cellH = kMinCellHeight;
        for (const auto& page : gridPages_)
            if (page.id == popupPageId_) { cellH = page.cellHeight; break; }
        RECT content = GetCollectionPopupContentRect(rPopup);
        int viewH = std::max(1, static_cast<int>(content.bottom - content.top));
        int targetY = popupRow * cellH;
        int maxPopupScroll = GetCollectionPopupMaxScrollOffset(
            widgets_[keyboardNavWidgetIndex_], rPopup);
        if (targetY < popupScrollOffset_)
            popupScrollOffset_ = targetY;
        else if (targetY + cellH > popupScrollOffset_ + viewH)
            popupScrollOffset_ = targetY + cellH - viewH;
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0, maxPopupScroll);
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}
