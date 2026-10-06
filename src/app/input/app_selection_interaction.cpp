#include "app/app.h"
#include "shell/shell_extension_service.h"
#include "drag_drop/drag_input_rules.h"

// Selection projection, keyboard focus sync and marquee selection.

namespace
{
std::wstring DesktopSelectionKey(const DesktopItem& item)
{
    return L"item:" + ToUpperInvariant(item.layoutKey);
}

std::wstring FolderSelectionKey(
    const DesktopWidget& owner, const FolderEntry& entry)
{
    return L"folder:" + owner.id + L":" + ToUpperInvariant(entry.fullPath);
}
}

std::vector<SelectionController::Target>
DesktopApp::GetDesktopSelectionTargets(const std::wstring& pageId)
{
    std::vector<DesktopItem*> ordered;
    for (auto& item : items_)
    {
        if (item.name.empty() || IsRectEmptyRect(item.bounds) ||
            IsItemInAnyWidget(item) ||
            (desktopIconsHidden_ && !IsRetainedLargeIcon(item)) ||
            (!pageId.empty() && item.gridCell.pageId != pageId))
            continue;
        if (!gridPages_.empty() && std::none_of(
                gridPages_.begin(), gridPages_.end(),
                [&](const GridPage& page) {
                    return page.id == item.gridCell.pageId;
                }))
            continue;
        ordered.push_back(&item);
    }
    // Match desktop keyboard traversal: top to bottom, then the next column.
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        if (a->gridCell.pageId != b->gridCell.pageId)
            return a->gridCell.pageId < b->gridCell.pageId;
        if (a->gridCell.column != b->gridCell.column)
            return a->gridCell.column < b->gridCell.column;
        if (a->gridCell.row != b->gridCell.row)
            return a->gridCell.row < b->gridCell.row;
        return a->layoutKey < b->layoutKey;
    });
    std::vector<SelectionController::Target> targets;
    for (auto* item : ordered)
        targets.push_back({ DesktopSelectionKey(*item), &item->selected });
    return targets;
}

std::vector<SelectionController::Target>
DesktopApp::GetWidgetSelectionTargets(size_t widgetIndex)
{
    std::vector<SelectionController::Target> targets;
    if (widgetIndex >= widgets_.size()) return targets;
    auto& widget = widgets_[widgetIndex];
    const auto appendKeys = [&](const auto& keys) {
        for (const auto& key : keys)
        {
            const size_t index = FindItemIndexByKey(key);
            if (index < items_.size())
                targets.push_back({ DesktopSelectionKey(items_[index]),
                    &items_[index].selected });
        }
    };
    const auto appendFolders = [&](DesktopWidget& owner, const auto& indices) {
        for (const size_t index : indices)
            if (index < owner.folderEntries.size())
            {
                auto& entry = owner.folderEntries[index];
                targets.push_back({ FolderSelectionKey(owner, entry),
                    &entry.selected });
            }
    };
    for (auto& container : containers_)
    {
        auto* view = dynamic_cast<WidgetContainer*>(container.get());
        if (!view || view->GetWidgetData() != &widget) continue;
        if (auto* fileGroup = dynamic_cast<FileGroup*>(view))
        {
            if (fileGroup->IsGroupSearchActive())
            {
                for (const auto& result : fileGroup->GetGroupSearchResults())
                {
                    if (!result.folderMapping)
                        appendKeys(std::vector<std::wstring>{ result.desktopKey });
                    else
                    {
                        const size_t source = FindWidgetIndexById(result.sourceId);
                        if (source < widgets_.size())
                            appendFolders(widgets_[source],
                                std::vector<size_t>{ result.folderEntryIndex });
                    }
                }
            }
            else
            {
                appendKeys(fileGroup->GetHostedVisibleItemKeys());
                if (auto* source = fileGroup->GetActiveSourceContainer();
                    source && source->GetWidgetData())
                    appendFolders(*source->GetWidgetData(),
                        fileGroup->GetHostedVisibleFolderIndices());
            }
        }
        else if (auto* collectionGroup = dynamic_cast<CollectionGroup*>(view))
            appendKeys(collectionGroup->GetVisibleItemKeys());
        else if (auto* categories = dynamic_cast<FileCategories*>(view))
            appendKeys(categories->GetSearchResultKeys());
        else if (auto* mapping = dynamic_cast<FolderMapping*>(view))
            appendFolders(widget, mapping->GetVisibleEntryIndices());
        else if (widget.type == DesktopWidgetType::Collection)
            appendKeys(widget.itemKeys);
        break;
    }
    return targets;
}

