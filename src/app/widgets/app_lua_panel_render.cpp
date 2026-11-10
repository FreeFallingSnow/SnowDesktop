#include "app/app.h"
#include "widget/runtime/widget_engine.h"
#include "common/utils.h"
#include "app/navigation/quick_navigation_theme.h"
#include "app/popup/popup_opacity_scope.h"

// Lua-widget panel layout and rendering.

RECT DesktopApp::GetLuaWidgetPanelRect() const
{
    if (luaWidgetPanelRequest_.widgetId.empty())
        return {};
    // The anchor belongs to the rendered opener, including a Lua stack's
    // visible frame. The member's stored grid page can be on another display.
    RECT work{};
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, luaWidgetPanelAnchorPoint_))
        {
            work = page.workArea;
            break;
        }
    }
    if (IsRectEmpty(&work))
    {
        const POINT screenAnchor{
            luaWidgetPanelAnchorPoint_.x + virtualLeft_,
            luaWidgetPanelAnchorPoint_.y + virtualTop_,
        };
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromPoint(screenAnchor, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            work = monitor.rcWork;
            OffsetRect(&work, -virtualLeft_, -virtualTop_);
        }
        else if (const auto* page = GetFirstPageGridPage())
            work = page->workArea;
    }
    if (IsRectEmpty(&work))
        return {};

    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    const int marginX = std::min(12, (workWidth - 1) / 2);
    const int marginY = std::min(12, (workHeight - 1) / 2);
    const int availableWidth = workWidth - 2 * marginX;
    const int availableHeight = workHeight - 2 * marginY;
    const bool popover =
        luaWidgetPanelRequest_.surface == "popover";
    const int width = std::clamp(
        luaWidgetPanelRequest_.width,
        std::min(popover ? 200 : 320, availableWidth),
        availableWidth);
    const int height = std::clamp(
        luaWidgetPanelRequest_.height,
        std::min(popover ? 120 : 280, availableHeight),
        availableHeight);
    if (luaWidgetPanelRequest_.surface == "dialog")
    {
        const int left = work.left +
            (workWidth - width) / 2;
        const int top = work.top +
            (workHeight - height) / 2;
        return MakeRect(
            left, top, left + width, top + height);
    }
    if (popover && luaWidgetPanelRequest_.hasAnchor)
    {
        constexpr int gap = 8;
        const RECT anchor = luaWidgetPanelRequest_.anchorRect;
        std::string placement = luaWidgetPanelRequest_.placement;
        if (placement == "auto")
        {
            if (work.bottom - anchor.bottom >= height + gap)
                placement = "bottomStart";
            else if (anchor.top - work.top >= height + gap)
                placement = "topStart";
            else if (work.right - anchor.right >= width + gap)
                placement = "right";
            else if (anchor.left - work.left >= width + gap)
                placement = "left";
            else
                placement = "bottomStart";
        }
        else if (placement.starts_with("bottom") &&
            work.bottom - anchor.bottom < height + gap &&
            anchor.top - work.top >= height + gap)
        {
            placement.replace(0, 6, "top");
        }
        else if (placement.starts_with("top") &&
            anchor.top - work.top < height + gap &&
            work.bottom - anchor.bottom >= height + gap)
        {
            placement.replace(0, 3, "bottom");
        }
        else if (placement == "right" &&
            work.right - anchor.right < width + gap &&
            anchor.left - work.left >= width + gap)
        {
            placement = "left";
        }
        else if (placement == "left" &&
            anchor.left - work.left < width + gap &&
            work.right - anchor.right >= width + gap)
        {
            placement = "right";
        }
        int left = anchor.left;
        int top = anchor.bottom + gap;
        if (placement == "top" || placement == "bottom")
            left = (anchor.left + anchor.right - width) / 2;
        else if (placement == "topEnd" ||
            placement == "bottomEnd")
            left = anchor.right - width;
        else if (placement == "left")
        {
            left = anchor.left - width - gap;
            top = (anchor.top + anchor.bottom - height) / 2;
        }
        else if (placement == "right")
        {
            left = anchor.right + gap;
            top = (anchor.top + anchor.bottom - height) / 2;
        }
        if (placement == "top" || placement == "topStart" ||
            placement == "topEnd")
            top = anchor.top - height - gap;
        left = std::clamp(left,
            static_cast<int>(work.left + marginX),
            static_cast<int>(work.right - width - marginX));
        top = std::clamp(top,
            static_cast<int>(work.top + marginY),
            static_cast<int>(work.bottom - height - marginY));
        return MakeRect(
            left, top, left + width, top + height);
    }
    int left =
        luaWidgetPanelAnchorPoint_.x + 12;
    int top =
        luaWidgetPanelAnchorPoint_.y + 12;
    if (left + width > work.right - marginX)
        left =
            luaWidgetPanelAnchorPoint_.x -
            width - 12;
    if (top + height > work.bottom - marginY)
        top =
            luaWidgetPanelAnchorPoint_.y -
            height - 12;
    left = std::clamp(
        left,
        static_cast<int>(work.left + marginX),
        static_cast<int>(work.right - width - marginX));
    top = std::clamp(
        top,
        static_cast<int>(work.top + marginY),
        static_cast<int>(work.bottom - height - marginY));
    return MakeRect(
        left, top, left + width, top + height);
}

