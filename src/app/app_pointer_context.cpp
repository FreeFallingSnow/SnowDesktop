#include "app.h"
#include "../right_click_contract.h"
#include "../widgets/lua_logical_slot.h"

// Page-navigation clicks and right-button context dispatch.

bool DesktopApp::HandlePageNavClick(POINT point)
{
    if (gridPages_.empty()) return false;
    if (MaxPageOffset() <= 0) return false;   // 无溢出页时不处理

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    int delta = 0;
    if (PtInRect(&prevRect, point)) delta = -1;
    else if (PtInRect(&nextRect, point)) delta = 1;

    // 点击落在导航按钮区域内但方向不可用 → 拦截点击（不穿透到下方图标）
    if (delta != 0 && !((delta == -1 && hasPrev) || (delta == 1 && hasNext)))
        return true;
    if (delta == 0) return false;

    int newOffset = NextNonEmptyOffset(pageOffset_, delta);
    if (newOffset == pageOffset_) return false;

    bool wasDragging = dragSession_.IsActive();
    const POINT oldGroupOrigin{
        dragGroupOriginX_, dragGroupOriginY_ };
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (wasDragging) MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();
    if (wasDragging && !dragSession_.IsActive())
    {
        mouseDownHit_ = nullptr;
        mouseDown_ = false;
    }
    wasDragging = wasDragging && dragSession_.IsActive();
    if (wasDragging) InvalidateDragStaticScene();
    if (wasDragging)
    {
        UpdateDragGroupOrigin();
        dragSession_.AdjustForGroupOriginChange(
            oldGroupOrigin,
            { dragGroupOriginX_, dragGroupOriginY_ });
    }
    // 页面迁移后预览缓存失效
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::ShowHostInputContextMenu(
    POINT screenPoint, const std::wstring& widgetId,
    POINT localPoint, std::string_view surface)
{
    if (!widgetEngine_)
        return false;

    snowdesktop::widget_runtime::HostInputContextMenuState state;
    if (!widgetEngine_->PrepareHostInputContextMenu(
            widgetId, localPoint.x, localPoint.y, surface,
            IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE,
            state))
        return false;

    const std::wstring previousPinnedWidgetId =
        interactionPinnedWidgetId_;
    interactionPinnedWidgetId_ = widgetId;
    ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);

    PrepareMenuIconsForPoint(screenPoint);
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        interactionPinnedWidgetId_ = previousPinnedWidgetId;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    const auto flags = [](bool enabled) {
        return MF_STRING | (enabled ? 0 : MF_GRAYED);
    };
    AppendMenuW(menu, flags(state.canCut),
        kContextCutCommand, _LW("app.menu.cut"));
    AppendMenuW(menu, flags(state.canCopy),
        kContextCopyCommand, _LW("app.menu.copy"));
    AppendMenuW(menu, flags(state.canPaste),
        kContextPasteCommand, _LW("app.menu.paste"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, flags(state.canSelectAll),
        kContextSelectAllCommand, _LW("app.menu.select_all"));

    SetMenuItemIcon(menu, kContextCutCommand, L"\uf0c4");
    SetMenuItemIcon(menu, kContextCopyCommand, L"\uf0c5");
    SetMenuItemIcon(menu, kContextPasteCommand, L"\uf0ea");
    SetMenuItemQuickAction(menu, kContextCutCommand);
    SetMenuItemQuickAction(menu, kContextCopyCommand);
    SetMenuItemQuickAction(menu, kContextPasteCommand);

    SetForegroundWindow(hwnd_);
    const UINT command = ShowModernMenu(
        menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();

    using snowdesktop::widget_runtime::HostInputEditCommand;
    switch (command)
    {
    case kContextSelectAllCommand:
        widgetEngine_->ExecuteHostInputEditCommand(
            HostInputEditCommand::SelectAll);
        break;
    case kContextCutCommand:
        widgetEngine_->ExecuteHostInputEditCommand(
            HostInputEditCommand::Cut);
        break;
    case kContextCopyCommand:
        widgetEngine_->ExecuteHostInputEditCommand(
            HostInputEditCommand::Copy);
        break;
    case kContextPasteCommand:
        widgetEngine_->ExecuteHostInputEditCommand(
            HostInputEditCommand::Paste);
        break;
    default:
        break;
    }

    interactionPinnedWidgetId_ = previousPinnedWidgetId;
    UpdateHostInputImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

/**
 * @brief 处理鼠标右键释放事件（显示上下文菜单）
 * @param lp LPARAM（含鼠标坐标）
 */
void DesktopApp::OnRightButtonUp(LPARAM lp)
{
    if (renameEdit_ != nullptr) return;
    keyboardNavVisualFocus_ = false;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT screenPt = pt;
    ClientToScreen(hwnd_, &screenPt);

    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelAnimation_.IsInteractive())
    {
        const RECT panel = GetLuaWidgetPanelRect();
        if (PtInRect(&panel, pt))
        {
            const RECT content = GetLuaWidgetPanelContentRect();
            if (PtInRect(&content, pt))
            {
                const size_t widgetIndex = FindWidgetIndexById(
                    luaWidgetPanelRequest_.widgetId);
                if (widgetIndex < widgets_.size())
                {
                    const POINT localPoint{
                        pt.x - content.left,
                        pt.y - content.top };
                    if (!ShowHostInputContextMenu(
                            screenPt, widgets_[widgetIndex].id,
                            localPoint,
                            luaWidgetPanelRequest_.surface))
                    {
                        ShowWidgetContextMenu(screenPt, widgetIndex,
                            std::nullopt, localPoint,
                            luaWidgetPanelRequest_.surface);
                    }
                }
            }
            return;
        }
        if (luaWidgetPanelRequest_.modal)
            return;
    }

    if (DockContainer* dock = GetDockContainerAtPoint(pt))
    {
        if (DockEntryItem* dockItem = dock->EntryAtPoint(pt))
        {
            const size_t entryIndex = dockItem->GetEntryIndex();
            if (entryIndex < dockEntries_.size())
            {
                ClearSelection();
                dockItem->SetSelected(true);
                const RECT dockItemBounds = dock->GetElementVisualRect(
                    dockItem->GetBounds(), pt);
                if (dockItem->GetEntryType() == DockEntryType::DesktopItem)
                {
                    size_t itemIndex = FindItemIndexByKey(dockItem->GetReference());
                    if (itemIndex < items_.size())
                    {
                        items_[itemIndex].selected =
                            dockItem->IsSelected();
                        items_[itemIndex].bounds = dockItemBounds;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        if (IsProtectedDesktopIcon(items_[itemIndex]))
                            ShowShellContextMenu(
                                screenPt,
                                static_cast<int>(itemIndex),
                                false, dockItemBounds);
                        else
                            ShowItemContextMenu(
                                screenPt,
                                static_cast<int>(itemIndex),
                                false, false, dockItemBounds,
                                dockEntries_[entryIndex].
                                        keepOnDesktop
                                    ? std::optional<size_t>(
                                          entryIndex)
                                    : std::nullopt,
                                true);
                    }
                }
                else
                {
                    size_t widgetIndex = FindWidgetIndexById(dockItem->GetReference());
                    if (widgetIndex < widgets_.size())
                    {
                        widgets_[widgetIndex].selected =
                            dockItem->IsSelected();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        ShowWidgetContextMenu(
                            screenPt, widgetIndex,
                            dockItemBounds);
                    }
                }
                return;
            }
        }

        if (DockFrequentItem* frequentItem = dock->FrequentItemAtPoint(pt))
        {
            const size_t itemIndex = frequentItem->GetItemIndex();
            if (itemIndex < items_.size())
            {
                ClearSelection();
                frequentItem->SetSelected(true);
                items_[itemIndex].bounds = dock->GetElementVisualRect(
                    frequentItem->GetBounds(), pt);
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (IsProtectedDesktopIcon(items_[itemIndex]))
                    ShowShellContextMenu(
                        screenPt,
                        static_cast<int>(itemIndex),
                        false, dock->GetElementVisualRect(
                            frequentItem->GetBounds(), pt));
                else
                    ShowItemContextMenu(
                        screenPt,
                        static_cast<int>(itemIndex),
                        true, false,
                        dock->GetElementVisualRect(
                            frequentItem->GetBounds(), pt),
                        std::nullopt, true);
                return;
            }
        }

        if (DockRunningItem* runningItem =
                dock->RunningItemAtPoint(pt))
        {
            const size_t runningIndex =
                runningItem->GetRunningIndex();
            if (runningIndex <
                dockUnpinnedRunningApps_.size())
            {
                ClearSelection();
                DismissDockWindowPreviewUntilLeave();
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                ShowDockRunningAppContextMenu(
                    screenPt, runningIndex);
                return;
            }
        }

        if (dock->ContainsInteractivePoint(pt))
        {
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
            ShowDockContextMenu(screenPt);
            return;
        }
    }

    const size_t standaloneInputWidget =
        HitTestStandaloneWidgetIndex(pt);
    if (standaloneInputWidget < widgets_.size() &&
        widgets_[standaloneInputWidget].type ==
            DesktopWidgetType::LuaScript)
    {
        const RECT frame = GetStandaloneWidgetFrameRect(
            widgets_[standaloneInputWidget]);
        if (ShowHostInputContextMenu(
                screenPt, widgets_[standaloneInputWidget].id,
                POINT{ pt.x - frame.left, pt.y - frame.top }))
            return;
    }

    // A hover-only widget must remain visible while any context menu opened
    // from its frame, contents, tabs, or collection popup is active.  The
    // guard deliberately keeps the pin when the selected command starts an
    // inline rename editor; CommitRename releases it when editing ends.
    const bool popupOccludesPoint =
        IsPointOccludedByOpenPopup(pt);
    size_t contextWidgetIndex = static_cast<size_t>(-1);
    if (!dockFolderPopupOpen_ &&
        popupWidgetIndex_ < widgets_.size() &&
        popupOccludesPoint)
    {
        contextWidgetIndex = popupWidgetIndex_;
    }
    if (contextWidgetIndex >= widgets_.size())
    {
        for (auto it = containers_.rbegin();
            it != containers_.rend(); ++it)
        {
            auto* container =
                dynamic_cast<WidgetContainer*>(it->get());
            if (!container)
                continue;
            const RECT frame = container->GetFrameRect();
            if (IsRectEmptyRect(frame) || !PtInRect(&frame, pt))
                continue;
            DesktopWidget* data = container->GetWidgetData();
            if (!data)
                continue;
            for (size_t i = 0; i < widgets_.size(); ++i)
            {
                if (&widgets_[i] == data)
                {
                    contextWidgetIndex = i;
                    break;
                }
            }
            if (contextWidgetIndex < widgets_.size())
                break;
        }
    }
    if (contextWidgetIndex >= widgets_.size())
        contextWidgetIndex = HitTestStandaloneWidgetIndex(pt);

    struct ContextWidgetVisibilityGuard
    {
        std::wstring& pinnedId;
        HWND owner;
        HWND& renameEdit;
        HWND& luaInlineEdit;
        std::wstring previousId;
        bool active = false;

        ~ContextWidgetVisibilityGuard()
        {
            if (!active || renameEdit || luaInlineEdit)
                return;
            pinnedId = std::move(previousId);
            InvalidateRect(owner, nullptr, FALSE);
        }
    } visibilityGuard{
        interactionPinnedWidgetId_, hwnd_, renameEdit_, luaInlineEdit_,
        interactionPinnedWidgetId_
    };
    if (contextWidgetIndex < widgets_.size())
    {
        visibilityGuard.active = true;
        interactionPinnedWidgetId_ = widgets_[contextWidgetIndex].id;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (dockFolderPopupOpen_ &&
        popupOccludesPoint)
    {
        RECT popup = GetCollectionPopupRect(
            dockFolderPopupWidget_);
        if (PtInRect(&popup, pt))
        {
            RECT content =
                GetCollectionPopupContentRect(popup);
            for (size_t i = 0;
                 i < dockFolderPopupWidget_.
                    folderEntries.size(); ++i)
            {
                RECT itemRect =
                    GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top =
                    std::max(clipped.top, content.top);
                clipped.bottom =
                    std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top ||
                    !PtInRect(&clipped, pt))
                    continue;
                ClearSelection();
                if (!dockFolderPopupWidget_.
                        folderEntries[i].selected)
                {
                    for (auto& entry :
                         dockFolderPopupWidget_.
                            folderEntries)
                        entry.selected = false;
                    dockFolderPopupWidget_.
                        folderEntries[i].
                            selected = true;
                }
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                ShowDockFolderPopupContextMenu(
                    screenPt, i);
                return;
            }
            ShowDockFolderPopupContextMenu(
                screenPt);
            return;
        }
    }
    else if (popupWidgetIndex_ < widgets_.size() &&
        popupOccludesPoint)
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt))
        {
            std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
            RECT content = GetCollectionPopupContentRect(popup);
            for (size_t i = 0; i < popupKeys.size(); ++i)
            {
                RECT itemRect = GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top = std::max(clipped.top, content.top);
                clipped.bottom = std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;

                size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                if (itemIndex != static_cast<size_t>(-1))
                {
                    if (!items_[itemIndex].selected)
                        SelectOnly(static_cast<int>(itemIndex));
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (IsProtectedDesktopIcon(items_[itemIndex]))
                        ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
                    else
                        ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
                    return;
                }
            }
            // 弹窗背景（标题栏/内边距/item 间隙）的右键归属弹窗所属组件，
            // 不得穿透到被遮挡的下层元素。与 Dock 文件夹弹窗的背景菜单行为对齐。
            ShowWidgetContextMenu(
                screenPt, popupWidgetIndex_);
            return;
        }
    }

    // File-group source tabs own their context menu; do not let the
    // surrounding widget frame consume a tab right-click.
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(it->get()))
            continue;
        auto* group =
            dynamic_cast<FileGroup*>(it->get());
        if (!group) continue;
        const std::wstring childId =
            group->SourceIdAtPoint(pt);
        if (childId.empty()) continue;
        DesktopWidget* groupData = group->GetWidgetData();
        const size_t groupIndex = groupData
            ? FindWidgetIndexById(groupData->id)
            : static_cast<size_t>(-1);
        const size_t childIndex =
            FindWidgetIndexById(childId);
        if (groupIndex >= widgets_.size() ||
            childIndex >= widgets_.size())
            break;

        const auto childIds =
            group->GetVisibleSourceIds();
        const auto childIt = std::find(
            childIds.begin(), childIds.end(), childId);
        const size_t tabIndex =
            childIt == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), childIt));
        ClearSelection();
        widgets_[groupIndex].activeCategoryId = childId;
        widgets_[groupIndex].scrollOffset = 0;
        widgets_[childIndex].selected = true;
        group->InvalidateHostedView();
        group->EnsureSourceTabVisible(tabIndex);
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowFileGroupSourceTabContextMenu(
            screenPt, groupIndex, childId);
        return;
    }

    // Collection-group tabs own their context menu.
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(it->get()))
            continue;
        auto* group =
            dynamic_cast<CollectionGroup*>(it->get());
        if (!group) continue;
        const std::wstring collectionId =
            group->CategoryIdAtPoint(pt);
        if (collectionId.empty()) continue;

        DesktopWidget* groupData =
            group->GetWidgetData();
        size_t groupIndex =
            static_cast<size_t>(-1);
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (&widgets_[i] == groupData)
            {
                groupIndex = i;
                break;
            }
        }
        const size_t childIndex =
            FindWidgetIndexById(collectionId);
        if (groupIndex >= widgets_.size() ||
            childIndex >= widgets_.size())
            break;

        const auto& childIds =
            group->GetVisibleCollectionIds();
        auto childIt = std::find(
            childIds.begin(), childIds.end(),
            collectionId);
        const size_t tabIndex =
            childIt == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), childIt));
        ClearSelection();
        widgets_[groupIndex].activeCategoryId =
            collectionId;
        widgets_[groupIndex].scrollOffset = 0;
        widgets_[childIndex].selected = true;
        group->InvalidateFilterCache();
        group->EnsureTabVisible(tabIndex);
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowCollectionGroupTabContextMenu(
            screenPt, groupIndex, collectionId);
        return;
    }

    // A logical slot item owns an element-only host menu. Resolve it before
    // ordinary widget members and the surrounding Lua widget frame.
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ && !IsRetainedContainer(it->get()))
            continue;
        auto* logicalSlot =
            dynamic_cast<LuaLogicalSlotContainer*>(it->get());
        if (!logicalSlot) continue;
        const auto itemHit = logicalSlot->ItemAtPoint(pt);
        if (!itemHit) continue;
        if (snowdesktop::right_click_contract::ResolveSlotItemMenu(
                logicalSlot->GetSlotSurfaceKind(),
                snowdesktop::right_click_contract::
                    SlotItemKind::LogicalSlotItem,
                false) != snowdesktop::right_click_contract::
                    ContextMenuKind::LogicalSlotItem)
            break;
        const size_t widgetIndex =
            FindWidgetIndexById(logicalSlot->WidgetId());
        if (widgetIndex >= widgets_.size()) break;
        SelectWidgetOnly(widgetIndex);
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowLuaLogicalSlotItemContextMenu(screenPt,
            logicalSlot->WidgetId(), logicalSlot->SlotId(),
            itemHit->itemId,
            itemHit->kind == snowdesktop::widget_runtime::
                LogicalSlotKind::Collection,
            itemHit->index, itemHit->itemCount,
            itemHit->canRemove);
        return;
    }

    // Check widget member items first; otherwise the widget frame menu steals member right-clicks.
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(it->get()))
            continue;
        auto* wc = dynamic_cast<WidgetContainer*>(it->get());
        if (!wc) continue;

        WidgetHit wh = wc->HitTestWidget(pt);
        if (wh == WidgetHit::MoveHandle || wh == WidgetHit::ResizeHandle)
            continue;

        RECT bodyRect = wc->GetBodyRect();

        auto& slots = wc->GetSlots();
        for (auto& slot : slots)
        {
            if (!slot) continue;
            RECT bounds = slot->GetBounds();
            if (!PtInRect(&bounds, pt)) continue;
            if (!PtInRect(&bodyRect, pt)) continue;

            auto* desktopIcon =
                dynamic_cast<DesktopIcon*>(slot->GetItem());
            DesktopItem* item = desktopIcon
                ? desktopIcon->GetDesktopItem()
                : nullptr;
            auto* folderIcon =
                dynamic_cast<FolderEntryIcon*>(
                    slot->GetItem());
            const auto itemKind = desktopIcon
                ? snowdesktop::right_click_contract::
                    SlotItemKind::DesktopItem
                : folderIcon
                ? snowdesktop::right_click_contract::
                    SlotItemKind::FolderEntry
                : snowdesktop::right_click_contract::
                    SlotItemKind::None;
            const auto itemMenuKind =
                snowdesktop::right_click_contract::
                    ResolveSlotItemMenu(
                        wc->GetSlotSurfaceKind(),
                        itemKind,
                        desktopIcon && item &&
                            IsProtectedDesktopIcon(*item));
            if (!item)
            {
                FolderEntry* entry = folderIcon ? folderIcon->GetFolderEntry() : nullptr;
                if (!entry) break;
                if (itemMenuKind !=
                    snowdesktop::right_click_contract::
                        ContextMenuKind::FolderEntry)
                    break;

                auto* folderWidget = dynamic_cast<WidgetContainer*>(wc);
                DesktopWidget* data = nullptr;
                if (folderWidget)
                {
                    if (auto* fileGroup =
                            dynamic_cast<FileGroup*>(folderWidget))
                    {
                        // FileGroup slots clone hosted entries; resolve the
                        // real source widget before matching the member index.
                        auto* source =
                            fileGroup->GetSourceContainerForItem(
                                folderIcon);
                        data = source
                            ? source->GetWidgetData()
                            : nullptr;
                    }
                    else
                    {
                        data = folderWidget->GetWidgetData();
                    }
                }
                size_t widgetIndex = static_cast<size_t>(-1);
                size_t memberIndex = static_cast<size_t>(-1);
                for (size_t wi = 0; wi < widgets_.size(); ++wi)
                {
                    if (&widgets_[wi] != data) continue;
                    widgetIndex = wi;
                    for (size_t mi = 0; mi < widgets_[wi].folderEntries.size(); ++mi)
                    {
                        if (&widgets_[wi].folderEntries[mi] == entry)
                        {
                            memberIndex = mi;
                            break;
                        }
                    }
                    break;
                }
                if (widgetIndex == static_cast<size_t>(-1) ||
                    memberIndex == static_cast<size_t>(-1))
                    break;

                size_t parentWidgetIndex =
                    static_cast<size_t>(-1);
                if (folderWidget)
                {
                    DesktopWidget* parentData =
                        folderWidget->GetWidgetData();
                    for (size_t wi = 0;
                         wi < widgets_.size(); ++wi)
                    {
                        if (&widgets_[wi] == parentData)
                        {
                            parentWidgetIndex = wi;
                            break;
                        }
                    }
                }
                if (parentWidgetIndex >= widgets_.size())
                    break;

                // Keep the owning hover-only component selected while the
                // member menu is used, matching widget-level right-click.
                if (snowdesktop::right_click_contract::
                        ShouldPreserveSelectionOnRightClick(
                            entry->selected))
                {
                    ClearSelectionOutsideWidget(
                        parentWidgetIndex);
                }
                else
                {
                    ClearSelection();
                }
                widgets_[parentWidgetIndex].selected = true;
                if (!entry->selected)
                {
                    widgets_[widgetIndex].
                        folderEntries[memberIndex].
                            selected = true;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                ShowFolderEntryContextMenu(screenPt, widgetIndex, memberIndex);
                return;
            }

            size_t itemIndex = FindItemIndexByKey(item->layoutKey);
            if (itemIndex == static_cast<size_t>(-1)) break;
            if (itemMenuKind !=
                    snowdesktop::right_click_contract::
                        ContextMenuKind::DesktopItem &&
                itemMenuKind !=
                    snowdesktop::right_click_contract::
                        ContextMenuKind::ShellDesktopItem)
                break;

            if (!items_[itemIndex].selected)
                SelectOnly(static_cast<int>(itemIndex));
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (IsProtectedDesktopIcon(items_[itemIndex]))
                ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
            else
                ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
            return;
        }
    }

    // Check widget hit after member items.
    size_t hitWidget = static_cast<size_t>(-1);
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        if (desktopIconsHidden_ &&
            !widgets_[wi].keepWhenDesktopHidden)
            continue;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgets_[wi]) continue;
            if (wc->HitTestWidget(pt) != WidgetHit::None)
            {
                hitWidget = wi;
                break;
            }
        }
        if (hitWidget != static_cast<size_t>(-1)) break;
    }

    if (hitWidget != static_cast<size_t>(-1))
    {
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc ||
                wc->GetWidgetData() != &widgets_[hitWidget])
                continue;
            if (snowdesktop::right_click_contract::
                    ResolveContainerMenu(
                        wc->GetSlotSurfaceKind()) !=
                snowdesktop::right_click_contract::
                    ContextMenuKind::Widget)
            {
                hitWidget = static_cast<size_t>(-1);
                break;
            }

            // Select the widget and show its context menu
            SelectWidgetOnly(hitWidget);
            InvalidateRect(hwnd_, nullptr, FALSE);
            ShowWidgetContextMenu(screenPt, hitWidget);
            return;
        }
    }

    size_t hitStandaloneWidget = HitTestStandaloneWidgetIndex(pt);
    if (hitStandaloneWidget != static_cast<size_t>(-1))
    {
        SelectWidgetOnly(hitStandaloneWidget);
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowWidgetContextMenu(screenPt, hitStandaloneWidget);
        return;
    }

    if (IsPointOverWidgetChrome(pt))
    {
        ClearSelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowBackgroundContextMenu(screenPt);
        return;
    }

    int hit = HitTestItem(pt);
    if (hit >= 0 && !items_[hit].selected)
        SelectOnly(hit);
    else if (hit < 0)
        ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);

    const auto desktopItemMenuKind =
        snowdesktop::right_click_contract::
            ResolveSlotItemMenu(
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Desktop,
                snowdesktop::right_click_contract::
                    SlotItemKind::DesktopItem,
                hit >= 0 &&
                    IsProtectedDesktopIcon(items_[hit]));
    if (hit >= 0)
    {
        if (desktopItemMenuKind ==
            snowdesktop::right_click_contract::
                ContextMenuKind::ShellDesktopItem)
            ShowShellContextMenu(screenPt, hit);
        else if (desktopItemMenuKind ==
                 snowdesktop::right_click_contract::
                     ContextMenuKind::DesktopItem)
            ShowItemContextMenu(screenPt, hit);
    }
    else
        ShowBackgroundContextMenu(screenPt);
}

/**
 * @brief 处理定时器事件
 * @param timerId 定时器 ID
 */
