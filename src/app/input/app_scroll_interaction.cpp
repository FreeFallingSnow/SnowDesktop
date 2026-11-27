#include "ui/input/private_pointer.h"
#include "ui/input/touch_input.h"
#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "widget/runtime/widget_engine.h"
#include "common/utils.h"
#include "app/dock/dock_tray_popup_state.h"
#include "app/popup/categorized_popup_scope.h"
#include "navigation/quick_navigation_rules.h"
#include "widgets/single_cell_widget_rules.h"

// Mouse-wheel routing across active drag, popup and widget containers.

void DesktopApp::InvalidateCollectionPopupControls(bool immediate)
{
    if (!GetOpenPopupWidget()) return;
    if (folderPreview_ && folderPreview_->inlineBounds)
    {
        if (dcompDevice_ && dcompVisual_ && !desktopBackdropFullCollectionPending_)
            (void)QueueDesktopWidgetComposition(folderPreview_->hostWidgetId);
        else if (hwnd_)
            InvalidateRect(hwnd_, &*folderPreview_->inlineBounds, FALSE);
        return;
    }
    InvalidateCollectionPopupContent();
    if (IsCollectionPopupHostedByFloatingWindow())
        InvalidateFloatingPopupWindow(immediate);
    else
        InvalidateCollectionPopupAnimation(false);
}

void DesktopApp::InvalidateWidgetControls(const WidgetContainer& owner, bool immediate)
{
    const auto* data = owner.GetWidgetData();
    if (!data) return;
    const bool popupOwner = data == GetOpenPopupWidget();
    const auto index = FindWidgetIndexById(data->id);
    if (index < widgets_.size() && &widgets_[index] == data &&
        !IsGroupedWidget(*data) && !IsRectEmpty(&data->bounds) &&
        !snowdesktop::single_cell_widget::IsDesktopSingleCell(*data))
    {
        // A shared popup view can temporarily be laid out in popup coordinates.
        // Let the desktop restore its own geometry before it draws that owner.
        if (popupOwner || data->showOnHoverOnly || desktopBackdropFullCollectionPending_ ||
            !dcompDevice_ || !dcompVisual_)
        {
            RECT bounds = GetStandaloneWidgetFrameRect(*data);
            InflateRect(&bounds, 4, 4);
            if (hwnd_) InvalidateRect(hwnd_, &bounds, FALSE);
        }
        else
            (void)QueueDesktopWidgetComposition(data->id);
    }
    if (popupOwner) InvalidateCollectionPopupControls(immediate);
}

