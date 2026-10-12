#include "ui/render/app_font.h"
#include "app/app.h"
#include "layout/page_navigation_rules.h"

// Desktop page-navigation layout and rendering.

const GridPage* DesktopApp::GetPageNavigationGridPage() const
{
    if (gridPages_.empty()) return nullptr;

    const GridPage* targetPage = nullptr;
    for (const auto& page : gridPages_)
    {
        if (!lastMonitorPageId_.empty() && page.id == lastMonitorPageId_)
        {
            targetPage = &page;
            break;
        }
    }
    if (!targetPage)
    {
        // 回退到渲染顺序的末屏（双锚点下可能不是 bounds.left 最右者）
        std::vector<size_t> order = BuildMonitorRenderOrder();
        if (!order.empty()) targetPage = &gridPages_[order.back()];
    }
    if (!targetPage) targetPage = &gridPages_.back();
    return targetPage;
}

void DesktopApp::GetNavHotEdgeRects(
    RECT& outPrev, RECT& outNext) const
{
    outPrev = {};
    outNext = {};
    const GridPage* targetPage = GetPageNavigationGridPage();
    if (!targetPage) return;
    snowdesktop::page_navigation_rules::BuildHotEdgeRects(
        targetPage->bounds, targetPage->dpiX,
        outPrev, outNext);
}

std::wstring DesktopApp::GetPageNavHotEdgeHintText(int side) const
{
    const bool dragging = widgetAction_ == WidgetAction::Move ||
        dragSession_.IsActive() || dragDropController_.IsTransportActive();
    const UINT modifiers = side < 0
        ? generalSettings_.pageNavigationPreviousModifiers
        : generalSettings_.pageNavigationNextModifiers;
    const UINT virtualKey = side < 0
        ? generalSettings_.pageNavigationPreviousVirtualKey
        : generalSettings_.pageNavigationNextVirtualKey;
    std::wstring message;
    if (dragging)
    {
        message = _LW("app.navigation.edge_drag_dwell");
    }
    else if (generalSettings_.pageNavigationKeyboardEnabled &&
        virtualKey != 0)
    {
        NavigationSettings displaySettings;
        displaySettings.modifiers = modifiers;
        displaySettings.virtualKey = virtualKey;
        const std::wstring shortcut =
            FormatNavigationHotkey(displaySettings);
        message = _LFW(
            side < 0
                ? "app.navigation.edge_previous_with_key"
                : "app.navigation.edge_next_with_key",
            shortcut);
    }
    else
    {
        message = _LW(side < 0
            ? "app.navigation.edge_previous"
            : "app.navigation.edge_next");
    }
    message.insert(0, side < 0
        ? L"\u25C0  " : L"\u25B6  ");
    return message;
}

RECT DesktopApp::GetPageNavHotEdgeHintBounds(
    int side, POINT point) const
{
    const GridPage* page = GetPageNavigationGridPage();
    if (!page || (side != -1 && side != 1)) return {};

    const float scale = std::max(
        1.0f, static_cast<float>(page->dpiX) /
            static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    if (!dwriteFactory_) return {};
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(snowdesktop::CreateNativeTooltipTextFormat(dwriteFactory_.Get(), &format, scale))) return {};
    snowdesktop::NativeTooltipTextLayout measured;
    snowdesktop::NativeTooltipLayoutOptions options;
    options.scale = scale;
    if (FAILED(snowdesktop::MeasureNativeTooltip(dwriteFactory_.Get(), format.Get(), {}, GetPageNavHotEdgeHintText(side),
        std::max(1.f, std::min(520.f * scale, static_cast<float>(page->workArea.right - page->workArea.left) - 8.f * scale)),
        std::max(1.f, std::min(192.f * scale, static_cast<float>(page->workArea.bottom - page->workArea.top) - 8.f * scale)),
        measured, options))) return {};
    const LONG width = static_cast<LONG>(std::ceil(measured.width));
    const LONG height = static_cast<LONG>(std::ceil(measured.height));
    const LONG gap = static_cast<LONG>(8.0f * scale);
    RECT previousEdge{};
    RECT nextEdge{};
    GetNavHotEdgeRects(previousEdge, nextEdge);
    const RECT edge = side < 0 ? previousEdge : nextEdge;

    LONG top = point.y - height / 2;
    top = std::clamp(
        top,
        page->workArea.top,
        std::max(page->workArea.top,
            page->workArea.bottom - height));
    LONG left = side < 0
        ? edge.right + gap
        : edge.left - gap - width;
    left = std::clamp(
        left,
        page->workArea.left,
        std::max(page->workArea.left,
            page->workArea.right - width));
    return MakeRect(left, top, left + width, top + height);
}

