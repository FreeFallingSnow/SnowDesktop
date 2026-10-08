#include "app/app.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "layout/page_navigation_rules.h"
#include "ui/menu/right_click_contract.h"
#include "widgets/lua_logical_slot.h"
#include "dock/dock_drop_rules.h"

#include <utility>

// Page-navigation clicks and right-button context dispatch.

void DesktopApp::ClearPopupMouseDownItem()
{
    Item* const popupItem = popupMouseDownItem_.get();
    if (mouseDownHit_ == popupItem)
        mouseDownHit_ = nullptr;
    if (pendingCtrlToggleWidgetItem_ == popupItem)
        pendingCtrlToggleWidgetItem_ = nullptr;
    popupMouseDownItem_.reset();
}

bool DesktopApp::HandlePageNavClick(POINT point)
{
    if (gridPages_.empty()) return false;
    if (MaxPageOffset() <= 0) return false;   // 无溢出页时不处理

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();

    RECT prevEdge{};
    RECT nextEdge{};
    GetNavHotEdgeRects(prevEdge, nextEdge);
    const auto target = snowdesktop::page_navigation_rules::
        HitTestPointerTarget(point, prevEdge, nextEdge);
    const int delta = snowdesktop::page_navigation_rules::
        PointerTargetDirection(target);
    if (delta == 0) return false;

    const bool directionAvailable =
        (delta == -1 && hasPrev) ||
        (delta == 1 && hasNext);
    if (!directionAvailable)
        return false;

    int newOffset = NextNonEmptyOffset(pageOffset_, delta);
    if (newOffset == pageOffset_) return false;

    NavigatePageOffset(delta);
    RefreshPageNavHotEdgeHoverAt(point);
    // Page navigation never commits a dragged source's placement.
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
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
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING,
        kContextWidgetOpenComponentPanel,
        _LW("app.interact.open_component_panel"));

    SetMenuItemIcon(menu, kContextCutCommand, L"\uf0c4");
    SetMenuItemIcon(menu, kContextCopyCommand, L"\uf0c5");
    SetMenuItemIcon(menu, kContextPasteCommand, L"\uf0ea");
    SetMenuItemIcon(menu, kContextSelectAllCommand,
        snowdesktop::menu_fluent_glyphs::kSelectAll,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextWidgetOpenComponentPanel,
        snowdesktop::menu_fluent_glyphs::kChevronRight,
        MenuIconFont::FluentRegular);
    SetMenuItemQuickAction(menu, kContextCutCommand);
    SetMenuItemQuickAction(menu, kContextCopyCommand);
    SetMenuItemQuickAction(menu, kContextPasteCommand);
    SetMenuItemQuickAction(menu, kContextSelectAllCommand);

    RestoreInteractionInputFocus();
    const UINT command = ShowModernMenu(
        menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command == kContextWidgetOpenComponentPanel)
    {
        const size_t widgetIndex = FindWidgetIndexById(widgetId);
        if (widgetIndex < widgets_.size())
        {
            ShowWidgetContextMenu(screenPoint, widgetIndex,
                std::nullopt, localPoint, surface, true);
        }
        else
        {
            RestoreInteractionInputFocus();
        }
        interactionPinnedWidgetId_ = previousPinnedWidgetId;
        UpdateHostInputImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

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
 * @brief 记录鼠标右键按下所属的输入表面。
 * @param dockHost 按下发生于 Dock 时为对应 Host，否则为空。
 */
void DesktopApp::OnRightButtonDown(
    PersistentDockHost* dockHost, POINT point)
{
    // Nonactivating Dock Hosts do not dismiss another menu on their own.
    // Unwind that session before release starts an asynchronous running-app
    // lookup: its delivery is fenced while a menu is active, and otherwise
    // captures the old menu HWND as a foreground that cannot survive teardown.
    DismissActiveContextMenuForPopupTransition();
    POINT cursor{};
    if (quickNavigationOpen_ && GetCursorPos(&cursor))
    {
        const POINT point{cursor.x - virtualLeft_, cursor.y - virtualTop_};
        if (!PtInRect(&quickNavigationRect_, point)) CloseQuickNavigation(false);
    }
    CancelPopupHover(true);
    CancelRenameClick();
    if (renameController_.IsActive())
        CommitRename(false);
    rightButtonDownDockHost_ = dockHost;
    // Right-click menu interaction is never an edge-swipe gesture. Cancel an
    // already armed stroke synchronously instead of waiting for the sampler.
    floatingDockEdgeSwipeDetector_.SuppressUntilEdgeLeave();
    suppressRightButtonUp_ = false;
    // Tray and quick-navigation menus also use this preparation hook, without
    // a native item press. Only routed pointer coordinates may start a drag.
    if (point.x == LONG_MIN || point.y == LONG_MIN) return;
    if (largeIconGesture_ || mouseDown_ || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() || middleButtonWidgetMove_ ||
        widgetAction_ != WidgetAction::None ||
        IsPointInUsageGuide(point))
        return;
    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelAnimation_.IsInteractive())
    {
        const RECT panel = GetLuaWidgetPanelRect();
        if (luaWidgetPanelRequest_.modal || PtInRect(&panel, point))
            return;
    }

    ClearPopupMouseDownItem();
    Item* pressed = nullptr;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    const DesktopWidget* popupWidget = GetOpenPopupWidget();
    if (popupWidget && IsCollectionPopupInteractive() &&
        (!desktopIconsHidden_ || IsOpenPopupRetained()))
    {
        const RECT popup = GetCollectionPopupRect(*popupWidget);
        if (PtInRect(&popup, point))
        {
            const RECT content = GetCollectionPopupContentRect(popup);
            if (!PtInRect(&content, point)) return;
            for (size_t i = 0; i < GetPopupItemCount(*popupWidget); ++i)
            {
                if (!HitTestCollectionPopupItem(popup, i, point)) continue;
                if (dockFolderPopupOpen_)
                {
                    const size_t index = GetPopupFolderEntryIndex(*popupWidget, i);
                    if (index >= dockFolderPopupWidget_.folderEntries.size()) return;
                    popupMouseDownItem_ = std::make_unique<FolderEntryIcon>(
                        &dockFolderPopupWidget_.folderEntries[index],
                        dockFolderPopupContainer_.get(), this);
                }
                else
                {
                    const auto keys = GetPopupItemKeys(*popupWidget);
                    if (i >= keys.size()) return;
                    const size_t index = FindItemIndexByKey(keys[i]);
                    if (index >= items_.size()) return;
                    for (auto& container : containers_)
                    {
                        auto* widget = dynamic_cast<WidgetContainer*>(container.get());
                        if (!widget || widget->GetWidgetData() != popupWidget) continue;
                        popupMouseDownItem_ = std::make_unique<DesktopIcon>(
                            &items_[index], widget, this);
                        mouseDownWidgetIndex_ = popupWidgetIndex_;
                        break;
                    }
                }
                if (!popupMouseDownItem_) return;
                popupMouseDownItem_->SetBounds(GetCollectionPopupItemRect(popup, i));
                pressed = popupMouseDownItem_.get();
                break;
            }
            if (!pressed) return; // Popup gaps must not reach covered icons.
        }
    }
    if (!pressed)
    {
        if (auto* dock = GetDockContainerAtPoint(point))
        {
            pressed = dock->EntryAtPoint(point);
            if (!pressed) return; // Running-app and search buttons are actions.
        }
    }
    if (!pressed)
    {
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(it->get());
            if (!widget || widget->IsCollapsed() ||
                (desktopIconsHidden_ && !IsRetainedContainer(widget))) continue;
            const WidgetHit hit = widget->HitTestWidget(point);
            if (hit == WidgetHit::None) continue;
            if (auto* group = dynamic_cast<FileGroup*>(widget);
                group && hit == WidgetHit::SourceTab)
                pressed = group->GetSourceTabItemAtPoint(point);
            else if (auto* collectionGroup = dynamic_cast<CollectionGroup*>(widget);
                collectionGroup && hit == WidgetHit::CategoryTab)
                pressed = collectionGroup->GetTabItemAtPoint(point);
            else if (hit == WidgetHit::Content)
            {
                const RECT body = widget->GetBodyRect();
                for (auto& slot : widget->GetSlots())
                {
                    const RECT bounds = slot->GetBounds();
                    if (PtInRect(&body, point) && PtInRect(&bounds, point))
                    {
                        pressed = slot->GetItem();
                        break;
                    }
                }
            }
            if (widget->GetWidgetData())
                mouseDownWidgetIndex_ = FindWidgetIndexById(widget->GetWidgetData()->id);
            if (!pressed) return; // Chrome and empty content keep their context menu.
            break;
        }
    }
    if (!pressed && !IsPointOverWidgetChrome(point))
        pressed = HitTestIcon(point);
    if (!pressed || !pressed->GetContainer()) return;
    if (!pressed->IsSelected())
    {
        ClearSelection();
        if (dockFolderPopupOpen_ &&
            pressed->GetContainer() == dockFolderPopupContainer_.get())
            for (auto& entry : dockFolderPopupWidget_.folderEntries)
                entry.selected = false;
        pressed->SetSelected(true);
    }
    mouseDown_ = true;
    mouseDownHit_ = pressed;
    mouseDownPoint_ = point;
    rightButtonItemDrag_ = true;
    const HWND capture = dockHost ? dockHost->hwnd :
        handlingFloatingPopupInput_ ? floatingPopupHwnd_ : hwnd_;
    SetCapture(capture);
    SyncKeyboardNavFromSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int DesktopApp::PointerGestureVirtualKey() const
{
    return middleButtonWidgetMove_ ? VK_MBUTTON :
        rightButtonItemDrag_ ? VK_RBUTTON : VK_LBUTTON;
}

