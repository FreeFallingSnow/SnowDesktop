#include "app.h"

// Transient page, privacy and widget-positioning overlays.

void DesktopApp::ShowPageNotify(const std::wstring& text)
{
    if (text.empty()) return;
    pageNotifyText_ = text;
    PreparePageNotifyTextCache();
    pageNotifyStartTick_ = GetTickCount();
    pageNotifyActive_ = true;
    pageNotifyUseAnimation_ =
        snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled();
    if (pageNotifyUseAnimation_)
    {
        PreparePageNotifyAnimationCache();
        const RECT bounds = pageNotifyAnimationOverlay_.bounds;
        const POINT anchor{
            (bounds.left + bounds.right) / 2,
            (bounds.top + bounds.bottom) / 2,
        };
        pageNotifyCompositorDriven_ =
            AnimateCompositionAnimationOverlay(
                pageNotifyAnimationOverlay_,
                1.0f, 1.0f, anchor,
                0.0f, 1.0f,
                kPageNotifyFadeMs);
        if (!pageNotifyCompositorDriven_)
            UpdatePageNotifyCompositionAnimation(0.0f);
    }
    else
    {
        ResetPageNotifyAnimationCache();
    }
    if (pageNotifyFadeOutToken_)
        uiAnimationScheduler_.Cancel(pageNotifyFadeOutToken_);
    const UINT wakeDelay = pageNotifyUseAnimation_
        ? kPageNotifyVisibleMs - kPageNotifyFadeMs
        : kPageNotifyVisibleMs;
    pageNotifyFadeOutToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            wakeDelay,
            [this](snowdesktop::UiScheduleToken token) {
                if (pageNotifyFadeOutToken_ == token)
                    pageNotifyFadeOutToken_ = 0;
                if (!pageNotifyActive_)
                    return;
                if (pageNotifyUseAnimation_)
                {
                    if (pageNotifyCompositorDriven_)
                    {
                        const RECT bounds =
                            pageNotifyAnimationOverlay_.bounds;
                        const POINT anchor{
                            (bounds.left + bounds.right) / 2,
                            (bounds.top + bounds.bottom) / 2,
                        };
                        if (AnimateCompositionAnimationOverlay(
                                pageNotifyAnimationOverlay_,
                                1.0f, 1.0f, anchor,
                                1.0f, 0.0f,
                                kPageNotifyFadeMs))
                        {
                            pageNotifyFadeOutToken_ =
                                uiAnimationScheduler_.ScheduleOnce(
                                    kPageNotifyFadeMs + 2,
                                    [this](
                                        snowdesktop::UiScheduleToken
                                            completionToken) {
                                        if (pageNotifyFadeOutToken_ !=
                                                completionToken)
                                            return;
                                        pageNotifyFadeOutToken_ = 0;
                                        if (!pageNotifyActive_)
                                            return;
                                        const RECT dirty =
                                            GetPageNotifyBounds();
                                        pageNotifyActive_ = false;
                                        pageNotifyText_.clear();
                                        ResetPageNotifyTextCache();
                                        if (hwnd_ && IsWindow(hwnd_))
                                        {
                                            InvalidateRect(
                                                hwnd_,
                                                IsRectEmpty(&dirty)
                                                    ? nullptr : &dirty,
                                                FALSE);
                                        }
                                    });
                            if (pageNotifyFadeOutToken_)
                                return;
                            pageNotifyCompositorDriven_ = false;
                        }
                        pageNotifyCompositorDriven_ = false;
                    }
                    EnsureUiAnimationFrame();
                    return;
                }
                const RECT dirty = GetPageNotifyBounds();
                pageNotifyActive_ = false;
                pageNotifyText_.clear();
                ResetPageNotifyTextCache();
                if (hwnd_ && IsWindow(hwnd_))
                {
                    InvalidateRect(
                        hwnd_,
                        IsRectEmpty(&dirty) ? nullptr : &dirty,
                        FALSE);
                }
            });
    if (!pageNotifyFadeOutToken_ && pageNotifyUseAnimation_)
        pageNotifyCompositorDriven_ = false;
    if (pageNotifyUseAnimation_ &&
        !pageNotifyCompositorDriven_)
        EnsureUiAnimationFrame();
    if (hwnd_)
    {
        const RECT dirty = GetPageNotifyBounds();
        InvalidateRect(
            hwnd_, IsRectEmpty(&dirty) ? nullptr : &dirty,
            FALSE);
    }
}