std::vector<SelectionController::Target> DesktopApp::GetPopupSelectionTargets()
{
    std::vector<SelectionController::Target> targets;
    auto* popup = GetOpenPopupWidget();
    if (!popup) return targets;
    if (dockFolderPopupOpen_)
    {
        for (size_t i = 0; i < GetPopupItemCount(*popup); ++i)
        {
            const size_t index = GetPopupFolderEntryIndex(*popup, i);
            if (index >= popup->folderEntries.size()) continue;
            auto& entry = popup->folderEntries[index];
            targets.push_back({ FolderSelectionKey(*popup, entry), &entry.selected });
        }
    }
    else
        for (const auto& key : GetPopupItemKeys(*popup))
        {
            const size_t index = FindItemIndexByKey(key);
            if (index < items_.size())
                targets.push_back({ DesktopSelectionKey(items_[index]),
                    &items_[index].selected });
        }
    return targets;
}

std::wstring DesktopApp::GetItemSelectionKey(Item* item) const
{
    if (auto* icon = dynamic_cast<DesktopIcon*>(item);
        icon && icon->GetDesktopItem())
        return DesktopSelectionKey(*icon->GetDesktopItem());
    if (auto* icon = dynamic_cast<FolderEntryIcon*>(item);
        icon && icon->GetFolderEntry())
    {
        auto* owner = dynamic_cast<WidgetContainer*>(icon->GetContainer());
        if (auto* group = dynamic_cast<FileGroup*>(owner))
            owner = group->GetSourceContainerForItem(item);
        if (owner && owner->GetWidgetData())
            return FolderSelectionKey(*owner->GetWidgetData(), *icon->GetFolderEntry());
    }
    return L"";
}

std::wstring DesktopApp::GetWidgetSelectionScope(size_t widgetIndex) const
{
    return widgetIndex < widgets_.size()
        ? L"widget:" + widgets_[widgetIndex].id : L"";
}

std::wstring DesktopApp::GetPopupSelectionScope() const
{
    return dockFolderPopupOpen_
        ? L"dock-popup:" + dockFolderPopupWidget_.id
        : L"popup:" + GetWidgetSelectionScope(popupWidgetIndex_);
}

size_t DesktopApp::GetSelectionWidgetIndex() const
{
    if (keyboardNavInsideWidget_ && keyboardNavWidgetIndex_ < widgets_.size())
        return keyboardNavWidgetIndex_;
    for (size_t index = 0; index < widgets_.size(); ++index)
        if (!IsGroupedWidget(widgets_[index]) &&
            (widgets_[index].selected || HasSelectedFilesInWidget(index)))
            return index;
    return static_cast<size_t>(-1);
}

bool DesktopApp::ExtendPointerSelection(const std::wstring& scope,
    const std::vector<SelectionController::Target>& targets,
    const std::wstring& key, bool additive)
{
    return selectionController_.SelectRange(scope, targets, key, additive, [&]() {
        ClearSelection();
        // Dock folder entries live outside the desktop model ranges.
        if (dockFolderPopupOpen_)
            for (auto& entry : dockFolderPopupWidget_.folderEntries)
                entry.selected = false;
    });
}

bool DesktopApp::IsMarqueePointerGesturePendingOrActive() const
{
    const bool hasMarqueeTarget =
        marqueeDockFolderPopup_ ||
        marqueeWidgetIndex_ < widgets_.size() ||
        mouseDownWidgetIndex_ >= widgets_.size();
    return snowdesktop::drag_input_rules::IsMarqueePointerGesture(
        marqueeActive_, mouseDown_, mouseDownHit_ != nullptr,
        pendingWidgetButtonAction_ != WidgetHit::None,
        widgetAction_ != WidgetAction::None,
        middleButtonWidgetMove_, detailColumnResizeActive_,
        widgetScrollbarDragging_, popupScrollbarDragging_,
        luaWidgetPanelMouseDown_, hasMarqueeTarget);
}

void DesktopApp::ClearSelection()
{
    selectionController_.ClearAll(
        items_, dockEntries_,
        dockUnpinnedRunningApps_, widgets_);
    keyboardNavInsideWidget_ = false;
    keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    keyboardNavMemberIndex_ = -1;
    keyboardNavVisualFocus_ = false;
    keyboardNavSearchBox_ = false;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;
}

