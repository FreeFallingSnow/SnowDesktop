#include "app.h"
#include "../modern_menu.h"

// Transient page, privacy and widget-positioning overlays.

void DesktopApp::DrawUsageGuideHintOverlay(ID2D1DeviceContext* ctx)
{
    using namespace snowdesktop::usage_guide;
    usageGuidePauseRect_ = usageGuideSettingsRect_ = usageGuideOpenSettingsRect_ = {};
    usageGuideFrame_ = usageGuideDragRect_ = usageGuideBodyRect_ = usageGuideScrollTrack_ = usageGuideScrollThumb_ = {};
    if (!ctx || !IsUsageGuideVisible()) return;
    const auto& lesson = *Find(*usageGuideTopic_);
    const bool showSettings = HasSettings(lesson);
    const std::wstring caption = std::wstring(_LW(SectionTitle(lesson.section))) + L" · " + _LW("start.dragPanel");
    const std::wstring title = _LW(lesson.title);
    const std::wstring hint = _LW(lesson.instructions);
    const std::wstring pauseText = _LW("start.pause"), settingsText = _LW("start.returnSettings"),
        openText = _LW("start.openSettings");
    POINT cursor{}; GetCursorPos(&cursor);
    // Pick a display once; following the mouse or menus during repaint would
    // move a button out from under the user. Only a deliberate drag follows it.
    const POINT monitorPoint = usageGuidePlacement_.dragOffset ? cursor :
        usageGuidePlacement_.anchor.value_or(cursor);
    const auto* page = GridPageFromScreenPoint(monitorPoint);
    if (!page) page = GetFirstPageGridPage();
    if (!page) return;
    auto area = page->workArea;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&area), 2);
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromPoint(monitorPoint, MONITOR_DEFAULTTONEAREST), &monitor))
        area = monitor.rcWork;
    const float scale = std::max(1.0f, page->dpiX / 96.0f);
    const auto px = [scale](float value) { return static_cast<int>(std::ceil(value * scale)); };
    const int padding = px(16), margin = px(24);
    const int width = std::min(px(440), static_cast<int>(area.right - area.left) - margin * 2);
    if (width < px(240)) return;
    const int textWidth = width - padding * 2;
    const int bodyWidth = textWidth - px(16);
    auto* factory = GetDWriteFactory();
    if (!factory) return;
    DWORD textScale = 100, bytes = sizeof(textScale);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor",
        RRF_RT_REG_DWORD, nullptr, &textScale, &bytes);
    const float fontScale = scale * std::clamp(textScale / 100.0f, 1.0f, 2.25f);
    ComPtr<IDWriteTextFormat> format, titleFormat, smallFormat;
    const auto makeFormat = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& value) {
        if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * fontScale, L"", &value))) return false;
        value->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); return true;
    };
    if (!makeFormat(14, DWRITE_FONT_WEIGHT_NORMAL, format) ||
        !makeFormat(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat) ||
        !makeFormat(12, DWRITE_FONT_WEIGHT_NORMAL, smallFormat)) return;
    const auto measure = [&](const std::wstring& text, IDWriteTextFormat* style, int availableWidth) {
        ComPtr<IDWriteTextLayout> layout; DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            style, static_cast<float>(availableWidth), 100000, &layout))) layout->GetMetrics(&metrics);
        return static_cast<int>(std::ceil(metrics.height));
    };
    const int captionHeight = measure(caption, smallFormat.Get(), textWidth);
    const int titleHeight = measure(title, titleFormat.Get(), textWidth);
    const int openHeight = showSettings ? std::max(px(36), measure(openText, format.Get(), textWidth - px(16)) + px(12)) : 0;
    const int halfWidth = (textWidth - px(8)) / 2;
    const int secondaryHeight = std::max(px(36), std::max(
        measure(settingsText, smallFormat.Get(), halfWidth - px(12)),
        measure(pauseText, smallFormat.Get(), halfWidth - px(12))) + px(12));
    const int fixedHeight = padding * 2 + captionHeight + px(4) + titleHeight + px(12) +
        px(12) + secondaryHeight + (showSettings ? openHeight + px(8) : 0);
    const int bodyLimit = static_cast<int>(area.bottom - area.top) - margin * 2 - fixedHeight;
    if (bodyLimit < px(24)) return;
    const int contentHeight = measure(hint, format.Get(), bodyWidth);
    const int textHeight = std::min(bodyLimit, contentHeight);
    usageGuideScroll_.Arrange(contentHeight, textHeight);
    const int height = fixedHeight + textHeight;
    if (height + margin * 2 > area.bottom - area.top) return;
    RECT frame = usageGuidePlacement_.Arrange(area, width, height, margin);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&frame), 2);
    usageGuideFrame_ = frame;
    usageGuideDragRect_ = {frame.left, frame.top, frame.right,
        frame.top + padding + captionHeight + px(4) + titleHeight + px(6)};
    POINT clientCursor = cursor;
    ScreenToClient(hwnd_, &clientCursor);
    // This floating panel stays in the desktop foreground. Context menus use
    // their own popup windows above the desktop, without moving this panel.
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const auto systemColor = [](int index) {
        const auto color = GetSysColor(index);
        return D2D1::ColorF(GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f);
    };
    const bool light = IsLightContentTheme();
    const auto background = highContrast ? systemColor(COLOR_WINDOW) : D2D1::ColorF(light ? 0xf9f9f9 : 0x292929, 0.98f);
    const auto foreground = highContrast ? systemColor(COLOR_WINDOWTEXT) : D2D1::ColorF(light ? 0x202020 : 0xf5f5f5);
    const auto secondary = highContrast ? foreground : D2D1::ColorF(light ? 0x606060 : 0xc5c5c5);
    const auto border = highContrast ? foreground : D2D1::ColorF(light ? 0xd6d6d6 : 0x505050);
    DrawD2DRoundedRectangle(ctx, frame, 8.0f * scale, background, border, 1);
    int y = frame.top + padding;
    const auto drawLine = [&](const std::wstring& value, int h, IDWriteTextFormat* style, D2D1_COLOR_F color) {
        RECT bounds{frame.left + padding, y, frame.right - padding, y + h};
        DrawD2DText(ctx, value, bounds, style, color); y += h;
    };
    drawLine(caption, captionHeight, smallFormat.Get(), secondary); y += px(4);
    drawLine(title, titleHeight, titleFormat.Get(), foreground); y += px(12);
    usageGuideBodyRect_ = {frame.left + padding, y, frame.right - padding - px(16), y + textHeight};
    RECT textBounds = usageGuideBodyRect_;
    textBounds.top -= usageGuideScroll_.offset;
    textBounds.bottom = textBounds.top + contentHeight;
    ctx->PushAxisAlignedClip(ToD2DRect(usageGuideBodyRect_), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    DrawD2DText(ctx, hint, textBounds, format.Get(), foreground);
    ctx->PopAxisAlignedClip();
    if (usageGuideScroll_.maximum > 0)
    {
        usageGuideScrollTrack_ = {frame.right - padding - px(12), y, frame.right - padding, y + textHeight};
        const int thumbHeight = std::min(textHeight, std::max(px(32), MulDiv(textHeight, textHeight, contentHeight)));
        const int thumbTop = y + MulDiv(textHeight - thumbHeight, usageGuideScroll_.offset, usageGuideScroll_.maximum);
        usageGuideScrollThumb_ = {usageGuideScrollTrack_.left, thumbTop, usageGuideScrollTrack_.right, thumbTop + thumbHeight};
        RECT visibleThumb = usageGuideScrollThumb_; InflateRect(&visibleThumb, -px(4), 0);
        DrawD2DRoundedRectangle(ctx, visibleThumb, 2.0f * scale, secondary, secondary, 0);
    }
    y += textHeight + px(12);
    const auto drawButton = [&](RECT& bounds, const std::wstring& label, IDWriteTextFormat* style) {
        const bool hover = PtInRect(&bounds, clientCursor) != FALSE;
        const auto fill = highContrast ? background :
            D2D1::ColorF(light ? (hover ? 0xe8e8e8 : 0xf9f9f9) : (hover ? 0x414141 : 0x292929));
        DrawD2DRoundedRectangle(ctx, bounds, 4.0f * scale, fill, border, 1);
        style->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        style->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        RECT labelBounds = bounds; InflateRect(&labelBounds, -px(6), 0);
        DrawD2DText(ctx, label, labelBounds, style, foreground);
    };
    if (showSettings)
    {
        usageGuideOpenSettingsRect_ = {frame.left + padding, y, frame.right - padding, y + openHeight};
        drawButton(usageGuideOpenSettingsRect_, openText, format.Get()); y += openHeight + px(8);
    }
    usageGuideSettingsRect_ = {frame.left + padding, y, frame.left + padding + halfWidth, y + secondaryHeight};
    usageGuidePauseRect_ = {usageGuideSettingsRect_.right + px(8), y, frame.right - padding, y + secondaryHeight};
    drawButton(usageGuideSettingsRect_, settingsText, smallFormat.Get());
    drawButton(usageGuidePauseRect_, pauseText, smallFormat.Get());
}