RECT DesktopApp::GetPageNotifyBounds() const
{
    const std::vector<size_t> order = BuildMonitorRenderOrder();
    if (order.empty() || order.back() >= gridPages_.size())
        return {};

    const GridPage& targetPage = gridPages_[order.back()];
    constexpr float padX = 28.0f;
    constexpr float padY = 14.0f;
    const float boxWidth = pageNotifyTextMetrics_.width + padX * 2.0f;
    const float boxHeight = pageNotifyTextMetrics_.height + padY * 2.0f;
    RECT bounds = MakeRect(
        targetPage.workArea.left + 20,
        targetPage.workArea.top + 20,
        static_cast<LONG>(std::ceil(
            static_cast<float>(targetPage.workArea.left) +
            28.0f + boxWidth)),
        static_cast<LONG>(std::ceil(
            static_cast<float>(targetPage.workArea.top) +
            28.0f + boxHeight)));
    return bounds;
}

void DesktopApp::ResetPageNotifyTextCache()
{
    ResetPageNotifyAnimationCache();
    pageNotifyTextLayout_.Reset();
    pageNotifyTextFormat_.Reset();
    pageNotifyTextMetrics_ = {};
}

void DesktopApp::ResetPageNotifyAnimationCache()
{
    pageNotifyCompositorDriven_ = false;
    ResetCompositionAnimationOverlay(
        pageNotifyAnimationOverlay_);
    pageNotifyAnimationRenderCache_.Reset();
    pageNotifyAnimationCacheRect_ = {};
}

