/**
 * @file guide_widget.cpp
 * @brief GuideWidget —— 新页面欢迎卡片的实现
 */

#include "widget.h"
#include "app.h"
#include "../l10n.h"

#include <algorithm>

RECT GuideWidget::GetPrimaryButtonRect(RECT body) const
{
    const int pad = Cu(22.0f);
    const int gap = Cu(10.0f);
    const int height = Cu(42.0f);
    const int bottom = body.bottom - Cu(32.0f);
    const int available = std::max(
        2, static_cast<int>(body.right - body.left) - pad * 2 - gap);
    const int primaryWidth = available / 2;
    return {
        body.left + pad,
        bottom - height,
        body.left + pad + primaryWidth,
        bottom,
    };
}

RECT GuideWidget::GetSecondaryButtonRect(RECT body) const
{
    const RECT primary = GetPrimaryButtonRect(body);
    const int gap = Cu(10.0f);
    return {
        primary.right + gap,
        primary.top,
        body.right - Cu(22.0f),
        primary.bottom,
    };
}

WidgetHit GuideWidget::HitTestWidget(POINT pt) const
{
    const WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base != WidgetHit::Content)
        return base;

    const RECT body = GetBodyRect();
    const RECT primary = GetPrimaryButtonRect(body);
    if (PtInRect(&primary, pt))
        return WidgetHit::GuideAddWidgetBtn;

    const RECT secondary = GetSecondaryButtonRect(body);
    if (PtInRect(&secondary, pt))
        return WidgetHit::GuideDetailsBtn;

    return base;
}

