#include "app.h"

DesktopApp::RenameClickHit DesktopApp::HitTestRenameClick(POINT point) const
{
    if (renameEdit_ || quickNavigationOpen_ || largeIconGesture_ ||
        !luaWidgetPanelRequest_.widgetId.empty() || HasActiveContextMenuSession() ||
        IsPointInUsageGuide(point))
        return {};

    const auto desktopHit = [&](const DesktopItem& item, RECT label,
                                const std::wstring& surface) -> RenameClickHit {
        if (!item.desktopIconClsid.empty() || item.layoutKey.empty()) return {};
        return {{RenameTargetKind::DesktopItem, item.layoutKey, surface},
            label, item.selected};
    };

    if (IsPointOccludedByOpenPopup(point))
    {
        const auto* popupWidget = GetOpenPopupWidget();
        if (!popupWidget || !IsCollectionPopupInteractive()) return {};
        const RECT popup = GetCollectionPopupRect(*popupWidget);
        const RECT content = GetCollectionPopupContentRect(popup);
        if (!PtInRect(&content, point)) return {};
        const auto keys = dockFolderPopupOpen_ ? std::vector<std::wstring>{}
            : GetPopupItemKeys(*popupWidget);
        const size_t count = dockFolderPopupOpen_
            ? dockFolderPopupWidget_.folderEntries.size() : keys.size();
        for (size_t i = 0; i < count; ++i)
        {
            if (!HitTestCollectionPopupItem(popup, i, point)) continue;
            const RECT label = GetCollectionPopupItemTextRect(
                GetCollectionPopupItemRect(popup, i));
            if (dockFolderPopupOpen_)
            {
                const auto& entry = dockFolderPopupWidget_.folderEntries[i];
                return {{RenameTargetKind::DockFolderEntry, entry.fullPath,
                    L"popup:" + dockFolderPopupSourceId_}, label, entry.selected};
            }
            const size_t index = FindItemIndexByKey(keys[i]);
            return index < items_.size() ? desktopHit(items_[index], label,
                L"popup:" + popupWidget->id) : RenameClickHit{};
        }
        return {};
    }
    // Dock icons have launch/switch semantics and no persistent name label.
    if (GetDockContainerAtPoint(point)) return {};
    for (size_t n = widgets_.size(); n > 0; --n)
        if (HitTestStandaloneWidget(n - 1, point) != WidgetHit::None)
            return {};

    // Use the same widget ordering, chrome and clipped slots as pointer-down.
    for (const auto& widget : widgets_)
    {
        if (desktopIconsHidden_ && !widget.keepWhenDesktopHidden) continue;
        for (const auto& container : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(container.get());
            if (!wc || wc->GetWidgetData() != &widget) continue;
            const auto hit = wc->HitTestWidget(point);
            if (hit == WidgetHit::None) break;
            if (hit != WidgetHit::Content) return {};
            const RECT body = wc->GetBodyRect();
            if (!PtInRect(&body, point)) return {};
            for (const auto& slot : wc->GetSlots())
            {
                const RECT bounds = slot->GetBounds();
                Item* item = slot->GetItem();
                if (!item || !PtInRect(&bounds, point)) continue;
                const auto* list = dynamic_cast<const ScrollingItemWidget*>(wc);
                const RECT label = list && list->SingleColumn()
                    ? list->GetListItemTextRect(bounds) : GetItemTextRect(bounds, true);
                if (const auto* icon = dynamic_cast<const DesktopIcon*>(item))
                    return desktopHit(*icon->GetDesktopItem(), label, widget.id);
                if (const auto* icon = dynamic_cast<const FolderEntryIcon*>(item))
                    return {{RenameTargetKind::FolderEntry,
                        icon->GetFolderEntry()->fullPath, widget.id},
                        label, item->IsSelected()};
                return {};
            }
            return {};
        }
    }
    if (const auto* icon = HitTestIcon(point))
        return desktopHit(*icon->GetDesktopItem(),
            GetItemTextRect(icon->GetBounds(), true), L"desktop");
    return {};
}

