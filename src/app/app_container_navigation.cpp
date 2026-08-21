#include "app.h"

// Keyboard entry/exit and activation for widget-contained items.

void DesktopApp::EnterWidget()
{
    int foundIdx = -1;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].selected) { foundIdx = static_cast<int>(i); break; }
    }
    if (foundIdx < 0) return;

    const auto& widget = widgets_[static_cast<size_t>(foundIdx)];
    if (widget.type == DesktopWidgetType::LuaScript ||
        widget.type == DesktopWidgetType::Guide)
        return;   // 此类组件无内部成员导航

    ClearSelection();

    keyboardNavInsideWidget_ = true;
    keyboardNavWidgetIndex_ = static_cast<size_t>(foundIdx);
    keyboardNavMemberIndex_ = 0;
    keyboardNavSearchBox_ = false;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;

    if (widget.type == DesktopWidgetType::FolderMapping)
    {
        size_t entryIndex = static_cast<size_t>(-1);
        FolderMapping* mapping = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<FolderMapping*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                mapping = candidate;
                const auto& visibleEntries =
                    mapping->GetVisibleEntryIndices();
                if (!visibleEntries.empty())
                    entryIndex = visibleEntries.front();
                break;
            }
        }
        if (entryIndex < widget.folderEntries.size())
        {
            widgets_[static_cast<size_t>(foundIdx)]
                .folderEntries[entryIndex].selected = true;
            keyboardNavMemberIndex_ = static_cast<int>(entryIndex);
        }
        else
        {
            if (mapping &&
                !IsRectEmptyRect(
                    mapping->GetSearchBoxRect()))
            {
                keyboardNavMemberIndex_ = -1;
                keyboardNavSearchBox_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
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
        std::vector<std::wstring> sourceIds;
        if (group)
        {
            const auto& visible =
                group->GetVisibleSourceIds();
            sourceIds.assign(
                visible.begin(), visible.end());
        }
        if (group &&
            group->IsGroupSearchActive())
        {
            if (group->GetSlotCount() == 0)
            {
                keyboardNavInsideWidget_ = false;
                keyboardNavWidgetIndex_ =
                    static_cast<size_t>(-1);
                keyboardNavMemberIndex_ = -1;
                widgets_[static_cast<size_t>(
                    foundIdx)].selected = true;
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                return;
            }
            Item* item = group->GetMemberItem(0);
            if (!item)
            {
                keyboardNavInsideWidget_ = false;
                keyboardNavWidgetIndex_ =
                    static_cast<size_t>(-1);
                keyboardNavMemberIndex_ = -1;
                widgets_[static_cast<size_t>(
                    foundIdx)].selected = true;
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                return;
            }
            item->SetSelected(true);
            keyboardNavMemberIndex_ = 0;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
        }
        else if (!group || sourceIds.empty())
        {
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ =
                static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(
                foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else
        {
            auto active = std::find(
                sourceIds.begin(), sourceIds.end(),
                group->GetActiveSourceId());
            const size_t tabIndex =
                active == sourceIds.end()
                    ? 0
                    : static_cast<size_t>(
                        std::distance(
                            sourceIds.begin(), active));
            widgets_[static_cast<size_t>(foundIdx)]
                .activeCategoryId = sourceIds[tabIndex];
            const size_t childIndex =
                FindWidgetIndexById(
                    sourceIds[tabIndex]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavMemberIndex_ =
                static_cast<int>(tabIndex);
            keyboardNavCollectionGroupTabs_ = true;
            keyboardNavFileGroupCategoryTabs_ = false;
            group->EnsureSourceTabVisible(tabIndex);
        }
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        CollectionGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<CollectionGroup*>(c.get());
            if (!candidate ||
                candidate->GetWidgetData() != &widget)
                continue;
            group = candidate;
            break;
        }
        if (!group)
        {
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ =
                static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const auto& childIds =
            group->GetVisibleCollectionIds();
        if (childIds.empty())
        {
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ =
                static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        auto active = std::find(
            childIds.begin(), childIds.end(),
            group->GetActiveCollectionId());
        const size_t tabIndex =
            active == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), active));
        widgets_[static_cast<size_t>(foundIdx)]
            .activeCategoryId = childIds[tabIndex];
        const size_t childIndex =
            FindWidgetIndexById(childIds[tabIndex]);
        if (childIndex < widgets_.size())
            widgets_[childIndex].selected = true;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        group->EnsureTabVisible(tabIndex);
    }
    else
    {
        if (!widget.itemKeys.empty())
        {
            size_t itemIdx = FindItemIndexByKey(widget.itemKeys[0]);
            if (itemIdx != static_cast<size_t>(-1))
                items_[itemIdx].selected = true;
        }
    }

    // 1 格集合（紧凑模式）：进入时直接打开弹窗
    if (widget.type == DesktopWidgetType::Collection && !widget.scrollContainerMode)
    {
        int cols = std::max(1, widget.gridSpan.columns);
        int rows = std::max(1, widget.gridSpan.rows);
        if (cols <= 1 && rows <= 1)
            OpenCollectionPopupAt(static_cast<size_t>(foundIdx),
                POINT{ widget.bounds.left, widget.bounds.top });
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 退出组件内部导航，返回桌面网格
 *
 * 清除组件内成员选中，恢复父组件的选中状态，
 * 并将导航上下文切换回桌面网格。
 */
void DesktopApp::ExitWidget()
{
    if (!keyboardNavInsideWidget_) return;

    size_t wi = keyboardNavWidgetIndex_;

    if (wi < widgets_.size())
    {
        auto& widget = widgets_[wi];
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            for (auto& e : widget.folderEntries)
                e.selected = false;
        }
        else if (widget.type == DesktopWidgetType::CollectionGroup)
        {
            for (const auto& childId : widget.childWidgetIds)
            {
                const size_t childIndex =
                    FindWidgetIndexById(childId);
                if (childIndex < widgets_.size())
                {
                    widgets_[childIndex].selected = false;
                    for (const auto& key :
                        widgets_[childIndex].itemKeys)
                    {
                        const size_t itemIndex =
                            FindItemIndexByKey(key);
                        if (itemIndex < items_.size())
                            items_[itemIndex].selected = false;
                    }
                }
            }
        }
        else if (widget.type == DesktopWidgetType::FileGroup)
        {
            for (const auto& childId :
                widget.childWidgetIds)
            {
                const size_t childIndex =
                    FindWidgetIndexById(childId);
                if (childIndex >= widgets_.size())
                    continue;
                DesktopWidget& child =
                    widgets_[childIndex];
                child.selected = false;
                for (auto& entry : child.folderEntries)
                    entry.selected = false;
                for (const auto& key : child.itemKeys)
                {
                    const size_t itemIndex =
                        FindItemIndexByKey(key);
                    if (itemIndex < items_.size())
                        items_[itemIndex].selected = false;
                }
            }
        }
        else
        {
            for (const auto& key : widget.itemKeys)
            {
                size_t itemIdx = FindItemIndexByKey(key);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
    }

    keyboardNavInsideWidget_ = false;
    keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    keyboardNavMemberIndex_ = -1;
    keyboardNavSearchBox_ = false;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;

    // 退出组件时关闭其弹窗
    if (popupWidgetIndex_ == wi)
        CloseCollectionPopup();

    if (wi < widgets_.size())
        widgets_[wi].selected = true;

    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 打开当前选中的桌面项
 *
 * 遍历 items_ 查找选中的项，通过后台 Shell 启动队列执行 "open" 动词。
 */
void DesktopApp::OpenSelectedDesktopItem()
{
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].selected && !items_[i].name.empty() &&
            !items_[i].parsingName.empty())
        {
            LaunchDesktopItem(i);
            break;
        }
    }
}

/**
 * @brief 打开组件内指定索引的成员项
 * @param widgetIndex 组件索引
 * @param memberIndex 成员索引（-1 表示无成员选中）
 *
 * 根据组件类型，通过后台 Shell 启动队列打开对应的文件或桌面项。
 */
void DesktopApp::OpenWidgetMember(size_t widgetIndex, int memberIndex)
{
    if (widgetIndex >= widgets_.size() || memberIndex < 0) return;
    const auto& widget = widgets_[widgetIndex];
    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        for (auto& c : containers_)
        {
            auto* group =
                dynamic_cast<CollectionGroup*>(c.get());
            if (!group || group->GetWidgetData() != &widget)
                continue;
            const auto& keys = group->GetVisibleItemKeys();
            if (static_cast<size_t>(memberIndex) < keys.size())
            {
                const size_t itemIndex =
                    FindItemIndexByKey(
                        keys[static_cast<size_t>(memberIndex)]);
                if (itemIndex < items_.size())
                    LaunchDesktopItem(itemIndex);
            }
            break;
        }
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        for (auto& c : containers_)
        {
            auto* group =
                dynamic_cast<FileGroup*>(c.get());
            if (!group ||
                group->GetWidgetData() != &widget)
                continue;
            if (group->IsGroupSearchActive())
            {
                Item* item = group->GetMemberItem(
                    static_cast<size_t>(memberIndex));
                if (auto* desktop =
                        dynamic_cast<DesktopIcon*>(item))
                {
                    DesktopItem* source =
                        desktop->GetDesktopItem();
                    if (source)
                    {
                        const size_t itemIndex =
                            FindItemIndexByKey(
                                source->layoutKey);
                        if (itemIndex < items_.size())
                            LaunchDesktopItem(itemIndex);
                    }
                }
                else if (item &&
                         !item->GetPath().empty())
                    shellLaunchWorker_.Enqueue(
                        hwnd_, item->GetPath());
                break;
            }
            const auto keys =
                group->GetHostedVisibleItemKeys();
            if (static_cast<size_t>(memberIndex) <
                keys.size())
            {
                const size_t itemIndex =
                    FindItemIndexByKey(
                        keys[static_cast<size_t>(
                            memberIndex)]);
                if (itemIndex < items_.size())
                    LaunchDesktopItem(itemIndex);
                break;
            }
            const auto entries =
                group->
                    GetHostedVisibleFolderIndices();
            auto* active =
                group->GetActiveSourceContainer();
            DesktopWidget* activeData = active
                ? active->GetWidgetData() : nullptr;
            if (activeData &&
                static_cast<size_t>(memberIndex) <
                    entries.size())
            {
                const size_t entryIndex =
                    entries[static_cast<size_t>(
                        memberIndex)];
                if (entryIndex <
                    activeData->folderEntries.size())
                {
                    const auto& entry =
                        activeData->
                            folderEntries[entryIndex];
                    if (!entry.fullPath.empty())
                        shellLaunchWorker_.Enqueue(
                            hwnd_, entry.fullPath);
                }
            }
            break;
        }
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        if (static_cast<size_t>(memberIndex) < widget.folderEntries.size())
        {
            const auto& entry = widget.folderEntries[static_cast<size_t>(memberIndex)];
            if (!entry.fullPath.empty())
                shellLaunchWorker_.Enqueue(
                    hwnd_, entry.fullPath);
        }
    }
    else if (!widget.itemKeys.empty() &&
        static_cast<size_t>(memberIndex) < widget.itemKeys.size())
    {
        size_t itemIdx = FindItemIndexByKey(
            widget.itemKeys[static_cast<size_t>(memberIndex)]);
        if (itemIdx != static_cast<size_t>(-1) &&
            !items_[itemIdx].parsingName.empty())
        {
            LaunchDesktopItem(itemIdx);
        }
    }
}

/**
 * @brief 处理页面导航按钮点击事件（上一页/下一页）
 * @param point 点击坐标
 * @return 是否已处理导航
 */