bool DesktopApp::OwnsWidgetKeyboardNavigation(
    const ScrollingItemWidget* widget) const
{
    if (!widget || widget->IsPreviewRendering() ||
        !keyboardNavInsideWidget_ ||
        keyboardNavWidgetIndex_ >= widgets_.size())
        return false;

    const DesktopWidget* data = widget->GetWidgetData();
    const auto& owner = widgets_[keyboardNavWidgetIndex_];
    if (data == &owner)
        return true;
    if (!widget->IsHosted() ||
        owner.type != DesktopWidgetType::FileGroup ||
        !data)
        return false;
    return std::find(
        owner.childWidgetIds.begin(),
        owner.childWidgetIds.end(),
        data->id) != owner.childWidgetIds.end();
}

bool DesktopApp::HasSelectedFilesInWidget(
    size_t widgetIndex) const
{
    if (widgetIndex >= widgets_.size())
        return false;

    const auto hasDirectSelection = [&](const DesktopWidget& widget) {
        for (const auto& key : widget.itemKeys)
        {
            const size_t itemIndex =
                FindItemIndexByKey(key);
            if (itemIndex < items_.size() &&
                items_[itemIndex].selected)
                return true;
        }
        return std::any_of(
            widget.folderEntries.begin(),
            widget.folderEntries.end(),
            [](const FolderEntry& entry) {
                return entry.selected;
            });
    };

    const auto& widget = widgets_[widgetIndex];
    if (hasDirectSelection(widget))
        return true;
    if (widget.type != DesktopWidgetType::CollectionGroup &&
        widget.type != DesktopWidgetType::FileGroup)
        return false;

    for (const auto& childId : widget.childWidgetIds)
    {
        const size_t childIndex =
            FindWidgetIndexById(childId);
        if (childIndex < widgets_.size() &&
            hasDirectSelection(widgets_[childIndex]))
            return true;
    }
    return false;
}

/**
 * @brief 根据当前选中状态同步键盘导航上下文
 *
 * 扫描所有组件成员项的选中状态，
 * 若有成员项被选中则将导航上下文切换到该组件内部，
 * 否则重置为桌面网格导航模式。
 */
