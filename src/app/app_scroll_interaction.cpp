#include "app.h"

// Mouse-wheel routing across active drag, popup and widget containers.

void DesktopApp::OnMouseWheel(WPARAM wp, LPARAM lp)
{
    if (renameController_.BlocksScrolling())
        return;

    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd_, &pt);
    int currentMods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) currentMods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    currentMods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   currentMods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(
            currentMods,
            dragDropController_.IsExternalDragActive()
                ? DropAction::Copy : DropAction::Move);

    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelAnimation_.IsInteractive())
    {
        const RECT content =
            GetLuaWidgetPanelContentRect();
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
                    localX, localY, delta, true, surface))
            {
                if (widgetEngine_)
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
            (void)PresentDesktopForegroundComposition(content);
            return;
        }
        return;
    }

    if (quickNavigationOpen_)
    {
        RECT overlay = quickNavigationRect_;
        if (PtInRect(&overlay, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (quickNavigationInitialJumpOpen_)
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
                    quickNavigationTabScrollOffset_ =
                        std::clamp(
                            quickNavigationTabScrollOffset_ -
                                delta / 2,
                            0, maxTabScroll);
                }
            }
            else
            {
                int maxScroll = GetQuickNavigationMaxScrollOffset(overlay);
                quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_ - delta / 2, 0, maxScroll);
            }
            InvalidateQuickNavigationWindow(true);
            return;
        }
    }

    size_t luaWidget = HitTestStandaloneWidgetIndex(pt);
    if (luaWidget != static_cast<size_t>(-1) &&
        widgets_[luaWidget].type == DesktopWidgetType::LuaScript &&
        HitTestStandaloneWidget(luaWidget, pt) == WidgetHit::Content &&
        widgetEngine_)
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        RECT frame = GetStandaloneWidgetFrameRect(widgets_[luaWidget]);
        widgetEngine_->EnsureWidgetLoaded(widgets_[luaWidget].id, widgets_[luaWidget].packageId);
        int localX = pt.x - frame.left;
        int localY = pt.y - frame.top;
        if (!widgetEngine_->HandleHostUiPointer(
                widgets_[luaWidget].id, localX, localY,
                delta, true))
            widgetEngine_->InvokeMouseEvent(widgets_[luaWidget].id, "onWheel",
                localX, localY, 0, delta);
        else
            UpdateHostInputImePosition();
        (void)QueueDesktopWidgetComposition(widgets_[luaWidget].id);
        return;
    }

    auto refreshDragAfterScroll = [&]() -> bool
    {
        if (!dragSession_.IsActive()) return false;
        RefreshDragTargetAt(pt, currentMods);
        InvalidateDragStaticScene();
        return true;
    };

    if (DockContainer* dock = GetDockContainerAtPoint(pt))
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (dock->ScrollByWheelDelta(pt, delta))
        {
            (void)refreshDragAfterScroll();
            RECT dirty = dock->GetVisualPanelBounds(pt);
            const RECT title = dock->GetHoveredTitleBounds(pt);
            if (!IsRectEmptyRect(title))
                UnionRect(&dirty, &dirty, &title);
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
        if (PtInRect(&popup, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
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
            (void)PresentDesktopForegroundComposition(popup);
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

        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        DesktopWidget* data = wc->GetWidgetData();

        // File category tabs use horizontal wheel scrolling.
        if (data->type == DesktopWidgetType::FileCategories ||
            data->type == DesktopWidgetType::FolderMapping ||
            data->type == DesktopWidgetType::CollectionGroup ||
            data->type == DesktopWidgetType::FileGroup)
        {
            auto* categorized = dynamic_cast<ScrollingItemWidget*>(wc);
            if (categorized && categorized->TryScrollTabs(pt, delta))
            {
                const bool dragRefreshed = refreshDragAfterScroll();
                SaveLayoutSlots();
                (void)QueueDesktopWidgetComposition(data->id);
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

        data->scrollOffset = std::clamp(data->scrollOffset - delta / 2, 0, maxScroll);
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
        SaveLayoutSlots();
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
