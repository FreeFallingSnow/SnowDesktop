#include "app.h"

bool DesktopApp::CanRenameWidget(
    const DesktopWidget& widget) const
{
    return widget.type != DesktopWidgetType::LuaScript ||
        widget.showTitle;
}

size_t DesktopApp::ResolveRenameVisibilityWidgetIndex(
    size_t widgetIndex) const
{
    if (widgetIndex >= widgets_.size())
        return RenameController::InvalidIndex;

    const DesktopWidget& widget = widgets_[widgetIndex];
    if (widget.type == DesktopWidgetType::Collection)
    {
        const size_t groupIndex =
            FindCollectionGroupIndexForChild(widget.id);
        if (groupIndex < widgets_.size())
            return groupIndex;
    }
    if (widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping)
    {
        const size_t groupIndex =
            FindFileGroupIndexForChild(widget.id);
        if (groupIndex < widgets_.size())
            return groupIndex;
    }
    return widgetIndex;
}

RECT DesktopApp::GetVisibleCollectionItemBounds(
    size_t itemIndex, size_t* visibilityWidgetIndex) const
{
    if (visibilityWidgetIndex)
        *visibilityWidgetIndex = RenameController::InvalidIndex;
    if (itemIndex >= items_.size()) return {};
    std::wstring key = ToUpperInvariant(items_[itemIndex].layoutKey);

    if (snowdesktop::popup_animation_rules::
            ShouldUsePopupItemBounds(
                popupWidgetIndex_ < widgets_.size(),
                IsCollectionPopupInteractive()))
    {
        const DesktopWidget& widget = widgets_[popupWidgetIndex_];
        std::vector<std::wstring> keys = GetPopupItemKeys(widget);
        RECT popup = GetCollectionPopupRect(widget);
        RECT content = GetCollectionPopupContentRect(popup);
        for (size_t i = 0; i < keys.size(); ++i)
        {
            if (ToUpperInvariant(keys[i]) != key) continue;
            RECT rect = GetCollectionPopupItemRect(popup, i);
            if (RectsIntersect(rect, content))
            {
                if (visibilityWidgetIndex)
                {
                    *visibilityWidgetIndex =
                        ResolveRenameVisibilityWidgetIndex(
                            popupWidgetIndex_);
                }
                return rect;
            }
        }
    }

    for (const auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc) continue;
        for (const auto& slot : wc->GetSlots())
        {
            auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem());
            if (icon && icon->GetDesktopItem() == &items_[itemIndex])
            {
                if (visibilityWidgetIndex)
                {
                    const DesktopWidget* widget =
                        wc->GetWidgetData();
                    *visibilityWidgetIndex = widget
                        ? ResolveRenameVisibilityWidgetIndex(
                            FindWidgetIndexById(widget->id))
                        : RenameController::InvalidIndex;
                }
                return slot->GetBounds();
            }
        }
    }
    return {};
}

bool DesktopApp::FindSingleSelectedFolderEntry(size_t& widgetIndex, size_t& memberIndex) const
{
    size_t foundWidget = static_cast<size_t>(-1);
    size_t foundMember = static_cast<size_t>(-1);
    int count = 0;
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const auto& widget = widgets_[wi];
        if (widget.type != DesktopWidgetType::FolderMapping) continue;
        for (size_t mi = 0; mi < widget.folderEntries.size(); ++mi)
        {
            if (!widget.folderEntries[mi].selected) continue;
            foundWidget = wi;
            foundMember = mi;
            ++count;
        }
    }
    if (count != 1) return false;
    widgetIndex = foundWidget;
    memberIndex = foundMember;
    return true;
}

RECT DesktopApp::GetFolderEntryRenameRect(size_t widgetIndex, size_t memberIndex) const
{
    if (widgetIndex >= widgets_.size() ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return {};

    const size_t fileGroupIndex =
        FindFileGroupIndexForChild(
            widgets_[widgetIndex].id);
    if (fileGroupIndex < widgets_.size())
    {
        for (const auto& c : containers_)
        {
            auto* group =
                dynamic_cast<FileGroup*>(c.get());
            if (!group ||
                group->GetWidgetData() !=
                    &widgets_[fileGroupIndex] ||
                group->GetActiveSourceId() !=
                    widgets_[widgetIndex].id)
                continue;
            for (const auto& slot : group->GetSlots())
            {
                auto* icon = slot
                    ? dynamic_cast<FolderEntryIcon*>(
                        slot->GetItem())
                    : nullptr;
                if (!icon ||
                    icon->GetFolderEntry() !=
                        &widgets_[widgetIndex].
                            folderEntries[memberIndex])
                    continue;
                const RECT itemRect = slot->GetBounds();
                if (widgets_[fileGroupIndex].listMode)
                {
                    const int itemH = std::max<int>(
                        1, itemRect.bottom -
                            itemRect.top);
                    const int iconSize =
                        std::min(32, itemH - 4);
                    return MakeRect(
                        itemRect.left + 4 +
                            iconSize + 6,
                        itemRect.top + 5,
                        itemRect.right - 6,
                        itemRect.bottom - 5);
                }
                return GetItemTextRect(
                    itemRect, true);
            }
        }
    }

    for (const auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc || wc->GetWidgetData() != &widgets_[widgetIndex]) continue;
        const auto& slots = wc->GetSlots();
        if (memberIndex >= slots.size()) break;
        RECT itemRect = slots[memberIndex]->GetBounds();
        if (widgets_[widgetIndex].listMode)
        {
            const int itemH = std::max<int>(1, static_cast<int>(itemRect.bottom - itemRect.top));
            const int iconSz = std::min(32, itemH - 4);
            return MakeRect(itemRect.left + 4 + iconSz + 6, itemRect.top + 5,
                itemRect.right - 6, itemRect.bottom - 5);
        }
        return GetItemTextRect(itemRect, true);
    }
    return {};
}

/**
 * @brief 开始对文件夹条目进行重命名（创建弹出式编辑框）
 * @param widgetIndex 部件索引
 * @param memberIndex 条目索引
 */


/**
 * @brief 获取文件夹条目重命名编辑框的矩形位置
 * @param widgetIndex 部件索引
 * @param memberIndex 条目索引
 * @return 重命名编辑框的矩形
 */


/**
 * @brief 查找唯一选中的文件夹条目
 * @param widgetIndex [out] 部件索引
 * @param memberIndex [out] 条目在部件中的索引
 * @return 是否恰好有一个选中条目
 */