void DesktopApp::SyncKeyboardNavFromSelection()
{
    if (snowdesktop::shell_extensions::HasOptIns(generalSettings_.shellExtensions))
    {
        snowdesktop::shell_extensions::Request request;
        request.paths = GetSelectedFolderEntryPaths();
        if (request.paths.empty())
            for (const auto &item : items_) if (item.selected && !item.parsingName.empty()) request.paths.push_back(item.parsingName);
        snowdesktop::shell_extensions::SharedMenuService().Prewarm(request);
    }

    // Pointer selection may seed the next arrow-key navigation position, but
    // it must not display the keyboard-only focus outline.
    keyboardNavVisualFocus_ = false;
    keyboardNavSearchBox_ = false;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const auto& w = widgets_[wi];
        if (IsGroupedWidget(w))
            continue;
        if (w.type == DesktopWidgetType::FileGroup)
        {
            for (auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<FileGroup*>(
                        container.get());
                if (!group ||
                    group->GetWidgetData() != &w)
                    continue;
                const auto& sourceIds =
                    group->GetVisibleSourceIds();
                for (size_t k = 0;
                    k < sourceIds.size(); ++k)
                {
                    const size_t childIndex =
                        FindWidgetIndexById(
                            sourceIds[k]);
                    if (childIndex < widgets_.size() &&
                        widgets_[childIndex].selected)
                    {
                        keyboardNavInsideWidget_ = true;
                        keyboardNavWidgetIndex_ = wi;
                        keyboardNavMemberIndex_ =
                            static_cast<int>(k);
                        keyboardNavCollectionGroupTabs_ =
                            true;
                        return;
                    }
                }
                const auto keys =
                    group->GetHostedVisibleItemKeys();
                for (size_t k = 0; k < keys.size(); ++k)
                {
                    const size_t itemIndex =
                        FindItemIndexByKey(keys[k]);
                    if (itemIndex < items_.size() &&
                        items_[itemIndex].selected)
                    {
                        keyboardNavInsideWidget_ = true;
                        keyboardNavWidgetIndex_ = wi;
                        keyboardNavMemberIndex_ =
                            static_cast<int>(k);
                        return;
                    }
                }
                auto* active =
                    group->GetActiveSourceContainer();
                DesktopWidget* activeData = active
                    ? active->GetWidgetData() : nullptr;
                const auto indices =
                    group->
                        GetHostedVisibleFolderIndices();
                if (activeData)
                    for (size_t k = 0;
                        k < indices.size(); ++k)
                    {
                        const size_t entryIndex =
                            indices[k];
                        if (entryIndex <
                                activeData->
                                    folderEntries.size() &&
                            activeData->
                                folderEntries[entryIndex].
                                    selected)
                        {
                            keyboardNavInsideWidget_ = true;
                            keyboardNavWidgetIndex_ = wi;
                            keyboardNavMemberIndex_ =
                                static_cast<int>(k);
                            return;
                        }
                    }
                break;
            }
        }
        if (w.type == DesktopWidgetType::CollectionGroup)
        {
            for (auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<CollectionGroup*>(container.get());
                if (!group || group->GetWidgetData() != &w)
                    continue;
                const auto& keys = group->GetVisibleItemKeys();
                for (size_t k = 0; k < keys.size(); ++k)
                {
                    const size_t itemIndex =
                        FindItemIndexByKey(keys[k]);
                    if (itemIndex < items_.size() &&
                        items_[itemIndex].selected)
                    {
                        keyboardNavInsideWidget_ = true;
                        keyboardNavWidgetIndex_ = wi;
                        keyboardNavMemberIndex_ =
                            static_cast<int>(k);
                        return;
                    }
                }
                break;
            }
        }
        for (size_t k = 0; k < w.itemKeys.size(); ++k)
        {
            size_t idx = FindItemIndexByKey(w.itemKeys[k]);
            if (idx != static_cast<size_t>(-1) && items_[idx].selected)
            {
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ = wi;
                keyboardNavMemberIndex_ = static_cast<int>(k);
                return;
            }
        }
        for (size_t k = 0; k < w.folderEntries.size(); ++k)
        {
            if (w.folderEntries[k].selected)
            {
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ = wi;
                keyboardNavMemberIndex_ = static_cast<int>(k);
                return;
            }
        }
        if (w.type == DesktopWidgetType::CollectionGroup)
        {
            for (size_t k = 0; k < w.childWidgetIds.size(); ++k)
            {
                const size_t childIndex =
                    FindWidgetIndexById(w.childWidgetIds[k]);
                if (childIndex < widgets_.size() &&
                    widgets_[childIndex].selected)
                {
                    keyboardNavInsideWidget_ = true;
                    keyboardNavWidgetIndex_ = wi;
                    keyboardNavMemberIndex_ =
                        static_cast<int>(k);
                    keyboardNavCollectionGroupTabs_ = true;
                    return;
                }
            }
        }
    }
    keyboardNavInsideWidget_ = false;
    keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    keyboardNavMemberIndex_ = -1;
    keyboardNavSearchBox_ = false;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;
}

/**
 * @brief 清除指定小部件之外的所有选中状态
 * @param widgetIndex 保留选中的小部件索引
 */
void DesktopApp::ClearSelectionOutsideWidget(size_t widgetIndex)
{
    for (auto& entry : dockEntries_)
        entry.selected = false;
    std::unordered_set<std::wstring> allowedKeys;
    std::unordered_set<std::wstring> allowedWidgetIds;
    std::unordered_set<std::wstring>
        allowedFolderWidgetIds;
    if (widgetIndex < widgets_.size())
    {
        for (const auto& key : widgets_[widgetIndex].itemKeys)
            allowedKeys.insert(ToUpperInvariant(key));
        if (widgets_[widgetIndex].type ==
                DesktopWidgetType::CollectionGroup ||
            widgets_[widgetIndex].type ==
                DesktopWidgetType::FileGroup)
        {
            allowedWidgetIds.insert(
                widgets_[widgetIndex].childWidgetIds.begin(),
                widgets_[widgetIndex].childWidgetIds.end());
            for (const auto& childId :
                widgets_[widgetIndex].childWidgetIds)
            {
                const size_t childIndex =
                    FindWidgetIndexById(childId);
                if (childIndex >= widgets_.size()) continue;
                for (const auto& key :
                    widgets_[childIndex].itemKeys)
                    allowedKeys.insert(ToUpperInvariant(key));
                if (widgets_[childIndex].type ==
                    DesktopWidgetType::FolderMapping)
                    allowedFolderWidgetIds.insert(
                        widgets_[childIndex].id);
            }
        }
    }

    for (auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!allowedKeys.contains(key))
            item.selected = false;
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        if (!allowedWidgetIds.contains(widgets_[wi].id))
            widgets_[wi].selected = false;
        if ((wi == widgetIndex &&
             widgets_[wi].type ==
                DesktopWidgetType::FolderMapping) ||
            allowedFolderWidgetIds.contains(
                widgets_[wi].id))
            continue;
        for (auto& entry : widgets_[wi].folderEntries)
            entry.selected = false;
    }
}

