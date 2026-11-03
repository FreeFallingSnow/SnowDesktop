#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "navigation/quick_navigation_animation_rules.h"
#include "app/navigation/quick_navigation_state.h"
#include "theme/personalization.h"
#include "common/utils.h"
#include "ui/input/text_input_window.h"
#include "navigation/quick_navigation_rules.h"

// Quick-navigation window and search-edit message dispatch.

namespace
{
// This timer belongs to the separate Quick Navigation HWND.
constexpr UINT_PTR kQuickNavigationTabHoverTimerId = 1;

bool IsCurrentProcessWindow(HWND candidate)
{
    if (!candidate)
        return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(candidate, &processId);
    return processId == GetCurrentProcessId();
}

bool IsWindowOwnedBy(
    HWND candidate, HWND expectedOwner)
{
    if (!candidate || !expectedOwner)
        return false;

    HWND current = candidate;
    while (current)
    {
        if (current == expectedOwner)
            return true;
        const HWND owner =
            GetWindow(current, GW_OWNER);
        if (!owner || owner == current)
            break;
        current = owner;
    }
    return false;
}
}

void DesktopApp::CancelQuickNavigationTabHover()
{
    if (quickNavigation_->tabHoverTimerArmed && quickNavigation_->hwnd)
        KillTimer(quickNavigation_->hwnd, kQuickNavigationTabHoverTimerId);
    quickNavigation_->tabHoverTimerArmed = false;
    quickNavigation_->tabHoverController.Reset();
}

void DesktopApp::UpdateQuickNavigationTabHover(POINT point, bool allowSwitch)
{
    if (!personalizationSettings_.groupTabHoverSwitch || !quickNavigation_->open ||
        quickNavigation_->preview || quickNavigation_->animation.IsAnimating() ||
        quickNavigation_->tabDragIndex != static_cast<size_t>(-1) || quickNavigation_->scrollbarDragging ||
        quickNavigation_->keyboardTargetKind != QuickNavigationKeyboardTargetKind::None ||
        renameController_.IsActive() || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() || snowdesktop::private_pointer::Capture() ||
        HasActiveContextMenuSession() ||
        snowdesktop::text_input::HasEditingMenu(quickNavigation_->searchEdit) ||
        ((snowdesktop::private_pointer::AsyncKeyState(VK_LBUTTON) | snowdesktop::private_pointer::AsyncKeyState(VK_RBUTTON) |
            snowdesktop::private_pointer::AsyncKeyState(VK_MBUTTON)) & 0x8000))
    {
        CancelQuickNavigationTabHover();
        return;
    }

    if (allowSwitch)
    {
        POINT screenPoint{};
        if (!snowdesktop::private_pointer::CursorPosition(&screenPoint) || snowdesktop::private_pointer::WindowAtPoint(screenPoint) != quickNavigation_->hwnd)
        {
            CancelQuickNavigationTabHover();
            return;
        }
        point = { screenPoint.x - virtualLeft_, screenPoint.y - virtualTop_ };
    }
    const auto tab = GetQuickNavigationTabAtPoint(point);
    if (!tab)
    {
        CancelQuickNavigationTabHover();
        return;
    }

    const auto indices = GetQuickNavigationCollectionIndices();
    const size_t target = *tab == 0 ? static_cast<size_t>(-1) :
        *tab == 1 ? static_cast<size_t>(-2) : indices[*tab - 2];
    if (target == quickNavigation_->activeWidgetIndex)
    {
        CancelQuickNavigationTabHover();
        return;
    }

    // Stable identity prevents a reordered/rebuilt tab from inheriting dwell.
    const DWORD delay = static_cast<DWORD>(NormalizeTabHoverDelayMs(personalizationSettings_.tabHoverDelayMs));
    const RECT bounds = GetQuickNavigationTabRect(quickNavigation_->rect, *tab);
    const std::wstring identity = *tab < 2 ? (*tab == 0 ? L"desktop" : L"mapping") :
        L"widget:" + widgets_[target].id;
    const std::wstring token = std::to_wstring(identity.size()) + L":" + identity + L":" +
        std::to_wstring(quickNavigation_->activeWidgetIndex) + L":" + std::to_wstring(pageMutationRevision_) + L":" +
        std::to_wstring(quickNavigation_->tabScrollOffset) + L":" + std::to_wstring(bounds.left) + L":" +
        std::to_wstring(bounds.top) + L":" + std::to_wstring(bounds.right) + L":" + std::to_wstring(delay);
    const DWORD now = GetTickCount();
    quickNavigation_->tabHoverController.Track(token, now);
    if (allowSwitch && quickNavigation_->tabHoverController.Consume(now, delay))
    {
        (void)ActivateQuickNavigationTab(*tab);
        return;
    }
    if (!quickNavigation_->tabHoverTimerArmed && quickNavigation_->hwnd)
        quickNavigation_->tabHoverTimerArmed = SetTimer(quickNavigation_->hwnd,
            kQuickNavigationTabHoverTimerId, kPopupHoverPollIntervalMs, nullptr) != 0;
}