bool DesktopApp::ChooseRightDragDropAction(POINT point, DWORD& keyState,
    DWORD allowedEffects, bool externalSource)
{
    const bool luaFileDrop = HitTestLuaFileDropTarget(point) < widgets_.size();
    Container* target = dragSession_.TargetContainer();
    Slot* slot = dragSession_.TargetSlot();
    const HitRegion region = dragSession_.TargetRegion();
    const bool dockRemoval = !externalSource &&
        !GetDockDragOutRemovalHint(point).empty();
    if (!luaFileDrop && !dockRemoval &&
        (!target || region == HitRegion::None || region == HitRegion::Blocked))
        return false;

    if (region == HitRegion::Handoff && slot && slot->GetItem())
    {
        const std::wstring path = slot->GetItem()->GetPath();
        const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES :
            GetFileAttributesW(path.c_str());
        // Application and namespace targets own their native right-drag verbs.
        // Folder handoffs use our materialization pipeline and need a choice
        // before it starts. Folder shortcuts still delegate to the Shell.
        if ((attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (!externalSource && dragSession_.SourceList().FilePaths().empty())) &&
            !dragSession_.SourceList().hasWidgets)
            return true;
    }

    struct Choice { UINT command; DWORD effect; DWORD modifiers; const char* label; };
    const Choice choices[] = {
        {1, DROPEFFECT_MOVE, MK_SHIFT, "widget.base.move"},
        {2, DROPEFFECT_COPY, MK_CONTROL, "widget.base.copy_label"},
        {3, DROPEFFECT_LINK, MK_ALT, "widget.base.create_shortcut"},
    };
    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    bool hasChoice = false;
    for (const auto& choice : choices)
    {
        if ((allowedEffects & choice.effect) == 0) continue;
        bool accepted = false;
        if (luaFileDrop)
            accepted = choice.effect == DROPEFFECT_COPY;
        else if (dockRemoval)
            accepted = choice.effect == DROPEFFECT_MOVE;
        else if (dynamic_cast<DockContainer*>(target))
        {
            if (externalSource)
                accepted = choice.effect == snowdesktop::dock_drop_rules::
                    ChooseExternalMappingEffect(allowedEffects);
            else if (dynamic_cast<DockContainer*>(dragSession_.Source()))
                accepted = choice.effect == DROPEFFECT_MOVE;
            else if (dragSession_.SourceList().hasFolderEntries)
                accepted = choice.effect == DROPEFFECT_LINK;
            else
                accepted = choice.effect == DROPEFFECT_MOVE || choice.effect == DROPEFFECT_COPY;
        }
        else if (dragSession_.SourceList().hasWidgets)
            accepted = choice.effect == DROPEFFECT_MOVE;
        else if (externalSource)
            accepted = dragDropController_.ExternalSummary().fileCount > 0 ||
                choice.effect == DROPEFFECT_COPY;
        else if (region == HitRegion::Handoff)
            accepted = !dragSession_.SourceList().FilePaths().empty();
        else
        {
            const DropPreviewList preview = BuildDropPreviewList(
                dragSession_.SourceList(), target, slot, region,
                static_cast<int>(choice.modifiers), point);
            const DropAction action = DropActionFromMods(static_cast<int>(choice.modifiers));
            accepted = !preview.Empty() &&
                (preview.action == action ||
                    (action == DropAction::Move && preview.consumeDockSource));
        }
        if (accepted)
        {
            const char* label = externalSource && dynamic_cast<DockContainer*>(target)
                ? "widget.base.create_shortcut" : choice.label;
            AppendMenuW(menu, MF_STRING, choice.command, _LW(label));
            hasChoice = true;
        }
    }
    if (!hasChoice) { DestroyMenu(menu); return false; }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, _LW("app.settings.cancel"));

    // Retain source/target bindings while the menu pumps messages, with no
    // active pointer gesture that could update the destination or commit twice.
    dragSession_.DeactivateForDrop();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    ReleaseCapturePreservingPointerState();
    CommitDragVisualEndBeforeShellOperation();
    POINT screenPoint = point;
    ClientToScreen(hwnd_, &screenPoint);
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    for (const auto& choice : choices)
    {
        if (choice.command != command) continue;
        if (!dragSession_.ResumeAfterDropDecision()) return false;
        // The user already selected a verb. Use a left-button Shell handoff so
        // the native target does not show a second right-drag action menu.
        keyState = MK_LBUTTON | choice.modifiers;
        dragSession_.UpdateActionFromMods(static_cast<int>(choice.modifiers));
        return true;
    }
    return false;
}