bool DesktopApp::HasSingleRenameClickSelection() const
{
    size_t count = 0;
    const auto countRange = [&count](const auto& values) {
        for (const auto& value : values)
            if (value.selected) ++count;
    };
    countRange(items_);
    countRange(dockEntries_);
    countRange(dockUnpinnedRunningApps_);
    countRange(widgets_);
    for (const auto& widget : widgets_) countRange(widget.folderEntries);
    if (dockFolderPopupOpen_) countRange(dockFolderPopupWidget_.folderEntries);
    return count == 1;
}

namespace
{
bool RenameClickUnmodified(WPARAM modifiers)
{
    return (modifiers & (MK_CONTROL | MK_SHIFT | MK_RBUTTON | MK_MBUTTON)) == 0 &&
        ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) |
            GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_RBUTTON) |
            GetAsyncKeyState(VK_MBUTTON)) & 0x8000) == 0;
}
}

void DesktopApp::BeginRenameClick(WPARAM modifiers, POINT point)
{
    KillTimer(hwnd_, kRenameClickTimerId);
    const auto hit = HitTestRenameClick(point);
    renameClickController_.Press(hit.target, hit.label, point,
        hit.selected && HasSingleRenameClickSelection(),
        RenameClickUnmodified(modifiers), GetTickCount64(), GetDoubleClickTime());
}

void DesktopApp::CompleteRenameClick(WPARAM modifiers, POINT point)
{
    if (!renameClickController_.Active()) return;
    renameClickController_.Move(point, GetSystemMetrics(SM_CXDRAG),
        GetSystemMetrics(SM_CYDRAG));
    const auto hit = HitTestRenameClick(point);
    if (!renameClickController_.Release(hit.target, hit.label, point,
            mouseDown_ && !marqueeActive_ && !dragSession_.IsActive() &&
            !dragDropController_.IsTransportActive() && hit.selected &&
            HasSingleRenameClickSelection() && RenameClickUnmodified(modifiers),
            GetTickCount64(), GetDoubleClickTime()))
        return;
    renameClickPoint_ = point;
    renameClickForeground_ = GetForegroundWindow();
    if (!SetTimer(hwnd_, kRenameClickTimerId, GetDoubleClickTime(), nullptr))
        CancelRenameClick();
}

void DesktopApp::CancelRenameClick()
{
    if (hwnd_) KillTimer(hwnd_, kRenameClickTimerId);
    renameClickController_.Cancel();
}

void DesktopApp::OnRenameClickTimer()
{
    const ULONGLONG now = GetTickCount64();
    if (renameClickController_.Waiting(now)) return;
    KillTimer(hwnd_, kRenameClickTimerId);
    const auto hit = HitTestRenameClick(renameClickPoint_);
    const bool eligible = !mouseDown_ && widgetAction_ == WidgetAction::None &&
        !middleButtonWidgetMove_ && !dragSession_.IsActive() &&
        !dragDropController_.IsTransportActive() &&
        GetForegroundWindow() == renameClickForeground_ &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
        RenameClickUnmodified(0) && hit.selected && HasSingleRenameClickSelection();
    if (!renameClickController_.TakeReady(hit.target, hit.label, eligible, now)) return;
    BeginRenameSelected();
}

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

    const size_t owner = ResolveRenameVisibilityWidgetIndex(widgetIndex);
    for (const auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc || owner >= widgets_.size() || wc->GetWidgetData() != &widgets_[owner])
            continue;
        for (const auto& slot : wc->GetSlots())
        {
            const auto* icon = dynamic_cast<const FolderEntryIcon*>(slot->GetItem());
            if (!icon || icon->GetFolderEntry() != &widgets_[widgetIndex].folderEntries[memberIndex])
                continue;
            const RECT bounds = slot->GetBounds();
            const auto* list = dynamic_cast<const ScrollingItemWidget*>(wc);
            return list && list->SingleColumn() ? list->GetListItemTextRect(bounds)
                : GetItemTextRect(bounds, true);
        }
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