void GuideWidget::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!context || !app_ || !data_ || IsRectEmptyRect(body))
        return;

    data_->scrollOffset = 0;

    // Guide has a product-defined light acrylic appearance and deliberately
    // does not inherit the global component preset.
    constexpr bool lightTheme = true;
    const D2D1_COLOR_F primaryText = lightTheme
        ? D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.94f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f);
    const D2D1_COLOR_F secondaryText = lightTheme
        ? D2D1::ColorF(0.10f, 0.13f, 0.18f, 0.64f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.66f);
    const D2D1_COLOR_F hintText = lightTheme
        ? D2D1::ColorF(0.10f, 0.13f, 0.18f, 0.48f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f);
    const D2D1_COLOR_F accent = D2D1::ColorF(0.18f, 0.47f, 0.96f, 0.96f);

    size_t pageIndex = 0;
    const auto page = std::find(
        app_->savedPageIds_.begin(), app_->savedPageIds_.end(),
        data_->gridCell.pageId);
    if (page != app_->savedPageIds_.end())
        pageIndex = static_cast<size_t>(page - app_->savedPageIds_.begin());
    const size_t pageCount = std::max<size_t>(1, app_->savedPageIds_.size());
    const size_t monitorCount = app_->gridPages_.size();

    const RECT primaryButton = GetPrimaryButtonRect(body);
    const RECT secondaryButton = GetSecondaryButtonRect(body);
    const bool primaryHovered =
        PtInRect(&primaryButton, app_->lastMousePoint_) != FALSE;
    const bool secondaryHovered =
        PtInRect(&secondaryButton, app_->lastMousePoint_) != FALSE;
    const bool primaryPressed = primaryHovered && app_->mouseDown_ &&
        app_->pendingGuideAction_ == WidgetHit::GuideAddWidgetBtn;
    const bool secondaryPressed = secondaryHovered && app_->mouseDown_ &&
        app_->pendingGuideAction_ == WidgetHit::GuideDetailsBtn;

    const D2D1_COLOR_F secondaryFill = lightTheme
        ? D2D1::ColorF(0.04f, 0.08f, 0.14f,
            secondaryPressed ? 0.16f : (secondaryHovered ? 0.11f : 0.065f))
        : D2D1::ColorF(1.0f, 1.0f, 1.0f,
            secondaryPressed ? 0.20f : (secondaryHovered ? 0.14f : 0.085f));
    const D2D1_COLOR_F secondaryStroke = lightTheme
        ? D2D1::ColorF(0.05f, 0.09f, 0.16f, 0.13f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    app_->DrawD2DRoundedRectangle(
        context, primaryButton, static_cast<float>(Cu(10.0f)),
        primaryPressed
            ? D2D1::ColorF(0.13f, 0.39f, 0.84f, 1.0f)
            : (primaryHovered
            ? D2D1::ColorF(0.23f, 0.52f, 1.0f, 1.0f)
            : accent),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
    app_->DrawD2DRoundedRectangle(
        context, secondaryButton, static_cast<float>(Cu(10.0f)),
        secondaryFill, secondaryStroke);

    IDWriteTextFormat* buttonFormat = GetCuTextFormatWeight(
        15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    app_->DrawD2DText(
        context, _LW("guide.add_widget"), primaryButton, buttonFormat,
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
    app_->DrawD2DText(
        context,
        detailsExpanded_ ? _LW("guide.back") : _LW("guide.learn_more"),
        secondaryButton, buttonFormat, primaryText);

    RECT hintRect{
        body.left + Cu(16.0f), primaryButton.bottom + Cu(3.0f),
        body.right - Cu(16.0f), body.bottom - Cu(1.0f),
    };
    app_->DrawD2DText(
        context, _LW("guide.remove_hint"), hintRect,
        GetCuTextFormatWeight(12.0f, DWRITE_FONT_WEIGHT_NORMAL, true),
        hintText);

    if (!detailsExpanded_)
    {
        RECT title{
            body.left + Cu(26.0f), body.top + Cu(17.0f),
            body.right - Cu(26.0f), body.top + Cu(57.0f),
        };
        app_->DrawD2DText(
            context,
            _LFW("guide.new_page_title", std::to_wstring(pageIndex + 1)),
            title,
            GetCuTextFormatWeight(25.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false),
            primaryText);

        RECT subtitle{
            title.left, title.bottom,
            body.right - Cu(26.0f), primaryButton.top - Cu(10.0f),
        };
        app_->DrawD2DText(
            context, _LW("guide.new_page_subtitle"), subtitle,
            GetCuTextFormatWeight(15.5f, DWRITE_FONT_WEIGHT_NORMAL, false),
            secondaryText, DWRITE_WORD_WRAPPING_WRAP);
        return;
    }

    RECT title{
        body.left + Cu(24.0f), body.top + Cu(14.0f),
        body.right - Cu(24.0f), body.top + Cu(50.0f),
    };
    app_->DrawD2DText(
        context, _LW("guide.details_title"), title,
        GetCuTextFormatWeight(23.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false),
        primaryText);

    RECT status{
        body.left + Cu(24.0f), title.bottom,
        body.right - Cu(24.0f), title.bottom + Cu(25.0f),
    };
    app_->DrawD2DText(
        context,
        _LFW("guide.page_status",
            std::to_wstring(pageIndex + 1),
            std::to_wstring(pageCount),
            std::to_wstring(monitorCount)),
        status,
        GetCuTextFormatWeight(12.5f, DWRITE_FONT_WEIGHT_NORMAL, false),
        hintText);

    const char* detailKeys[] = {
        "guide.details_pages",
        "guide.details_drag",
        "guide.details_remove",
    };
    constexpr size_t detailCount = sizeof(detailKeys) / sizeof(detailKeys[0]);
    const int detailTop = status.bottom + Cu(2.0f);
    const int detailBottom = primaryButton.top - Cu(7.0f);
    const int rowHeight = std::max(
        1, (detailBottom - detailTop) / static_cast<int>(detailCount));
    for (size_t i = 0; i < detailCount; ++i)
    {
        RECT bullet{
            body.left + Cu(26.0f),
            detailTop + static_cast<int>(i) * rowHeight,
            body.right - Cu(26.0f),
            detailTop + static_cast<int>(i + 1) * rowHeight,
        };
        std::wstring text = L"•  ";
        text += _LW(detailKeys[i]);
        app_->DrawD2DText(
            context, text, bullet,
            GetCuTextFormatWeight(14.0f, DWRITE_FONT_WEIGHT_NORMAL, false),
            secondaryText, DWRITE_WORD_WRAPPING_WRAP);
    }
}