void DesktopApp::OnMouseWheel(WPARAM wp, LPARAM lp)
{
    CancelRenameClick();
    if (renameController_.BlocksScrolling())
        return;

    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd_, &pt);
    ActivateInlineFolderPreviewAtPoint(pt);
    if (dockTrayPopup_ && IsPointOccludedByOpenPopup(pt))
    {
        const RECT popup = GetCollectionPopupRect(dockTrayPopup_->widget);
        const int maximum = GetCollectionPopupMaxScrollOffset(dockTrayPopup_->widget, popup);
        popupScrollOffset_ = std::clamp(popupScrollOffset_ - GET_WHEEL_DELTA_WPARAM(wp) / 2, 0, maximum);
        dockTrayPopup_->keyboardNavigation = false;
        if (snowdesktop::touch_input::CanHover()) DockTrayPopupPointerMove(pt);
        else dockTrayPopup_->hovered = -1;
        InvalidateCollectionPopupContent();
        InvalidateFloatingPopupWindow(false);
        return;
    }
    int currentMods = 0;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) currentMods |= MK_CONTROL;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000)    currentMods |= MK_ALT;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000)   currentMods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(
            currentMods,
            dragDropController_.IsExternalDragActive()
                ? DropAction::Copy : DropAction::Move);

    const bool overStackIndicator = IsLuaStackIndicatorVisible() &&
        PtInRect(&luaStackIndicator_.bounds, pt);
    if (!overStackIndicator && !luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelAnimation_.IsInteractive())
    {
        const RECT content =
            GetLuaWidgetPanelContentRect();
        if (!PtInRect(&content, pt) &&
            PtInRect(&luaWidgetPanelRect_, pt))
        {
            if (widgetEngine_ && widgetEngine_->HandleHostUiPointer(
                    luaWidgetPanelRequest_.widgetId,
                    pt.x - content.left, pt.y - content.top,
                    GET_WHEEL_DELTA_WPARAM(wp), true,
                    luaWidgetPanelRequest_.surface, snowdesktop::touch_input::panHorizontal))
                (void)PresentDesktopForegroundComposition(
                    GetLuaWidgetPanelRect());
            return;
        }
        if (PtInRect(&content, pt))
        {
            const int delta =
                GET_WHEEL_DELTA_WPARAM(wp);
            const int localX =
                pt.x - content.left;
            const int localY =
                pt.y - content.top;
            const std::string& surface =
                luaWidgetPanelRequest_.surface;
            if (!widgetEngine_ ||
                !widgetEngine_->HandleHostUiPointer(
                    luaWidgetPanelRequest_.widgetId,
                    localX, localY, delta, true, surface, snowdesktop::touch_input::panHorizontal))
            {
                if (widgetEngine_ && !snowdesktop::touch_input::panHorizontal.value_or(false))
                {
                    const char* eventName = surface == "dialog"
                        ? "onDialogWheel"
                        : (surface == "popover"
                            ? "onPopoverWheel" : "onPanelWheel");
                    widgetEngine_->InvokeMouseEvent(
                        luaWidgetPanelRequest_.widgetId,
                        eventName,
                        localX, localY, 0, delta);
                }
            }
            UpdateHostInputImePosition();
            (void)PresentDesktopForegroundComposition(GetLuaWidgetPanelRect());
            return;
        }
        return;
    }

    if (quickNavigation_->open)
    {
        RECT overlay = quickNavigation_->rect;
        if (PtInRect(&overlay, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (quickNavigation_->initialJumpOpen)
            {
                InvalidateQuickNavigationWindow(true);
                return;
            }
            RECT tabs = GetQuickNavigationTabsRect(overlay);
            if (GetQuickNavigationEffectiveSearchText().empty() &&
                PtInRect(&tabs, pt))
            {
                if (pt.x >=
                    GetQuickNavigationTabsStart(
                        overlay))
                {
                    int maxTabScroll =
                        GetQuickNavigationMaxTabScrollOffset(
                            overlay);
                    snowdesktop::quick_navigation_rules::SetTabScrollOffset(
                        std::clamp(
                            quickNavigation_->tabScrollOffset -
                                delta / 2,
                            0, maxTabScroll),
                        quickNavigation_->tabScrollOffset,
                        quickNavigation_->tabDragIndex != static_cast<size_t>(-1),
                        quickNavigation_->tabDragStartPoint,
                        quickNavigation_->tabDragDeltaX);
                }
            }
            else
            {
                int maxScroll = GetQuickNavigationMaxScrollOffset(overlay);
                quickNavigation_->scrollOffset = std::clamp(quickNavigation_->scrollOffset - delta / 2, 0, maxScroll);
            }
            InvalidateQuickNavigationWindow(true);
            return;
        }
    }

    if (overStackIndicator)
    {
        (void)ScrollLuaWidgetStack(luaStackIndicator_.stackId, GET_WHEEL_DELTA_WPARAM(wp));
        return;
    }
    size_t luaWidget = HitTestStandaloneWidgetIndex(pt);
    if (luaWidget != static_cast<size_t>(-1) &&
        IsLuaDesktopSurface(widgets_[luaWidget]) &&
        HitTestStandaloneWidget(luaWidget, pt) == WidgetHit::Content &&
        widgetEngine_)
    {
        auto* luaContent = ResolveLuaWidgetContent(&widgets_[luaWidget]);
        if (!luaContent) return;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        RECT frame = GetStandaloneWidgetFrameRect(widgets_[luaWidget]);
        widgetEngine_->EnsureWidgetLoaded(luaContent->id, luaContent->packageId);
        int localX = pt.x - frame.left;
        int localY = pt.y - frame.top;
        if (widgets_[luaWidget].type == DesktopWidgetType::LuaStack)
        {
            if (!widgetEngine_->OwnsSurfaceWheel(luaContent->id, localX, localY, delta,
                    "desktop", snowdesktop::touch_input::panHorizontal))
            {
                if (!snowdesktop::touch_input::panHorizontal)
                    (void)ScrollLuaWidgetStack(widgets_[luaWidget].id, delta);
                return;
            }
            if (!snowdesktop::touch_input::panHorizontal)
                (void)luaStackWheel_.Consume(widgets_[luaWidget].id, delta, GetTickCount64(), true);
        }
        if (!widgetEngine_->HandleHostUiPointer(
                luaContent->id, localX, localY,
                delta, true, "desktop", snowdesktop::touch_input::panHorizontal))
        {
            if (!snowdesktop::touch_input::panHorizontal.value_or(false))
                widgetEngine_->InvokeMouseEvent(luaContent->id, "onWheel", localX, localY, 0, delta);
        }
        else
            UpdateHostInputImePosition();
        (void)QueueDesktopWidgetComposition(luaContent->id);
        return;
    }

    auto refreshDragAfterScroll = [&]() -> bool
    {
        if (!dragSession_.IsActive()) return false;
        RefreshDragTargetAt(pt, currentMods);
        InvalidateDragStaticScene();
        return true;
    };

    const bool insideFolderPreview = folderPreview_ && IsPointInsideOpenPopup(pt);
    if (DockContainer* dock = insideFolderPreview ? nullptr : GetDockContainerAtPoint(pt))
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (dock->ScrollByWheelDelta(pt, delta))
        {
            (void)refreshDragAfterScroll();
            RECT dirty = dock->GetVisualPanelBounds(pt);
            InflateRect(&dirty, 4, 4);
            (void)PresentDesktopForegroundComposition(dirty);
            return;
        }
    }

    if (DesktopWidget* popupWidget =
            GetOpenPopupWidget();
        IsCollectionPopupInteractive() &&
        (!desktopIconsHidden_ || IsOpenPopupRetained()) &&
        popupWidget)
    {
        RECT popup =
            GetCollectionPopupRect(*popupWidget);
        const auto presented = GetPresentedCollectionPopupGeometry();
        if (PtInRect(&presented.panel, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (auto* group = GetGroupPopupView())
            {
                const RECT frame = GetCategorizedPopupFrame(popup);
                group->SetPopupFrame(&frame);
                bool tabsChanged = false;
                if (group->TryScrollTabs(pt, delta, &tabsChanged))
                {
                    if (tabsChanged)
                    {
                        if (refreshDragAfterScroll()) (void)PresentDesktopForegroundComposition(presented.panel);
                        else InvalidateWidgetControls(*group, true);
                    }
                    return;
                }
                const int next = std::clamp(group->GetScrollOffset() - delta / 2, 0, group->GetMaxScrollOffset());
                if (!group->SetFolderSearchScrollOffset(next)) group->GetWidgetData()->scrollOffset = next;
                if (auto* files = dynamic_cast<FileGroup*>(group)) files->InvalidateHostedView();
                else group->InvalidateSlots();
                (void)refreshDragAfterScroll();
                (void)PresentDesktopForegroundComposition(presented.panel);
                return;
            }
            if (auto* view = GetCategorizedPopupView())
            {
                CategorizedPopupScope scope(view, GetCategorizedPopupFrame(popup));
                if (view->IsSearchActive() && view->SetFolderSearchScrollOffset(view->GetScrollOffset() - delta / 2))
                {
                    (void)refreshDragAfterScroll();
                    (void)PresentDesktopForegroundComposition(presented.panel);
                    return;
                }
                bool tabsChanged = false;
                if (view->TryScrollTabs(pt, delta, &tabsChanged))
                {
                    if (tabsChanged) InvalidateWidgetControls(*view, true);
                    return;
                }
            }
            if (UsesCollectionPopupFan(*popupWidget))
            {
                const double steps = static_cast<double>(delta) / WHEEL_DELTA;
                ScrollCollectionPopupFan(snowdesktop::touch_input::routingTouch ? steps : -steps);
                (void)refreshDragAfterScroll();
                return;
            }
            int maxScroll =
                GetCollectionPopupMaxScrollOffset(
                    *popupWidget, popup);
            popupScrollOffset_ = std::clamp(popupScrollOffset_ - delta / 2, 0, maxScroll);
            if (marqueeActive_ &&
                ((dockFolderPopupOpen_ &&
                  marqueeDockFolderPopup_) ||
                 (!dockFolderPopupOpen_ &&
                  marqueeWidgetIndex_ ==
                    popupWidgetIndex_)))
                UpdateMarqueeSelection(pt);
            (void)refreshDragAfterScroll();
            if (folderPreview_ && folderPreview_->inlineBounds)
                (void)QueueDesktopWidgetComposition(folderPreview_->hostWidgetId);
            else
                (void)PresentDesktopForegroundComposition(presented.panel);
            return;
        }
    }

    // Scroll widgets with overflow content
    for (auto& c : containers_)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(c.get()))
            continue;
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc || !wc->GetWidgetData()) continue;
        RECT frame = wc->GetFrameRect();
        if (!PtInRect(&frame, pt)) continue;
        if (wc->IsCollapsed()) return;

        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        DesktopWidget* data = wc->GetWidgetData();

        // File category tabs use horizontal wheel scrolling.
        if (data->type == DesktopWidgetType::FileCategories ||
            data->type == DesktopWidgetType::FolderMapping ||
            data->type == DesktopWidgetType::CollectionGroup ||
            data->type == DesktopWidgetType::FileGroup)
        {
            auto* categorized = dynamic_cast<ScrollingItemWidget*>(wc);
            bool tabsChanged = false;
            if (categorized && categorized->TryScrollTabs(pt, delta, &tabsChanged))
            {
                if (!tabsChanged) return;
                const bool dragRefreshed = refreshDragAfterScroll();
                DeferScrollLayoutSave();
                InvalidateWidgetControls(*categorized);
                if (dragRefreshed)
                {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    PresentDesktopPointerUpdate();
                }
                return;
            }
        }

        int maxScroll = wc->GetMaxScrollOffset();
        if (maxScroll <= 0) continue;

        const int nextScroll = std::clamp(wc->GetScrollOffset() - delta / 2, 0, maxScroll);
        if (nextScroll == wc->GetScrollOffset()) return;
        auto* search = dynamic_cast<ScrollingItemWidget*>(wc);
        if (!search || !search->SetFolderSearchScrollOffset(nextScroll)) data->scrollOffset = nextScroll;
        if (auto* group =
                dynamic_cast<FileGroup*>(wc))
            group->InvalidateHostedView();
        else
            wc->InvalidateSlots();
        if (marqueeActive_ && marqueeWidgetIndex_ < widgets_.size() &&
            &widgets_[marqueeWidgetIndex_] == data)
        {
            UpdateMarqueeSelection(pt);
        }
        if (mouseDownHit_ && mouseDownHit_->GetContainer() == wc)
            mouseDownHit_ = nullptr;
        const bool dragRefreshed = refreshDragAfterScroll();
        DeferScrollLayoutSave();
        (void)QueueDesktopWidgetComposition(data->id);
        if (dragRefreshed)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            PresentDesktopPointerUpdate();
        }
        return;
    }
}

// ── Lua inline editing ──────────────────────────────────────
