#include "app/app.h"
#include "app/dock/persistent_dock_host.h"
#include "app/navigation/quick_navigation_state.h"
#include "app/popup/categorized_popup_scope.h"
#include "dock/dock_settings_rules.h"
#include "dock/dock_window_preview.h"
#include "layout/collection_popup_layout.h"
#include "layout/floating_popup_rules.h"
#include "layout/folder_preview_header_layout.h"
#include "system/status_bar/status_bar.h"
#include "ui/input/private_pointer.h"
#include "ui/input/touch_input.h"
#include "ui/input/touch_simulator.h"
#include "widget/runtime/widget_engine.h"

#include <array>

namespace
{
namespace touch = snowdesktop::touch_input;

bool IsVertical(DockPosition position)
{
    return position == DockPosition::Left || position == DockPosition::Right;
}

int InwardTravel(DockPosition position, POINT start, POINT end)
{
    switch (position)
    {
    case DockPosition::Left: return end.x - start.x;
    case DockPosition::Right: return start.x - end.x;
    case DockPosition::Top: return end.y - start.y;
    case DockPosition::Bottom: return start.y - end.y;
    }
    return 0;
}

bool IsOutwardSwipe(DockPosition position, POINT start, POINT end, int threshold)
{
    const int alongBar = IsVertical(position) ? std::abs(end.y - start.y) :
        std::abs(end.x - start.x);
    const int inward = InwardTravel(position, start, end);
    return inward >= threshold && inward >= alongBar;
}

bool IsNewContact(UINT message, WPARAM pointer)
{
    return message == WM_POINTERDOWN && IS_POINTER_PRIMARY_WPARAM(pointer) &&
        touch::IsTouchPointer(pointer);
}

struct DockTouch
{
    HWND window = nullptr;
    touch::Contact contact;
    touch::GestureAction action = touch::GestureAction::Pending;
    bool scrollable = false;
    bool draggable = false;
    bool preview = false;
};

struct ScrollTarget
{
    bool horizontal = false;
    bool vertical = false;
    bool draggable = false;
    bool doubleTap = false;
    bool lua = false;
    bool moveWidget = false;
    bool directWidgetDrag = false;
    std::wstring stackId;
};

struct ScrollTouch
{
    HWND window = nullptr;
    touch::Contact contact;
    ScrollTarget target;
    touch::GestureAction action = touch::GestureAction::Pending;
    bool horizontal = false;
};

struct EdgeTouch
{
    HWND window = nullptr;
    touch::Contact contact;
};

struct TouchEdgeWindow
{
    DesktopApp* app = nullptr;
    HMONITOR monitor = nullptr;
    DockPosition edge = DockPosition::Bottom;
    touch::Contact contact;
};

constexpr wchar_t kTouchEdgeWindowClass[] = L"SnowDesktop.TouchEdge";

RECT TouchEdgeBounds(RECT bounds, DockPosition side, UINT dpi)
{
    constexpr int bandDip = 8;
    const int band = touch::Dips(bandDip, dpi);
    const int corner = touch::Dips(20, dpi);
    if (IsVertical(side))
    {
        bounds.top += corner;
        bounds.bottom -= corner;
        if (side == DockPosition::Left) bounds.right = bounds.left + band;
        else bounds.left = bounds.right - band;
    }
    else
    {
        bounds.left += corner;
        bounds.right -= corner;
        if (side == DockPosition::Top) bounds.bottom = bounds.top + band;
        else bounds.top = bounds.bottom - band;
    }
    return bounds;
}
}

