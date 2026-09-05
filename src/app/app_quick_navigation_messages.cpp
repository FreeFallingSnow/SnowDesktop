#include "app.h"
#include "quick_navigation_rules.h"

// Quick-navigation window and search-edit message dispatch.

namespace
{
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

LRESULT DesktopApp::HandleQuickNavigationMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
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

    switch (msg)
    {
    case WM_NCHITTEST:
        if (!quickNavigationOpen_)
            return HTTRANSPARENT;
        {
            POINT point{
                GET_X_LPARAM(lp) - virtualLeft_,
                GET_Y_LPARAM(lp) - virtualTop_
            };
            const auto visual =
                quickNavigationAnimation_.GetVisual();
            const auto scaled = [scale = visual.scale](
                LONG value, LONG anchor) {
                return static_cast<LONG>(std::lround(
                    static_cast<double>(anchor) +
                    static_cast<double>(value - anchor) *
                        static_cast<double>(scale)));
            };
            RECT visible{
                scaled(
                    quickNavigationRect_.left,
                    quickNavigationAnimationAnchorPoint_.x),
                scaled(
                    quickNavigationRect_.top,
                    quickNavigationAnimationAnchorPoint_.y),
                scaled(
                    quickNavigationRect_.right,
                    quickNavigationAnimationAnchorPoint_.x),
                scaled(
                    quickNavigationRect_.bottom,
                    quickNavigationAnimationAnchorPoint_.y)
            };
            if (!snowdesktop::quick_navigation_rules::
                    ShouldAcceptPointerHit(
                        quickNavigationOpen_,
                        point,
                        quickNavigationRect_,
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
        if (wp ==
            kQuickNavigationEverythingSearchTimerId)
        {
            StartQueuedQuickNavigationEverythingSearch();
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == quickNavigationSearchEdit_)
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
        ResetQuickNavigationKeyboardTarget();
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigationHostRect_.left,
            pt.y + quickNavigationHostRect_.top
        };

        {
            RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
            const int trackW = QuickNavScale(5);
            RECT scrollCol = MakeRect(content.right - trackW - QuickNavScale(4), content.top,
                content.right, content.bottom);
            if (!quickNavigationInitialJumpOpen_ &&
                PtInRect(&scrollCol, appPoint))
            {
                if (renameController_.BlocksScrolling())
                    return 0;

                RECT track{}, thumb{};
                int maxScroll = 0, contentHeight = 0;
                if (GetQuickNavigationScrollbarGeometry(quickNavigationRect_,
                    track, thumb, maxScroll, contentHeight))
                {
                    if (PtInRect(&thumb, appPoint))
                    {
                        quickNavScrollbarDragging_ = true;
                        quickNavScrollbarDragStartY_ = appPoint.y;
                        quickNavScrollbarDragThumbTop_ = static_cast<int>(thumb.top);
                        quickNavScrollbarDragStartOffset_ = quickNavigationScrollOffset_;
                        SetCapture(hwnd);
                        return 0;
                    }
                    if (PtInRect(&track, appPoint))
                    {
                        int pageSize = std::max(
                            QuickNavScale(kQuickNavigationCellHeight + kQuickNavigationItemRowGap),
                            static_cast<int>(content.bottom - content.top) - QuickNavScale(28));
                        if (appPoint.y < thumb.top)
                            quickNavigationScrollOffset_ = std::max(0,
                                quickNavigationScrollOffset_ - pageSize);
                        else
                            quickNavigationScrollOffset_ = std::min(maxScroll,
                                quickNavigationScrollOffset_ + pageSize);
                        InvalidateQuickNavigationWindow();
                        return 0;
                    }
                }
                return 0;
            }
        }

        if (GetQuickNavigationEffectiveSearchText().empty())
        {
            RECT overlay = quickNavigationRect_;
            std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
            RECT tabs = GetQuickNavigationTabsRect(overlay);
            const int gap = QuickNavScale(8);
            const int sepGap = QuickNavScale(6);
            const int fixedWidth = quickNavTabWidths_.size() >= 2
                ? quickNavTabWidths_[0] + gap + quickNavTabWidths_[1]
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
                    quickNavTabDragIndex_ = tab;
                    quickNavTabDragStartPoint_ = appPoint;
                    quickNavTabDragDeltaX_ = 0;
                    quickNavTabDragging_ = false;
                    SetCapture(hwnd);
                    return 0;
                }
            }
        }

        HandleQuickNavigationClick(appPoint);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        OnRightButtonDown(nullptr);
        return 0;
    case WM_RBUTTONUP:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigationHostRect_.left,
            pt.y + quickNavigationHostRect_.top
        };
        POINT screenPoint{ pt.x, pt.y };
        ClientToScreen(hwnd, &screenPoint);
        if (HandleQuickNavigationRightClick(appPoint, screenPoint))
            return 0;
        break;
    }
    case WM_CONTEXTMENU:
    {
        if (reinterpret_cast<HWND>(wp) != hwnd)
            break;
        POINT screenPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT clientPoint = screenPoint;
        if (screenPoint.x == -1 && screenPoint.y == -1)
        {
            clientPoint =
                quickNavigationLastMousePoint_;
            screenPoint = clientPoint;
            screenPoint.x -=
                quickNavigationHostRect_.left;
            screenPoint.y -=
                quickNavigationHostRect_.top;
            ClientToScreen(hwnd, &screenPoint);
        }
        else
        {
            ScreenToClient(hwnd, &clientPoint);
            clientPoint.x +=
                quickNavigationHostRect_.left;
            clientPoint.y +=
                quickNavigationHostRect_.top;
        }
        if (HandleQuickNavigationRightClick(clientPoint, screenPoint))
            return 0;
        break;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{
            pt.x + quickNavigationHostRect_.left,
            pt.y + quickNavigationHostRect_.top
        };
        POINT previousMouse =
            quickNavigationLastMousePoint_;
        quickNavigationLastMousePoint_ = appPoint;
        const bool keyboardHoverCleared =
            (previousMouse.x != appPoint.x ||
                previousMouse.y != appPoint.y) &&
            quickNavigationKeyboardTargetKind_ !=
                QuickNavigationKeyboardTargetKind::None;
        if (keyboardHoverCleared)
            ResetQuickNavigationKeyboardTarget();
        TRACKMOUSEEVENT mouseTrack{};
        mouseTrack.cbSize = sizeof(mouseTrack);
        mouseTrack.dwFlags = TME_LEAVE;
        mouseTrack.hwndTrack = hwnd;
        TrackMouseEvent(&mouseTrack);

        auto queuePointerFrame = [&]() {
            InvalidateQuickNavigationWindow(true);
        };

        if (quickNavScrollbarDragging_)
        {
            RECT track{}, thumb{};
            int maxScroll = 0, contentHeight = 0;
            if (GetQuickNavigationScrollbarGeometry(quickNavigationRect_,
                track, thumb, maxScroll, contentHeight))
            {
                const int trackH = std::max<LONG>(1, track.bottom - track.top);
                const int thumbH = std::max<LONG>(1, thumb.bottom - thumb.top);
                int newThumbTop = appPoint.y - (quickNavScrollbarDragStartY_ -
                    quickNavScrollbarDragThumbTop_);
                newThumbTop = std::clamp(newThumbTop, static_cast<int>(track.top),
                    static_cast<int>(track.bottom - thumbH));
                const int rangeH = std::max(1, trackH - thumbH);
                quickNavigationScrollOffset_ = (newThumbTop - static_cast<int>(track.top))
                    * maxScroll / rangeH;
                quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0, maxScroll);
                queuePointerFrame();
            }
            return 0;
        }

        if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            int dx = appPoint.x - quickNavTabDragStartPoint_.x;
            if (!quickNavTabDragging_ && std::abs(dx) > 4)
                quickNavTabDragging_ = true;
            if (quickNavTabDragging_)
                quickNavTabDragDeltaX_ = dx;
            queuePointerFrame();
            return 0;
        }

        bool wasHovered = quickNavScrollbarHovered_;
        quickNavScrollbarHovered_ = false;
        {
            RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
            const int trackW = QuickNavScale(5);
            RECT scrollCol = MakeRect(content.right - trackW - QuickNavScale(4), content.top,
                content.right, content.bottom);
            if (!quickNavigationInitialJumpOpen_ &&
                PtInRect(&scrollCol, appPoint))
            {
                if (GetQuickNavigationContentHeight(quickNavigationRect_) >
                    static_cast<int>(content.bottom - content.top))
                {
                    RECT track{}, thumb{};
                    int ms = 0, ch = 0;
                    if (GetQuickNavigationScrollbarGeometry(quickNavigationRect_,
                        track, thumb, ms, ch) && PtInRect(&thumb, appPoint))
                    {
                        quickNavScrollbarHovered_ = true;
                    }
                }
            }
        }
        const QuickNavigationPointerTarget pointerTarget =
            HitTestQuickNavigationPointerTarget(
                appPoint);
        const bool hoverChanged =
            pointerTarget !=
                quickNavigationPointerTarget_;
        const bool hoverMapMissing =
            quickNavigationHoverRegions_.empty();
        quickNavigationPointerTarget_ =
            pointerTarget;
        if (wasHovered != quickNavScrollbarHovered_ ||
            hoverChanged || keyboardHoverCleared ||
            hoverMapMissing)
            queuePointerFrame();
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        quickNavigationLastMousePoint_ = {
            LONG_MIN, LONG_MIN };
        quickNavScrollbarHovered_ = false;
        quickNavigationPointerTarget_ = {};
        InvalidateQuickNavigationWindow(true);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (quickNavScrollbarDragging_)
        {
            ReleaseCapture();
            quickNavScrollbarDragging_ = false;
            InvalidateQuickNavigationWindow();
            return 0;
        }

        if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            ReleaseCapture();
            size_t dragTab = quickNavTabDragIndex_;

            if (quickNavTabDragging_)
            {
                std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
                int targetTab = GetQuickNavTabDragTarget(dragTab, quickNavTabDragDeltaX_);

                if (targetTab != static_cast<int>(dragTab) && targetTab >= 2 &&
                    static_cast<size_t>(targetTab) >= 2 &&
                    static_cast<size_t>(targetTab - 2) < ci.size())
                {
                    size_t srcIdx = dragTab - 2;
                    size_t dstIdx = static_cast<size_t>(targetTab) - 2;
                    EnsureNavTabOrder();

                    if (srcIdx < navTabOrder_.size() && dstIdx < navTabOrder_.size())
                    {
                        std::wstring id = navTabOrder_[srcIdx];
                        navTabOrder_.erase(navTabOrder_.begin() + srcIdx);
                        navTabOrder_.insert(navTabOrder_.begin() + dstIdx, id);
                        quickNavigationActiveWidgetIndex_ = ci[srcIdx];
                        quickNavigationScrollOffset_ = 0;
                        quickNavigationInitialJumpOpen_ = false;
                        SaveLayoutSlots();
                    }
                }
            }
            else
            {
                std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
                if (dragTab >= 2 && dragTab - 2 < ci.size())
                {
                    quickNavigationActiveWidgetIndex_ = ci[dragTab - 2];
                    quickNavigationScrollOffset_ = 0;
                    quickNavigationInitialJumpOpen_ = false;
                }
            }

            quickNavTabDragIndex_ = static_cast<size_t>(-1);
            quickNavTabDragDeltaX_ = 0;
            quickNavTabDragging_ = false;
            InvalidateQuickNavigationWindow();
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        OnMouseWheel(wp, lp);
        return 0;
    case WM_COMMAND:
        if (renameEdit_ && reinterpret_cast<HWND>(lp) == renameEdit_ &&
            HIWORD(wp) == EN_UPDATE)
        {
            renameEditLayout_.Update(renameEdit_);
            return 0;
        }
        if (reinterpret_cast<HWND>(lp) == quickNavigationSearchEdit_ && HIWORD(wp) == EN_CHANGE)
        {
            RefreshQuickNavigationSearchText();
            quickNavigationScrollOffset_ = 0;
            InvalidateQuickNavigationWindow();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (HandleQuickNavigationKeyboardInput(wp))
            return 0;
        if (wp == VK_ESCAPE)
        {
            if (quickNavScrollbarDragging_)
            {
                ReleaseCapture();
                quickNavScrollbarDragging_ = false;
                quickNavigationScrollOffset_ = quickNavScrollbarDragStartOffset_;
                InvalidateQuickNavigationWindow();
                return 0;
            }
            if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
            {
                ReleaseCapture();
                quickNavTabDragIndex_ = static_cast<size_t>(-1);
                quickNavTabDragDeltaX_ = 0;
                quickNavTabDragging_ = false;
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
                activatedWindow == quickNavigationSearchEdit_ ||
                (renameController_.
                    IsQuickNavigationPresentation() &&
                    activatedWindow == renameEdit_) ||
                quickNavBackdropCompositor_.IsBackdropWindow(
                    activatedWindow) ||
                IsWindowOwnedBy(
                    activatedWindow,
                    quickNavigationHwnd_) ||
                IsCurrentProcessWindow(activatedWindow);
            if (!snowdesktop::quick_navigation_rules::
                    ShouldCloseOnDeactivate(
                        retainedInteraction))
                return 0;
            if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
            {
                ReleaseCapture();
                quickNavTabDragIndex_ = static_cast<size_t>(-1);
                quickNavTabDragDeltaX_ = 0;
                quickNavTabDragging_ = false;
            }
            CloseQuickNavigation();
            return 0;
        }
        break;
    case WM_CLOSE:
        CloseQuickNavigation();
        return 0;
    case WM_DESTROY:
        if (quickNavigationHwnd_ == hwnd)
            quickNavigationHwnd_ = nullptr;
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

    if (message == WM_NCHITTEST)
    {
        if (app->quickNavigationAnimation_.IsAnimating())
            return HTTRANSPARENT;
        const POINT desktopPoint{
            GET_X_LPARAM(lParam) - app->virtualLeft_,
            GET_Y_LPARAM(lParam) - app->virtualTop_
        };
        const RECT search = app->GetQuickNavigationSearchRect(
            app->quickNavigationRect_);
        const RECT targetEdit{
            search.left + app->QuickNavScale(4),
            search.top + app->QuickNavScale(6),
            search.right - app->QuickNavScale(4),
            search.bottom - app->QuickNavScale(4)
        };
        RECT animatedEdit{};
        GetWindowRect(hwnd, &animatedEdit);
        OffsetRect(
            &animatedEdit,
            -app->virtualLeft_,
            -app->virtualTop_);
        if (!snowdesktop::quick_navigation_rules::
                ShouldAcceptPointerHit(
                    app->quickNavigationOpen_,
                    desktopPoint,
                    targetEdit,
                    animatedEdit))
        {
            return HTTRANSPARENT;
        }
    }

    if (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
    {
        const HWND activatedWindow = reinterpret_cast<HWND>(lParam);
        const bool retainedInteraction =
            activatedWindow == app->quickNavigationHwnd_ ||
            app->quickNavBackdropCompositor_.IsBackdropWindow(
                activatedWindow) ||
            IsWindowOwnedBy(
                activatedWindow,
                app->quickNavigationHwnd_) ||
            IsCurrentProcessWindow(activatedWindow);
        if (snowdesktop::quick_navigation_rules::
                ShouldCloseOnDeactivate(
                    retainedInteraction))
        {
            app->CloseQuickNavigation();
            return 0;
        }
    }

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
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
        app->quickNavigationSearchCompositionText_.empty() &&
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
        app->ClearQuickNavigationSearchCompositionText();
    }
    if (message == WM_IME_COMPOSITION)
    {
        app->RefreshQuickNavigationSearchCompositionText(hwnd, lParam);
    }
    if (message == WM_IME_ENDCOMPOSITION)
    {
        app->ClearQuickNavigationSearchCompositionText();
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}
