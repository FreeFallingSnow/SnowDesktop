#include "app.h"

// Desktop page-navigation layout and rendering.

void DesktopApp::GetNavButtonRects(RECT& outPrev, RECT& outNext) const
{
    outPrev = {};
    outNext = {};
    if (gridPages_.empty()) return;

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

    constexpr LONG buttonW = 40, buttonH = 96, padX = 0;
    const LONG cy = (targetPage->workArea.top + targetPage->workArea.bottom) / 2;
    const LONG halfH = buttonH / 2;

    outPrev = MakeRect(
        targetPage->workArea.left + padX, cy - halfH,
        targetPage->workArea.left + padX + buttonW, cy + halfH);
    outNext = MakeRect(
        targetPage->workArea.right - padX - buttonW, cy - halfH,
        targetPage->workArea.right - padX, cy + halfH);
}

void DesktopApp::DrawPageNavButtons(ID2D1DeviceContext* ctx)
{
    if (MaxPageOffset() <= 0) return;
    if (gridPages_.empty()) return;

    // 默认隐藏翻页按钮；仅在换页通知期间或拖拽悬停时显示，提示用户按钮位置
    const bool dragging = (dragSession_.IsActive() &&
        (!dragSession_.Items().empty() ||
            dragDropController_.IsTransportActive())) ||
        widgetAction_ == WidgetAction::Move;
    const bool hovering = (navHoverSide_ != 0);
    if (!pageNotifyActive_ && !dragging && !hovering) return;

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    auto drawArrow = [&](const RECT& rect, const std::wstring& arrow, bool enabled, bool hovered) {
        D2D1_RECT_F d2dRect = D2D1::RectF(
            static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right), static_cast<float>(rect.bottom));

        if (enabled)
        {
            float bgAlpha = hovered ? 1.0f : 0.85f;
            ComPtr<ID2D1SolidColorBrush> bg;
            ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, bgAlpha), &bg);
            if (bg) ctx->FillRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), bg.Get());

            ComPtr<ID2D1SolidColorBrush> borderBrush;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f), &borderBrush);
            if (borderBrush) ctx->DrawRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), borderBrush.Get(), 1.0f);
        }
        else
        {
            // 置灰：半透明深色背景
            ComPtr<ID2D1SolidColorBrush> bg;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.35f), &bg);
            if (bg) ctx->FillRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), bg.Get());
        }

        float textAlpha = enabled ? (hovered ? 1.0f : 0.65f) : 0.3f;
        ComPtr<ID2D1SolidColorBrush> textBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, textAlpha), &textBrush);
        if (!textBrush || !dwriteFactory_) return;

        float w = static_cast<float>(rect.right - rect.left);
        float h = static_cast<float>(rect.bottom - rect.top);

        ComPtr<IDWriteTextFormat> arrowFmt;
        dwriteFactory_->CreateTextFormat(L"Segoe UI Symbol", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"", &arrowFmt);
        if (!arrowFmt) return;
        arrowFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        arrowFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(arrow.c_str(),
            static_cast<UINT32>(arrow.size()), arrowFmt.Get(), w, h, &layout)) && layout)
        {
            ctx->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)),
                layout.Get(), textBrush.Get());
        }
    };

    bool hoverPrev = (navHoverSide_ == -1);
    bool hoverNext = (navHoverSide_ == 1);

    drawArrow(prevRect, L"\u25C0", hasPrev, dragging || hoverPrev);
    drawArrow(nextRect, L"\u25B6", hasNext, dragging || hoverNext);
}