void DesktopApp::PreparePageNotifyAnimationCache()
{
    ResetPageNotifyAnimationCache();
    if (!d2dDevice_ || !pageNotifyTextLayout_ ||
        !pageNotifyActive_)
        return;
    pageNotifyAnimationCacheRect_ = GetPageNotifyBounds();
    if (IsRectEmpty(&pageNotifyAnimationCacheRect_))
        return;
    const UINT width = static_cast<UINT>(std::max<LONG>(
        1, pageNotifyAnimationCacheRect_.right -
            pageNotifyAnimationCacheRect_.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(
        1, pageNotifyAnimationCacheRect_.bottom -
            pageNotifyAnimationCacheRect_.top));
    const bool ready = pageNotifyAnimationRenderCache_.Ensure(
        d2dDevice_.Get(), D2D1::SizeU(width, height), 1,
        [&](ID2D1DeviceContext* cacheContext) {
            cacheContext->SetTransform(
                D2D1::Matrix3x2F::Translation(
                    static_cast<float>(
                        -pageNotifyAnimationCacheRect_.left),
                    static_cast<float>(
                        -pageNotifyAnimationCacheRect_.top)));
            DrawPageNotify(cacheContext, false);
        });
    if (!ready)
    {
        pageNotifyAnimationCacheRect_ = {};
        return;
    }
    PrepareCompositionAnimationOverlay(
        pageNotifyAnimationOverlay_,
        pageNotifyAnimationRenderCache_,
        pageNotifyAnimationCacheRect_,
        UiCompositionAnimationHost::Desktop);
    brushCache_.clear();
    brushCacheContext_ = nullptr;
}

bool DesktopApp::UpdatePageNotifyCompositionAnimation(
    float opacity, bool commit)
{
    if (!pageNotifyAnimationOverlay_.active)
        return false;
    const RECT bounds = pageNotifyAnimationOverlay_.bounds;
    const POINT anchor{
        (bounds.left + bounds.right) / 2,
        (bounds.top + bounds.bottom) / 2,
    };
    return UpdateCompositionAnimationOverlay(
        pageNotifyAnimationOverlay_, 1.0f,
        anchor, opacity, commit);
}

void DesktopApp::PreparePageNotifyTextCache()
{
    ResetPageNotifyTextCache();
    auto* dwrite = GetDWriteFactory();
    if (!dwrite || pageNotifyText_.empty())
        return;
    if (FAILED(dwrite->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            42.0f, L"", &pageNotifyTextFormat_)) ||
        !pageNotifyTextFormat_)
        return;
    pageNotifyTextFormat_->SetTextAlignment(
        DWRITE_TEXT_ALIGNMENT_CENTER);
    pageNotifyTextFormat_->SetParagraphAlignment(
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    pageNotifyTextFormat_->SetWordWrapping(
        DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteTextLayout> measured;
    if (FAILED(dwrite->CreateTextLayout(
            pageNotifyText_.c_str(),
            static_cast<UINT32>(pageNotifyText_.size()),
            pageNotifyTextFormat_.Get(),
            2000.0f, 200.0f, &measured)) ||
        !measured ||
        FAILED(measured->GetMetrics(&pageNotifyTextMetrics_)))
    {
        ResetPageNotifyTextCache();
        return;
    }
    if (FAILED(dwrite->CreateTextLayout(
            pageNotifyText_.c_str(),
            static_cast<UINT32>(pageNotifyText_.size()),
            pageNotifyTextFormat_.Get(),
            std::max(1.0f, pageNotifyTextMetrics_.width),
            std::max(1.0f, pageNotifyTextMetrics_.height),
            &pageNotifyTextLayout_)))
    {
        ResetPageNotifyTextCache();
    }
}

/**
 * @brief 绘制换页通知覆盖层（左上角角标，类似电视台换台）。
 *
 * 显示 kPageNotifyVisibleMs 毫秒，最后 kPageNotifyFadeMs 毫秒淡出。
 * 位置：末屏左上角（若有末屏），否则主屏左上角。
 * @param ctx D2D 设备上下文。
 */
void DesktopApp::DrawPageNotify(
    ID2D1DeviceContext* ctx,
    bool applyAnimation)
{
    if (!ctx || !pageNotifyActive_ || pageNotifyText_.empty()) return;

    if (applyAnimation && pageNotifyAnimationOverlay_.active)
        return;

    const DWORD now = GetTickCount();
    const DWORD elapsed = now - pageNotifyStartTick_;
    if (applyAnimation && elapsed >= kPageNotifyVisibleMs)
    {
        pageNotifyActive_ = false;
        pageNotifyText_.clear();
        ResetPageNotifyTextCache();
        return;
    }

    // 计算透明度：前 kPageNotifyFadeMs 淡入，最后 kPageNotifyFadeMs 淡出。
    // 系统关闭动画时直接呈现稳定终态，由单次截止时间负责清理。
    float alpha = 1.0f;
    const DWORD fadeMs = kPageNotifyFadeMs;
    if (applyAnimation && pageNotifyUseAnimation_ && elapsed < fadeMs)
        alpha = static_cast<float>(elapsed) / static_cast<float>(fadeMs);
    else if (applyAnimation && pageNotifyUseAnimation_ &&
        elapsed > kPageNotifyVisibleMs - fadeMs)
        alpha = static_cast<float>(kPageNotifyVisibleMs - elapsed) / static_cast<float>(fadeMs);
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    // 定位：渲染顺序的末屏显示器（不依赖 lastMonitorPageId_，单屏时也能定位）
    const GridPage* targetPage = nullptr;
    {
        std::vector<size_t> order = BuildMonitorRenderOrder();
        if (!order.empty()) targetPage = &gridPages_[order.back()];
    }
    if (!targetPage) return;

    if (!pageNotifyTextLayout_)
        PreparePageNotifyTextCache();
    if (!pageNotifyTextLayout_)
        return;

    // 背景圆角矩形（半透明深色）
    const float padX = 28.0f;
    const float padY = 14.0f;
    const float boxW = pageNotifyTextMetrics_.width + padX * 2.0f;
    const float boxH = pageNotifyTextMetrics_.height + padY * 2.0f;
    const float boxLeft = static_cast<float>(targetPage->workArea.left) + 24.0f;
    const float boxTop = static_cast<float>(targetPage->workArea.top) + 24.0f;

    const RECT backgroundRect = MakeRect(
        static_cast<LONG>(std::floor(boxLeft)),
        static_cast<LONG>(std::floor(boxTop)),
        static_cast<LONG>(std::ceil(boxLeft + boxW)),
        static_cast<LONG>(std::ceil(boxTop + boxH)));
    DrawD2DRoundedRectangle(
        ctx, backgroundRect, 10.0f,
        D2D1::ColorF(
            0.05f, 0.05f, 0.08f, 0.72f * alpha),
        D2D1::ColorF(
            0.4f, 0.6f, 1.0f, 0.5f * alpha),
        1.5f);

    // 文本（带阴影）
    const float textX = boxLeft + padX;
    const float textY = boxTop + padY;

    ComPtr<ID2D1SolidColorBrush> shadowBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f * alpha), &shadowBrush);
    if (shadowBrush)
        ctx->DrawTextLayout(D2D1::Point2F(textX + 2.0f, textY + 2.0f),
            pageNotifyTextLayout_.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    ComPtr<ID2D1SolidColorBrush> textBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.97f * alpha), &textBrush);
    if (textBrush)
        ctx->DrawTextLayout(D2D1::Point2F(textX, textY),
            pageNotifyTextLayout_.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DesktopApp::DrawHiddenHintOverlay(ID2D1DeviceContext* ctx)
{
    if (!ctx || !showHiddenHint_) return;

    auto* dwrite = GetDWriteFactory();
    if (!dwrite) return;

    RECT workArea{};
    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        const GridPage* page = GridPageFromScreenPoint(cursor);
        if (page) workArea = page->workArea;
    }
    if (IsRectEmptyRect(workArea))
    {
        if (const GridPage* firstPage = GetFirstPageGridPage())
            workArea = firstPage->workArea;
        if (IsRectEmptyRect(workArea))
        {
            workArea.left = 0;
            workArea.top = 0;
            workArea.right = GetSystemMetrics(SM_CXSCREEN);
            workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    const std::wstring hintText = _LW("app.overlay.hide_hint");

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &fmt)) || !fmt)
        return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Measure text width using a temporary layout
    ComPtr<IDWriteTextLayout> measureLayout;
    if (SUCCEEDED(dwrite->CreateTextLayout(hintText.c_str(),
        static_cast<UINT32>(hintText.size()), fmt.Get(), 2000.0f, 40.0f, &measureLayout)) && measureLayout)
    {
        DWRITE_TEXT_METRICS metrics{};
        measureLayout->GetMetrics(&metrics);

        constexpr float hintPadding = 24.0f;
        constexpr float hintHeight = 36.0f;
        constexpr float marginTop = 60.0f;

        const float textW = metrics.width + hintPadding * 2.0f;
        const int areaW = workArea.right - workArea.left;

        RECT hintRect = MakeRect(
            static_cast<int>(workArea.left + (areaW - textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop),
            static_cast<int>(workArea.left + (areaW + textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop + hintHeight));

        DrawD2DRoundedRectangle(ctx, hintRect, 10.0f,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f),
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);

        DrawD2DText(ctx, hintText, hintRect, fmt.Get(),
            D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.90f));
    }
}