/**
 * @brief 处理鼠标右键释放事件（显示上下文菜单）
 * @param lp LPARAM（含鼠标坐标）
 */
void DesktopApp::OnRightButtonUp(LPARAM lp)
{
    PersistentDockHost* const rightButtonPressDockHost =
        std::exchange(rightButtonDownDockHost_, nullptr);
    const bool consumeRelease = std::exchange(suppressRightButtonUp_, false);
    if (rightButtonItemDrag_)
    {
        if (dragSession_.IsActive())
        {
            OnLeftButtonUpAt(MK_RBUTTON, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return;
        }
        CancelPointerPressWithoutCaptureRelease();
        ReleaseCapturePreservingPointerState();
    }
    if (consumeRelease) return;
    // A right click cancels placement as one complete press/release gesture.
    // Preserve surface ownership on press, then consume release without a menu.
    if (largeIconGesture_) { CancelLargeIconGesture(); return; }
    if (renameController_.IsActive()) return;
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

    DockContainer* dock = GetDockContainerAtPoint(pt);
    PersistentDockHost* contextDockHost = dock
        ? FindPersistentDockHost(dock)
        : nullptr;
    // A menu can dismiss between button-down and button-up, allowing the up
    // message to land on an overlapping desktop-band DockHost. Only a press
    // that began on the matching Host may claim that Dock's context menu and
    // promote it to the floating band.
    const bool dockOwnsContextInput =
        dock &&
        snowdesktop::floating_dock_rules::
            ShouldDispatchDockContextMenu(
                contextDockHost && contextDockHost->active,
                contextDockHost &&
                    rightButtonPressDockHost ==
                        contextDockHost);
    if (dock && contextDockHost && contextDockHost->active &&
        !dockOwnsContextInput)
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock context summon ignored: right-button press did not begin on matching DockHost");
        // The release belongs to a press handled by another surface (most
        // commonly a menu that disappeared after button-down). Do not let it
        // fall through to an unrelated desktop or widget context menu.
        return;
    }
    if (dockOwnsContextInput)
    {
        EnsureFloatingDockVisibleForAssociatedSurface(
            screenPt);

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
                        ShowItemContextMenu(
                                screenPt,
                                static_cast<int>(itemIndex),
                                false, false, dockItemBounds,
                                dockEntries_[entryIndex].
                                        keepOnDesktop
                                    ? std::optional<size_t>(
                                          entryIndex)
                                    : std::nullopt,
                                true,
                                entryIndex);
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
        interactionPinnedWidgetId_, hwnd_, renameInputWindow_, luaInlineEdit_,
        interactionPinnedWidgetId_
    };
    if (contextWidgetIndex < widgets_.size())
    {
        visibilityGuard.active = true;
        interactionPinnedWidgetId_ = widgets_[contextWidgetIndex].id;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (popupOccludesPoint)
        popupFanActionFocused_ = false;

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
                 i < GetPopupItemCount(dockFolderPopupWidget_); ++i)
            {
                RECT itemRect =
                    GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top =
                    std::max(clipped.top, content.top);
                clipped.bottom =
                    std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top ||
                    !HitTestCollectionPopupItem(popup, i, pt))
                    continue;
                ClearSelection();
                if (!dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(dockFolderPopupWidget_, i)].selected)
                {
                    for (auto& entry :
                         dockFolderPopupWidget_.
                            folderEntries)
                        entry.selected = false;
                    dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(dockFolderPopupWidget_, i)].
                            selected = true;
                }
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                ShowDockFolderPopupContextMenu(
                    screenPt, GetPopupFolderEntryIndex(dockFolderPopupWidget_, i));
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
                if (clipped.bottom <= clipped.top || !HitTestCollectionPopupItem(popup, i, pt)) continue;

                size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                if (itemIndex != static_cast<size_t>(-1))
                {
                    if (!items_[itemIndex].selected)
                        SelectOnly(static_cast<int>(itemIndex));
                    InvalidateRect(hwnd_, nullptr, FALSE);
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
        if (!wc || wc->IsCollapsed()) continue;

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