void DesktopApp::SetPageNavHotEdgeHover(int side)
{
    if (side != -1 && side != 1)
        side = 0;
    if (navHoverSide_ == side &&
        navHotEdgeHover_ == (side != 0))
        return;

    if (navHotEdgeHintToken_)
        uiAnimationScheduler_.Cancel(navHotEdgeHintToken_);
    navHotEdgeHintToken_ = 0;
    navHoverSide_ = side;
    navHotEdgeHover_ = side != 0;
    navHotEdgeHintVisible_ = false;
    if (side == 0) return;

    navHotEdgeHintToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            snowdesktop::page_navigation_rules::
                kHotEdgeHintDelayMs,
            [this, side](snowdesktop::UiScheduleToken token) {
                if (navHotEdgeHintToken_ != token)
                    return;
                navHotEdgeHintToken_ = 0;
                if (!navHotEdgeHover_ ||
                    navHoverSide_ != side)
                    return;
                navHotEdgeHintVisible_ = true;
                RECT dirty = GetPageNavHotEdgeHintBounds(
                    side, lastMousePoint_);
                InflateRect(&dirty, 4, 4);
                if (!PresentDesktopForegroundComposition(dirty) &&
                    hwnd_ && IsWindow(hwnd_))
                {
                    InvalidateRect(hwnd_, &dirty, FALSE);
                    PresentDesktopPointerUpdate();
                }
            });
}

void DesktopApp::RefreshPageNavHotEdgeHoverAt(POINT point)
{
    lastMousePoint_ = point;
    int side = 0;
    if (!desktopIconsHidden_ &&
        (MaxPageOffset() > 0 || pageOffset_ > 0))
    {
        RECT previousEdge{};
        RECT nextEdge{};
        GetNavHotEdgeRects(previousEdge, nextEdge);
        const auto target = snowdesktop::page_navigation_rules::
            HitTestPointerTarget(point, previousEdge, nextEdge);
        const int direction = snowdesktop::page_navigation_rules::
            PointerTargetDirection(target);
        if ((direction == -1 && pageOffset_ > 0) ||
            (direction == 1 && pageOffset_ < MaxPageOffset()))
            side = direction;
    }
    SetPageNavHotEdgeHover(side);
}

void DesktopApp::DrawPageNavHotEdgeHint(
    ID2D1DeviceContext* ctx)
{
    auto tooltipPaint = navHotEdgeHintBackdrop_.BeginPaint([this](std::uintptr_t key) { ClearInlineTooltipBackdrop(key); });
    if (!ctx) return;
    const bool dragging =
        widgetAction_ == WidgetAction::Move ||
        dragSession_.IsActive() ||
        dragDropController_.IsTransportActive();
    const bool hovering =
        navHotEdgeHover_ &&
        (navHoverSide_ == -1 || navHoverSide_ == 1);
    if (!dragging && !hovering) return;

    const auto rails = snowdesktop::page_navigation_rules::
        ResolveHotEdgeRailVisibility(
            dragging,
            hovering ? navHoverSide_ : 0,
            pageOffset_ > 0,
            pageOffset_ < MaxPageOffset());
    if (!rails.previous && !rails.next) return;

    const GridPage* page = GetPageNavigationGridPage();
    if (!page) return;
    const float scale = std::max(
        1.0f, static_cast<float>(page->dpiX) /
            static_cast<float>(USER_DEFAULT_SCREEN_DPI));

    RECT previousEdge{};
    RECT nextEdge{};
    GetNavHotEdgeRects(previousEdge, nextEdge);

    ComPtr<ID2D1SolidColorBrush> accentBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.18f, 0.45f, 0.90f, 0.72f),
        &accentBrush);
    if (accentBrush)
    {
        if (rails.previous)
            ctx->FillRectangle(
                ToD2DRect(previousEdge), accentBrush.Get());
        if (rails.next)
            ctx->FillRectangle(
                ToD2DRect(nextEdge), accentBrush.Get());
    }

    const bool hoveredDirectionVisible = hovering &&
        (navHoverSide_ < 0 ? rails.previous : rails.next);
    if (!hoveredDirectionVisible ||
        (!dragging && !navHotEdgeHintVisible_))
        return;

    const std::wstring message = GetPageNavHotEdgeHintText(navHoverSide_);

    const RECT hint = GetPageNavHotEdgeHintBounds(
        navHoverSide_, lastMousePoint_);
    if (IsRectEmptyRect(hint)) return;
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(snowdesktop::CreateNativeTooltipTextFormat(dwriteFactory_.Get(), &format, scale))) return;
    snowdesktop::NativeTooltipTextLayout measured;
    snowdesktop::NativeTooltipLayoutOptions options;
    options.scale = scale;
    if (FAILED(snowdesktop::MeasureNativeTooltip(dwriteFactory_.Get(), format.Get(), {}, message,
        static_cast<float>(hint.right - hint.left), static_cast<float>(hint.bottom - hint.top), measured, options)))
        return;
    tooltipPaint.Keep(DrawInlineTooltip(ctx, hint, measured, scale, navHotEdgeHintBackdrop_));
}
