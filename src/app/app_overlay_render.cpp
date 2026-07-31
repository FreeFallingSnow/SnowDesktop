#include "app.h"

// Transient page, privacy and widget-positioning overlays.

void DesktopApp::ShowPageNotify(const std::wstring& text)
{
    if (text.empty()) return;
    pageNotifyText_ = text;
    pageNotifyStartTick_ = GetTickCount();
    pageNotifyActive_ = true;
    SetTimer(hwnd_, kPageNotifyTimerId, kPageNotifyTimerIntervalMs, nullptr);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 绘制换页通知覆盖层（左上角角标，类似电视台换台）。
 *
 * 显示 kPageNotifyVisibleMs 毫秒，最后 kPageNotifyFadeMs 毫秒淡出。
 * 位置：末屏左上角（若有末屏），否则主屏左上角。
 * @param ctx D2D 设备上下文。
 */
void DesktopApp::DrawPageNotify(ID2D1DeviceContext* ctx)
{
    if (!ctx || !pageNotifyActive_ || pageNotifyText_.empty()) return;

    const DWORD now = GetTickCount();
    const DWORD elapsed = now - pageNotifyStartTick_;
    if (elapsed >= kPageNotifyVisibleMs)
    {
        pageNotifyActive_ = false;
        pageNotifyText_.clear();
        KillTimer(hwnd_, kPageNotifyTimerId);
        return;
    }

    // 计算透明度：前 kPageNotifyFadeMs 淡入，最后 kPageNotifyFadeMs 淡出
    float alpha = 1.0f;
    const DWORD fadeMs = kPageNotifyFadeMs;
    if (elapsed < fadeMs)
        alpha = static_cast<float>(elapsed) / static_cast<float>(fadeMs);
    else if (elapsed > kPageNotifyVisibleMs - fadeMs)
        alpha = static_cast<float>(kPageNotifyVisibleMs - elapsed) / static_cast<float>(fadeMs);
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    // 定位：渲染顺序的末屏显示器（不依赖 lastMonitorPageId_，单屏时也能定位）
    const GridPage* targetPage = nullptr;
    {
        std::vector<size_t> order = BuildMonitorRenderOrder();
        if (!order.empty()) targetPage = &gridPages_[order.back()];
    }
    if (!targetPage) return;

    auto* dwrite = GetDWriteFactory();
    if (!dwrite) return;

    // 大号字体
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 42.0f, L"", &fmt)) || !fmt)
        return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 先用大尺寸测量文本
    ComPtr<IDWriteTextLayout> measureLayout;
    if (FAILED(dwrite->CreateTextLayout(pageNotifyText_.c_str(),
        static_cast<UINT32>(pageNotifyText_.size()),
        fmt.Get(), 2000.0f, 200.0f, &measureLayout)) || !measureLayout)
        return;

    DWRITE_TEXT_METRICS metrics{};
    measureLayout->GetMetrics(&metrics);

    // 背景圆角矩形（半透明深色）
    const float padX = 28.0f;
    const float padY = 14.0f;
    const float boxW = metrics.width + padX * 2.0f;
    const float boxH = metrics.height + padY * 2.0f;
    const float boxLeft = static_cast<float>(targetPage->workArea.left) + 24.0f;
    const float boxTop = static_cast<float>(targetPage->workArea.top) + 24.0f;

    // 用实际文本尺寸重建布局，使居中对齐生效
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(pageNotifyText_.c_str(),
        static_cast<UINT32>(pageNotifyText_.size()),
        fmt.Get(), metrics.width, metrics.height, &layout)) || !layout)
        return;

    ComPtr<ID2D1SolidColorBrush> bgBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.05f, 0.05f, 0.08f, 0.72f * alpha), &bgBrush);
    if (bgBrush)
    {
        D2D1_RECT_F bgRect = D2D1::RectF(boxLeft, boxTop, boxLeft + boxW, boxTop + boxH);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(bgRect, 10.0f, 10.0f), bgBrush.Get());
    }

    // 边框（细高亮线）
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.4f, 0.6f, 1.0f, 0.5f * alpha), &borderBrush);
    if (borderBrush)
    {
        D2D1_RECT_F bgRect = D2D1::RectF(boxLeft, boxTop, boxLeft + boxW, boxTop + boxH);
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(bgRect, 10.0f, 10.0f),
            borderBrush.Get(), 1.5f);
    }

    // 文本（带阴影）
    const float textX = boxLeft + padX;
    const float textY = boxTop + padY;

    ComPtr<ID2D1SolidColorBrush> shadowBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f * alpha), &shadowBrush);
    if (shadowBrush)
        ctx->DrawTextLayout(D2D1::Point2F(textX + 2.0f, textY + 2.0f),
            layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    ComPtr<ID2D1SolidColorBrush> textBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.97f * alpha), &textBrush);
    if (textBrush)
        ctx->DrawTextLayout(D2D1::Point2F(textX, textY),
            layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