void DesktopApp::ObserveDesktopTouch(HWND window, UINT message, WPARAM pointer, LPARAM position)
{
    if (snowdesktop::private_pointer::Active()) return;
    const bool touched = touch::Observe(message, pointer);
    static thread_local bool visualTouchMode = false;
    if (visualTouchMode != touch::touchMode)
    {
        visualTouchMode = touch::touchMode;
        if (!visualTouchMode && widgetEngine_) widgetEngine_->ClearDesktopTouchHoverUnless();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    const bool touchCaptureEnded = message == WM_POINTERCAPTURECHANGED && !touch::CanHover();
    if ((!touched && !touchCaptureEnded) || touch::routingTouch) return;
    if (message != WM_POINTERDOWN && message != WM_POINTERUP && message != WM_LBUTTONDOWN &&
        message != WM_LBUTTONUP && message != WM_POINTERCAPTURECHANGED) return;

    OnMouseLeave();
    UpdateLargeIconHover();

    if (message != WM_POINTERDOWN || !IS_POINTER_PRIMARY_WPARAM(pointer)) return;
    const POINT screen = touch::ScreenPoint(position);
    if (widgetEngine_)
    {
        POINT desktop = screen;
        const DesktopWidget* content = nullptr;
        if (window == hwnd_ && ScreenToClient(hwnd_, &desktop) && !GetDockContainerAtPoint(desktop) &&
            !IsPointOccludedByOpenPopup(desktop) && !IsPointInUsageGuide(desktop))
        {
            const bool onIndicator = IsLuaStackIndicatorVisible() && PtInRect(&luaStackIndicator_.bounds, desktop);
            const size_t index = onIndicator ? FindWidgetIndexById(luaStackIndicator_.stackId) :
                HitTestStandaloneWidgetIndex(desktop);
            if (index < widgets_.size()) content = ResolveLuaWidgetContent(&widgets_[index]);
        }
        if (widgetEngine_->ClearDesktopTouchHoverUnless(content ? std::wstring_view(content->id) : std::wstring_view{}) &&
            ClearSelection()) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (dockWindowPreview_ && dockWindowPreview_->TouchOpened() &&
        !dockWindowPreview_->ContainsInteractionPoint(screen) &&
        !PtInRect(&dockWindowPreviewAnchorScreen_, screen)) HideDockWindowPreview();
    if (window != floatingPopupHwnd_ && floatingPopupHwnd_ && IsWindowVisible(floatingPopupHwnd_))
        HandleFloatingPopupExternalPointerDown(floatingPopupMouseHookGeneration_,
            snowdesktop::floating_popup_rules::PackScreenPoint(screen));
}

bool DesktopApp::HandleDockTouch(HWND window, UINT message, WPARAM pointer, LPARAM position)
{
    if (!touch::IsPointerMessage(message) || snowdesktop::private_pointer::Active()) return false;
    using Action = touch::GestureAction;
    static thread_local DockTouch gesture;
    static thread_local touch::TapSequence taps;
    if (gesture.window && !IsWindow(gesture.window)) gesture = {};
    const POINT screen = touch::ScreenPoint(position);
    if (IsNewContact(message, pointer))
    {
        if (gesture.contact.Active() || !hwnd_ || HasActiveContextMenuSession()) return false;
        POINT desktop = screen;
        if (!ScreenToClient(hwnd_, &desktop)) return false;
        DockContainer* dock = GetDockContainerAtPoint(desktop);
        if (!dock || IsPointOccludedByOpenPopup(desktop) ||
            dock->IsWindowsButtonPoint(desktop) ||
            (!dockSettings_.freeItemOrder && dock->IsSearchPoint(desktop)))
        {
            taps.Reset();
            return false;
        }
        const bool draggable = dock->EntryAtPoint(desktop) || dock->RunningItemAtPoint(desktop) ||
            dock->FrequentItemAtPoint(desktop);
        const bool scrollable = dock->CanScrollAt(desktop);
        if (!draggable && !scrollable) return false;
        gesture = {};
        gesture.window = window;
        gesture.draggable = draggable;
        gesture.scrollable = scrollable;
        DockWindowPreviewTarget target;
        gesture.preview = dockSettings_.showWindowPreviews && ResolveDockWindowPreviewTarget(desktop, target);
        gesture.contact.Begin(pointer, screen);
        return true;
    }
    if (gesture.window != window || !gesture.contact.Owns(pointer)) return false;
    if (message == WM_POINTERCAPTURECHANGED || touch::IsCanceled(pointer))
    {
        const bool dragging = gesture.action == Action::Drag;
        gesture = {};
        taps.Reset();
        if (dragging) SendMessageW(window, WM_CANCELMODE, 0, 0);
        return true;
    }
    if (message == WM_POINTERUPDATE || message == WM_POINTERUP)
    {
        auto& contact = gesture.contact;
        const int slop = touch::Dips(12, GetDpiForWindow(window));
        const int dx = screen.x - contact.start.x;
        const int dy = screen.y - contact.start.y;
        const bool vertical = IsVertical(dockSettings_.position);
        if (gesture.action == Action::Pending && contact.Moved(screen, slop))
        {
            taps.Reset();
            const bool openPreview = gesture.preview &&
                IsOutwardSwipe(dockSettings_.position, contact.start, screen, slop);
            const bool scrollAlongDock = gesture.scrollable &&
                (vertical ? std::abs(dy) >= std::abs(dx) : std::abs(dx) >= std::abs(dy));
            if (gesture.draggable && (contact.Held() || (!openPreview && !scrollAlongDock)))
            {
                gesture.action = Action::Drag;
                touch::RouteMouse(window, WM_LBUTTONDOWN, contact.start, MK_LBUTTON);
            }
            else if (openPreview)
                gesture.action = Action::Swipe;
            else if (scrollAlongDock)
                gesture.action = Action::Pan;
            else gesture.action = Action::Consumed;
        }
        if (gesture.action == Action::Pan)
        {
            const int pixels = vertical ? screen.y - contact.last.y : screen.x - contact.last.x;
            const touch::RoutedTouch scope(contact.start);
            if (pixels) OnMouseWheel(touch::PanWheel(pixels), MAKELPARAM(contact.start.x, contact.start.y));
        }
        contact.last = screen;
        if (gesture.action == Action::Drag) touch::RouteMouse(window, WM_MOUSEMOVE, screen, MK_LBUTTON);
        if (message != WM_POINTERUP) return true;

        const DockTouch released = gesture;
        gesture = {};
        if (released.action == Action::Drag) touch::RouteMouse(window, WM_LBUTTONUP, screen);
        else if (released.action == Action::Swipe)
        {
            POINT origin = released.contact.start;
            if (IsOutwardSwipe(dockSettings_.position, released.contact.start, screen, slop) &&
                ScreenToClient(hwnd_, &origin)) (void)TryOpenDockWindowPicker(origin, true);
        }
        else if (released.action == Action::Pending)
        {
            if (released.contact.Held())
            {
                taps.Reset();
                touch::RouteClick(window, released.contact.start, true);
            }
            else if (taps.Complete(window, released.contact.start))
            {
                touch::RouteMouse(window, WM_LBUTTONDBLCLK, released.contact.start, MK_LBUTTON);
                if (IsWindow(window)) touch::RouteMouse(window, WM_LBUTTONUP, screen);
            }
            else touch::RouteClick(window, released.contact.start, false);
        }
    }
    return true;
}

bool DesktopApp::HandleTouchScroll(HWND window, UINT message, WPARAM pointer, LPARAM position)
{
    if (snowdesktop::private_pointer::Active()) return false;
    using Action = touch::GestureAction;
    static thread_local ScrollTouch gesture;
    static thread_local touch::TapSequence taps;
    if (gesture.window && !IsWindow(gesture.window)) gesture = {};
    if (gesture.window == window && (message == WM_CANCELMODE || message == WM_NCDESTROY))
        gesture = {};
    if (!touch::IsPointerMessage(message)) return false;
    const POINT screen = touch::ScreenPoint(position);
    if (IsNewContact(message, pointer))
    {
        if (gesture.contact.Active() || !hwnd_ || HasActiveContextMenuSession() ||
            quickNavigation_->open || dragSession_.HasContext()) return false;
        POINT desktop = screen;
        if (!ScreenToClient(hwnd_, &desktop)) return false;
        if (window == hwnd_) ActivateInlineFolderPreviewAtPoint(desktop);
        ScrollTarget target;
        const auto nativeTarget = [&](WidgetContainer& view, bool desktopOwner = false) {
            const WidgetHit hit = view.HitTestWidget(desktop);
            target.directWidgetDrag = desktopOwner &&
                (hit == WidgetHit::MoveHandle || hit == WidgetHit::IconTitle);
            target.moveWidget = desktopOwner && hit == WidgetHit::CollectionOpenBtn;
            if (view.IsCollapsed() || (view.GetWidgetData()->showOnHoverOnly &&
                !view.GetWidgetData()->selected)) return;
            auto* scrolling = dynamic_cast<ScrollingItemWidget*>(&view);
            const bool onTab = hit == WidgetHit::CategoryTab || hit == WidgetHit::SourceTab;
            target.horizontal = scrolling && scrolling->TryScrollTabs(desktop, 0);
            if (onTab) target.draggable = true;
            if (hit != WidgetHit::Content || target.horizontal) return;
            const RECT viewport = view.GetContentViewportRect();
            if (!PtInRect(&viewport, desktop)) return;
            target.vertical = view.GetMaxScrollOffset() > 0;
            const auto& slots = view.GetSlots();
            target.draggable = std::any_of(slots.begin(), slots.end(), [&](const auto& slot) {
                const RECT bounds = slot->GetBounds();
                return slot->GetItem() && PtInRect(&bounds, desktop);
            });
            target.doubleTap = target.draggable;
            target.moveWidget = desktopOwner && !target.draggable;
        };
        const auto luaTarget = [&](const std::wstring& id, RECT bounds, std::string_view surface) {
            if (!widgetEngine_) return false;
            const int x = desktop.x - bounds.left;
            const int y = desktop.y - bounds.top;
            if (widgetEngine_->IsSurfaceSliderAt(id, x, y, surface) ||
                widgetEngine_->IsHostInputAt(id, x, y, surface)) return false;
            target.lua = true;
            target.horizontal = widgetEngine_->OwnsSurfaceWheel(id, x, y, 0, surface, true);
            target.vertical = widgetEngine_->OwnsSurfaceWheel(id, x, y, 0, surface, false);
            target.draggable = widgetEngine_->OwnsSurfaceDrag(id, x, y, surface);
            return true;
        };
        if (luaWidgetPanelAnimation_.IsInteractive() && !luaWidgetPanelRequest_.widgetId.empty())
        {
            const RECT content = GetLuaWidgetPanelContentRect();
            if (PtInRect(&content, desktop))
                luaTarget(luaWidgetPanelRequest_.widgetId, content, luaWidgetPanelRequest_.surface);
        }
        else if (auto* popup = GetOpenPopupWidget(); popup && IsPointInsideOpenPopup(desktop) &&
            IsCollectionPopupInteractive())
        {
            const RECT bounds = GetCollectionPopupRect(*popup);
            auto* group = GetGroupPopupView();
            auto* categorized = group ? group : GetCategorizedPopupView();
            CategorizedPopupScope scope(categorized, GetCategorizedPopupFrame(bounds));
            if (categorized) nativeTarget(*categorized);
            const RECT content = GetCollectionPopupContentRect(bounds);
            if (folderPreview_ && folderPreview_->inlineBounds &&
                PtInRect(&folderPreview_->inlineTitleBar, desktop))
            {
                const auto* owner = GetFolderPreviewComponent();
                for (const auto& container : containers_)
                    if (const auto* view = dynamic_cast<const WidgetContainer*>(container.get());
                        view && owner && view->GetWidgetData() == owner &&
                        view->HitTestWidget(desktop) == WidgetHit::MoveHandle)
                    {
                        const auto row = GetFolderPreviewNavigationRow(bounds);
                        target.directWidgetDrag = !PtInRect(&folderPreview_->inlineTitle, desktop) &&
                            !PtInRect(&folderPreview_->inlineExplorerButton, desktop) &&
                            !PtInRect(&row.back, desktop) && !PtInRect(&row.home, desktop) &&
                            !PtInRect(&row.searchButton, desktop) && !PtInRect(&row.breadcrumb, desktop);
                        break;
                    }
            }
            const bool fan = UsesCollectionPopupFan(*popup);
            if (!target.horizontal && (fan || PtInRect(&content, desktop)))
            {
                const bool popupScrolls = fan
                    ? snowdesktop::collection_popup_layout::FanMaximumScroll(
                        GetPopupItemCount(*popup), GetCollectionPopupFanVisibleCount(bounds)) > 0
                    : GetCollectionPopupMaxScrollOffset(*popup, bounds) > 0;
                target.vertical |= popupScrolls;
                if (!group && !target.draggable && !dockTrayPopup_)
                {
                    const size_t count = GetPopupItemCount(*popup);
                    for (size_t index = 0; index < count; ++index)
                    {
                        if (!HitTestCollectionPopupItem(bounds, index, desktop)) continue;
                        target.draggable = target.doubleTap = true;
                        break;
                    }
                }
                if (folderPreview_ && folderPreview_->inlineBounds && !target.draggable &&
                    PtInRect(&content, desktop))
                    target.moveWidget = true;
            }
        }
        else if (window == hwnd_ && !IsPointInUsageGuide(desktop))
        {
            const bool onIndicator = IsLuaStackIndicatorVisible() && PtInRect(&luaStackIndicator_.bounds, desktop);
            const size_t index = onIndicator ? FindWidgetIndexById(luaStackIndicator_.stackId) :
                HitTestStandaloneWidgetIndex(desktop);
            if (index < widgets_.size())
            {
                auto& owner = widgets_[index];
                const auto hit = onIndicator ? WidgetHit::None : HitTestStandaloneWidget(index, desktop);
                if (onIndicator)
                    target.stackId = owner.id;
                else if (hit == WidgetHit::MoveHandle)
                    target.directWidgetDrag = true;
                else if (auto* content = ResolveLuaWidgetContent(&owner);
                    hit == WidgetHit::Content && content && widgetEngine_)
                {
                    widgetEngine_->EnsureWidgetLoaded(content->id, content->packageId);
                    if (luaTarget(content->id, GetStandaloneWidgetFrameRect(owner), "desktop"))
                    {
                        target.moveWidget = !target.draggable;
                        if (owner.type == DesktopWidgetType::LuaStack && owner.childWidgetIds.size() > 1)
                            target.stackId = owner.id;
                    }
                }
            }
            else
            {
                for (auto entry = containers_.rbegin(); entry != containers_.rend(); ++entry)
                {
                    auto* view = dynamic_cast<WidgetContainer*>(entry->get());
                    if (!view || !view->GetWidgetData() ||
                        (desktopIconsHidden_ && !IsRetainedContainer(view)) ||
                        view->HitTestWidget(desktop) == WidgetHit::None) continue;
                    nativeTarget(*view, true);
                    break;
                }
            }
        }
        if (!target.horizontal && !target.vertical && !target.draggable &&
            !target.directWidgetDrag && !target.moveWidget && target.stackId.empty())
        {
            taps.Reset();
            return false;
        }
        gesture = {};
        gesture.window = window;
        gesture.target = std::move(target);
        gesture.contact.Begin(pointer, screen);
        return true;
    }
    if (gesture.window != window || !gesture.contact.Owns(pointer)) return false;
    if (message == WM_POINTERCAPTURECHANGED || touch::IsCanceled(pointer))
    {
        const bool dragging = gesture.action == Action::Drag;
        gesture = {};
        taps.Reset();
        if (dragging) SendMessageW(window, WM_CANCELMODE, 0, 0);
        return true;
    }
    if (message != WM_POINTERUPDATE && message != WM_POINTERUP) return true;

    auto& contact = gesture.contact;
    const int slop = touch::Dips(8, GetDpiForWindow(window));
    const int dx = screen.x - contact.start.x;
    const int dy = screen.y - contact.start.y;
    if (gesture.action == Action::Pending && contact.Moved(screen, slop))
    {
        taps.Reset();
        gesture.horizontal = std::abs(dx) > std::abs(dy);
        const bool ownsScroll = gesture.horizontal ? gesture.target.horizontal : gesture.target.vertical;
        const bool changesPage = !ownsScroll && !gesture.target.stackId.empty();
        const bool dragItem = gesture.target.draggable &&
            (contact.Held() || (!ownsScroll && !changesPage));
        if (dragItem || gesture.target.directWidgetDrag || (gesture.target.moveWidget && contact.Held()))
        {
            gesture.action = Action::Drag;
            if (gesture.target.directWidgetDrag) gesture.target.moveWidget = false;
            if (gesture.target.moveWidget && widgetEngine_) widgetEngine_->ClearDesktopTouchHoverUnless();
            touch::RouteMouse(window, gesture.target.moveWidget ? WM_MBUTTONDOWN : WM_LBUTTONDOWN,
                contact.start, gesture.target.moveWidget ? MK_MBUTTON : MK_LBUTTON);
        }
        else if (changesPage)
            gesture.action = Action::Swipe;
        else if (ownsScroll)
            gesture.action = Action::Pan;
        else gesture.action = Action::Consumed;
    }
    if (gesture.action == Action::Pan)
    {
        const int pixels = gesture.horizontal ? screen.x - contact.last.x : screen.y - contact.last.y;
        if (pixels)
        {
            const touch::RoutedTouch scope(contact.start, 0, gesture.target.lua
                ? std::optional<bool>(gesture.horizontal) : std::nullopt);
            OnMouseWheel(touch::PanWheel(pixels), MAKELPARAM(contact.start.x, contact.start.y));
        }
    }
    contact.last = screen;
    if (gesture.action == Action::Drag) touch::RouteMouse(window, WM_MOUSEMOVE, screen,
        gesture.target.moveWidget ? MK_MBUTTON : MK_LBUTTON);
    if (message != WM_POINTERUP) return true;

    ScrollTouch released = std::move(gesture);
    gesture = {};
    if (released.action == Action::Drag)
        touch::RouteMouse(window, released.target.moveWidget ? WM_MBUTTONUP : WM_LBUTTONUP, screen);
    else if (released.action == Action::Swipe)
    {
        const int travel = released.horizontal ? dx : dy;
        if (std::abs(travel) >= touch::Dips(28, GetDpiForWindow(window)) &&
            StepLuaWidgetStack(released.target.stackId, travel < 0 ? 1 : -1))
        {
            const auto index = FindWidgetIndexById(released.target.stackId);
            if (index < widgets_.size())
            {
                const auto ownerId = widgets_[index].id;
                SelectWidgetOnly(index);
                POINT desktop = released.contact.start;
                const auto current = FindWidgetIndexById(ownerId);
                if (current < widgets_.size() && widgetEngine_ && ScreenToClient(hwnd_, &desktop))
                {
                    const auto* content = ResolveLuaWidgetContent(&widgets_[current]);
                    const RECT frame = GetStandaloneWidgetFrameRect(widgets_[current]);
                    if (content) widgetEngine_->SetDesktopTouchHover(content->id, desktop.x - frame.left, desktop.y - frame.top);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
    }
    else if (released.action == Action::Pending)
    {
        if (released.contact.Held())
        {
            taps.Reset();
            touch::RouteClick(window, released.contact.start, true);
        }
        else if (released.target.doubleTap && taps.Complete(window, released.contact.start))
        {
            touch::RouteMouse(window, WM_LBUTTONDBLCLK, released.contact.start, MK_LBUTTON);
            if (IsWindow(window)) touch::RouteMouse(window, WM_LBUTTONUP, screen);
        }
        else touch::RouteClick(window, released.contact.start, false);
    }
    return true;
}

bool DesktopApp::CanRevealDockFromTouch(HMONITOR monitor) const
{
    if (!generalSettings_.dockEnabled ||
        !snowdesktop::dock_settings_rules::IsFloatingEdgeSwipeEnabled(
            dockSettings_.showOnlyWhenSummoned, dockSettings_.floatingEdgeSwipeEnabled) ||
        FullscreenBlocksDockReveal(DockRevealSource::Gesture, monitor)) return false;
    const auto* dock = SelectFloatingDockContainerForMonitor(monitor);
    if (!dock || dock->GetMonitor() != monitor) return false;
    const auto* host = FindPersistentDockHost(dock);
    return !host || !IsPersistentDockHostPromoted(*host);
}

bool DesktopApp::TryRevealTouchEdge(POINT start, std::optional<POINT> end)
{
    if (!hwnd_ || exitRequested_ || desktopPassthroughActive_ || HasActiveContextMenuSession() ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive()) return false;
    const HMONITOR monitor = MonitorFromPoint(start, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    UINT dpi = 96;
    if (UINT x = 0, y = 0; SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) dpi = x;
    const auto qualifies = [&](DockPosition position) {
        const RECT bounds = TouchEdgeBounds(info.rcMonitor, position, dpi);
        return PtInRect(&bounds, start) &&
            (!end || IsOutwardSwipe(position, start, *end, touch::Dips(24, dpi)));
    };
    const bool dock = qualifies(dockSettings_.position) && CanRevealDockFromTouch(monitor);
    const bool status = statusBar_ && qualifies(generalSettings_.statusBar.position) &&
        statusBar_->CanRevealFromTouch(monitor);
    if (end && (dock || status))
    {
        touch::touchMode = true;
        if (dock) ShowFloatingDock(monitor, DockRevealSource::Gesture);
        if (status && statusBar_) statusBar_->RevealFromTouch(monitor);
    }
    return dock || status;
}

bool DesktopApp::HandleDesktopTouchEdge(HWND window, UINT message, WPARAM pointer, LPARAM position)
{
    if (!touch::IsPointerMessage(message) || snowdesktop::private_pointer::Active()) return false;
    static thread_local EdgeTouch gesture;
    if (gesture.window && !IsWindow(gesture.window)) gesture = {};
    const POINT screen = touch::ScreenPoint(position);
    if (IsNewContact(message, pointer))
    {
        if (gesture.contact.Active() || !TryRevealTouchEdge(screen)) return false;
        gesture = {};
        gesture.window = window;
        gesture.contact.Begin(pointer, screen);
        return true;
    }
    if (gesture.window != window || !gesture.contact.Owns(pointer)) return false;
    if (message == WM_POINTERCAPTURECHANGED || touch::IsCanceled(pointer))
    {
        gesture = {};
        return true;
    }
    if (message == WM_POINTERUP)
    {
        const POINT start = gesture.contact.start;
        gesture = {};
        (void)TryRevealTouchEdge(start, screen);
    }
    return true;
}

LRESULT CALLBACK DesktopApp::TouchEdgeWndProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* edge = reinterpret_cast<TouchEdgeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!edge) return DefWindowProcW(window, message, wp, lp);
    if (message == WM_NCHITTEST)
    {
        INPUT_MESSAGE_SOURCE source{};
        const bool touchContact = touch::IsPromotedTouch() ||
            (GetCurrentInputMessageSource(&source) && source.deviceType == IMDT_TOUCH);
        return touchContact ? HTCLIENT : HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_POINTERACTIVATE) return PA_NOACTIVATE;
    if (touch::IsPointerMessage(message) && edge->app->HandleDesktopTouchEdge(window, message, wp, lp))
        return 0;
    if (message == WM_CANCELMODE || message == WM_POINTERCAPTURECHANGED)
        edge->contact.Reset();
    if (message == WM_NCDESTROY)
    {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete edge;
    }
    if (message != WM_TOUCH) return DefWindowProcW(window, message, wp, lp);

    std::array<TOUCHINPUT, 32> input{};
    const UINT count = LOWORD(wp);
    const auto handle = reinterpret_cast<HTOUCHINPUT>(lp);
    const bool received = count <= input.size() && GetTouchInputInfo(
        handle, count, input.data(), sizeof(TOUCHINPUT));
    CloseTouchInputHandle(handle);
    if (!received || snowdesktop::private_pointer::Active()) return 0;

    for (UINT index = 0; index < count; ++index)
    {
        const auto& sample = input[index];
        const POINT point{TOUCH_COORD_TO_PIXEL(sample.x), TOUCH_COORD_TO_PIXEL(sample.y)};
        if ((sample.dwFlags & (TOUCHEVENTF_PRIMARY | TOUCHEVENTF_DOWN)) ==
            (TOUCHEVENTF_PRIMARY | TOUCHEVENTF_DOWN) &&
            (sample.dwFlags & (TOUCHEVENTF_PEN | TOUCHEVENTF_PALM)) == 0 &&
            edge->app->TryRevealTouchEdge(point))
        {
            touch::touchMode = true;
            edge->contact.id = sample.dwID;
            edge->contact.start = edge->contact.last = point;
            edge->contact.started = GetTickCount64();
            touch::lastTouchPress = GetTickCount64();
            edge->app->OnMouseLeave();
        }
        if (edge->contact.id != sample.dwID || (sample.dwFlags & TOUCHEVENTF_UP) == 0) continue;
        const auto released = edge->contact;
        edge->contact.Reset();
        if (released.Active()) (void)edge->app->TryRevealTouchEdge(released.start, point);
    }
    return 0;
}

void DesktopApp::SyncTouchEdgeWindows()
{
    const bool enabled = hwnd_ && !exitRequested_ && !desktopPassthroughActive_ &&
        ((GetSystemMetrics(SM_DIGITIZER) & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH)) ||
            generalSettings_.touchSimulationEnabled);
    const bool dock = enabled && generalSettings_.dockEnabled &&
        snowdesktop::dock_settings_rules::IsFloatingEdgeSwipeEnabled(
            dockSettings_.showOnlyWhenSummoned, dockSettings_.floatingEdgeSwipeEnabled);
    const bool status = enabled && statusBar_ && generalSettings_.statusBar.showOnlyWhenSummoned;
    if (!dock && !status) { DestroyTouchEdgeWindows(); return; }

    struct Target { HMONITOR monitor; DockPosition edge; RECT bounds; };
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM owner) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(owner)->push_back(monitor);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    std::vector<Target> targets;
    if (!HasActiveContextMenuSession() && !dragSession_.HasContext() && !dragDropController_.IsTransportActive())
    {
        for (const auto monitor : monitors)
        {
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info)) continue;
            UINT dpi = 96;
            if (UINT x = 0, y = 0; SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) dpi = x;
            const bool dockEligible = dock && CanRevealDockFromTouch(monitor);
            const auto add = [&](DockPosition edge) {
                const RECT bounds = TouchEdgeBounds(info.rcMonitor, edge, dpi);
                if (!IsRectEmpty(&bounds)) targets.push_back({monitor, edge, bounds});
            };
            if (dockEligible) add(dockSettings_.position);
            if (status && statusBar_->CanRevealFromTouch(monitor) &&
                (!dockEligible || generalSettings_.statusBar.position != dockSettings_.position))
                add(generalSettings_.statusBar.position);
        }
    }
    std::vector<RECT> simulatedZones;
    if (generalSettings_.touchSimulationEnabled)
    {
        simulatedZones.reserve(targets.size());
        for (const auto& target : targets) simulatedZones.push_back(target.bounds);
    }
    snowdesktop::touch_simulation::SetEdgeGestureTargets(hwnd_, std::move(simulatedZones));

    std::erase_if(touchEdgeWindows_, [&](HWND window) {
        const auto* edge = reinterpret_cast<TouchEdgeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (edge && std::ranges::any_of(targets, [&](const auto& target) {
            return target.monitor == edge->monitor && target.edge == edge->edge;
        })) return false;
        DestroyWindow(window);
        return true;
    });
    if (targets.empty()) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = TouchEdgeWndProc;
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kTouchEdgeWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    for (const auto& target : targets)
    {
        const auto it = std::ranges::find_if(touchEdgeWindows_, [&](HWND window) {
            const auto* edge = reinterpret_cast<TouchEdgeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            return edge && target.monitor == edge->monitor && target.edge == edge->edge;
        });
        HWND window = it == touchEdgeWindows_.end() ? nullptr : *it;
        const auto& bounds = target.bounds;
        if (!window)
        {
            window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kTouchEdgeWindowClass, nullptr, WS_POPUP, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance_, nullptr);
            if (!window) continue;
            if (!RegisterTouchWindow(window, 0) || !SetLayeredWindowAttributes(window, 0, 1, LWA_ALPHA))
            { DestroyWindow(window); continue; }
            auto edge = std::make_unique<TouchEdgeWindow>(TouchEdgeWindow{this, target.monitor, target.edge, {}});
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(edge.release()));
            touchEdgeWindows_.push_back(window);
        }
        RECT placed{};
        GetWindowRect(window, &placed);
        if (!IsWindowVisible(window) || !EqualRect(&placed, &bounds))
            SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
}

void DesktopApp::DestroyTouchEdgeWindows()
{
    snowdesktop::touch_simulation::SetEdgeGestureTargets(nullptr);
    for (const HWND window : touchEdgeWindows_) DestroyWindow(window);
    touchEdgeWindows_.clear();
}