/**
 * @brief 清除桌面区域之外（即小部件内）的所有选中状态
 */
void DesktopApp::ClearSelectionOutsideDesktop()
{
    for (auto& entry : dockEntries_)
        entry.selected = false;
    for (auto& item : items_)
    {
        if (IsItemInAnyWidget(item))
            item.selected = false;
    }
    for (auto& widget : widgets_)
    {
        widget.selected = false;
        for (auto& entry : widget.folderEntries)
            entry.selected = false;
    }
}

/**
 * @brief 仅选中指定索引的桌面项（清除其他所有选中状态）
 * @param index 桌面项索引
 */
void DesktopApp::SelectOnly(int index)
{
    ClearSelection();
    if (index >= 0 && static_cast<size_t>(index) < items_.size())
    {
        // Find the OO icon for this item
        selectionController_.SelectDesktop(
            items_, static_cast<size_t>(index));
    }
}

/**
 * @brief 切换指定桌面项的选中/未选中状态
 * @param index 桌面项索引
 */
void DesktopApp::ToggleSelection(int index)
{
    if (index >= 0 && static_cast<size_t>(index) < items_.size())
        selectionController_.ToggleDesktop(
            items_, static_cast<size_t>(index));
}

int DesktopApp::GetMarqueeScrollOffset() const
{
    if (marqueeDockFolderPopup_ &&
        dockFolderPopupOpen_)
        return popupScrollOffset_;
    if (marqueeWidgetIndex_ >= widgets_.size())
        return 0;
    if (popupWidgetIndex_ == marqueeWidgetIndex_)
        return popupScrollOffset_;

    for (const auto& container : containers_)
    {
        auto* widgetContainer = dynamic_cast<WidgetContainer*>(container.get());
        if (widgetContainer &&
            widgetContainer->GetWidgetData() == &widgets_[marqueeWidgetIndex_])
        {
            return widgetContainer->GetScrollOffset();
        }
    }
    return 0;
}

RECT DesktopApp::GetMarqueeViewportRect() const
{
    if (marqueeDockFolderPopup_ &&
        dockFolderPopupOpen_)
    {
        return GetCollectionPopupContentRect(
            GetCollectionPopupRect(
                dockFolderPopupWidget_));
    }
    if (marqueeWidgetIndex_ >= widgets_.size())
    {
        RECT client{};
        if (hwnd_)
            GetClientRect(hwnd_, &client);
        return client;
    }
    if (popupWidgetIndex_ == marqueeWidgetIndex_)
    {
        return GetCollectionPopupContentRect(
            GetCollectionPopupRect(widgets_[popupWidgetIndex_]));
    }

    for (const auto& container : containers_)
    {
        auto* widgetContainer = dynamic_cast<WidgetContainer*>(container.get());
        if (widgetContainer &&
            widgetContainer->GetWidgetData() == &widgets_[marqueeWidgetIndex_])
        {
            return widgetContainer->GetContentViewportRect();
        }
    }
    return {};
}

