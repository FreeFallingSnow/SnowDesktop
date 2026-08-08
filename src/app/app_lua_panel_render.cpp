#include "app.h"

// Lua-widget panel layout and rendering.

RECT DesktopApp::GetLuaWidgetPanelRect() const
{
    if (luaWidgetPanelRequest_.widgetId.empty())
        return {};
    RECT work = layoutWorkArea_;
    const DesktopWidget* source = nullptr;
    for (const auto& widget : widgets_)
    {
        if (widget.id ==
            luaWidgetPanelRequest_.widgetId)
        {
            source = &widget;
            break;
        }
    }
    const GridPage* sourcePage = nullptr;
    if (source)
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == source->gridCell.pageId)
            {
                sourcePage = &page;
                break;
            }
        }
    }
    if (!sourcePage)
    {
        for (const auto& page : gridPages_)
        {
            if (PtInRect(
                    &page.bounds,
                    luaWidgetPanelAnchorPoint_))
            {
                sourcePage = &page;
                break;
            }
        }
    }
    if (sourcePage)
        work = sourcePage->workArea;
    const int availableWidth =
        std::max(1, static_cast<int>(
            work.right - work.left) - 24);
    const int availableHeight =
        std::max(1, static_cast<int>(
            work.bottom - work.top) - 24);
    const int width = std::clamp(
        luaWidgetPanelRequest_.width,
        std::min(320, availableWidth),
        availableWidth);
    const int height = std::clamp(
        luaWidgetPanelRequest_.height,
        std::min(280, availableHeight),
        availableHeight);
    int left =
        luaWidgetPanelAnchorPoint_.x + 12;
    int top =
        luaWidgetPanelAnchorPoint_.y + 12;
    if (left + width > work.right - 12)
        left =
            luaWidgetPanelAnchorPoint_.x -
            width - 12;
    if (top + height > work.bottom - 12)
        top =
            luaWidgetPanelAnchorPoint_.y -
            height - 12;
    left = std::clamp(
        left,
        static_cast<int>(work.left + 12),
        static_cast<int>(std::max<LONG>(
            work.left + 12,
            work.right - width - 12)));
    top = std::clamp(
        top,
        static_cast<int>(work.top + 12),
        static_cast<int>(std::max<LONG>(
            work.top + 12,
            work.bottom - height - 12)));
    return MakeRect(
        left, top, left + width, top + height);
}

RECT DesktopApp::GetLuaWidgetPanelContentRect() const
{
    const RECT panel = GetLuaWidgetPanelRect();
    if (IsRectEmptyRect(panel))
        return {};
    return MakeRect(
        panel.left + 18, panel.top + 56,
        panel.right - 18, panel.bottom - 18);
}

RECT DesktopApp::GetLuaWidgetPanelCloseRect() const
{
    const RECT panel = GetLuaWidgetPanelRect();
    if (IsRectEmptyRect(panel))
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
    luaWidgetPanelAnimationRenderCache_.Reset();
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
        PrepareCompositionAnimationOverlay(
            luaWidgetPanelAnimationOverlay_,
            luaWidgetPanelAnimationRenderCache_,
            luaWidgetPanelAnimationCacheRect_);
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
    const bool animationApplied =
        applyAnimation && animation.progress < 1.0f;
    if (animationApplied)
    {
        ctx->GetTransform(&previousTransform);
        const D2D1_POINT_2F origin =
            D2D1::Point2F(
                std::clamp(
                    static_cast<float>(
                        luaWidgetPanelAnchorPoint_.x),
                    static_cast<float>(
                        luaWidgetPanelRect_.left),
                    static_cast<float>(
                        luaWidgetPanelRect_.right)),
                std::clamp(
                    static_cast<float>(
                        luaWidgetPanelAnchorPoint_.y),
                    static_cast<float>(
                        luaWidgetPanelRect_.top),
                    static_cast<float>(
                        luaWidgetPanelRect_.bottom)));
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

    const bool darkText =
        widgetEngine_->RuntimeGetWidgetTheme(
            luaWidgetPanelRequest_.widgetId)
            .contentTheme == 1;
    const D2D1_COLOR_F background = darkText
        ? D2D1::ColorF(
            0.96f, 0.97f, 0.98f, 0.98f)
        : D2D1::ColorF(
            0.08f, 0.10f, 0.13f, 0.98f);
    const D2D1_COLOR_F foreground = darkText
        ? D2D1::ColorF(
            0.04f, 0.05f, 0.07f, 1.0f)
        : D2D1::ColorF(
            1.0f, 1.0f, 1.0f, 1.0f);
    DrawD2DRoundedRectangle(
        ctx, luaWidgetPanelRect_, 18.0f,
        background,
        D2D1::ColorF(
            foreground.r, foreground.g,
            foreground.b, 0.34f),
        1.2f);

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
    ctx->PushAxisAlignedClip(
        ToD2DRect(content),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    widgetEngine_->RenderWidgetPanel(
        source->id, ctx, content);
    ctx->PopAxisAlignedClip();
    if (animationApplied)
        ctx->SetTransform(previousTransform);
}

extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);

// ── Static background layer (icons + widget chrome + popup) ──
