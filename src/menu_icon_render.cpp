#include "menu_icon_render.h"

#include <d2d1helper.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::menu_icon
{
namespace
{

using Microsoft::WRL::ComPtr;

int Scale(int value, UINT dpi)
{
    return std::max(1, MulDiv(value, static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

D2D1_COLOR_F ToColor(COLORREF color, float alpha = 1.0f)
{
    return D2D1::ColorF(
        GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f,
        alpha);
}

void SetBrush(RenderContext& ctx, COLORREF color, float alpha = 1.0f)
{
    ctx.brush->SetColor(ToColor(color, alpha));
}

void FillRect(RenderContext& ctx, const D2D1_RECT_F& rect,
    COLORREF color, float alpha = 1.0f)
{
    SetBrush(ctx, color, alpha);
    ctx.dc->FillRectangle(rect, ctx.brush);
}

void FillRoundedRect(RenderContext& ctx, const D2D1_RECT_F& rect,
    float radius, COLORREF color, float alpha = 1.0f)
{
    SetBrush(ctx, color, alpha);
    ctx.dc->FillRoundedRectangle(
        D2D1::RoundedRect(rect, radius, radius), ctx.brush);
}

/** @brief 使用 DWrite 测量一段文本在给定格式下的宽度（DIP）。 */
float MeasureTextWidth(RenderContext& ctx, IDWriteTextFormat* format,
    const wchar_t* text, size_t length)
{
    if (!text || length == 0)
        return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(ctx.writeFactory->CreateTextLayout(text,
            static_cast<UINT32>(length), format, 100000.0f, 100.0f,
            &layout)))
    {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)))
        return 0.0f;
    return metrics.width;
}

/** @brief 将文本截断到 maxWidth 内并附加省略号。 */
std::wstring TruncateWithEllipsis(RenderContext& ctx,
    IDWriteTextFormat* format, const wchar_t* text, size_t length,
    float maxWidth)
{
    const std::wstring_view view(text, length);
    if (MeasureTextWidth(ctx, format, text, length) <= maxWidth)
        return std::wstring(view);
    constexpr wchar_t kEllipsis[] = L"\u2026";
    size_t low = 0;
    size_t high = length;
    while (low < high)
    {
        const size_t mid = (low + high + 1) / 2;
        const std::wstring candidate(
            std::wstring(view.substr(0, mid)) + kEllipsis);
        if (MeasureTextWidth(ctx, format, candidate.data(),
                candidate.size()) <= maxWidth)
        {
            low = mid;
        }
        else
        {
            high = mid - 1;
        }
    }
    return std::wstring(view.substr(0, low)) + kEllipsis;
}

/**
 * @brief 以 DWrite 绘制一段文本。
 *
 * 水平对齐通过 format 切换；文本在 rect 内垂直居中（format 需在初始化时
 * 设置 DWRITE_PARAGRAPH_ALIGNMENT_CENTER）。ellipsis 为 true 时超宽文本
 * 会截断并追加省略号。
 */
void DrawTextLayout(RenderContext& ctx, IDWriteTextFormat* format,
    const wchar_t* text, size_t length, const D2D1_RECT_F& rect,
    COLORREF color, DWRITE_TEXT_ALIGNMENT align =
        DWRITE_TEXT_ALIGNMENT_LEADING,
    bool ellipsis = true)
{
    if (!text || length == 0)
        return;
    format->SetTextAlignment(align);
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width <= 0.0f || height <= 0.0f)
        return;

    std::wstring truncated;
    const wchar_t* drawText = text;
    size_t drawLength = length;
    if (ellipsis)
    {
        truncated = TruncateWithEllipsis(ctx, format, text, length, width);
        drawText = truncated.c_str();
        drawLength = truncated.size();
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(ctx.writeFactory->CreateTextLayout(drawText,
            static_cast<UINT32>(drawLength), format, width, height,
            &layout)))
    {
        return;
    }
    SetBrush(ctx, color, ctx.contentAlpha);
    ctx.dc->DrawTextLayout(D2D1::Point2F(rect.left, rect.top),
        layout.Get(), ctx.brush);
}

/** @brief 在矩形内水平居中绘制图标字形。 */
void DrawGlyph(RenderContext& ctx, IDWriteTextFormat* format,
    const wchar_t* glyph, const D2D1_RECT_F& rect, COLORREF color,
    const D2D1_RECT_F* clip = nullptr)
{
    if (!glyph || !*glyph)
        return;
    if (clip)
        ctx.dc->PushAxisAlignedClip(*clip,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    DrawTextLayout(ctx, format, glyph, std::wcslen(glyph), rect, color,
        DWRITE_TEXT_ALIGNMENT_CENTER, false);
    if (clip)
        ctx.dc->PopAxisAlignedClip();
}

const wchar_t* ResolveQuickGlyph(
    MenuQuickIcon icon, const wchar_t* fallback)
{
    switch (icon)
    {
    case MenuQuickIcon::Paste: return L"\uF2D5";
    case MenuQuickIcon::NewItem: return L"\uF10C";
    case MenuQuickIcon::Refresh: return L"\uF13D";
    case MenuQuickIcon::Cut: return L"\uF33A";
    case MenuQuickIcon::Copy: return L"\uF32B";
    case MenuQuickIcon::Rename: return L"\U000F0A39";
    case MenuQuickIcon::Delete: return L"\uF34C";
    case MenuQuickIcon::Edit: return L"\uF3DD";
    case MenuQuickIcon::Settings: return L"\uF6A9";
    case MenuQuickIcon::Open: return L"\uF582";
    case MenuQuickIcon::FontGlyph:
    default:
        return fallback ? fallback : L"";
    }
}

/** @brief 绘制快捷操作的双色语义图标（clip 局部着色）。 */
void DrawQuickLayeredGlyph(RenderContext& ctx, MenuQuickIcon icon,
    const wchar_t* fallback, const D2D1_RECT_F& bounds,
    COLORREF foreground, COLORREF accent, bool disabled)
{
    const wchar_t* glyph = ResolveQuickGlyph(icon, fallback);
    DrawGlyph(ctx, ctx.iconFormat, glyph, bounds, foreground);
    if (disabled)
        return;
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    const auto drawAccent = [&](float leftPercent, float topPercent,
                                float rightPercent, float bottomPercent) {
        const D2D1_RECT_F clip{
            bounds.left + width * leftPercent / 100.0f,
            bounds.top + height * topPercent / 100.0f,
            bounds.left + width * rightPercent / 100.0f,
            bounds.top + height * bottomPercent / 100.0f,
        };
        DrawGlyph(ctx, ctx.iconFormat, glyph, bounds, accent, &clip);
    };
    switch (icon)
    {
    case MenuQuickIcon::NewItem:
        drawAccent(30, 30, 70, 70);
        return;
    case MenuQuickIcon::Cut:
        drawAccent(0, 52, 100, 100);
        return;
    case MenuQuickIcon::Copy:
        drawAccent(40, 15, 100, 85);
        return;
    case MenuQuickIcon::Rename:
        drawAccent(58, 0, 67, 100);
        drawAccent(50, 0, 75, 22);
        drawAccent(50, 78, 75, 100);
        return;
    case MenuQuickIcon::Open:
        drawAccent(48, 0, 100, 56);
        return;
    case MenuQuickIcon::Paste:
    case MenuQuickIcon::Refresh:
    case MenuQuickIcon::Delete:
    case MenuQuickIcon::Edit:
    case MenuQuickIcon::Settings:
    case MenuQuickIcon::FontGlyph:
    default:
        return;
    }
}

void DrawCheckmark(RenderContext& ctx, const D2D1_RECT_F& bounds,
    const Metrics& metrics, COLORREF color)
{
    const float centerX = bounds.left + metrics.leftPadding +
        metrics.iconColumnWidth / 2.0f;
    const float centerY = (bounds.top + bounds.bottom) / 2.0f;
    const float stroke = std::max(1.0f, metrics.rowHeight / 17.0f);
    SetBrush(ctx, color, ctx.contentAlpha);
    const D2D1_POINT_2F tip{
        centerX - 1.0f,
        centerY + metrics.iconColumnWidth / 5.0f,
    };
    ctx.dc->DrawLine(
        D2D1::Point2F(centerX - metrics.iconColumnWidth / 4.0f, centerY),
        tip, ctx.brush, stroke);
    ctx.dc->DrawLine(tip,
        D2D1::Point2F(centerX + metrics.iconColumnWidth / 3.0f,
            centerY - metrics.iconColumnWidth / 4.0f),
        ctx.brush, stroke);
}

void DrawSubmenuArrow(RenderContext& ctx, const D2D1_RECT_F& bounds,
    const Metrics& metrics, COLORREF color)
{
    const float centerX = bounds.right - metrics.rightPadding -
        metrics.arrowColumnWidth / 2.0f;
    const float centerY = (bounds.top + bounds.bottom) / 2.0f;
    const float halfHeight = std::max(2.0f, metrics.arrowColumnWidth / 5.0f);
    const float stroke = std::max(1.0f, metrics.rowHeight / 24.0f);
    SetBrush(ctx, color, ctx.contentAlpha);
    ctx.dc->DrawLine(
        D2D1::Point2F(centerX - halfHeight / 2.0f, centerY - halfHeight),
        D2D1::Point2F(centerX + halfHeight / 2.0f, centerY),
        ctx.brush, stroke);
    ctx.dc->DrawLine(
        D2D1::Point2F(centerX + halfHeight / 2.0f, centerY),
        D2D1::Point2F(centerX - halfHeight / 2.0f, centerY + halfHeight),
        ctx.brush, stroke);
}

} // namespace

Palette ResolvePalette(bool lightTheme)
{
    if (lightTheme)
    {
        return {
            RGB(250, 250, 250),
            RGB(232, 232, 232),
            RGB(26, 26, 26),
            RGB(118, 118, 118),
            RGB(225, 225, 225),
            RGB(0, 120, 212),
        };
    }
    return {
        RGB(44, 44, 44),
        RGB(56, 56, 56),
        RGB(255, 255, 255),
        RGB(158, 158, 158),
        RGB(68, 68, 68),
        RGB(96, 205, 255),
    };
}

Metrics ResolveMetrics(UINT dpi)
{
    const UINT effectiveDpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
    return {
        Scale(32, effectiveDpi),
        Scale(8, effectiveDpi),
        Scale(192, effectiveDpi),
        Scale(4, effectiveDpi),
        Scale(2, effectiveDpi),
        Scale(4, effectiveDpi),
        Scale(10, effectiveDpi),
        Scale(22, effectiveDpi),
        Scale(7, effectiveDpi),
        Scale(9, effectiveDpi),
        Scale(16, effectiveDpi),
        Scale(52, effectiveDpi),
        Scale(46, effectiveDpi),
        Scale(64, effectiveDpi),
        Scale(20, effectiveDpi),
        Scale(1, effectiveDpi),
        Scale(18, effectiveDpi),
        Scale(18, effectiveDpi),
    };
}

float MeasureItemWidth(IDWriteFactory* factory, IDWriteTextFormat* format,
    const ItemView& item, const Metrics& metrics)
{
    if (item.separator)
        return 0.0f;
    if (!factory || !format)
        return static_cast<float>(metrics.minimumWidth);

    const std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    const std::wstring_view primary = label.substr(0, tab);
    const std::wstring_view shortcut = tab == std::wstring_view::npos
        ? std::wstring_view{} : label.substr(tab + 1);

    RenderContext probe{};
    probe.writeFactory = factory;
    const auto measure = [&](const std::wstring_view& part) {
        if (part.empty())
            return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory->CreateTextLayout(part.data(),
                static_cast<UINT32>(part.size()), format, 100000.0f,
                100.0f, &layout)))
        {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS textMetrics{};
        return SUCCEEDED(layout->GetMetrics(&textMetrics))
            ? textMetrics.width : 0.0f;
    };
    const float primaryWidth = measure(primary);
    const float shortcutWidth = measure(shortcut);
    const float shortcutGap = shortcut.empty() ? 0.0f
        : static_cast<float>(metrics.textGap) * 3.0f;
    const float arrowWidth = item.hasSubmenu
        ? static_cast<float>(metrics.arrowColumnWidth) : 0.0f;
    return static_cast<float>(metrics.leftPadding) +
        static_cast<float>(metrics.iconColumnWidth) +
        static_cast<float>(metrics.textGap) + primaryWidth +
        shortcutGap + shortcutWidth + arrowWidth +
        static_cast<float>(metrics.rightPadding);
}