void DesktopApp::UpdateMarqueeSelection(POINT current)
{
    if (marqueeDockFolderPopup_ &&
        dockFolderPopupOpen_)
    {
        const int currentScroll =
            popupScrollOffset_;
        const RECT popup =
            GetCollectionPopupRect(
                dockFolderPopupWidget_);
        const RECT viewport =
            GetCollectionPopupContentRect(popup);
        const POINT contentAnchor{
            marqueeAnchorPoint_.x,
            marqueeAnchorPoint_.y +
                marqueeInitialScrollOffset_
        };
        const POINT contentCurrent{
            std::clamp<LONG>(
                current.x,
                viewport.left,
                viewport.right),
            std::clamp<LONG>(
                current.y,
                viewport.top,
                viewport.bottom) +
                currentScroll
        };
        const RECT contentSelectionRect =
            NormalizeRect(
                contentAnchor,
                contentCurrent);

        marqueeRect_ = contentSelectionRect;
        OffsetRect(
            &marqueeRect_, 0,
            -currentScroll);
        for (size_t i = 0;
            i < GetPopupItemCount(dockFolderPopupWidget_); ++i)
        {
            RECT itemRect =
                GetCollectionPopupItemRect(
                    popup, i);
            OffsetRect(
                &itemRect, 0,
                currentScroll);
            dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(dockFolderPopupWidget_, i)].selected =
                (GetPopupFolderEntryIndex(dockFolderPopupWidget_, i) <
                    dockFolderPopupMarqueeInitialSelection_.
                        size() &&
                 dockFolderPopupMarqueeInitialSelection_[GetPopupFolderEntryIndex(dockFolderPopupWidget_, i)]) ||
                (!IsRectEmptyRect(itemRect) && RectsIntersect(
                    itemRect,
                    contentSelectionRect));
        }
        return;
    }

    if (marqueeWidgetIndex_ < widgets_.size())
    {
        const int currentScroll = GetMarqueeScrollOffset();
        RECT viewport = GetMarqueeViewportRect();
        POINT contentAnchor{
            marqueeAnchorPoint_.x,
            marqueeAnchorPoint_.y + marqueeInitialScrollOffset_
        };
        POINT contentCurrent{
            std::clamp<LONG>(current.x, viewport.left, viewport.right),
            std::clamp<LONG>(current.y, viewport.top, viewport.bottom) + currentScroll
        };
        RECT contentSelectionRect = NormalizeRect(contentAnchor, contentCurrent);

        marqueeRect_ = contentSelectionRect;
        OffsetRect(&marqueeRect_, 0, -currentScroll);

        if (popupWidgetIndex_ == marqueeWidgetIndex_)
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            std::vector<std::wstring> popupKeys =
                GetPopupItemKeys(widgets_[popupWidgetIndex_]);
            for (size_t i = 0; i < popupKeys.size(); ++i)
            {
                RECT itemRect = GetCollectionPopupItemRect(popup, i);
                OffsetRect(&itemRect, 0, currentScroll);
                size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                if (itemIndex == static_cast<size_t>(-1))
                    continue;
                items_[itemIndex].selected =
                    !IsRectEmptyRect(itemRect) && RectsIntersect(itemRect, contentSelectionRect);
            }
        }
        else
        {
            for (auto& container : containers_)
            {
                auto* widgetContainer =
                    dynamic_cast<WidgetContainer*>(container.get());
                if (widgetContainer &&
                    widgetContainer->GetWidgetData() ==
                        &widgets_[marqueeWidgetIndex_])
                {
                    widgetContainer->ApplyMarqueeSelection(
                        contentSelectionRect);
                    break;
                }
            }
        }
    }
    else
    {
        marqueeRect_ = NormalizeRect(marqueeAnchorPoint_, current);
        for (auto& itemObject : items_oo_)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(itemObject.get());
            if (!icon)
                continue;
            DesktopItem* item = icon->GetDesktopItem();
            if (!item || IsItemInAnyWidget(*item) || IsRectEmptyRect(item->bounds))
                continue;
            RECT selectionRect = item->largeIcon ? GetLargeIconFrameRect(*item)
                : GetItemSelectionRect(item->bounds, false);
            item->selected = RectsIntersect(selectionRect, marqueeRect_);
        }
    }
}

/**
 * @brief 仅选中指定小部件（清除其他所有选中状态）
 * @param index 小部件索引
 */
void DesktopApp::SelectWidgetOnly(size_t index)
{
    if (index >= widgets_.size()) return;
    ClearSelection();
    selectionController_.SelectWidget(widgets_, index);
    if (widgetEngine_ && widgets_[index].type == DesktopWidgetType::LuaScript &&
        widgetEngine_->EnsureWidgetLoaded(widgets_[index].id, widgets_[index].packageId))
        widgetEngine_->InvokeSelected(widgets_[index].id);
}

/**
 * @brief 处理鼠标左键按下事件
 * @param wp WPARAM（含修饰键状态）
 * @param lp LPARAM（含鼠标坐标）
 * @details 处理逻辑：集合弹窗点击 -> 页面导航点击 -> 小部件点击 -> 桌面图标点击
 */