POINT DesktopApp::GetLuaWidgetPanelAnimationAnchor(const RECT& panel) const
{
    if (luaWidgetPanelRequest_.surface == "dialog")
        return { (panel.left + panel.right) / 2, (panel.top + panel.bottom) / 2 };
    return { std::clamp(luaWidgetPanelAnchorPoint_.x, panel.left, panel.right),
        std::clamp(luaWidgetPanelAnchorPoint_.y, panel.top, panel.bottom) };
}

RECT DesktopApp::GetLuaWidgetPanelContentRect() const
{
    const RECT panel = GetLuaWidgetPanelRect();
    if (IsRectEmptyRect(panel))
        return {};
    const bool compact =
        luaWidgetPanelRequest_.surface == "popover";
    const int horizontalInset = compact ? 14 : 18;
    const int bottomInset = compact ? 14 : 18;
    const int topInset = luaWidgetPanelRequest_.showHeader ? 56 : 14;
    return MakeRect(
        panel.left + horizontalInset, panel.top + topInset,
        panel.right - horizontalInset, panel.bottom - bottomInset);
}

RECT DesktopApp::GetLuaWidgetPanelCloseRect() const
{
    const RECT panel = GetLuaWidgetPanelRect();
    if (IsRectEmptyRect(panel))
        return {};
    if (!luaWidgetPanelRequest_.showHeader)
        return {};
    return MakeRect(
        panel.right - 50, panel.top + 12,
        panel.right - 14, panel.top + 48);
}

void DesktopApp::ResetLuaWidgetPanelAnimationCache()
{
    if (luaWidgetPanelAnimationCompletionToken_)
    {
        uiAnimationScheduler_.Cancel(
            luaWidgetPanelAnimationCompletionToken_);
    }
    luaWidgetPanelAnimationCompletionToken_ = 0;
    luaWidgetPanelAnimationCompositorDriven_ = false;
    ResetCompositionAnimationOverlay(
        luaWidgetPanelAnimationOverlay_);
    luaWidgetPanelAnimationRenderCache_.Invalidate();
    luaWidgetPanelAnimationCacheRect_ = {};
}