void DesktopApp::DrawWidgetAddedHintOverlay(ID2D1DeviceContext* ctx)
{
    if (!ctx || !showWidgetAddedHint_) return;

    auto* dwrite = GetDWriteFactory();
    if (!dwrite) return;

    RECT workArea{};
    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        const GridPage* page = GridPageFromScreenPoint(cursor);
        if (page) workArea = page->workArea;
    }
    if (IsRectEmptyRect(workArea))
    {
        if (const GridPage* firstPage = GetFirstPageGridPage())
            workArea = firstPage->workArea;
        if (IsRectEmptyRect(workArea))
        {
            workArea.left = 0;
            workArea.top = 0;
            workArea.right = GetSystemMetrics(SM_CXSCREEN);
            workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    const std::wstring hintText =
        _LW("app.overlay.widget_move_hint");

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &fmt)) || !fmt)
        return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteTextLayout> measureLayout;
    if (SUCCEEDED(dwrite->CreateTextLayout(hintText.c_str(),
        static_cast<UINT32>(hintText.size()), fmt.Get(), 2000.0f, 40.0f, &measureLayout)) && measureLayout)
    {
        DWRITE_TEXT_METRICS metrics{};
        measureLayout->GetMetrics(&metrics);

        constexpr float hintPadding = 24.0f;
        constexpr float hintHeight = 36.0f;
        constexpr float marginTop = 60.0f;

        const float textW = metrics.width + hintPadding * 2.0f;
        const int areaW = workArea.right - workArea.left;

        RECT hintRect = MakeRect(
            static_cast<int>(workArea.left + (areaW - textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop),
            static_cast<int>(workArea.left + (areaW + textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop + hintHeight));

        DrawD2DRoundedRectangle(ctx, hintRect, 10.0f,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f),
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);

        DrawD2DText(ctx, hintText, hintRect, fmt.Get(),
            D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.90f));
    }
}