bool DrawItem(RenderContext& ctx, const ItemView& item,
    const D2D1_RECT_F& bounds, UINT itemState)
{
    if (!ctx.dc || !ctx.palette || !ctx.metrics || !ctx.brush ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top)
    {
        return false;
    }
    const Palette& palette = *ctx.palette;
    const Metrics& metrics = *ctx.metrics;

    FillRect(ctx, bounds, palette.background, ctx.backgroundAlpha);
    if (item.separator)
    {
        const float centerY = (bounds.top + bounds.bottom) * 0.5f;
        FillRect(ctx, D2D1::RectF(
            bounds.left + metrics.outerInset * 2, centerY,
            bounds.right - metrics.outerInset * 2, centerY + 1.0f),
            palette.separator);
        return true;
    }

    const bool disabled =
        (itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (itemState & ODS_SELECTED) != 0;
    if (selected && !disabled)
    {
        const D2D1_RECT_F selection{
            bounds.left + metrics.outerInset,
            bounds.top + metrics.selectionInsetY,
            bounds.right - metrics.outerInset,
            bounds.bottom - metrics.selectionInsetY,
        };
        FillRoundedRect(ctx, selection,
            static_cast<float>(metrics.selectionRadius),
            palette.hoverBackground, ctx.hoverAlpha);
    }

    const COLORREF foreground = disabled
        ? palette.disabledText : palette.text;

    const D2D1_RECT_F iconBounds{
        bounds.left + metrics.leftPadding,
        bounds.top,
        bounds.left + metrics.leftPadding + metrics.iconColumnWidth,
        bounds.bottom,
    };
    if (item.checked || (itemState & ODS_CHECKED) != 0)
    {
        DrawCheckmark(ctx, bounds, metrics, foreground);
    }
    else if (item.glyph && *item.glyph)
    {
        if (item.semanticIcon == MenuQuickIcon::FontGlyph)
        {
            DrawGlyph(ctx, ctx.iconFormat, item.glyph, iconBounds,
                foreground);
        }
        else
        {
            const COLORREF accent = disabled
                ? palette.disabledText : palette.accent;
            DrawQuickLayeredGlyph(ctx, item.semanticIcon, item.glyph,
                iconBounds, foreground, accent, disabled);
        }
    }

    const D2D1_RECT_F textBounds{
        bounds.left + metrics.leftPadding + metrics.iconColumnWidth +
            metrics.textGap,
        bounds.top,
        bounds.right - metrics.rightPadding -
            (item.hasSubmenu ? metrics.arrowColumnWidth : 0),
        bounds.bottom,
    };
    const std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    const std::wstring_view primary = label.substr(0, tab);
    DrawTextLayout(ctx, ctx.textFormat, primary.data(), primary.size(),
        textBounds, foreground, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    if (tab != std::wstring_view::npos)
    {
        const std::wstring_view shortcut = label.substr(tab + 1);
        DrawTextLayout(ctx, ctx.textFormat, shortcut.data(),
            shortcut.size(), textBounds, foreground,
            DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    }

    if (item.hasSubmenu)
        DrawSubmenuArrow(ctx, bounds, metrics, foreground);
    return true;
}

bool DrawQuickAction(RenderContext& ctx, MenuQuickIcon quickIcon,
    const ItemView& item, const D2D1_RECT_F& bounds, UINT itemState)
{
    if (!ctx.dc || !ctx.palette || !ctx.metrics || !ctx.brush ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top)
    {
        return false;
    }
    const Palette& palette = *ctx.palette;
    const Metrics& metrics = *ctx.metrics;

    FillRect(ctx, bounds, palette.background, ctx.backgroundAlpha);
    const bool disabled =
        (itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (itemState & ODS_SELECTED) != 0;
    if (selected && !disabled)
    {
        const D2D1_RECT_F selection{
            bounds.left + metrics.outerInset,
            bounds.top + metrics.selectionInsetY,
            bounds.right - metrics.outerInset,
            bounds.bottom - metrics.selectionInsetY,
        };
        FillRoundedRect(ctx, selection,
            static_cast<float>(metrics.selectionRadius),
            palette.hoverBackground, ctx.hoverAlpha);
    }

    const COLORREF foreground = disabled
        ? palette.disabledText : palette.text;
    const D2D1_RECT_F iconBounds{
        bounds.left + metrics.outerInset,
        bounds.top + metrics.outerInset,
        bounds.right - metrics.outerInset,
        bounds.top + metrics.outerInset + metrics.quickActionIconHeight,
    };
    const COLORREF accent = disabled
        ? palette.disabledText : palette.accent;
    DrawQuickLayeredGlyph(ctx, quickIcon, item.glyph, iconBounds,
        foreground, accent, disabled);

    const D2D1_RECT_F labelBounds{
        bounds.left + metrics.outerInset,
        iconBounds.bottom + metrics.quickActionLabelGap,
        bounds.right - metrics.outerInset,
        bounds.bottom - metrics.outerInset,
    };
    const std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    const std::wstring_view primary = label.substr(0, tab);
    DrawTextLayout(ctx, ctx.quickTextFormat, primary.data(),
        primary.size(), labelBounds, foreground,
        DWRITE_TEXT_ALIGNMENT_CENTER, true);
    return true;
}

bool DrawInlineAction(RenderContext& ctx, const ItemView& item,
    const D2D1_RECT_F& bounds, UINT itemState)
{
    if (!ctx.dc || !ctx.palette || !ctx.metrics || !ctx.brush ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top)
    {
        return false;
    }
    const Palette& palette = *ctx.palette;
    const Metrics& metrics = *ctx.metrics;

    FillRect(ctx, bounds, palette.background, ctx.backgroundAlpha);
    const bool disabled =
        (itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (itemState & ODS_SELECTED) != 0;
    const bool highlighted = (selected && !disabled) || item.checked;
    if (highlighted)
    {
        const D2D1_RECT_F selection{
            bounds.left + metrics.outerInset,
            bounds.top + metrics.selectionInsetY,
            bounds.right - metrics.outerInset,
            bounds.bottom - metrics.selectionInsetY,
        };
        FillRoundedRect(ctx, selection,
            static_cast<float>(metrics.selectionRadius),
            palette.hoverBackground, ctx.hoverAlpha);
    }

    const COLORREF foreground = disabled
        ? palette.disabledText
        : (item.checked ? palette.accent : palette.text);
    const bool hasGlyph = item.glyph && *item.glyph;
    if (hasGlyph)
    {
        const D2D1_RECT_F glyphBounds{
            bounds.left + metrics.outerInset,
            bounds.top,
            bounds.left + metrics.outerInset + metrics.iconColumnWidth,
            bounds.bottom,
        };
        DrawGlyph(ctx, ctx.iconFormat, item.glyph, glyphBounds,
            foreground);
    }

    const bool hasLabel = item.label && *item.label;
    if (hasLabel)
    {
        D2D1_RECT_F labelBounds = bounds;
        labelBounds.left += hasGlyph
            ? metrics.leftPadding + metrics.iconColumnWidth +
                metrics.textGap
            : metrics.outerInset * 2;
        labelBounds.right -= metrics.outerInset * 2;
        DrawTextLayout(ctx, ctx.textFormat, item.label,
            std::wcslen(item.label), labelBounds, foreground,
            hasGlyph ? DWRITE_TEXT_ALIGNMENT_LEADING
                     : DWRITE_TEXT_ALIGNMENT_CENTER,
            true);
    }
    return true;
}

bool DrawTextInput(RenderContext& ctx, const ItemView& item,
    const TextInputView& input, const D2D1_RECT_F& bounds)
{
    if (!ctx.dc || !ctx.palette || !ctx.metrics || !ctx.brush ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top)
    {
        return false;
    }
    const Palette& palette = *ctx.palette;
    const Metrics& metrics = *ctx.metrics;

    FillRect(ctx, bounds, palette.background, ctx.backgroundAlpha);
    const D2D1_RECT_F field{
        bounds.left + metrics.outerInset,
        bounds.top + metrics.selectionInsetY,
        bounds.right - metrics.outerInset,
        bounds.bottom - metrics.selectionInsetY,
    };
    const float radius = static_cast<float>(metrics.selectionRadius);
    FillRoundedRect(ctx, field, radius, palette.hoverBackground,
        ctx.hoverAlpha);
    SetBrush(ctx, input.focused ? palette.accent : palette.separator,
        ctx.contentAlpha);
    ctx.dc->DrawRoundedRectangle(D2D1::RoundedRect(field, radius, radius),
        ctx.brush, 1.0f);

    const D2D1_RECT_F glyphBounds{
        field.left + metrics.leftPadding / 2,
        field.top,
        field.left + metrics.leftPadding / 2 + metrics.iconColumnWidth,
        field.bottom,
    };
    DrawGlyph(ctx, ctx.iconFormat, item.glyph, glyphBounds,
        palette.disabledText, &field);

    const std::wstring committed = input.text ? input.text : L"";
    const size_t cursor = std::min(input.cursor, committed.size());
    const size_t anchor = std::min(
        input.selectionAnchor, committed.size());
    const size_t selectionStart = std::min(cursor, anchor);
    const size_t selectionEnd = std::max(cursor, anchor);
    const std::wstring composition = input.compositionText
        ? input.compositionText : L"";
    std::wstring display = committed;
    size_t displayCursor = cursor;
    size_t compositionStart = 0;
    if (!composition.empty())
    {
        display = committed.substr(0, selectionStart);
        display += composition;
        display += committed.substr(selectionEnd);
        compositionStart = selectionStart;
        displayCursor = compositionStart + std::min(
            input.compositionCursor, composition.size());
    }
    const bool showingPlaceholder = display.empty() && !input.focused;
    const std::wstring visibleText = showingPlaceholder
        ? std::wstring(item.label ? item.label : L"") : display;

    const D2D1_RECT_F textBounds{
        glyphBounds.right + metrics.textGap,
        field.top,
        field.right - metrics.rightPadding,
        field.bottom,
    };
    const float availableWidth = std::max(
        1.0f, textBounds.right - textBounds.left);
    const auto measurePrefix = [&](size_t length) {
        const size_t safeLength = std::min(length, display.size());
        return MeasureTextWidth(ctx, ctx.textFormat, display.data(),
            safeLength);
    };
    const float caretAdvance = measurePrefix(displayCursor);
    const float horizontalOffset = std::max(
        0.0f, caretAdvance - availableWidth + metrics.outerInset * 2);
    const D2D1_RECT_F drawBounds{
        textBounds.left - horizontalOffset,
        textBounds.top,
        textBounds.right + horizontalOffset,
        textBounds.bottom,
    };

    ctx.dc->PushAxisAlignedClip(textBounds,
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const COLORREF textColor = showingPlaceholder
        ? palette.disabledText : palette.text;
    if (input.focused && composition.empty() && cursor != anchor)
    {
        const D2D1_RECT_F selection{
            textBounds.left + measurePrefix(selectionStart) -
                horizontalOffset,
            textBounds.top + metrics.outerInset,
            textBounds.left + measurePrefix(selectionEnd) -
                horizontalOffset,
            textBounds.bottom - metrics.outerInset,
        };
        FillRect(ctx, selection, palette.separator, ctx.contentAlpha);
    }

    DrawTextLayout(ctx, ctx.textFormat, visibleText.data(),
        visibleText.size(), drawBounds, textColor,
        DWRITE_TEXT_ALIGNMENT_LEADING, false);

    if (input.focused && !composition.empty())
    {
        const float compositionLeft = textBounds.left +
            measurePrefix(compositionStart) - horizontalOffset;
        const float compositionRight = textBounds.left +
            measurePrefix(compositionStart + composition.size()) -
            horizontalOffset;
        SetBrush(ctx, palette.text);
        const float y = textBounds.bottom - metrics.outerInset;
        ctx.dc->DrawLine(
            D2D1::Point2F(compositionLeft, y),
            D2D1::Point2F(std::max(compositionLeft + 1.0f,
                compositionRight), y),
            ctx.brush, 1.0f);
    }

    if (input.focused && input.caretVisible)
    {
        const float caretX = std::clamp(
            textBounds.left + caretAdvance - horizontalOffset,
            textBounds.left,
            std::max(textBounds.left, textBounds.right - 1.0f));
        SetBrush(ctx, palette.accent);
        ctx.dc->DrawLine(
            D2D1::Point2F(caretX, field.top + metrics.outerInset),
            D2D1::Point2F(caretX, field.bottom - metrics.outerInset),
            ctx.brush, 1.0f);
    }

    ctx.dc->PopAxisAlignedClip();
    return true;
}

} // namespace snowdesktop::menu_icon