void DesktopApp::PrepareLuaWidgetPanelAnimationCache()
{
    ResetLuaWidgetPanelAnimationCache();
    if (!d2dDevice_ ||
        luaWidgetPanelRequest_.widgetId.empty())
        return;

    luaWidgetPanelAnimationCacheRect_ =
        GetLuaWidgetPanelRect();
    InflateRect(&luaWidgetPanelAnimationCacheRect_, 4, 4);
    const UINT width = static_cast<UINT>(std::max<LONG>(
        1,
        luaWidgetPanelAnimationCacheRect_.right -
            luaWidgetPanelAnimationCacheRect_.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(
        1,
        luaWidgetPanelAnimationCacheRect_.bottom -
            luaWidgetPanelAnimationCacheRect_.top));

    const bool ready =
        luaWidgetPanelAnimationRenderCache_.Ensure(
            d2dDevice_.Get(), D2D1::SizeU(width, height), 1,
            [&](ID2D1DeviceContext* cacheContext) {
                cacheContext->SetTransform(
                    D2D1::Matrix3x2F::Translation(
                        static_cast<float>(
                            -luaWidgetPanelAnimationCacheRect_.left),
                        static_cast<float>(
                            -luaWidgetPanelAnimationCacheRect_.top)));
                DrawLuaWidgetPanel(cacheContext, false);
            });
    if (!ready)
        luaWidgetPanelAnimationCacheRect_ = {};
    else
    {
        (void)PrepareCompositionAnimationOverlay(
            luaWidgetPanelAnimationOverlay_,
            luaWidgetPanelAnimationRenderCache_,
            luaWidgetPanelAnimationCacheRect_,
            UiCompositionAnimationHost::FloatingPopup);
    }
    brushCache_.clear();
    brushCacheContext_ = nullptr;
}

void DesktopApp::DrawLuaWidgetPanel(
    ID2D1DeviceContext* ctx,
    bool applyAnimation)
{
    if (!ctx || !widgetEngine_ ||
        luaWidgetPanelRequest_.widgetId.empty())
        return;
    luaWidgetPanelRect_ = GetLuaWidgetPanelRect();
    const RECT content =
        GetLuaWidgetPanelContentRect();
    if (IsRectEmptyRect(luaWidgetPanelRect_) ||
        IsRectEmptyRect(content))
        return;

    const auto animation =
        luaWidgetPanelAnimation_.GetVisual();
    if (applyAnimation && !animation.visible)
        return;
    if (applyAnimation &&
        luaWidgetPanelAnimationOverlay_.active)
        return;
    D2D1_MATRIX_3X2_F previousTransform{};
    PopupOpacityScope opacity(ctx, applyAnimation, animation.opacity);
    const bool animationApplied =
        applyAnimation && animation.progress < 1.0f;
    if (animationApplied)
    {
        ctx->GetTransform(&previousTransform);
        const POINT anchor = GetLuaWidgetPanelAnimationAnchor(luaWidgetPanelRect_);
        const auto origin = D2D1::Point2F(
            static_cast<float>(anchor.x), static_cast<float>(anchor.y));
        ctx->SetTransform(
            D2D1::Matrix3x2F::Scale(
                animation.scale,
                animation.scale,
                origin) *
            previousTransform);
        if (!IsRectEmptyRect(
                luaWidgetPanelAnimationCacheRect_) &&
            luaWidgetPanelAnimationRenderCache_.DrawAt(
                ctx,
                D2D1::Point2F(
                    static_cast<float>(
                        luaWidgetPanelAnimationCacheRect_.left),
                    static_cast<float>(
                        luaWidgetPanelAnimationCacheRect_.top)),
                D2D1_INTERPOLATION_MODE_LINEAR))
        {
            ctx->SetTransform(previousTransform);
            return;
        }
    }

    const auto& appearance = collectionPopupAppearance_;
    const auto foreground = (collectionPopupLightTheme_ ? kQuickNavLight : kQuickNavDark).popupTitle;
    DrawWidgetPanelBackground(ctx, luaWidgetPanelRect_, kLuaWidgetPanelCornerRadius,
        D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB,
            std::clamp(appearance.widgetAlpha, 0.0f, 1.0f)),
        D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB,
            std::clamp(appearance.widgetBorderAlpha, 0.0f, 1.0f)),
        false, std::clamp(appearance.widgetBorderWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth),
        &appearance, false);

    if (luaWidgetPanelRequest_.showHeader)
    {
        RECT titleRect = MakeRect(
            luaWidgetPanelRect_.left + 22,
            luaWidgetPanelRect_.top + 13,
            luaWidgetPanelRect_.right - 58,
            luaWidgetPanelRect_.top + 49);
        DrawD2DTextEllipsis(
            ctx, luaWidgetPanelRequest_.title,
            titleRect, itemTextFormat_.Get(),
            foreground,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const RECT closeRect =
            GetLuaWidgetPanelCloseRect();
        const bool closeHovered =
            PtInRect(&closeRect, lastMousePoint_) != FALSE;
        if (closeHovered)
        {
            DrawD2DRoundedRectangle(
                ctx, closeRect, 9.0f,
                D2D1::ColorF(
                    foreground.r, foreground.g,
                    foreground.b, 0.12f),
                D2D1::ColorF(
                    foreground.r, foreground.g,
                    foreground.b, 0.0f),
                0.0f);
        }
        DrawD2DTextEllipsis(
            ctx, L"×", closeRect,
            itemTextFormat_.Get(), foreground,
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false);
    }

    const auto source = std::find_if(
        widgets_.begin(), widgets_.end(),
        [&](const DesktopWidget& widget) {
            return widget.id ==
                luaWidgetPanelRequest_.widgetId &&
                widget.type ==
                    DesktopWidgetType::LuaScript;
        });
    if (source == widgets_.end())
    {
        if (animationApplied)
            ctx->SetTransform(previousTransform);
        return;
    }
    widgetEngine_->EnsureWidgetLoaded(
        source->id, source->packageId);
    // Preserve Lua content metrics while letting outer tracks use host gutters.
    RECT scrollbarBounds = content;
    scrollbarBounds.right = luaWidgetPanelRect_.right - 4;
    scrollbarBounds.bottom = luaWidgetPanelRect_.bottom - 4;
    ctx->PushAxisAlignedClip(
        ToD2DRect(scrollbarBounds),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    widgetEngine_->RenderWidgetPanel(
        source->id, ctx, content,
        luaWidgetPanelRequest_.surface, &scrollbarBounds);
    ctx->PopAxisAlignedClip();
    if (animationApplied)
        ctx->SetTransform(previousTransform);
}

extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);

// ── Static background layer (icons + widget chrome + popup) ──
