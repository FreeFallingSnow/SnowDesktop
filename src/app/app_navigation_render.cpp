#include "app.h"
#include "../page_navigation_rules.h"

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

void DesktopApp::GetNavButtonRects(RECT& outPrev, RECT& outNext) const
{
    outPrev = {};
    outNext = {};
    const GridPage* targetPage = GetPageNavigationGridPage();
    if (!targetPage) return;

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

void DesktopApp::GetNavHotEdgeRects(
    RECT& outPrev, RECT& outNext) const
{
    outPrev = {};
    outNext = {};
    const GridPage* targetPage = GetPageNavigationGridPage();
    if (!targetPage) return;
    snowdesktop::page_navigation_rules::BuildHotEdgeRects(
        targetPage->workArea, targetPage->dpiX,
        outPrev, outNext);
}

RECT DesktopApp::GetPageNavHotEdgeHintBounds(
    int side, POINT point) const
{
    const GridPage* page = GetPageNavigationGridPage();
    if (!page || (side != -1 && side != 1)) return {};

    const float scale = std::max(
        1.0f, static_cast<float>(page->dpiX) /
            static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    const LONG width = static_cast<LONG>(280.0f * scale);
    const LONG height = static_cast<LONG>(44.0f * scale);
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

void DesktopApp::DrawPageNavButtons(ID2D1DeviceContext* ctx)
{
    if (MaxPageOffset() <= 0) return;
    if (gridPages_.empty()) return;

    // 默认隐藏翻页按钮；仅在换页通知期间或拖拽悬停时显示，提示用户按钮位置
    const bool dragging = (dragSession_.IsActive() &&
        (!dragSession_.Items().empty() ||
            dragDropController_.IsTransportActive())) ||
        widgetAction_ == WidgetAction::Move;
    const bool hovering =
        navHoverSide_ != 0 && !navHotEdgeHover_;
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

    bool hoverPrev = hovering && navHoverSide_ == -1;
    bool hoverNext = hovering && navHoverSide_ == 1;

    drawArrow(prevRect, L"\u25C0", hasPrev, dragging || hoverPrev);
    drawArrow(nextRect, L"\u25B6", hasNext, dragging || hoverNext);
}

void DesktopApp::DrawPageNavHotEdgeHint(
    ID2D1DeviceContext* ctx)
{
    if (!ctx || !navHotEdgeHover_ ||
        (navHoverSide_ != -1 && navHoverSide_ != 1))
        return;

    const bool enabled = navHoverSide_ < 0
        ? pageOffset_ > 0
        : pageOffset_ < MaxPageOffset();
    if (!enabled) return;

    const GridPage* page = GetPageNavigationGridPage();
    if (!page) return;
    const float scale = std::max(
        1.0f, static_cast<float>(page->dpiX) /
            static_cast<float>(USER_DEFAULT_SCREEN_DPI));

    RECT previousEdge{};
    RECT nextEdge{};
    RECT previousButton{};
    RECT nextButton{};
    GetNavHotEdgeRects(previousEdge, nextEdge);
    GetNavButtonRects(previousButton, nextButton);
    const RECT edge = navHoverSide_ < 0
        ? previousEdge : nextEdge;
    const RECT button = navHoverSide_ < 0
        ? previousButton : nextButton;

    ComPtr<ID2D1SolidColorBrush> accentBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.18f, 0.45f, 0.90f, 0.72f),
        &accentBrush);
    if (accentBrush)
    {
        RECT topRail = edge;
        topRail.bottom = std::clamp(
            button.top, edge.top, edge.bottom);
        RECT bottomRail = edge;
        bottomRail.top = std::clamp(
            button.bottom, edge.top, edge.bottom);
        if (!IsRectEmptyRect(topRail))
            ctx->FillRectangle(ToD2DRect(topRail), accentBrush.Get());
        if (!IsRectEmptyRect(bottomRail))
            ctx->FillRectangle(ToD2DRect(bottomRail), accentBrush.Get());
    }

    const UINT modifiers = navHoverSide_ < 0
        ? generalSettings_.pageNavigationPreviousModifiers
        : generalSettings_.pageNavigationNextModifiers;
    const UINT virtualKey = navHoverSide_ < 0
        ? generalSettings_.pageNavigationPreviousVirtualKey
        : generalSettings_.pageNavigationNextVirtualKey;
    std::wstring message;
    if (generalSettings_.pageNavigationKeyboardEnabled &&
        virtualKey != 0)
    {
        NavigationSettings displaySettings;
        displaySettings.modifiers = modifiers;
        displaySettings.virtualKey = virtualKey;
        const std::wstring shortcut =
            FormatNavigationHotkey(displaySettings);
        message = _LFW(
            navHoverSide_ < 0
                ? "app.navigation.edge_previous_with_key"
                : "app.navigation.edge_next_with_key",
            shortcut);
    }
    else
    {
        message = _LW(navHoverSide_ < 0
            ? "app.navigation.edge_previous"
            : "app.navigation.edge_next");
    }
    message.insert(0, navHoverSide_ < 0
        ? L"\u25C0  " : L"\u25B6  ");

    const RECT hint = GetPageNavHotEdgeHintBounds(
        navHoverSide_, lastMousePoint_);
    if (IsRectEmptyRect(hint)) return;
    const D2D1_RECT_F hintRect = ToD2DRect(hint);

    ComPtr<ID2D1SolidColorBrush> backgroundBrush;
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f),
        &backgroundBrush);
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f),
        &borderBrush);
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.08f, 0.12f, 0.18f, 0.92f),
        &textBrush);
    const float radius = 8.0f * scale;
    if (backgroundBrush)
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(hintRect, radius, radius),
            backgroundBrush.Get());
    if (borderBrush)
        ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(hintRect, radius, radius),
            borderBrush.Get(), std::max(1.0f, scale));

    if (!dwriteFactory_ || !textBrush) return;
    ComPtr<IDWriteTextFormat> format;
    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f * scale, L"", &format);
    if (!format) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    D2D1_RECT_F textRect = hintRect;
    textRect.left += 14.0f * scale;
    textRect.right -= 14.0f * scale;
    ctx->DrawTextW(
        message.c_str(), static_cast<UINT32>(message.size()),
        format.Get(), textRect, textBrush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