void DesktopApp::ShowPageNotify(const std::wstring& text)
{
    if (text.empty()) return;
    if (pageNotifyAnimationFrameToken_)
        uiAnimationScheduler_.Cancel(pageNotifyAnimationFrameToken_);
    pageNotifyAnimationFrameToken_ = 0;
    pageNotifyText_ = text;
    PreparePageNotifyTextCache();
    pageNotifyStartTick_ = GetTickCount();
    pageNotifyActive_ = true;
    pageNotifyUseAnimation_ =
        snowdesktop::animation::RuntimeAnimationsEnabled();
    pageNotifyFadeMs_ = static_cast<DWORD>(std::max(1.0,
        std::round(kPageNotifyFadeMs *
            snowdesktop::animation::RuntimeDurationScale())));
    // Keep the reading pause unchanged while both fades follow animation speed.
    const DWORD visibleMs = pageNotifyUseAnimation_
        ? kPageNotifyVisibleMs - 2 * kPageNotifyFadeMs +
            2 * pageNotifyFadeMs_
        : kPageNotifyVisibleMs;
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
                pageNotifyFadeMs_);
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
        ? visibleMs - pageNotifyFadeMs_
        : visibleMs;
    pageNotifyFadeOutToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            wakeDelay,
            [this, visibleMs](snowdesktop::UiScheduleToken token) {
                if (pageNotifyFadeOutToken_ != token)
                    return;
                pageNotifyFadeOutToken_ = 0;
                if (!pageNotifyActive_)
                    return;
                const DWORD elapsed = GetTickCount() - pageNotifyStartTick_;
                if (pageNotifyUseAnimation_ &&
                    snowdesktop::animation::RuntimeAnimationsEnabled() &&
                    elapsed < visibleMs)
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
                                std::min(pageNotifyFadeMs_, visibleMs - elapsed)))
                        {
                            pageNotifyFadeOutToken_ =
                                uiAnimationScheduler_.ScheduleOnce(
                                    visibleMs - elapsed + 2,
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
    const bool hadOverlay = pageNotifyAnimationOverlay_.active;
    pageNotifyCompositorDriven_ = false;
    ResetCompositionAnimationOverlay(
        pageNotifyAnimationOverlay_);
    pageNotifyAnimationRenderCache_.Reset();
    pageNotifyAnimationCacheRect_ = {};
    if (hadOverlay)
        CommitCompositionAnimationFrame();
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
 * 保留固定阅读停留时间，淡入淡出时长遵循动画速度设置。
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
    const DWORD visibleMs = pageNotifyUseAnimation_
        ? kPageNotifyVisibleMs - 2 * kPageNotifyFadeMs +
            2 * pageNotifyFadeMs_
        : kPageNotifyVisibleMs;
    if (applyAnimation && elapsed >= visibleMs)
    {
        pageNotifyActive_ = false;
        pageNotifyText_.clear();
        ResetPageNotifyTextCache();
        return;
    }

    // 动画关闭时直接呈现稳定终态，由单次截止时间负责清理。
    float alpha = 1.0f;
    const DWORD fadeMs = pageNotifyFadeMs_;
    const bool animate = applyAnimation && pageNotifyUseAnimation_ &&
        snowdesktop::animation::RuntimeAnimationsEnabled();
    if (animate && elapsed < fadeMs)
        alpha = static_cast<float>(elapsed) / static_cast<float>(fadeMs);
    else if (animate && elapsed > visibleMs - fadeMs)
        alpha = static_cast<float>(visibleMs - elapsed) / static_cast<float>(fadeMs);
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

    const std::wstring hintText = _LW("app.overlay.widget_move_hint");

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