LRESULT DesktopApp::HandleQuickNavigationMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!snowdesktop::private_pointer::MessageAllowed(hwnd, msg, wp, lp)) return 0;
    struct NativeMenuPresentationScope final
    {
        DesktopApp& app;
        ~NativeMenuPresentationScope()
        {
            app.FlushNativeMenuPresentation();
        }
    } nativeMenuPresentationScope{ *this };

    // Shell context menus send owner-draw and submenu messages to the
    // TrackPopupMenu owner. Quick Navigation owns menus opened from its
    // entries so it can stay active while the menu is visible.
    LRESULT shellMenuResult = 0;
    if (HandleShellContextMenuMessage(
            msg, wp, lp, shellMenuResult))
        return shellMenuResult;

    if (!quickNavigation_->animation.IsAnimating() &&
        snowdesktop::text_input::RoutePointer(quickNavigation_->searchEdit, msg, wp,
            {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
        return 0;

    switch (msg)
    {
    case WM_NCHITTEST:
        if (!quickNavigation_->open)
            return HTTRANSPARENT;
        // During the warp, controls are not at their final hit rectangles.
        // Keep keyboard focus in the hidden edit, but let Dock clicks through.
        if (quickNavigation_->animation.IsAnimating() &&
            snowdesktop::quick_navigation_animation_rules::IsDeformationEffect(
                quickNavigation_->animation.GetEffect()))
            return HTTRANSPARENT;
        {
            POINT point{
                GET_X_LPARAM(lp) - virtualLeft_,
                GET_Y_LPARAM(lp) - virtualTop_
            };
            const auto visual =
                quickNavigation_->animation.GetVisual();
            const auto scaled = [scale = visual.scale](
                LONG value, LONG anchor) {
                return static_cast<LONG>(std::lround(
                    static_cast<double>(anchor) +
                    static_cast<double>(value - anchor) *
                        static_cast<double>(scale)));
            };
            RECT visible{
                scaled(
                    quickNavigation_->rect.left,
                    quickNavigation_->animationAnchorPoint.x),
                scaled(
                    quickNavigation_->rect.top,
                    quickNavigation_->animationAnchorPoint.y),
                scaled(
                    quickNavigation_->rect.right,
                    quickNavigation_->animationAnchorPoint.x),
                scaled(
                    quickNavigation_->rect.bottom,
                    quickNavigation_->animationAnchorPoint.y)
            };
            if (!snowdesktop::quick_navigation_rules::
                    ShouldAcceptPointerHit(
                        quickNavigation_->open,
                        point,
                        quickNavigation_->rect,
                        visible))
                return HTTRANSPARENT;
        }
        break;
    case WM_PAINT:
        PaintQuickNavigationWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == kQuickNavigationTabHoverTimerId)
        {
            if (quickNavigation_->tabHoverTimerArmed)
                UpdateQuickNavigationTabHover(quickNavigation_->lastMousePoint, true);
            return 0;
        }
        if (wp ==
            kQuickNavigationEverythingSearchTimerId)
        {
            StartQueuedQuickNavigationEverythingSearch();
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == quickNavigation_->searchEdit)
        {
            HDC hdcEdit = reinterpret_cast<HDC>(wp);
            SetBkMode(hdcEdit, OPAQUE);
            SetTextColor(hdcEdit, RGB(28, 34, 44));
            SetBkColor(hdcEdit, RGB(255, 255, 255));
            SetDCBrushColor(hdcEdit, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
    case WM_LBUTTONDOWN:
    {
        CancelQuickNavigationTabHover();
        ResetQuickNavigationKeyboardTarget();
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigation_->hostRect.left,
            pt.y + quickNavigation_->hostRect.top
        };
        if (DismissQuickNavigationMenuAtPoint(appPoint)) return 0;

        {
            RECT content = GetQuickNavigationContentRect(quickNavigation_->rect);
            RECT track{}, thumb{};
            int maxScroll = 0, contentHeight = 0;
            if (!quickNavigation_->initialJumpOpen &&
                GetQuickNavigationScrollbarGeometry(quickNavigation_->rect,
                    track, thumb, maxScroll, contentHeight) && PtInRect(&track, appPoint))
            {
                if (renameController_.BlocksScrolling())
                    return 0;

                {
                    if (PtInRect(&thumb, appPoint))
                    {
                        quickNavigation_->scrollbarDragging = true;
                        quickNavigation_->scrollbarDragStartY = appPoint.y;
                        quickNavigation_->scrollbarDragThumbTop = static_cast<int>(thumb.top);
                        quickNavigation_->scrollbarDragStartOffset = quickNavigation_->scrollOffset;
                        snowdesktop::private_pointer::SetPointerCapture(hwnd);
                        return 0;
                    }
                    if (PtInRect(&track, appPoint))
                    {
                        int pageSize = std::max(
                            QuickNavScale(kQuickNavigationCellHeight + kQuickNavigationItemRowGap),
                            static_cast<int>(content.bottom - content.top) - QuickNavScale(28));
                        if (appPoint.y < thumb.top)
                            quickNavigation_->scrollOffset = std::max(0,
                                quickNavigation_->scrollOffset - pageSize);
                        else
                            quickNavigation_->scrollOffset = std::min(maxScroll,
                                quickNavigation_->scrollOffset + pageSize);
                        InvalidateQuickNavigationWindow();
                        return 0;
                    }
                }
                return 0;
            }
        }

        if (!UseQuickNavigationList() && quickNavigation_->menu == QuickNavigationMenu::None && GetQuickNavigationEffectiveSearchText().empty())
        {
            RECT overlay = quickNavigation_->rect;
            std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
            RECT tabs = GetQuickNavigationTabsRect(overlay);
            const int gap = QuickNavScale(8);
            const int sepGap = QuickNavScale(6);
            const int fixedWidth = quickNavigation_->tabWidths.size() >= 2
                ? quickNavigation_->tabWidths[0] + gap + quickNavigation_->tabWidths[1]
                : 0;
            const int scrollPad = sepGap + QuickNavScale(1) + gap;
            const int tabsStart =
                GetQuickNavigationTabsStart(
                    overlay);
            RECT scrollHitBounds = MakeRect(
                tabsStart + fixedWidth +
                    scrollPad,
                tabs.top,
                tabs.right,
                tabs.bottom);
            for (size_t tab = 2; tab < ci.size() + 2; ++tab)
            {
                RECT tabRect = GetQuickNavigationTabRect(overlay, tab);
                RECT visibleTabRect{};
                if (IntersectRect(&visibleTabRect, &tabRect, &scrollHitBounds) &&
                    PtInRect(&visibleTabRect, appPoint))
                {
                    quickNavigation_->tabDragIndex = tab;
                    quickNavigation_->tabDragStartPoint = appPoint;
                    quickNavigation_->tabDragDeltaX = 0;
                    quickNavigation_->tabDragging = false;
                    snowdesktop::private_pointer::SetPointerCapture(hwnd);
                    return 0;
                }
            }
        }

        HandleQuickNavigationClick(appPoint);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        CancelQuickNavigationTabHover();
        OnRightButtonDown(nullptr);
        return 0;
    case WM_RBUTTONUP:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigation_->hostRect.left,
            pt.y + quickNavigation_->hostRect.top
        };
        POINT screenPoint{ pt.x, pt.y };
        ClientToScreen(hwnd, &screenPoint);
        if (HandleQuickNavigationRightClick(appPoint, screenPoint))
            return 0;
        break;
    }
    case WM_CONTEXTMENU:
    {
        CancelQuickNavigationTabHover();
        if (reinterpret_cast<HWND>(wp) != hwnd)
            break;
        POINT screenPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT clientPoint = screenPoint;
        if (screenPoint.x == -1 && screenPoint.y == -1)
        {
            clientPoint =
                quickNavigation_->lastMousePoint;
            screenPoint = clientPoint;
            screenPoint.x -=
                quickNavigation_->hostRect.left;
            screenPoint.y -=
                quickNavigation_->hostRect.top;
            ClientToScreen(hwnd, &screenPoint);
        }
        else
        {
            ScreenToClient(hwnd, &clientPoint);
            clientPoint.x +=
                quickNavigation_->hostRect.left;
            clientPoint.y +=
                quickNavigation_->hostRect.top;
        }
        if (HandleQuickNavigationRightClick(clientPoint, screenPoint))
            return 0;
        break;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigation_->hostRect.left,
            pt.y + quickNavigation_->hostRect.top
        };
        POINT previousMouse =
            quickNavigation_->lastMousePoint;
        quickNavigation_->lastMousePoint = appPoint;
        const bool keyboardHoverCleared =
            (previousMouse.x != appPoint.x ||
                previousMouse.y != appPoint.y) &&
            quickNavigation_->keyboardTargetKind !=
                QuickNavigationKeyboardTargetKind::None;
        if (keyboardHoverCleared)
            ResetQuickNavigationKeyboardTarget();
        TRACKMOUSEEVENT mouseTrack{};
        mouseTrack.cbSize = sizeof(mouseTrack);
        mouseTrack.dwFlags = TME_LEAVE;
        mouseTrack.hwndTrack = hwnd;
        snowdesktop::private_pointer::TrackLeave(&mouseTrack);

        auto queuePointerFrame = [&]() {
            InvalidateQuickNavigationWindow(true);
        };

        if (quickNavigation_->scrollbarDragging)
        {
            RECT track{}, thumb{};
            int maxScroll = 0, contentHeight = 0;
            if (GetQuickNavigationScrollbarGeometry(quickNavigation_->rect,
                track, thumb, maxScroll, contentHeight))
            {
                const int trackH = std::max<LONG>(1, track.bottom - track.top);
                const int thumbH = std::max<LONG>(1, thumb.bottom - thumb.top);
                int newThumbTop = appPoint.y - (quickNavigation_->scrollbarDragStartY -
                    quickNavigation_->scrollbarDragThumbTop);
                newThumbTop = std::clamp(newThumbTop, static_cast<int>(track.top),
                    static_cast<int>(track.bottom - thumbH));
                const int rangeH = std::max(1, trackH - thumbH);
                quickNavigation_->scrollOffset = (newThumbTop - static_cast<int>(track.top))
                    * maxScroll / rangeH;
                quickNavigation_->scrollOffset = std::clamp(quickNavigation_->scrollOffset, 0, maxScroll);
                queuePointerFrame();
            }
            return 0;
        }

        if (quickNavigation_->tabDragIndex != static_cast<size_t>(-1))
        {
            int dx = appPoint.x - quickNavigation_->tabDragStartPoint.x;
            if (!quickNavigation_->tabDragging &&
                snowdesktop::quick_navigation_rules::TabDragExceededThreshold(
                    dx, quickNavigation_->tabDragDeltaX, 4))
                quickNavigation_->tabDragging = true;
            if (quickNavigation_->tabDragging)
                quickNavigation_->tabDragDeltaX = dx;
            queuePointerFrame();
            return 0;
        }

        bool wasHovered = quickNavigation_->scrollbarHovered;
        quickNavigation_->scrollbarHovered = false;
        {
            RECT track{}, thumb{};
            int maxScroll = 0, contentHeight = 0;
            if (!quickNavigation_->initialJumpOpen &&
                GetQuickNavigationScrollbarGeometry(quickNavigation_->rect,
                    track, thumb, maxScroll, contentHeight))
                quickNavigation_->scrollbarHovered = PtInRect(&thumb, appPoint) != FALSE;
        }
        const QuickNavigationPointerTarget pointerTarget =
            HitTestQuickNavigationPointerTarget(
                appPoint);
        const bool hoverChanged =
            pointerTarget !=
                quickNavigation_->pointerTarget;
        const bool hoverMapMissing =
            quickNavigation_->hoverRegions.empty();
        quickNavigation_->pointerTarget =
            pointerTarget;
        UpdateQuickNavigationTabHover(appPoint);
        if (UseQuickNavigationList() || quickNavigation_->menu != QuickNavigationMenu::None ||
            (appPoint.y < GetQuickNavigationSearchRect(quickNavigation_->rect).bottom) ||
            wasHovered != quickNavigation_->scrollbarHovered ||
            hoverChanged || keyboardHoverCleared ||
            hoverMapMissing)
            queuePointerFrame();
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        CancelQuickNavigationTabHover();
        quickNavigation_->lastMousePoint = {
            LONG_MIN, LONG_MIN };
        quickNavigation_->scrollbarHovered = false;
        quickNavigation_->pointerTarget = {};
        InvalidateQuickNavigationWindow(true);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (quickNavigation_->scrollbarDragging)
        {
            snowdesktop::private_pointer::ReleasePointerCapture();
            quickNavigation_->scrollbarDragging = false;
            InvalidateQuickNavigationWindow();
            return 0;
        }

        if (quickNavigation_->tabDragIndex != static_cast<size_t>(-1))
        {
            snowdesktop::private_pointer::ReleasePointerCapture();
            size_t dragTab = quickNavigation_->tabDragIndex;

            if (quickNavigation_->tabDragging)
            {
                std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
                int targetTab = GetQuickNavTabDragTarget(dragTab, quickNavigation_->tabDragDeltaX);

                if (targetTab != static_cast<int>(dragTab) && targetTab >= 2 &&
                    static_cast<size_t>(targetTab) >= 2 &&
                    static_cast<size_t>(targetTab - 2) < ci.size())
                {
                    size_t srcIdx = dragTab - 2;
                    size_t dstIdx = static_cast<size_t>(targetTab) - 2;
                    EnsureNavTabOrder();

                    if (srcIdx < quickNavigation_->tabOrder.size() && dstIdx < quickNavigation_->tabOrder.size())
                    {
                        std::wstring id = quickNavigation_->tabOrder[srcIdx];
                        quickNavigation_->tabOrder.erase(quickNavigation_->tabOrder.begin() + srcIdx);
                        quickNavigation_->tabOrder.insert(quickNavigation_->tabOrder.begin() + dstIdx, id);
                        quickNavigation_->activeWidgetIndex = ci[srcIdx];
                        quickNavigation_->scrollOffset = 0;
                        quickNavigation_->initialJumpOpen = false;
                        SaveLayoutSlots();
                    }
                }
            }
            else
            {
                (void)ActivateQuickNavigationTab(dragTab);
            }

            quickNavigation_->tabDragIndex = static_cast<size_t>(-1);
            quickNavigation_->tabDragDeltaX = 0;
            quickNavigation_->tabDragging = false;
            InvalidateQuickNavigationWindow();
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        CancelQuickNavigationTabHover();
        OnMouseWheel(wp, lp);
        return 0;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        CancelQuickNavigationTabHover();
        break;
    case WM_COMMAND:
        if (renameInputWindow_ && reinterpret_cast<HWND>(lp) == renameInputWindow_ &&
            HIWORD(wp) == EN_UPDATE)
        {
            renameEditLayout_.Update(renameInputWindow_);
            return 0;
        }
        if (reinterpret_cast<HWND>(lp) == quickNavigation_->searchEdit && HIWORD(wp) == EN_CHANGE)
        {
            RefreshQuickNavigationSearchText();
            quickNavigation_->scrollOffset = 0;
            InvalidateQuickNavigationWindow();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        CancelQuickNavigationTabHover();
        if (snowdesktop::text_input::IsComposing(quickNavigation_->searchEdit) || !quickNavigation_->searchCompositionText.empty()) break;
        if (HandleQuickNavigationSearchKey(wp)) return 0;
        if (HandleQuickNavigationKeyboardInput(wp))
            return 0;
        if (wp == VK_ESCAPE)
        {
            if (quickNavigation_->scrollbarDragging)
            {
                snowdesktop::private_pointer::ReleasePointerCapture();
                quickNavigation_->scrollbarDragging = false;
                quickNavigation_->scrollOffset = quickNavigation_->scrollbarDragStartOffset;
                InvalidateQuickNavigationWindow();
                return 0;
            }
            if (quickNavigation_->tabDragIndex != static_cast<size_t>(-1))
            {
                snowdesktop::private_pointer::ReleasePointerCapture();
                quickNavigation_->tabDragIndex = static_cast<size_t>(-1);
                quickNavigation_->tabDragDeltaX = 0;
                quickNavigation_->tabDragging = false;
                InvalidateQuickNavigationWindow();
                return 0;
            }
            CloseQuickNavigation();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
        {
            const HWND activatedWindow = reinterpret_cast<HWND>(lp);
            const bool retainedInteraction =
                activatedWindow == quickNavigation_->searchEdit ||
                (renameController_.
                    IsQuickNavigationPresentation() &&
                    activatedWindow == renameInputWindow_) ||
                quickNavigation_->backdropCompositor.IsBackdropWindow(
                    activatedWindow) ||
                IsWindowOwnedBy(
                    activatedWindow,
                    quickNavigation_->hwnd) ||
                (IsCurrentProcessWindow(activatedWindow) &&
                    !IsSettingsApplicationWindow(activatedWindow));
            if (!snowdesktop::quick_navigation_rules::
                    ShouldCloseOnDeactivate(
                        retainedInteraction))
                return 0;
            if (quickNavigation_->tabDragIndex != static_cast<size_t>(-1))
            {
                snowdesktop::private_pointer::ReleasePointerCapture();
                quickNavigation_->tabDragIndex = static_cast<size_t>(-1);
                quickNavigation_->tabDragDeltaX = 0;
                quickNavigation_->tabDragging = false;
            }
            // A bar action may be waiting for this surface's closing animation.
            // An external activation cancels that handoff even when open_ is
            // already false and CloseQuickNavigation would otherwise do nothing.
            if (statusBarActivationMonitor_ && quickNavigation_->postCloseAction)
                CancelStatusBarActivation(statusBarActivationMonitor_);
            CloseQuickNavigation(false);
            return 0;
        }
        break;
    case WM_CLOSE:
        CloseQuickNavigation();
        return 0;
    case WM_DESTROY:
        CancelQuickNavigationTabHover();
        if (quickNavigation_->hwnd == hwnd)
            quickNavigation_->hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 快捷导航搜索编辑框的子类化窗口过程
 * @param hwnd 编辑框句柄
 * @param message 消息 ID
 * @param wParam wParam
 * @param lParam lParam
 * @param subclassId 子类化 ID
 * @param refData 引用数据（指向 DesktopApp 实例）
 * @return 消息处理结果
 */
LRESULT CALLBACK DesktopApp::QuickNavigationSearchSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    if (message == WM_KEYDOWN || message == WM_MOUSEWHEEL ||
        message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
        message == WM_IME_STARTCOMPOSITION)
        app->CancelQuickNavigationTabHover();

    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN)
    {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd, &point);
        point.x -= app->virtualLeft_;
        point.y -= app->virtualTop_;
        app->DismissQuickNavigationMenuAtPoint(point);
    }
    if (message == WM_NCHITTEST)
    {
        if (app->quickNavigation_->animation.IsAnimating())
            return HTTRANSPARENT;
        const POINT desktopPoint{
            GET_X_LPARAM(lParam) - app->virtualLeft_,
            GET_Y_LPARAM(lParam) - app->virtualTop_
        };
        const RECT targetEdit = app->GetQuickNavigationInputRect(app->quickNavigation_->rect);
        RECT animatedEdit{};
        GetWindowRect(hwnd, &animatedEdit);
        OffsetRect(
            &animatedEdit,
            -app->virtualLeft_,
            -app->virtualTop_);
        if (!snowdesktop::quick_navigation_rules::
                ShouldAcceptPointerHit(
                    app->quickNavigation_->open,
                    desktopPoint,
                    targetEdit,
                    animatedEdit))
        {
            return HTTRANSPARENT;
        }
    }

    if (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
    {
        if (snowdesktop::text_input::HasEditingMenu(hwnd))
            return DefSubclassProc(hwnd,message,wParam,lParam);
        const HWND activatedWindow = reinterpret_cast<HWND>(lParam);
        const bool retainedInteraction =
            activatedWindow == app->quickNavigation_->hwnd ||
            app->quickNavigation_->backdropCompositor.IsBackdropWindow(
                activatedWindow) ||
            IsWindowOwnedBy(
                activatedWindow,
                app->quickNavigation_->hwnd) ||
            (IsCurrentProcessWindow(activatedWindow) &&
                !app->IsSettingsApplicationWindow(activatedWindow));
        if (snowdesktop::quick_navigation_rules::
                ShouldCloseOnDeactivate(
                    retainedInteraction))
        {
            if (app->statusBarActivationMonitor_ && app->quickNavigation_->postCloseAction)
                app->CancelStatusBarActivation(app->statusBarActivationMonitor_);
            app->CloseQuickNavigation(false);
            return 0;
        }
    }

    if (message == WM_KEYDOWN && !snowdesktop::text_input::IsComposing(hwnd) &&
        app->quickNavigation_->searchCompositionText.empty() && app->HandleQuickNavigationSearchKey(wParam)) return 0;

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE &&
        !snowdesktop::text_input::IsComposing(hwnd) && app->quickNavigation_->searchCompositionText.empty())
    {
        if (app->
            HandleQuickNavigationInitialJumpKeyboardInput(
                wParam))
            return 0;
        app->CloseQuickNavigation();
        return 0;
    }
    if (message == WM_KEYDOWN &&
        snowdesktop::quick_navigation_rules::
            ShouldRouteSearchEditKeyToResults(
                wParam) &&
        !snowdesktop::text_input::IsComposing(hwnd) &&
        app->quickNavigation_->searchCompositionText.empty() &&
        app->HandleQuickNavigationKeyboardInput(wParam))
    {
        return 0;
    }
    if (message == WM_MOUSEWHEEL)
    {
        app->OnMouseWheel(wParam, lParam);
        return 0;
    }
    if (message == WM_IME_STARTCOMPOSITION)
    {
        const auto result=DefSubclassProc(hwnd,message,wParam,lParam);
        app->ClearQuickNavigationSearchCompositionText();
        return result;
    }
    if (message == WM_IME_COMPOSITION)
    {
        const auto result=DefSubclassProc(hwnd,message,wParam,lParam);
        app->RefreshQuickNavigationSearchCompositionText(hwnd, lParam);
        return result;
    }
    if (message == WM_IME_ENDCOMPOSITION)
    {
        const auto result=DefSubclassProc(hwnd,message,wParam,lParam);
        app->ClearQuickNavigationSearchCompositionText();
        return result;
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}
