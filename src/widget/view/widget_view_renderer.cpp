#include "widget_view_renderer.h"

#include "widget_button_fill.h"
#include "widget_draw_context.h"
#include "widget_draw_geometry.h"
#include "widget_interaction_region.h"
#include "widget_scroll_rules.h"
#include "widget_surface_renderer.h"
#include "widget_surface_theme.h"
#include "widget_view_text.h"
#include "widget_view_tree.h"
#include "common/utils.h"
#include "diagnostics/performance_trace.h"
#include "settings/animation_settings.h"
#include "ui/render/native_control_geometry.h"
#include "ui/render/native_tooltip_content.h"
#include "widget/packages/widget_package_image_cache.h"

#include <d2d1effects.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

static std::uint32_t WidgetRgbFromColorRef(COLORREF color) noexcept
{
    return (static_cast<std::uint32_t>(GetRValue(color)) << 16) |
        (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
        static_cast<std::uint32_t>(GetBValue(color));
}

static std::uint32_t MixWidgetRgb(std::uint32_t first,
    std::uint32_t second, float amount) noexcept
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    const auto mixChannel = [amount](std::uint32_t a, std::uint32_t b) {
        return static_cast<std::uint32_t>(std::lround(
            static_cast<float>(a) +
            (static_cast<float>(b) - static_cast<float>(a)) * amount));
    };
    const std::uint32_t red = mixChannel(
        (first >> 16) & 0xffu, (second >> 16) & 0xffu);
    const std::uint32_t green = mixChannel(
        (first >> 8) & 0xffu, (second >> 8) & 0xffu);
    const std::uint32_t blue = mixChannel(
        first & 0xffu, second & 0xffu);
    return (red << 16) | (green << 8) | blue;
}

static std::uint32_t WidgetContrastText(std::uint32_t background) noexcept
{
    const auto linearChannel = [](std::uint32_t value) {
        const float channel = static_cast<float>(value) / 255.0f;
        return channel <= 0.04045f
            ? channel / 12.92f
            : std::pow((channel + 0.055f) / 1.055f, 2.4f);
    };
    const float luminance =
        linearChannel((background >> 16) & 0xffu) * 0.2126f +
        linearChannel((background >> 8) & 0xffu) * 0.7152f +
        linearChannel(background & 0xffu) * 0.0722f;
    const float blackContrast = (luminance + 0.05f) / 0.05f;
    const float whiteContrast = 1.05f / (luminance + 0.05f);
    return blackContrast >= whiteContrast ? 0x000000u : 0xFFFFFFu;
}

ViewThemePalette BuildWidgetViewThemePalette(const LuaWidgetTheme& theme,
    bool highContrast, int accentColor)
{
    ViewThemePalette result;
    if (highContrast)
    {
        result.widgetBackground = WidgetRgbFromColorRef(
            GetSysColor(COLOR_WINDOW));
        result.surface = WidgetRgbFromColorRef(GetSysColor(COLOR_WINDOW));
        result.surfaceVariant = WidgetRgbFromColorRef(
            GetSysColor(COLOR_BTNFACE));
        result.textPrimary = WidgetRgbFromColorRef(
            GetSysColor(COLOR_WINDOWTEXT));
        result.textSecondary = result.textPrimary;
        result.textDisabled = WidgetRgbFromColorRef(
            GetSysColor(COLOR_GRAYTEXT));
        result.border = result.textPrimary;
        result.borderStrong = result.textPrimary;
        result.systemAccent = WidgetRgbFromColorRef(
            GetSysColor(COLOR_HIGHLIGHT));
        result.accentText = WidgetRgbFromColorRef(
            GetSysColor(COLOR_HIGHLIGHTTEXT));
        result.info = result.textPrimary;
        result.success = result.textPrimary;
        result.warning = result.textPrimary;
        result.error = result.textPrimary;
        return result;
    }

    const bool light = theme.contentTheme == 1;
    result.widgetBackground =
        static_cast<std::uint32_t>(theme.bg) & 0x00ffffffu;
    result.textPrimary = light ? 0x161616u : 0xFFFFFFu;
    result.surface = MixWidgetRgb(result.widgetBackground,
        result.textPrimary, light ? 0.04f : 0.07f);
    result.surfaceVariant = MixWidgetRgb(result.widgetBackground,
        result.textPrimary, light ? 0.09f : 0.14f);
    result.textSecondary = MixWidgetRgb(
        result.textPrimary, result.widgetBackground, 0.32f);
    result.textDisabled = MixWidgetRgb(
        result.textPrimary, result.widgetBackground, 0.56f);
    result.border = static_cast<std::uint32_t>(theme.border) & 0x00ffffffu;
    result.borderStrong = MixWidgetRgb(result.border,
        result.textPrimary, 0.55f);
    result.systemAccent =
        static_cast<std::uint32_t>(accentColor) & 0x00ffffffu;
    result.accentText = WidgetContrastText(result.systemAccent);
    result.info = light ? 0x005A9Eu : 0x72C7FFu;
    result.success = light ? 0x107C10u : 0x55C271u;
    result.warning = light ? 0x8A4B00u : 0xF2C94Cu;
    result.error = light ? 0xC42B1Cu : 0xFF6B6Bu;
    return result;
}

static void OverlayViewStyle(
    snowdesktop::widget_runtime::ViewStyle& result,
    const snowdesktop::widget_runtime::ViewStyle& style)
{
    if (style.background || style.backgroundToken)
    {
        result.background = style.background;
        result.backgroundToken = style.backgroundToken;
    }
    if (style.foreground || style.foregroundToken)
    {
        result.foreground = style.foreground;
        result.foregroundToken = style.foregroundToken;
    }
    if (style.borderColor || style.borderColorToken)
    {
        result.borderColor = style.borderColor;
        result.borderColorToken = style.borderColorToken;
    }
    if (style.borderWidth) result.borderWidth = style.borderWidth;
    if (style.cornerRadius) result.cornerRadius = style.cornerRadius;
    if (style.opacity) result.opacity = style.opacity;
}

static snowdesktop::widget_runtime::ViewStyle ResolveViewStyle(
    const snowdesktop::widget_runtime::ViewNode& node,
    bool hovered, bool pressed, bool focused,
    std::optional<bool> checkedOverride = std::nullopt,
    std::optional<bool> enabledOverride = std::nullopt)
{
    using snowdesktop::widget_runtime::ViewStyle;
    ViewStyle result = node.style;
    if (node.selected)
        OverlayViewStyle(result, node.selectedStyle);
    if (checkedOverride.value_or(node.checked) || node.indeterminate)
        OverlayViewStyle(result, node.checkedStyle);
    if (hovered) OverlayViewStyle(result, node.hoverStyle);
    if (pressed) OverlayViewStyle(result, node.pressedStyle);
    if (node.validationState !=
        snowdesktop::widget_runtime::ViewValidationState::None)
    {
        OverlayViewStyle(result, node.validationStyle);
        if (!node.validationStyle.borderColor &&
            !node.validationStyle.borderColorToken)
        {
            using snowdesktop::widget_runtime::ViewValidationState;
            switch (node.validationState)
            {
            case ViewValidationState::Info:
                result.borderColorToken = snowdesktop::widget_runtime::
                    ViewThemeColorToken::Info;
                break;
            case ViewValidationState::Success:
                result.borderColorToken = snowdesktop::widget_runtime::
                    ViewThemeColorToken::Success;
                break;
            case ViewValidationState::Warning:
                result.borderColorToken = snowdesktop::widget_runtime::
                    ViewThemeColorToken::Warning;
                break;
            case ViewValidationState::Error:
                result.borderColorToken = snowdesktop::widget_runtime::
                    ViewThemeColorToken::Error;
                break;
            default:
                break;
            }
        }
        if (!node.validationStyle.borderWidth)
            result.borderWidth = std::max(
                result.borderWidth.value_or(0.0f), 1.5f);
    }
    if (focused)
    {
        OverlayViewStyle(result, node.focusStyle);
        if (!result.borderColor && !result.borderColorToken)
            result.borderColorToken = snowdesktop::widget_runtime::
                ViewThemeColorToken::SystemAccent;
        if (!result.borderWidth) result.borderWidth = 1.5f;
    }
    if (!enabledOverride.value_or(node.enabled))
    {
        OverlayViewStyle(result, node.disabledStyle);
        if (!node.disabledStyle.opacity)
        {
            result.opacity = std::clamp(
                result.opacity.value_or(1.0f) * 0.42f, 0.0f, 1.0f);
        }
    }
    return result;
}

static float WidgetIndeterminateProgressPhase(
    snowdesktop::widget_runtime::ViewTransitionRuntime::TimePoint now,
    bool reducedMotion) noexcept
{
    if (reducedMotion) return 0.25f;
    const auto PeriodMilliseconds = static_cast<std::int64_t>(std::lround(
        1400.0 * snowdesktop::animation::RuntimeDurationScale()));
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto wrapped = ((elapsed % PeriodMilliseconds) +
        PeriodMilliseconds) % PeriodMilliseconds;
    return static_cast<float>(wrapped) /
        static_cast<float>(PeriodMilliseconds);
}

static void DrawWidgetProgressRing(WidgetDrawContext* state,
    const D2D1_RECT_F& rect, float value, bool indeterminate, float phase,
    float thickness,
    std::uint32_t trackColor, float trackOpacity,
    std::uint32_t fillColor, float fillOpacity)
{
    if (!state || !state->ctx) return;
    const float radius = std::max(0.0f,
        std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.5f -
            thickness * 0.5f);
    if (radius <= 0.0f) return;
    const D2D1_POINT_2F center = D2D1::Point2F(
        (rect.left + rect.right) * 0.5f,
        (rect.top + rect.bottom) * 0.5f);
    const D2D1_ELLIPSE ellipse = D2D1::Ellipse(center, radius, radius);
    if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
            static_cast<int>(trackColor), trackOpacity))
        state->ctx->DrawEllipse(ellipse, track, thickness);
    const float visibleFraction = indeterminate ? 0.24f : value;
    if (visibleFraction <= 0.0f) return;
    ID2D1SolidColorBrush* fill = GetCachedBrush(state,
        static_cast<int>(fillColor), fillOpacity);
    if (!fill) return;
    if (!indeterminate && visibleFraction >= 0.9999f)
    {
        state->ctx->DrawEllipse(ellipse, fill, thickness);
        return;
    }

    ComPtr<ID2D1Factory> factory;
    state->ctx->GetFactory(&factory);
    if (!factory) return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink)) || !sink) return;
    constexpr float Pi = 3.14159265358979323846f;
    const float startAngle = indeterminate
        ? phase * 2.0f * Pi : 0.0f;
    const float angle = visibleFraction * 2.0f * Pi;
    const D2D1_POINT_2F start = D2D1::Point2F(
        center.x + std::sin(startAngle) * radius,
        center.y - std::cos(startAngle) * radius);
    const D2D1_POINT_2F end = D2D1::Point2F(
        center.x + std::sin(startAngle + angle) * radius,
        center.y - std::cos(startAngle + angle) * radius);
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(radius, radius),
        0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
        visibleFraction > 0.5f
            ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close()))
        state->ctx->DrawGeometry(geometry.Get(), fill, thickness);
}

static bool IsWidgetDataSeriesNode(
    snowdesktop::widget_runtime::ViewNodeType type) noexcept
{
    using snowdesktop::widget_runtime::ViewNodeType;
    return type == ViewNodeType::Sparkline ||
        type == ViewNodeType::LineChart ||
        type == ViewNodeType::BarChart ||
        type == ViewNodeType::Waveform ||
        type == ViewNodeType::Spectrum;
}

static void DrawWidgetDataSeries(WidgetDrawContext* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style,
    const D2D1_RECT_F& bounds, float opacity)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (!state || !state->ctx || node.values.empty()) return;
    const auto content = snowdesktop::widget_runtime::
        ViewNodeContentRect(node);
    const D2D1_RECT_F rect = D2D1::RectF(
        bounds.left + content.x - node.frame.x,
        bounds.top + content.y - node.frame.y,
        bounds.left + content.x - node.frame.x + content.width,
        bounds.top + content.y - node.frame.y + content.height);
    const float width = std::max(0.0f, rect.right - rect.left);
    const float height = std::max(0.0f, rect.bottom - rect.top);
    if (width <= 0.0f || height <= 0.0f) return;

    float minimum = 0.0f;
    float maximum = 1.0f;
    if (node.seriesMinimum && node.seriesMaximum)
    {
        minimum = *node.seriesMinimum;
        maximum = *node.seriesMaximum;
    }
    else if (node.type == ViewNodeType::Waveform)
    {
        minimum = -1.0f;
        maximum = 1.0f;
    }
    else if (node.type == ViewNodeType::Spectrum)
    {
        minimum = 0.0f;
        maximum = 1.0f;
    }
    else
    {
        const auto range = std::minmax_element(
            node.values.begin(), node.values.end());
        minimum = *range.first;
        maximum = *range.second;
        if (node.type == ViewNodeType::BarChart)
        {
            minimum = std::min(0.0f, minimum);
            maximum = std::max(0.0f, maximum);
        }
        if (minimum == maximum)
        {
            const float padding = std::max(1.0f,
                std::abs(minimum) * 0.1f);
            minimum -= padding;
            maximum += padding;
        }
    }
    const float span = maximum - minimum;
    if (!std::isfinite(span) || span <= 0.0f) return;
    const auto valueY = [&](float value) {
        const float normalized = std::clamp(
            (value - minimum) / span, 0.0f, 1.0f);
        return rect.bottom - normalized * height;
    };

    const std::uint32_t color = style.foreground.value_or(0xFFFFFF);
    ID2D1SolidColorBrush* brush = GetCachedBrush(state,
        static_cast<int>(color), opacity * node.fillOpacity);
    ID2D1SolidColorBrush* track = GetCachedBrush(state,
        static_cast<int>(color), opacity * node.trackOpacity * 0.2f);
    if (!brush) return;

    if (track && node.type == ViewNodeType::LineChart)
    {
        for (int line = 1; line < 4; ++line)
        {
            const float y = rect.top + height * line / 4.0f;
            state->ctx->DrawLine(D2D1::Point2F(rect.left, y),
                D2D1::Point2F(rect.right, y), track, 1.0f);
        }
    }
    else if (track && (node.type == ViewNodeType::Waveform ||
            node.type == ViewNodeType::BarChart) &&
        minimum <= 0.0f && maximum >= 0.0f)
    {
        const float y = valueY(0.0f);
        state->ctx->DrawLine(D2D1::Point2F(rect.left, y),
            D2D1::Point2F(rect.right, y), track, 1.0f);
    }

    if (node.type == ViewNodeType::BarChart ||
        node.type == ViewNodeType::Spectrum)
    {
        const float slot = width /
            static_cast<float>(node.values.size());
        const float gap = std::min(2.0f, slot * 0.2f);
        const float baseline = node.type == ViewNodeType::Spectrum
            ? rect.bottom : valueY(0.0f);
        for (std::size_t index = 0; index < node.values.size(); ++index)
        {
            const float y = valueY(node.values[index]);
            const float left = rect.left + slot *
                static_cast<float>(index) + gap * 0.5f;
            const float right = std::max(left,
                rect.left + slot * static_cast<float>(index + 1) -
                    gap * 0.5f);
            state->ctx->FillRectangle(D2D1::RectF(
                left, std::min(y, baseline), right,
                std::max(y, baseline)), brush);
        }
        return;
    }

    const float stroke = std::min(node.thickness,
        std::max(0.5f, std::min(width, height)));
    if (node.values.size() == 1)
    {
        state->ctx->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(rect.left + width * 0.5f,
                valueY(node.values.front())),
            stroke, stroke), brush);
        return;
    }
    const float step = width /
        static_cast<float>(node.values.size() - 1);
    D2D1_POINT_2F previous = D2D1::Point2F(
        rect.left, valueY(node.values.front()));
    for (std::size_t index = 1; index < node.values.size(); ++index)
    {
        const D2D1_POINT_2F next = D2D1::Point2F(
            rect.left + step * static_cast<float>(index),
            valueY(node.values[index]));
        state->ctx->DrawLine(previous, next, brush, stroke);
        previous = next;
    }
}

static std::string DeclarativeInputValue(
    const snowdesktop::widget_runtime::ViewNode& node)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (node.type != ViewNodeType::NumberInput)
        return node.inputValue;
    std::string value = std::to_string(node.value);
    while (value.size() > 1 && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.pop_back();
    return value;
}

static void DrawDeclarativeViewInput(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity,
    float cumulativeScale)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (!state || !state->ctx) return;
    const bool multiline = node.type == ViewNodeType::TextArea;
    const float x = node.frame.x;
    const float y = node.frame.y;
    const float width = node.frame.width;
    const float height = node.frame.height;
    const auto content = snowdesktop::widget_runtime::
        ViewNodeContentRect(node);
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    std::wstring focusedText;
    std::wstring compositionText;
    size_t compositionCursor = 0;
    const bool focused = host.ReadFocusedInput(node.key, focusedText, cursor,
        selectionAnchor, compositionText, compositionCursor);
    const bool widgetSelected = host.IsSelected();
    const std::uint32_t background = style.background.value_or(0xFFFFFF);
    const std::uint32_t foreground = style.foreground.value_or(0xFFFFFF);
    const std::uint32_t border = focused && !widgetSelected
        ? 0x64A8FF : style.borderColor.value_or(0xFFFFFF);
    const float radius = std::max(0.0f,
        style.cornerRadius.value_or(6.0f));
    DrawHostRect(state, x, y, width, height,
        static_cast<int>(background), radius,
        opacity * (style.background ? 1.0f : (focused ? 0.10f : 0.07f)));
    DrawHostStrokeRect(state, x, y, width, height,
        static_cast<int>(border), radius,
        std::max(1.0f, style.borderWidth.value_or(1.0f)),
        opacity * (focused && !widgetSelected
            ? 0.72f : (style.borderColor ? 1.0f : 0.18f)));

    const HostInputDisplayText focusedDisplay =
        BuildHostInputDisplayText(focusedText, cursor,
            selectionAnchor, compositionText, compositionCursor);
    const std::wstring controlledText = Utf8ToWide(
        DeclarativeInputValue(node));
    const bool showingPlaceholder = !focused && controlledText.empty();
    const std::wstring displayText = focused
        ? focusedDisplay.text
        : (showingPlaceholder ? Utf8ToWide(node.placeholder)
            : controlledText);
    const float scrollbarReserve = multiline ? 8.0f : 0.0f;
    const float innerWidth = std::max(1.0f,
        content.width - scrollbarReserve);
    ComPtr<IDWriteTextLayout> layout = multiline
        ? CreateHostMultilineTextLayout(state, displayText,
            node.fontSize, innerWidth)
        : CreateHostSingleLineTextLayout(state, displayText,
            node.fontSize, innerWidth, content.height);
    if (!layout) return;
    ApplyViewTextLocaleAndDirection(layout.Get(), node, displayText);
    SetViewTextLayoutAlignment(layout.Get(), node.textAlign);

    const int scaledScrollOffset = multiline
        ? host.InputScrollOffset(node.key) : 0;
    const float scrollOffset = cumulativeScale > 0.0f
        ? static_cast<float>(scaledScrollOffset) / cumulativeScale : 0.0f;
    const float originX = content.x;
    const float originY = multiline
        ? content.y - scrollOffset : content.y;
    const D2D1_RECT_F clip = D2D1::RectF(
        state->widgetRect.left + content.x,
        state->widgetRect.top + content.y,
        state->widgetRect.left + content.x + content.width,
        state->widgetRect.top + content.y + content.height);
    state->ctx->PushAxisAlignedClip(
        clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (focused && compositionText.empty() &&
        selectionAnchor != cursor)
    {
        DrawHostTextSelection(state, layout.Get(),
            std::min(selectionAnchor, cursor),
            std::max(selectionAnchor, cursor),
            originX, originY, 0x64A8FF);
    }
    ID2D1SolidColorBrush* textBrush = GetCachedBrush(state,
        static_cast<int>(showingPlaceholder ? 0x94A3B8 : foreground),
        opacity);
    if (textBrush)
        state->ctx->DrawTextLayout(D2D1::Point2F(
            state->widgetRect.left + originX,
            state->widgetRect.top + originY), layout.Get(), textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (focused && focusedDisplay.compositionLength > 0)
    {
        DrawHostCompositionUnderline(state, layout.Get(),
            focusedDisplay.compositionStart,
            focusedDisplay.compositionLength,
            originX, originY, static_cast<int>(foreground));
    }
    if (focused)
    {
        const size_t safeCursor = std::min(
            focusedDisplay.cursor, focusedDisplay.text.size());
        UINT32 hitPosition = 0;
        BOOL trailing = FALSE;
        if (!focusedDisplay.text.empty())
        {
            if (safeCursor >= focusedDisplay.text.size())
            {
                hitPosition = static_cast<UINT32>(
                    focusedDisplay.text.size() - 1);
                trailing = TRUE;
            }
            else
                hitPosition = static_cast<UINT32>(safeCursor);
        }
        float caretX = 0.0f;
        float caretY = 0.0f;
        DWRITE_HIT_TEST_METRICS metrics{};
        if (SUCCEEDED(layout->HitTestTextPosition(hitPosition,
                trailing, &caretX, &caretY, &metrics)))
        {
            DrawHostRect(state, originX + caretX,
                originY + caretY, 1.5f,
                std::max(metrics.height, node.fontSize),
                static_cast<int>(foreground), 0.0f, opacity);
        }
    }
    state->ctx->PopAxisAlignedClip();
}

static void DrawDeclarativeSelect(WidgetDrawContext* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity)
{
    if (!state || !state->ctx) return;
    const float radius = std::max(0.0f,
        style.cornerRadius.value_or(6.0f));
    DrawHostRect(state, node.frame.x, node.frame.y,
        node.frame.width, node.frame.height,
        static_cast<int>(style.background.value_or(0xFFFFFF)), radius,
        opacity * (style.background ? 1.0f : 0.07f));
    DrawHostStrokeRect(state, node.frame.x, node.frame.y,
        node.frame.width, node.frame.height,
        static_cast<int>(style.borderColor.value_or(0xFFFFFF)), radius,
        std::max(1.0f, style.borderWidth.value_or(1.0f)),
        opacity * (style.borderColor ? 1.0f : 0.18f));
    std::string label = node.placeholder;
    for (const auto& option : node.options)
        if (option.value == node.selectedValue) label = option.label;
    const auto content = snowdesktop::widget_runtime::
        ViewNodeContentRect(node);
    const std::wstring text = Utf8ToWide(label);
    const bool rightToLeft = ResolveViewTextDirection(node, text) ==
        DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
    const float textLeft = std::max(
        node.frame.x + (rightToLeft ? 28.0f : 8.0f), content.x);
    const float textRight = std::min(
        node.frame.x + node.frame.width - (rightToLeft ? 8.0f : 28.0f),
        content.x + content.width);
    if (state->dwrite && !label.empty())
    {
        IDWriteTextFormat* format = GetCachedTextFormat(state,
            node.fontSize, state->itemFontWeight, false,
            DWRITE_WORD_WRAPPING_NO_WRAP);
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(style.foreground.value_or(
                node.selectedValue.empty() ? 0x94A3B8 : 0xFFFFFF)),
            opacity);
        ComPtr<IDWriteTextLayout> layout;
        const float textWidth = std::max(0.0f, textRight - textLeft);
        if (format && brush && textWidth > 0.0f && SUCCEEDED(
                state->dwrite->CreateTextLayout(text.data(),
                    static_cast<UINT32>(text.size()), format,
                    textWidth, content.height, &layout)) && layout)
        {
            ApplyViewTextLocaleAndDirection(layout.Get(), node, text);
            SetViewTextLayoutAlignment(layout.Get(), node.textAlign);
            layout->SetParagraphAlignment(
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            state->ctx->DrawTextLayout(D2D1::Point2F(
                state->widgetRect.left + textLeft,
                state->widgetRect.top + content.y), layout.Get(), brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(style.foreground.value_or(0xFFFFFF)),
            opacity * 0.8f))
    {
        const float cx = state->widgetRect.left + node.frame.x +
            (rightToLeft ? 14.0f : node.frame.width - 14.0f);
        const float cy = state->widgetRect.top + node.frame.y +
            node.frame.height * 0.5f;
        const float direction = node.expanded ? -1.0f : 1.0f;
        state->ctx->DrawLine(D2D1::Point2F(cx - 4.0f,
                cy - 2.0f * direction),
            D2D1::Point2F(cx, cy + 2.0f * direction), brush, 1.5f);
        state->ctx->DrawLine(D2D1::Point2F(cx,
                cy + 2.0f * direction),
            D2D1::Point2F(cx + 4.0f,
                cy - 2.0f * direction), brush, 1.5f);
    }
}

static void DrawWidgetMonthCalendar(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    std::string_view focusedKey,
    const snowdesktop::widget_runtime::ViewThemePalette& palette)
{
    using snowdesktop::widget_runtime::ViewMonthCalendarCellFrame;
    using snowdesktop::widget_runtime::ViewMonthCalendarWeekdayFrame;
    using snowdesktop::widget_runtime::ViewStyle;
    if (!state || !state->ctx || !state->dwrite) return;
    std::array<snowdesktop::widget_runtime::ViewMonthCalendarCell, 42> cells;
    std::string error;
    if (!snowdesktop::widget_runtime::BuildViewMonthCalendarCells(
            node, cells, error))
        return;

    std::optional<std::wstring> privateFontPath;
    if (!node.fontResourceName.empty())
        privateFontPath = host.ResolveFont(node.fontResourceName);
    IDWriteTextFormat* dayFormat = node.fontResourceName.empty() ||
            privateFontPath
        ? GetCachedTextFormat(state, node.fontSize,
            node.bold ? DWRITE_FONT_WEIGHT_BOLD : state->itemFontWeight,
            false, DWRITE_WORD_WRAPPING_NO_WRAP, false, false, false,
            privateFontPath ? &*privateFontPath : nullptr)
        : nullptr;
    IDWriteTextFormat* weekdayFormat = node.fontResourceName.empty() ||
            privateFontPath
        ? GetCachedTextFormat(state, std::max(8.0f, node.fontSize * 0.78f),
            DWRITE_FONT_WEIGHT_SEMI_BOLD, false,
            DWRITE_WORD_WRAPPING_NO_WRAP, false, false, false,
            privateFontPath ? &*privateFontPath : nullptr)
        : nullptr;
    if (!dayFormat || !weekdayFormat) return;

    const auto drawCentered = [&](std::wstring_view text,
            const snowdesktop::widget_runtime::ViewRect& frame,
            IDWriteTextFormat* format, std::uint32_t color, float alpha) {
        if (text.empty() || frame.width <= 0.0f || frame.height <= 0.0f)
            return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(state->dwrite->CreateTextLayout(text.data(),
                static_cast<UINT32>(text.size()), format,
                frame.width, frame.height, &layout)) || !layout)
            return;
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                static_cast<int>(color), alpha))
            state->ctx->DrawTextLayout(D2D1::Point2F(
                state->widgetRect.left + frame.x,
                state->widgetRect.top + frame.y), layout.Get(), brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const std::uint32_t baseForeground = style.foreground.value_or(0xFFFFFF);
    for (std::size_t column = 0; column < 7; ++column)
    {
        const std::size_t labelIndex = static_cast<std::size_t>(
            (node.firstDayOfWeek - 1 + static_cast<int>(column)) % 7);
        drawCentered(Utf8ToWide(node.weekdayLabels[labelIndex]),
            ViewMonthCalendarWeekdayFrame(node, column), weekdayFormat,
            baseForeground, opacity * 0.62f);
    }

    for (std::size_t index = 0; index < cells.size(); ++index)
    {
        const auto& cell = cells[index];
        if (!cell.currentMonth && !node.showAdjacentDates) continue;
        const auto frame = ViewMonthCalendarCellFrame(node, index);
        const std::string key = node.key + "/" + cell.date;
        const bool hovered = regions.IsHovered(key);
        const bool pressed = regions.IsPressed(key);
        const bool focused = key == focusedKey;
        ViewStyle cellStyle;
        cellStyle.foreground = baseForeground;
        cellStyle.opacity = style.opacity;
        if (!cell.currentMonth)
            OverlayViewStyle(cellStyle, node.adjacentStyle);
        if (cell.today) OverlayViewStyle(cellStyle, node.todayStyle);
        if (cell.selected)
            OverlayViewStyle(cellStyle, node.selectedStyle);
        if (hovered) OverlayViewStyle(cellStyle, node.hoverStyle);
        if (pressed) OverlayViewStyle(cellStyle, node.pressedStyle);
        if (focused)
        {
            OverlayViewStyle(cellStyle, node.focusStyle);
            if (!cellStyle.borderColor) cellStyle.borderColor = 0x72C7FF;
            if (!cellStyle.borderWidth) cellStyle.borderWidth = 1.5f;
        }
        if (!node.enabled)
            OverlayViewStyle(cellStyle, node.disabledStyle);
        cellStyle = snowdesktop::widget_runtime::ResolveViewThemeStyle(
            cellStyle, palette);
        const float cellOpacity = std::clamp(
            cellStyle.opacity.value_or(opacity), 0.0f, 1.0f) *
            (cell.currentMonth ? 1.0f :
                (node.adjacentStyle.opacity ? 1.0f : 0.42f));
        const float diameter = std::max(0.0f,
            std::min(frame.width, frame.height) * 0.76f);
        const float centerX = state->widgetRect.left + frame.x +
            frame.width * 0.5f;
        const float centerY = state->widgetRect.top + frame.y +
            frame.height * 0.46f;
        const D2D1_ELLIPSE indicator = D2D1::Ellipse(
            D2D1::Point2F(centerX, centerY), diameter * 0.5f,
            diameter * 0.5f);
        if (cell.selected || cellStyle.background || hovered || pressed)
        {
            const std::uint32_t fillColor = cellStyle.background.value_or(
                cell.selected ? 0x4C9AFF : 0xFFFFFF);
            const float fillAlpha = cellOpacity *
                (cellStyle.background || cell.selected ? 1.0f :
                    (pressed ? 0.18f : 0.11f));
            if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                    static_cast<int>(fillColor), fillAlpha))
                state->ctx->FillEllipse(indicator, fill);
        }
        if (cell.today && !cell.selected)
        {
            const ViewStyle todayStyle = snowdesktop::widget_runtime::
                ResolveViewThemeStyle(node.todayStyle, palette);
            const std::uint32_t borderColor =
                todayStyle.borderColor.value_or(baseForeground);
            if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                    static_cast<int>(borderColor), cellOpacity * 0.78f))
                state->ctx->DrawEllipse(indicator, border,
                    std::max(1.0f,
                        todayStyle.borderWidth.value_or(1.25f)));
        }
        if (focused && cellStyle.borderColor)
        {
            if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                    static_cast<int>(*cellStyle.borderColor), cellOpacity))
                state->ctx->DrawEllipse(indicator, border,
                    std::max(1.0f,
                        cellStyle.borderWidth.value_or(1.5f)));
        }
        const auto textFrame = snowdesktop::widget_runtime::ViewRect{
            frame.x + (frame.width - diameter) * 0.5f,
            frame.y + frame.height * 0.46f - diameter * 0.5f,
            diameter, diameter };
        drawCentered(std::to_wstring(cell.day), textFrame, dayFormat,
            cellStyle.foreground.value_or(baseForeground), cellOpacity);
        if (cell.hasEvent)
        {
            ViewStyle eventStyle = cellStyle;
            OverlayViewStyle(eventStyle, node.eventStyle);
            eventStyle = snowdesktop::widget_runtime::ResolveViewThemeStyle(
                eventStyle, palette);
            const float radius = std::max(1.0f,
                std::min(frame.width, frame.height) * 0.035f);
            if (ID2D1SolidColorBrush* marker = GetCachedBrush(state,
                    static_cast<int>(eventStyle.foreground.value_or(
                        baseForeground)), cellOpacity))
                state->ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(
                    centerX, state->widgetRect.top + frame.y +
                        frame.height - radius * 2.0f), radius, radius),
                    marker);
        }
    }
}

static void DrawWidgetViewBitmap(WidgetDrawContext* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    ID2D1Bitmap1* bitmap, const D2D1_RECT_F& rect,
    float opacity, float borderWidth, float clipRadius, bool reducedMotion,
    const snowdesktop::widget_runtime::ViewThemePalette& palette,
    snowdesktop::widget_runtime::PackageImageSize original = {})
{
    if (!state || !state->ctx || !bitmap) return;
    using snowdesktop::widget_runtime::DrawImageAlignment;
    using snowdesktop::widget_runtime::DrawImageFit;
    using snowdesktop::widget_runtime::ViewImageAlignment;
    using snowdesktop::widget_runtime::ViewImageFit;
    using snowdesktop::widget_runtime::ViewImageInterpolation;
    const auto content = snowdesktop::widget_runtime::
        ViewNodeContentRect(node);
    const float targetLeft = rect.left + content.x - node.frame.x +
        borderWidth;
    const float targetTop = rect.top + content.y - node.frame.y +
        borderWidth;
    const float targetRight = rect.right -
        (node.frame.x + node.frame.width - content.x - content.width) -
        borderWidth;
    const float targetBottom = rect.bottom -
        (node.frame.y + node.frame.height - content.y - content.height) -
        borderWidth;
    const float targetWidth = std::max(0.0f, targetRight - targetLeft);
    const float targetHeight = std::max(0.0f, targetBottom - targetTop);
    const D2D1_SIZE_F sourceSize = bitmap->GetSize();
    const D2D1_SIZE_F logicalSize = original.width && original.height ?
        D2D1::SizeF(static_cast<float>(original.width), static_cast<float>(original.height)) :
        sourceSize;
    DrawImageFit fit = DrawImageFit::Contain;
    if (node.imageFit == ViewImageFit::Fill) fit = DrawImageFit::Fill;
    else if (node.imageFit == ViewImageFit::Cover) fit = DrawImageFit::Cover;
    else if (node.imageFit == ViewImageFit::None) fit = DrawImageFit::None;
    DrawImageAlignment alignment = DrawImageAlignment::Center;
    if (node.imageAlignment == ViewImageAlignment::Start)
        alignment = DrawImageAlignment::Start;
    else if (node.imageAlignment == ViewImageAlignment::End)
        alignment = DrawImageAlignment::End;
    const auto placement = snowdesktop::widget_runtime::
        ResolveDrawImagePlacement(logicalSize.width, logicalSize.height,
            { targetLeft, targetTop,
                targetWidth, targetHeight }, fit, alignment);
    if (!placement.valid) return;
    const D2D1_RECT_F destination = D2D1::RectF(
        placement.destination.x, placement.destination.y,
        placement.destination.x + placement.destination.width,
        placement.destination.y + placement.destination.height);
    const auto sampled = state->imageSamplingCache.Resolve(state->ctx, bitmap,
        destination, D2D1::RectF(
            placement.source.x * sourceSize.width / logicalSize.width,
            placement.source.y * sourceSize.height / logicalSize.height,
            (placement.source.x + placement.source.width) * sourceSize.width / logicalSize.width,
            (placement.source.y + placement.source.height) * sourceSize.height / logicalSize.height),
        node.imageInterpolation == ViewImageInterpolation::Nearest);
    bitmap = sampled.bitmap.Get();
    const auto& sourceRect = sampled.source;
    const auto interpolation = sampled.interpolation;
    const std::optional<std::uint32_t> imageTint =
        snowdesktop::widget_runtime::ResolveViewThemeColor(
            node.imageTint, node.imageTintToken, palette);
    auto* spin = state->desktopImageSpin;
    const float width = destination.right - destination.left;
    const float height = destination.bottom - destination.top;
    // A separate visual sits above the component surface. Only opt-in,
    // unclipped circular artwork can be lifted out of the normal paint order.
    if (spin && !spin->bitmap && node.imageSpinSpeed && !imageTint &&
        state->widgetClipDepth == 0 && width >= 1.0f &&
        std::abs(width - height) < 0.001f &&
        clipRadius >= width * 0.5f - 0.001f &&
        std::abs(rect.left - destination.left) < 0.001f &&
        std::abs(rect.top - destination.top) < 0.001f &&
        std::abs(rect.right - destination.right) < 0.001f &&
        std::abs(rect.bottom - destination.bottom) < 0.001f)
    {
        D2D1_MATRIX_3X2_F transform{};
        state->ctx->GetTransform(&transform);
        if (transform._11 == 1.0f && transform._22 == 1.0f &&
            transform._12 == 0.0f && transform._21 == 0.0f)
        {
            const auto absolute = D2D1::RectF(
                destination.left + transform._31, destination.top + transform._32,
                destination.right + transform._31, destination.bottom + transform._32);
            const auto& widget = state->widgetRect;
            if (absolute.left >= widget.left && absolute.top >= widget.top &&
                absolute.right <= widget.right && absolute.bottom <= widget.bottom)
            {
                *spin = {sampled.bitmap, absolute, sourceRect, interpolation,
                    opacity, reducedMotion ? 0.0f : *node.imageSpinSpeed};
                snowdesktop::performance::Value("widget.composition",
                    "image_spin.layer", state->currentWidgetId, 1);
                return;
            }
        }
    }
    if (imageTint)
    {
        ComPtr<ID2D1Effect> tintEffect;
        if (SUCCEEDED(state->ctx->CreateEffect(
                CLSID_D2D1ColorMatrix, &tintEffect)) && tintEffect)
        {
            const D2D1_COLOR_F tint = D2D1::ColorF(*imageTint);
            // PREMULTIPLIED mode unpremultiplies the input and premultiplies
            // the matrix output. Supply constant straight RGB and scale only
            // alpha; putting RGB in the alpha row would multiply alpha twice.
            const D2D1_MATRIX_5X4_F matrix = {
                0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, opacity,
                tint.r, tint.g, tint.b, 0.0f,
            };
            tintEffect->SetInput(0, bitmap);
            if (SUCCEEDED(tintEffect->SetValue(
                    D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix)) &&
                SUCCEEDED(tintEffect->SetValue(
                    D2D1_COLORMATRIX_PROP_ALPHA_MODE,
                    D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED)))
            {
                D2D1_MATRIX_3X2_F previous{};
                state->ctx->GetTransform(&previous);
                const float scaleX = placement.destination.width /
                    (sourceRect.right - sourceRect.left);
                const float scaleY = placement.destination.height /
                    (sourceRect.bottom - sourceRect.top);
                const D2D1_MATRIX_3X2_F placementTransform =
                    D2D1::Matrix3x2F::Scale(scaleX, scaleY) *
                    D2D1::Matrix3x2F::Translation(
                        placement.destination.x,
                        placement.destination.y);
                state->ctx->SetTransform(placementTransform * previous);
                state->ctx->DrawImage(tintEffect.Get(),
                    D2D1::Point2F(-sourceRect.left, -sourceRect.top),
                    sourceRect, interpolation,
                    D2D1_COMPOSITE_MODE_SOURCE_OVER);
                state->ctx->SetTransform(previous);
                return;
            }
        }
    }
    state->ctx->DrawBitmap(bitmap, destination, opacity, interpolation, sourceRect);
}

static void DrawWidgetViewShadow(WidgetDrawContext* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    float radius, float opacity,
    const snowdesktop::widget_runtime::ViewThemePalette& palette)
{
    if (!state || !state->ctx || !node.shadow || opacity <= 0.0f) return;
    std::vector<snowdesktop::widget_runtime::DrawShadowLayer> layers;
    std::string error;
    if (!snowdesktop::widget_runtime::BuildDrawShadowLayers(
            { node.frame.x, node.frame.y,
                node.frame.width, node.frame.height },
            node.shadow->blur,
            std::min(radius,
                std::min(node.frame.width, node.frame.height) * 0.5f),
            node.shadow->offsetX, node.shadow->offsetY,
            node.shadow->alpha * opacity, layers, error))
        return;
    for (const auto& layer : layers)
    {
        const std::uint32_t shadowColor = node.shadow->colorToken
            ? snowdesktop::widget_runtime::ResolveViewThemeColor(
                *node.shadow->colorToken, palette)
            : node.shadow->color;
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(shadowColor), layer.alpha);
        if (!brush) continue;
        const D2D1_RECT_F shadowRect = D2D1::RectF(
            state->widgetRect.left + layer.bounds.x,
            state->widgetRect.top + layer.bounds.y,
            state->widgetRect.left + layer.bounds.x + layer.bounds.width,
            state->widgetRect.top + layer.bounds.y + layer.bounds.height);
        if (layer.radius > 0.0f)
            state->ctx->FillRoundedRectangle(
                D2D1::RoundedRect(shadowRect,
                    layer.radius, layer.radius), brush);
        else
            state->ctx->FillRectangle(shadowRect, brush);
    }
}

class WidgetViewTransformScope
{
public:
    WidgetViewTransformScope(WidgetDrawContext* state,
        const snowdesktop::widget_runtime::ViewNode& node)
        : WidgetViewTransformScope(state, node, node.transform)
    {
    }

    WidgetViewTransformScope(WidgetDrawContext* state,
        const snowdesktop::widget_runtime::ViewNode& node,
        const std::optional<snowdesktop::widget_runtime::ViewTransform>& transform)
        : WidgetViewTransformScope(state, snowdesktop::widget_runtime::
            ResolveViewLocalTransform(node.frame, transform))
    {
    }

    WidgetViewTransformScope(WidgetDrawContext* state,
        const snowdesktop::widget_runtime::ViewResolvedTransform& resolved)
        : context_(state ? state->ctx : nullptr)
    {
        if (!context_) return;
        if (std::abs(resolved.m11 - 1.0f) <= 0.000001f &&
            std::abs(resolved.m22 - 1.0f) <= 0.000001f &&
            std::abs(resolved.m12) <= 0.000001f &&
            std::abs(resolved.m21) <= 0.000001f &&
            std::abs(resolved.dx) <= 0.000001f &&
            std::abs(resolved.dy) <= 0.000001f)
            return;
        context_->GetTransform(&previous_);
        const float left = state->widgetRect.left;
        const float top = state->widgetRect.top;
        const D2D1_MATRIX_3X2_F local{
            resolved.m11,
            resolved.m12,
            resolved.m21,
            resolved.m22,
            resolved.dx + left - left * resolved.m11 -
                top * resolved.m21,
            resolved.dy + top - left * resolved.m12 -
                top * resolved.m22,
        };
        context_->SetTransform(local * previous_);
        active_ = true;
    }

    ~WidgetViewTransformScope()
    {
        if (active_) context_->SetTransform(previous_);
    }

    WidgetViewTransformScope(const WidgetViewTransformScope&) = delete;
    WidgetViewTransformScope& operator=(
        const WidgetViewTransformScope&) = delete;

private:
    ID2D1DeviceContext* context_ = nullptr;
    D2D1_MATRIX_3X2_F previous_{};
    bool active_ = false;
};

WidgetPanelContentClip::WidgetPanelContentClip(ID2D1DeviceContext* context, const RECT& bounds)
    : context_(context), bounds_(D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)))
{
    Resume();
}

WidgetPanelContentClip::~WidgetPanelContentClip()
{
    Suspend();
}

void WidgetPanelContentClip::Suspend()
{
    if (!active_) return;
    context_->PopAxisAlignedClip();
    active_ = false;
}

void WidgetPanelContentClip::Resume()
{
    if (active_) return;
    context_->PushAxisAlignedClip(bounds_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    active_ = true;
}

struct ViewScrollbarPaint
{
    const snowdesktop::widget_runtime::ViewNode* node;
    snowdesktop::widget_runtime::ViewStyle style;
    float opacity;
};

static void DrawWidgetViewScrollbar(WidgetDrawContext* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity)
{
    using snowdesktop::widget_runtime::ViewOrientation;
    if (!node.clipFrame) return;
    const auto& clip = *node.clipFrame;
    if (node.showScrollbar && node.scrollContentExtent >
            node.scrollViewportExtent &&
        node.scrollViewportExtent > 0.0f)
    {
        const bool vertical = node.orientation ==
            ViewOrientation::Vertical;
        const auto& track = node.scrollbarFrame.value_or(clip);
        const float crossExtent = vertical ? track.width : track.height;
        const float edgeInset = std::min(2.0f, crossExtent * 0.25f);
        const float barThickness = std::min(3.0f,
            std::max(0.0f, crossExtent - edgeInset));
        const int viewportExtent = std::max(1,
            static_cast<int>(std::lround(
                vertical ? clip.height : clip.width)));
        const auto scrollbar = snowdesktop::widget_scroll_rules::
            ResolveScrollbarAxisGeometry(
                0, viewportExtent,
                std::max(viewportExtent,
                    static_cast<int>(std::lround(
                        node.scrollContentExtent))),
                viewportExtent,
                static_cast<int>(std::lround(node.scrollOffset)),
                CalculateWidgetCellScale(
                    state->gridCellW, state->gridCellH));
        const std::uint32_t scrollbarColor =
            style.foreground.value_or(0xFFFFFF);
        ID2D1SolidColorBrush* trackBrush = GetCachedBrush(state,
            static_cast<int>(scrollbarColor), opacity * 0.10f);
        ID2D1SolidColorBrush* thumbBrush = GetCachedBrush(state,
            static_cast<int>(scrollbarColor), opacity * 0.48f);
        D2D1_RECT_F trackRect{};
        D2D1_RECT_F thumbRect{};
        if (vertical)
        {
            const float right = state->widgetRect.left + track.x +
                track.width - edgeInset;
            trackRect = D2D1::RectF(right - barThickness,
                state->widgetRect.top + clip.y + scrollbar.trackStart,
                right,
                state->widgetRect.top + clip.y + scrollbar.trackEnd);
            thumbRect = D2D1::RectF(trackRect.left,
                state->widgetRect.top + clip.y + scrollbar.thumbStart,
                trackRect.right,
                state->widgetRect.top + clip.y + scrollbar.thumbEnd);
        }
        else
        {
            const float bottom = state->widgetRect.top + track.y +
                track.height - edgeInset;
            trackRect = D2D1::RectF(
                state->widgetRect.left + clip.x + scrollbar.trackStart,
                bottom - barThickness,
                state->widgetRect.left + clip.x + scrollbar.trackEnd,
                bottom);
            thumbRect = D2D1::RectF(
                state->widgetRect.left + clip.x + scrollbar.thumbStart,
                trackRect.top,
                state->widgetRect.left + clip.x + scrollbar.thumbEnd,
                trackRect.bottom);
        }
        if (trackBrush)
            state->ctx->FillRoundedRectangle(
                D2D1::RoundedRect(trackRect, 1.5f, 1.5f), trackBrush);
        if (thumbBrush)
            state->ctx->FillRoundedRectangle(
                D2D1::RoundedRect(thumbRect, 1.5f, 1.5f), thumbBrush);
    }
}

static void DrawWidgetViewNode(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    std::string_view focusedKey,
    snowdesktop::widget_runtime::ViewTransitionRuntime* transitions,
    snowdesktop::widget_runtime::ViewTransitionRuntime::TimePoint now,
    bool reducedMotion,
    const snowdesktop::widget_runtime::ViewThemePalette& palette,
    float inheritedScale = 1.0f,
    const snowdesktop::widget_runtime::ViewTransitionPresentation*
        forcedPresentation = nullptr,
    std::vector<ViewScrollbarPaint>* gutterPaints = nullptr)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    using snowdesktop::widget_runtime::ViewShapeKind;
    using snowdesktop::widget_runtime::ViewIconFont;
    using snowdesktop::widget_runtime::ViewImageAlignment;
    using snowdesktop::widget_runtime::ViewImageFit;
    using snowdesktop::widget_runtime::ViewImageInterpolation;
    using snowdesktop::widget_runtime::ViewOrientation;
    using snowdesktop::widget_runtime::ViewTextAlignment;
    if (!state || !state->ctx || !node.visible ||
        node.visibility == snowdesktop::widget_runtime::
            ViewVisibility::Hidden ||
        node.frame.width <= 0.0f || node.frame.height <= 0.0f)
        return;
    const bool hovered = regions.IsHovered(node.key);
    const bool pressed = regions.IsPressed(node.key);
    const bool focused = node.key == focusedKey;
    auto style = snowdesktop::widget_runtime::ResolveViewThemeStyle(
        ResolveViewStyle(node, hovered, pressed, focused), palette);
    if (node.type == ViewNodeType::Link)
    {
        if (!style.foreground) style.foreground = palette.systemAccent;
        if (pressed && !node.pressedStyle.opacity)
            style.opacity = 0.65f;
        else if (hovered && !node.hoverStyle.opacity)
            style.opacity = 0.82f;
    }
    auto presentedTransform = node.transform;
    if (forcedPresentation)
    {
        style = forcedPresentation->style;
        presentedTransform = forcedPresentation->transform;
    }
    else if (transitions)
    {
        auto presentation = transitions->ResolvePresentation(node.key,
            style, node.transform,
            node.transition, node.enterTransition, now, reducedMotion);
        style = std::move(presentation.style);
        presentedTransform = std::move(presentation.transform);
    }
    WidgetViewTransformScope transformScope(state, node, presentedTransform);
    const float cumulativeScale = inheritedScale *
        (presentedTransform
            ? presentedTransform->scale * presentedTransform->scaleY
            : 1.0f);
    const bool badgeNode = node.type == ViewNodeType::Badge;
    const float opacity = std::clamp(
        style.opacity.value_or(1.0f), 0.0f, 1.0f);
    const float radius = std::max(0.0f,
        style.cornerRadius.value_or(badgeNode
            ? node.frame.height * 0.5f : 0.0f));
    const D2D1_RECT_F rect = D2D1::RectF(
        state->widgetRect.left + node.frame.x,
        state->widgetRect.top + node.frame.y,
        state->widgetRect.left + node.frame.x + node.frame.width,
        state->widgetRect.top + node.frame.y + node.frame.height);

    DrawWidgetViewShadow(state, node, radius, opacity, palette);

    const bool buttonNode = node.type == ViewNodeType::Button ||
        node.type == ViewNodeType::IconButton;
    const bool checkControlNode = node.type == ViewNodeType::Toggle ||
        node.type == ViewNodeType::Checkbox;
    const bool iconNode = node.type == ViewNodeType::Icon ||
        node.type == ViewNodeType::IconButton;
    const bool specialGeometry = node.type == ViewNodeType::Shape ||
        node.type == ViewNodeType::Divider ||
        node.type == ViewNodeType::ProgressBar ||
        node.type == ViewNodeType::ProgressRing ||
        node.type == ViewNodeType::Meter ||
        node.type == ViewNodeType::Icon || checkControlNode ||
        node.type == ViewNodeType::RadioGroup ||
        node.type == ViewNodeType::Slider ||
        IsDeclarativeInputNode(node.type) ||
        node.type == ViewNodeType::Select;
    const bool implicitSurfaceBackground = !style.background &&
        (buttonNode || badgeNode);
    std::optional<std::uint32_t> background = style.background;
    float backgroundAlpha = implicitSurfaceBackground ? 0.12f : 1.0f;
    if (buttonNode)
    {
        const auto fill = snowdesktop::widget_runtime::ResolveWidgetButtonFill(
            background, palette.textPrimary, node.enabled, hovered, pressed);
        background = fill.color;
        backgroundAlpha = fill.alpha;
    }
    else if (!background && badgeNode)
        background = 0xFFFFFF;
    if (background && !specialGeometry)
    {
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(*background), opacity * backgroundAlpha);
        if (brush)
        {
            if (radius > 0.0f)
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(rect, radius, radius), brush);
            else
                state->ctx->FillRectangle(rect, brush);
        }
    }
    if (buttonNode && implicitSurfaceBackground && !style.borderWidth &&
        !style.borderColor)
    {
        if (auto* brush = GetCachedBrush(state,
                static_cast<int>(palette.textPrimary), opacity * 0.20f))
        {
            const auto inset = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f,
                rect.right - 0.5f, rect.bottom - 0.5f);
            state->ctx->DrawRoundedRectangle(D2D1::RoundedRect(inset,
                std::max(0.0f, radius - 0.5f),
                std::max(0.0f, radius - 0.5f)), brush, 1.0f);
        }
    }
    const float borderWidth = std::max(0.0f,
        style.borderWidth.value_or(0.0f));
    if (borderWidth > 0.0f && style.borderColor && !specialGeometry)
    {
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(*style.borderColor), opacity);
        if (brush)
        {
            if (radius > 0.0f)
                state->ctx->DrawRoundedRectangle(
                    D2D1::RoundedRect(rect, radius, radius), brush,
                    borderWidth);
            else
                state->ctx->DrawRectangle(rect, brush, borderWidth);
        }
    }

    if (IsDeclarativeInputNode(node.type))
    {
        DrawDeclarativeViewInput(
            state, host, node, style, opacity, cumulativeScale);
        return;
    }
    if (node.type == ViewNodeType::Select)
    {
        DrawDeclarativeSelect(state, node, style, opacity);
        return;
    }
    if (node.type == ViewNodeType::StyledText)
    {
        DrawWidgetStyledText(state, host, node, style, opacity, regions, palette);
        return;
    }
    if (node.type == ViewNodeType::MonthCalendar)
    {
        DrawWidgetMonthCalendar(
            state, host, node, style, opacity, regions, focusedKey, palette);
        return;
    }
    if (node.type == ViewNodeType::Toggle)
    {
        const auto content = snowdesktop::widget_runtime::
            ViewNodeContentRect(node);
        const bool rightToLeft = ResolveViewTextDirection(
            node, Utf8ToWide(node.text)) ==
            DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
        const float trackWidth = std::min(36.0f,
            std::max(0.0f, content.width));
        const float trackHeight = std::min(20.0f, content.height);
        const float contentLeft = state->widgetRect.left + content.x;
        const float contentRight = contentLeft + content.width;
        const float left = rightToLeft ? contentLeft :
            std::max(contentLeft, contentRight - trackWidth);
        const float right = std::min(contentRight, left + trackWidth);
        const float top = state->widgetRect.top + content.y +
            (content.height - trackHeight) * 0.5f;
        const D2D1_RECT_F trackRect = D2D1::RectF(
            left, top, right, top + trackHeight);
        const std::uint32_t trackColor = style.background.value_or(
            node.checked ? 0x4C9AFF : 0xFFFFFF);
        const float trackAlpha = opacity *
            (style.background || node.checked ? 1.0f : 0.18f);
        if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
                static_cast<int>(trackColor), trackAlpha))
            state->ctx->FillRoundedRectangle(
                D2D1::RoundedRect(trackRect,
                    trackHeight * 0.5f, trackHeight * 0.5f), track);
        if (borderWidth > 0.0f && style.borderColor)
        {
            if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                    static_cast<int>(*style.borderColor), opacity))
                state->ctx->DrawRoundedRectangle(
                    D2D1::RoundedRect(trackRect,
                        trackHeight * 0.5f, trackHeight * 0.5f), border,
                    borderWidth);
        }
        const float knobRadius = std::max(0.0f, std::min(
            7.0f, std::min(trackHeight * 0.35f, trackWidth * 0.25f)));
        const float knobInset = std::min(trackWidth * 0.5f,
            knobRadius + std::min(3.0f, trackWidth * 0.1f));
        const float knobX = node.checked
            ? trackRect.right - knobInset : trackRect.left + knobInset;
        if (ID2D1SolidColorBrush* knob = GetCachedBrush(state,
                static_cast<int>(style.foreground.value_or(0xFFFFFF)),
                opacity))
            state->ctx->FillEllipse(D2D1::Ellipse(
                D2D1::Point2F(knobX,
                    (trackRect.top + trackRect.bottom) * 0.5f),
                knobRadius, knobRadius), knob);
    }
    else if (node.type == ViewNodeType::Checkbox)
    {
        const auto content = snowdesktop::widget_runtime::
            ViewNodeContentRect(node);
        const bool rightToLeft = ResolveViewTextDirection(
            node, Utf8ToWide(node.text)) ==
            DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
        const float boxSize = std::min(18.0f, std::max(0.0f,
            std::min(content.width, content.height)));
        const float contentLeft = state->widgetRect.left + content.x;
        const float left = rightToLeft
            ? contentLeft + content.width - boxSize : contentLeft;
        const float top = state->widgetRect.top + content.y +
            (content.height - boxSize) * 0.5f;
        const D2D1_RECT_F box = D2D1::RectF(
            left, top, left + boxSize, top + boxSize);
        if (node.checked || node.indeterminate || style.background)
        {
            const std::uint32_t fillColor = style.background.value_or(
                node.checked || node.indeterminate ? 0x4C9AFF : 0xFFFFFF);
            if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                    static_cast<int>(fillColor), opacity))
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(box,
                        std::min(3.0f, boxSize * 0.2f),
                        std::min(3.0f, boxSize * 0.2f)), fill);
        }
        const std::uint32_t borderColor = style.borderColor.value_or(
            style.foreground.value_or(0xFFFFFF));
        if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                static_cast<int>(borderColor), opacity *
                    (style.borderColor ? 1.0f : 0.55f)))
            state->ctx->DrawRoundedRectangle(
                D2D1::RoundedRect(box,
                    std::min(3.0f, boxSize * 0.2f),
                    std::min(3.0f, boxSize * 0.2f)), border,
                std::max(1.0f, borderWidth));
        if (node.checked || node.indeterminate)
        {
            if (ID2D1SolidColorBrush* mark = GetCachedBrush(state,
                    static_cast<int>(style.foreground.value_or(0xFFFFFF)),
                    opacity))
            {
                if (node.indeterminate)
                {
                    state->ctx->DrawLine(D2D1::Point2F(
                            left + boxSize * 0.24f,
                            top + boxSize * 0.50f),
                        D2D1::Point2F(left + boxSize * 0.76f,
                            top + boxSize * 0.50f), mark,
                        std::min(2.0f, boxSize * 0.12f));
                }
                else
                {
                    state->ctx->DrawLine(D2D1::Point2F(
                            left + boxSize * 0.22f,
                            top + boxSize * 0.50f),
                        D2D1::Point2F(left + boxSize * 0.42f,
                            top + boxSize * 0.72f), mark,
                        std::min(2.0f, boxSize * 0.12f));
                    state->ctx->DrawLine(D2D1::Point2F(
                            left + boxSize * 0.42f,
                            top + boxSize * 0.72f),
                        D2D1::Point2F(left + boxSize * 0.81f,
                            top + boxSize * 0.31f), mark,
                        std::min(2.0f, boxSize * 0.12f));
                }
            }
        }
    }
    else if (node.type == ViewNodeType::RadioGroup)
    {
        std::optional<std::wstring> privateFontPath;
        if (!node.fontResourceName.empty())
            privateFontPath = host.ResolveFont(node.fontResourceName);
        IDWriteTextFormat* format = node.fontResourceName.empty() ||
                privateFontPath
            ? GetCachedTextFormat(state, node.fontSize,
                node.bold ? DWRITE_FONT_WEIGHT_BOLD : state->itemFontWeight,
                false, DWRITE_WORD_WRAPPING_NO_WRAP, false, false, false,
                privateFontPath ? &*privateFontPath : nullptr)
            : nullptr;
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            const auto& option = node.options[index];
            const std::string optionKey = node.key + "/" + option.key;
            const bool optionHovered = regions.IsHovered(optionKey);
            const bool optionPressed = regions.IsPressed(optionKey);
            const bool optionFocused = optionKey == focusedKey;
            const bool selected = option.value == node.selectedValue;
            const auto optionStyle = snowdesktop::widget_runtime::
                ResolveViewThemeStyle(ResolveViewStyle(
                    node, optionHovered, optionPressed, optionFocused,
                    selected, node.enabled && option.enabled), palette);
            const float optionOpacity = std::clamp(
                optionStyle.opacity.value_or(1.0f), 0.0f, 1.0f);
            const auto frame = snowdesktop::widget_runtime::
                ViewRadioOptionFrame(node, index);
            const D2D1_RECT_F optionRect = D2D1::RectF(
                state->widgetRect.left + frame.x,
                state->widgetRect.top + frame.y,
                state->widgetRect.left + frame.x + frame.width,
                state->widgetRect.top + frame.y + frame.height);
            const float optionRadius = std::max(0.0f,
                optionStyle.cornerRadius.value_or(4.0f));
            if (optionStyle.background || optionHovered || optionPressed)
            {
                const std::uint32_t color =
                    optionStyle.background.value_or(0xFFFFFF);
                const float alpha = optionOpacity *
                    (optionStyle.background ? 1.0f :
                        (optionPressed ? 0.16f : 0.10f));
                if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                        static_cast<int>(color), alpha))
                    state->ctx->FillRoundedRectangle(
                        D2D1::RoundedRect(optionRect,
                            optionRadius, optionRadius), brush);
            }
            if (optionFocused && optionStyle.borderColor)
            {
                if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                        static_cast<int>(*optionStyle.borderColor),
                        optionOpacity))
                    state->ctx->DrawRoundedRectangle(
                        D2D1::RoundedRect(optionRect,
                            optionRadius, optionRadius), border,
                        std::max(1.0f,
                            optionStyle.borderWidth.value_or(1.5f)));
            }
            const float indicatorRadius = std::min(8.0f,
                std::max(0.0f, frame.height * 0.28f));
            const std::wstring label = Utf8ToWide(option.label);
            const bool rightToLeft = ResolveViewTextDirection(node, label) ==
                DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
            const float indicatorInset = std::min(frame.width * 0.5f,
                (rightToLeft ? node.padding.right : node.padding.left) +
                    10.0f);
            const float indicatorX = rightToLeft
                ? optionRect.right - indicatorInset
                : optionRect.left + indicatorInset;
            const float indicatorY =
                (optionRect.top + optionRect.bottom) * 0.5f;
            const std::uint32_t foreground =
                optionStyle.foreground.value_or(0xFFFFFF);
            if (ID2D1SolidColorBrush* outline = GetCachedBrush(state,
                    static_cast<int>(selected ? 0x4C9AFF : foreground),
                    optionOpacity * (selected ? 1.0f : 0.62f)))
                state->ctx->DrawEllipse(D2D1::Ellipse(
                    D2D1::Point2F(indicatorX, indicatorY),
                    indicatorRadius, indicatorRadius), outline,
                    std::max(1.0f,
                        optionStyle.borderWidth.value_or(1.5f)));
            if (selected)
            {
                if (ID2D1SolidColorBrush* mark = GetCachedBrush(state,
                        static_cast<int>(optionStyle.foreground.value_or(
                            0x4C9AFF)), optionOpacity))
                    state->ctx->FillEllipse(D2D1::Ellipse(
                        D2D1::Point2F(indicatorX, indicatorY),
                        indicatorRadius * 0.5f,
                        indicatorRadius * 0.5f), mark);
            }
            if (format && state->dwrite)
            {
                ComPtr<IDWriteTextLayout> layout;
                const float textLeft = rightToLeft
                    ? optionRect.left + node.padding.left
                    : indicatorX + indicatorRadius + 8.0f;
                const float textRight = rightToLeft
                    ? indicatorX - indicatorRadius - 8.0f
                    : optionRect.right - node.padding.right;
                const float textWidth = std::max(
                    0.0f, textRight - textLeft);
                if (!label.empty() && SUCCEEDED(
                        state->dwrite->CreateTextLayout(label.data(),
                            static_cast<UINT32>(label.size()), format,
                            textWidth, frame.height, &layout)) && layout)
                {
                    ApplyViewTextLocaleAndDirection(
                        layout.Get(), node, label);
                    SetViewTextLayoutAlignment(layout.Get(), node.textAlign);
                    layout->SetParagraphAlignment(
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DWRITE_TRIMMING trimming{};
                    trimming.granularity =
                        DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                    ComPtr<IDWriteInlineObject> ellipsis;
                    if (SUCCEEDED(state->dwrite->
                            CreateEllipsisTrimmingSign(format, &ellipsis)) &&
                        ellipsis)
                        layout->SetTrimming(&trimming, ellipsis.Get());
                    if (ID2D1SolidColorBrush* textBrush = GetCachedBrush(
                            state, static_cast<int>(foreground),
                            optionOpacity))
                        state->ctx->DrawTextLayout(D2D1::Point2F(
                            textLeft, optionRect.top), layout.Get(),
                            textBrush);
                }
            }
        }
    }
    else if (node.type == ViewNodeType::Slider)
    {
        const bool vertical = node.orientation == ViewOrientation::Vertical;
        const float span = node.maximum - node.minimum;
        const float normalized = span > 0.0f
            ? std::clamp((node.value - node.minimum) / span, 0.0f, 1.0f)
            : 0.0f;
        const float thumbRadius = std::min(8.0f, std::max(3.0f,
            std::min(node.frame.width, node.frame.height) * 0.3f));
        const float trackThickness = std::min(4.0f,
            vertical ? node.frame.width : node.frame.height);
        const auto content = snowdesktop::widget_runtime::ViewNodeContentRect(node);
        const auto geometry = snowdesktop::native_controls::Slider(
            D2D1::RectF(state->widgetRect.left + content.x, state->widgetRect.top + content.y,
                state->widgetRect.left + content.x + content.width,
                state->widgetRect.top + content.y + content.height),
            thumbRadius, trackThickness, normalized, vertical);
        const auto& trackRect = geometry.track;
        const auto& fillRect = geometry.fill;
        const auto& thumb = geometry.thumb;
        const float trackRadius = geometry.trackRadius;
        if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
                static_cast<int>(style.background.value_or(0xFFFFFF)),
                opacity * (style.background ? 1.0f : 0.22f)))
            state->ctx->FillRoundedRectangle(D2D1::RoundedRect(
                trackRect, trackRadius, trackRadius), track);
        const std::uint32_t fillColor =
            style.foreground.value_or(0x4C9AFF);
        if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                static_cast<int>(fillColor), opacity))
        {
            state->ctx->FillRoundedRectangle(D2D1::RoundedRect(
                fillRect, trackRadius, trackRadius), fill);
            state->ctx->FillEllipse(D2D1::Ellipse(
                thumb, thumbRadius, thumbRadius), fill);
        }
        if (borderWidth > 0.0f && style.borderColor)
        {
            if (ID2D1SolidColorBrush* outline = GetCachedBrush(state,
                    static_cast<int>(*style.borderColor), opacity))
                state->ctx->DrawEllipse(D2D1::Ellipse(
                    thumb, thumbRadius, thumbRadius), outline, borderWidth);
        }
    }
    else if (node.type == ViewNodeType::Divider)
    {
        const std::uint32_t dividerColor = style.foreground.value_or(
            style.background.value_or(0xFFFFFF));
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                static_cast<int>(dividerColor), opacity))
        {
            const float stroke = std::min(node.thickness,
                std::max(0.5f, node.orientation ==
                        ViewOrientation::Horizontal
                    ? node.frame.height : node.frame.width));
            if (node.orientation == ViewOrientation::Horizontal)
            {
                const float y = (rect.top + rect.bottom) * 0.5f;
                state->ctx->DrawLine(D2D1::Point2F(rect.left, y),
                    D2D1::Point2F(rect.right, y), brush, stroke);
            }
            else
            {
                const float x = (rect.left + rect.right) * 0.5f;
                state->ctx->DrawLine(D2D1::Point2F(x, rect.top),
                    D2D1::Point2F(x, rect.bottom), brush, stroke);
            }
        }
    }
    else if (node.type == ViewNodeType::Shape)
    {
        const bool ellipse = node.shapeKind == ViewShapeKind::Circle ||
            node.shapeKind == ViewShapeKind::Ellipse;
        const float centerX = (rect.left + rect.right) * 0.5f;
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        float radiusX = (rect.right - rect.left) * 0.5f;
        float radiusY = (rect.bottom - rect.top) * 0.5f;
        if (node.shapeKind == ViewShapeKind::Circle)
            radiusX = radiusY = std::min(radiusX, radiusY);
        const D2D1_ELLIPSE ellipseGeometry = D2D1::Ellipse(
            D2D1::Point2F(centerX, centerY), radiusX, radiusY);
        const float shapeRadius = node.shapeKind ==
                ViewShapeKind::RoundedRectangle
            ? std::max(0.0f, style.cornerRadius.value_or(4.0f))
            : radius;
        if (style.background)
        {
            if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                    static_cast<int>(*style.background), opacity))
            {
                if (ellipse) state->ctx->FillEllipse(ellipseGeometry, brush);
                else if (shapeRadius > 0.0f)
                    state->ctx->FillRoundedRectangle(
                        D2D1::RoundedRect(rect, shapeRadius, shapeRadius),
                        brush);
                else state->ctx->FillRectangle(rect, brush);
            }
        }
        if (borderWidth > 0.0f && style.borderColor)
        {
            if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                    static_cast<int>(*style.borderColor), opacity))
            {
                if (ellipse)
                    state->ctx->DrawEllipse(
                        ellipseGeometry, brush, borderWidth);
                else if (shapeRadius > 0.0f)
                    state->ctx->DrawRoundedRectangle(
                        D2D1::RoundedRect(rect, shapeRadius, shapeRadius),
                        brush, borderWidth);
                else state->ctx->DrawRectangle(rect, brush, borderWidth);
            }
        }
    }
    else if (node.type == ViewNodeType::ProgressBar ||
        node.type == ViewNodeType::Meter)
    {
        const std::uint32_t trackColor =
            style.background.value_or(0xFFFFFF);
        const std::uint32_t fillColor =
            style.foreground.value_or(0xFFFFFF);
        const float barRadius = std::max(0.0f,
            style.cornerRadius.value_or(
                (rect.bottom - rect.top) * 0.5f));
        if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
                static_cast<int>(trackColor),
                opacity * node.trackOpacity))
            state->ctx->FillRoundedRectangle(
                D2D1::RoundedRect(rect, barRadius, barRadius), track);
        if (node.indeterminate && node.type == ViewNodeType::ProgressBar)
        {
            constexpr float SegmentFraction = 0.28f;
            constexpr float Pi = 3.14159265358979323846f;
            const float phase = WidgetIndeterminateProgressPhase(
                now, reducedMotion);
            const float travel = 0.5f - 0.5f *
                std::cos(phase * 2.0f * Pi);
            const float width = rect.right - rect.left;
            const float segmentWidth = width * SegmentFraction;
            D2D1_RECT_F fillRect = rect;
            fillRect.left += (width - segmentWidth) * travel;
            fillRect.right = fillRect.left + segmentWidth;
            if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                    static_cast<int>(fillColor),
                    opacity * node.fillOpacity))
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        fillRect, barRadius, barRadius), fill);
        }
        else if (node.value > 0.0f)
        {
            D2D1_RECT_F fillRect = rect;
            fillRect.right = fillRect.left +
                (fillRect.right - fillRect.left) * node.value;
            if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                    static_cast<int>(fillColor),
                    opacity * node.fillOpacity))
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        fillRect, barRadius, barRadius), fill);
        }
    }
    else if (node.type == ViewNodeType::ProgressRing)
    {
        DrawWidgetProgressRing(state, rect, node.value,
            node.indeterminate,
            WidgetIndeterminateProgressPhase(now, reducedMotion),
            std::min(node.thickness,
                std::min(node.frame.width, node.frame.height)),
            style.background.value_or(0xFFFFFF),
            opacity * node.trackOpacity,
            style.foreground.value_or(0xFFFFFF),
            opacity * node.fillOpacity);
    }
    else if (IsWidgetDataSeriesNode(node.type))
    {
        DrawWidgetDataSeries(state, node, style, rect, opacity);
    }
    else if ((node.type == ViewNodeType::Image ||
            node.type == ViewNodeType::ReferenceIcon))
    {
        ID2D1Bitmap1* bitmap = nullptr;
        snowdesktop::widget_runtime::PackageImageSize original;
        if (node.type == ViewNodeType::Image)
        {
            const auto content = ViewNodeContentRect(node);
            using Fit = DrawImageFit;
            const auto fit = node.imageFit == ViewImageFit::Cover ? Fit::Cover :
                node.imageFit == ViewImageFit::Fill ? Fit::Fill :
                node.imageFit == ViewImageFit::None ? Fit::None : Fit::Contain;
            bitmap = host.LoadImage(node.imageResourceName,
                std::max(0.0f, content.width - borderWidth * 2.0f),
                std::max(0.0f, content.height - borderWidth * 2.0f), fit,
                node.imageInterpolation == ViewImageInterpolation::Nearest, original);
        }
        else
        {
            const auto target = host.ResolveReference(node.itemReference);
            if (target && host.IsPreview())
            {
                const auto content = snowdesktop::widget_runtime::
                    ViewNodeContentRect(node);
                const float availableWidth = std::max(
                    0.0f, content.width - borderWidth * 2.0f);
                const float availableHeight = std::max(
                    0.0f, content.height - borderWidth * 2.0f);
                const float size = std::max(0.0f, std::min(
                    availableWidth, availableHeight));
                float x = content.x + borderWidth;
                float y = content.y + borderWidth;
                if (node.imageAlignment == ViewImageAlignment::Center)
                {
                    x += std::max(0.0f, (availableWidth - size) * 0.5f);
                    y += std::max(0.0f, (availableHeight - size) * 0.5f);
                }
                else if (node.imageAlignment == ViewImageAlignment::End)
                {
                    x += std::max(0.0f, availableWidth - size);
                    y += std::max(0.0f, availableHeight - size);
                }
                DrawSimulatedPreviewIcon(state, *target,
                    Utf8ToWide(node.alt), x, y, size, opacity);
                return;
            }
            if (target)
                bitmap = host.LoadShellIcon(*target);
        }
        ComPtr<ID2D1RoundedRectangleGeometry> imageClip;
        bool imageClipPushed = false;
        float imageClipRadius = 0.0f;
        if (node.type == ViewNodeType::Image && bitmap && radius > 0.0f)
        {
            ComPtr<ID2D1Factory> factory;
            state->ctx->GetFactory(&factory);
            const float clipRadius = std::min(radius,
                std::min(node.frame.width, node.frame.height) * 0.5f);
            if (factory && SUCCEEDED(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(rect, clipRadius, clipRadius),
                    &imageClip)) && imageClip)
            {
                state->ctx->PushLayer(D2D1::LayerParameters(
                    rect, imageClip.Get(),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
                imageClipPushed = true;
                imageClipRadius = clipRadius;
            }
        }
        if (auto* paint = state->desktopSurfacePaint;
            paint && paint->revision != 0)
            paint->images.emplace_back(bitmap);
        DrawWidgetViewBitmap(
            state, node, bitmap, rect, opacity, borderWidth, imageClipRadius,
            reducedMotion, palette, original);
        if (imageClipPushed)
            state->ctx->PopLayer();
    }

    bool drewGlyph = false;
    if (iconNode && node.glyphInkCentered)
    {
        const auto content = snowdesktop::widget_runtime::ViewNodeContentRect(node);
        auto* brush = GetCachedBrush(state,
            static_cast<int>(style.foreground.value_or(0xFFFFFF)), opacity);
        D2D1_RECT_F bounds{};
        drewGlyph = brush && state->DrawCenteredGlyph(
            snowdesktop::widget_runtime::DecodeIconGlyph(node.text),
            node.iconFont == ViewIconFont::Fluent,
            state->widgetRect.left + content.x + content.width * 0.5f,
            state->widgetRect.top + content.y + content.height * 0.5f,
            node.fontSize, *brush, &bounds);
        if (auto* paint = state->desktopSurfacePaint;
            drewGlyph && paint && paint->revision != 0 && node.localRefresh)
        {
            constexpr float padding = 2.0f;
            const RECT ink{static_cast<LONG>(std::floor(bounds.left - padding)),
                static_cast<LONG>(std::floor(bounds.top - padding)),
                static_cast<LONG>(std::ceil(bounds.right + padding)),
                static_cast<LONG>(std::ceil(bounds.bottom + padding))};
            UnionRect(&paint->textBounds, &paint->textBounds, &ink);
        }
    }
    if (!drewGlyph && (node.type == ViewNodeType::Text ||
            node.type == ViewNodeType::Badge ||
            node.type == ViewNodeType::Button ||
            node.type == ViewNodeType::Link || checkControlNode ||
            iconNode) &&
        !node.text.empty() && state->dwrite)
    {
        std::optional<std::wstring> privateFontPath;
        if (!node.fontResourceName.empty())
            privateFontPath = host.ResolveFont(node.fontResourceName);
        IDWriteTextFormat* format = node.fontResourceName.empty() ||
                privateFontPath
            ? GetCachedTextFormat(state,
            node.fontSize,
            ViewFontWeight(node, state->itemFontWeight),
            iconNode, ViewTextWrapping(node),
            iconNode && node.iconFont == ViewIconFont::FontAwesome,
            iconNode,
            iconNode && node.iconFont == ViewIconFont::Fluent,
            privateFontPath ? &*privateFontPath : nullptr)
            : nullptr;
        const std::uint32_t foreground =
            style.foreground.value_or(0xFFFFFF);
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(foreground), opacity);
        const std::wstring text = Utf8ToWide(node.text);
        if (format && brush && !text.empty())
        {
            const auto content = snowdesktop::widget_runtime::
                ViewNodeContentRect(node);
            float textLeft = state->widgetRect.left + content.x;
            float textRight = textLeft + content.width;
            if (node.type == ViewNodeType::Checkbox)
            {
                const bool rightToLeft = ResolveViewTextDirection(
                    node, text) == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
                if (rightToLeft)
                    textRight = std::max(textLeft, textRight - 26.0f);
                else
                    textLeft = std::min(textRight, textLeft + 26.0f);
            }
            else if (node.type == ViewNodeType::Toggle)
            {
                const bool rightToLeft = ResolveViewTextDirection(
                    node, text) == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
                if (rightToLeft)
                    textLeft = std::min(textRight, textLeft + 44.0f);
                else
                    textRight = std::max(textLeft, textRight - 44.0f);
            }
            const float textWidth = std::max(0.0f,
                textRight - textLeft);
            const float textHeight = content.height;
            const float layoutHeight = ViewTextLayoutHeight(
                node, textHeight);
            using TextLayoutCache =
                snowdesktop::widget_runtime::WidgetTextLayoutCache;
            const TextLayoutCache::Options options{
                textWidth, layoutHeight, node.fontStyle, node.textDirection,
                node.overflowText,
                iconNode || node.textAlign == ViewTextAlignment::Center
                    ? DWRITE_TEXT_ALIGNMENT_CENTER
                    : node.textAlign == ViewTextAlignment::End
                        ? DWRITE_TEXT_ALIGNMENT_TRAILING
                        : DWRITE_TEXT_ALIGNMENT_LEADING,
                node.lineHeight, node.letterSpacing,
                node.type == ViewNodeType::Link };
            const auto layout = state->textLayoutCache.Resolve(
                state->currentWidgetId, CurrentWidgetSurface(state), node.key,
                format, options, text, node.locale, [&]() {
                snowdesktop::performance::Scope scope(
                    "widget.view", "text.layout.create", state->currentWidgetId);
                ComPtr<IDWriteTextLayout> created;
                if (FAILED(state->dwrite->CreateTextLayout(
                        text.data(), static_cast<UINT32>(text.size()),
                        format, textWidth, layoutHeight, &created)) || !created)
                    return ComPtr<IDWriteTextLayout>{};
                // Configuration happens only on a new layout. Cached layouts
                // have no drawing effects and are never mutated after storage.
                ApplyViewTextLocaleAndDirection(created.Get(), node, text);
                if (iconNode || node.textAlign == ViewTextAlignment::Center)
                    created->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                else if (node.textAlign == ViewTextAlignment::End)
                    created->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
                else
                    created->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_LEADING);
                created->SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                ApplyViewTextTypography(created.Get(), node,
                    static_cast<UINT32>(text.size()));
                if (node.type == ViewNodeType::Link)
                    created->SetUnderline(TRUE,
                        DWRITE_TEXT_RANGE{ 0,
                            static_cast<UINT32>(text.size()) });
                SetViewTextOverflow(state, format, created.Get(), node);
                return created;
            });
            if (layout)
            {
                DWRITE_TEXT_METRICS metrics{};
                const float contentHeight = SUCCEEDED(
                        layout->GetMetrics(&metrics))
                    ? std::min(layoutHeight, metrics.height) : layoutHeight;
                // Icon ink is centered against the full layout box below.
                // Applying the text metrics offset first would center the line
                // box and then center the glyph a second time.
                const float verticalOffset = iconNode ? 0.0f :
                    ViewTextVerticalOffset(
                        node, textHeight, contentHeight);
                D2D1_POINT_2F origin = D2D1::Point2F(
                    textLeft, state->widgetRect.top + content.y +
                        verticalOffset);
                D2D1_RECT_F inkBounds{};
                if (node.textInkAligned &&
                    GetTextLayoutInkBounds(*layout.Get(), inkBounds) &&
                    inkBounds.right > inkBounds.left &&
                    inkBounds.bottom > inkBounds.top)
                {
                    if (inkBounds.right - inkBounds.left <= textWidth)
                    {
                        if (node.textAlign == ViewTextAlignment::Center)
                            origin.x += (textWidth - inkBounds.left - inkBounds.right) * 0.5f;
                        else
                        {
                            const bool rtl = layout->GetReadingDirection() ==
                                DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
                            const bool rightAligned = (node.textAlign == ViewTextAlignment::End) != rtl;
                            origin.x += rightAligned
                                ? textWidth - inkBounds.right : -inkBounds.left;
                        }
                    }
                    if (inkBounds.bottom - inkBounds.top <= textHeight)
                    {
                        const float gap = textHeight - (inkBounds.bottom - inkBounds.top);
                        const float offset = ViewTextVerticalOffset(node, gap, 0.0f);
                        origin.y += offset - verticalOffset - inkBounds.top;
                    }
                }
                if (iconNode)
                {
                    DWRITE_OVERHANG_METRICS overhang{};
                    if (SUCCEEDED(layout->GetOverhangMetrics(&overhang)))
                    {
                        origin.x -= (overhang.right - overhang.left) * 0.5f;
                        origin.y -= (overhang.bottom - overhang.top) * 0.5f;
                    }
                }
                if (node.textGlowRadius && *node.textGlowRadius > 0.0f)
                    state->textGlowCache.Draw(*state->ctx, *layout.Get(),
                        *brush, origin, *node.textGlowRadius);
                else
                {
                    state->ctx->DrawTextLayout(origin, layout.Get(), brush);
                    if (auto* paint = state->desktopSurfacePaint;
                        paint && paint->revision != 0 && node.localRefresh)
                    {
                        DWRITE_OVERHANG_METRICS overhang{};
                        if (SUCCEEDED(layout->GetOverhangMetrics(&overhang)))
                        {
                            constexpr float padding = 2.0f;
                            const RECT ink{
                                static_cast<LONG>(std::floor(origin.x -
                                    std::max(0.0f, overhang.left) - padding)),
                                static_cast<LONG>(std::floor(origin.y -
                                    std::max(0.0f, overhang.top) - padding)),
                                static_cast<LONG>(std::ceil(origin.x + textWidth +
                                    std::max(0.0f, overhang.right) + padding)),
                                static_cast<LONG>(std::ceil(origin.y + layoutHeight +
                                    std::max(0.0f, overhang.bottom) + padding)),
                            };
                            UnionRect(&paint->textBounds, &paint->textBounds, &ink);
                        }
                        else
                            paint->revision = 0;
                    }
                }
            }
        }
    }

    if (node.clipFrame)
    {
        const auto& clip = *node.clipFrame;
        state->ctx->PushAxisAlignedClip(D2D1::RectF(
            state->widgetRect.left + clip.x,
            state->widgetRect.top + clip.y,
            state->widgetRect.left + clip.x + clip.width,
            state->widgetRect.top + clip.y + clip.height),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ++state->widgetClipDepth;
        for (const auto* child : snowdesktop::widget_runtime::
                ViewChildrenInPaintOrder(node))
            DrawWidgetViewNode(
                state, host, *child, regions, focusedKey, transitions,
                now, reducedMotion, palette, cumulativeScale, nullptr, gutterPaints);
        state->ctx->PopAxisAlignedClip();
        --state->widgetClipDepth;

        if (node.scrollbarFrame && gutterPaints)
            gutterPaints->push_back({&node, style, opacity});
        else
            DrawWidgetViewScrollbar(state, node, style, opacity);
        return;
    }
    for (const auto* child : snowdesktop::widget_runtime::
            ViewChildrenInPaintOrder(node))
        DrawWidgetViewNode(
            state, host, *child, regions, focusedKey, transitions,
            now, reducedMotion, palette, cumulativeScale, nullptr, gutterPaints);
    if (focused && specialGeometry && style.borderColor)
    {
        if (ID2D1SolidColorBrush* focus = GetCachedBrush(state,
                static_cast<int>(*style.borderColor), opacity))
            state->ctx->DrawRoundedRectangle(
                D2D1::RoundedRect(rect, std::max(3.0f, radius),
                    std::max(3.0f, radius)), focus,
                std::max(1.0f, style.borderWidth.value_or(1.5f)));
    }
}

bool DrawWidgetViewTree(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& tree,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    std::string_view focusedKey,
    snowdesktop::widget_runtime::ViewTransitionRuntime& transitions,
    bool reducedMotion, WidgetPanelContentClip* contentClip)
{
    snowdesktop::performance::Scope scope("widget.view", "draw",
        state ? std::wstring_view(state->currentWidgetId) : std::wstring_view{});
    if (!state || !state->ctx) return false;
    std::vector<ViewScrollbarPaint> gutterPaints;
    const auto now = snowdesktop::widget_runtime::
        ViewTransitionRuntime::Clock::now();
    const auto palette = host.ThemePalette();
    const auto textCountsBefore = state->textLayoutCache.Statistics();
    transitions.SetDurationScale(snowdesktop::animation::RuntimeDurationScale());
    const auto& painted = transitions.LayoutForPaint(tree, now, reducedMotion);
    transitions.BeginFrame();
    DrawWidgetViewNode(state, host, painted, regions, focusedKey,
        &transitions, now, reducedMotion, palette, 1.0f, nullptr, &gutterPaints);
    transitions.EndFrame();
    for (const auto& exit : transitions.ExitFrames(painted, now, reducedMotion))
    {
        if (!exit.node) continue;
        bool pushedClip = false;
        if (exit.parentClip)
        {
            const auto& clip = *exit.parentClip;
            state->ctx->PushAxisAlignedClip(D2D1::RectF(
                state->widgetRect.left + clip.x,
                state->widgetRect.top + clip.y,
                state->widgetRect.left + clip.x + clip.width,
                state->widgetRect.top + clip.y + clip.height),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            ++state->widgetClipDepth;
            pushedClip = true;
        }
        {
            WidgetViewTransformScope parentTransform(
                state, exit.parentTransform);
            const float inheritedScale = std::hypot(
                exit.parentTransform.m21,
                exit.parentTransform.m22);
            DrawWidgetViewNode(state, host, *exit.node, regions, {}, nullptr,
                now, reducedMotion, palette,
                std::max(0.000001f, inheritedScale),
                &exit.presentation, &gutterPaints);
        }
        if (pushedClip)
        {
            state->ctx->PopAxisAlignedClip();
            --state->widgetClipDepth;
        }
    }
    // Content stays inside its original viewport; host edge tracks paint last.
    if (!gutterPaints.empty())
    {
        if (contentClip) contentClip->Suspend();
        for (const auto& paint : gutterPaints)
            DrawWidgetViewScrollbar(state, *paint.node, paint.style, paint.opacity);
        if (contentClip) contentClip->Resume();
    }
    const auto textCountsAfter = state->textLayoutCache.Statistics();
    if (textCountsAfter.hits != textCountsBefore.hits)
        snowdesktop::performance::Value("widget.text.layout", "hits_per_draw",
            state->currentWidgetId,
            static_cast<double>(textCountsAfter.hits - textCountsBefore.hits));
    if (textCountsAfter.misses != textCountsBefore.misses)
        snowdesktop::performance::Value("widget.text.layout", "misses_per_draw",
            state->currentWidgetId,
            static_cast<double>(textCountsAfter.misses - textCountsBefore.misses));
    if (textCountsAfter.bypasses != textCountsBefore.bypasses)
        snowdesktop::performance::Value("widget.text.layout", "bypasses_per_draw",
            state->currentWidgetId,
            static_cast<double>(textCountsAfter.bypasses - textCountsBefore.bypasses));
    return transitions.HasActive();
}

bool MeasureWidgetViewTooltip(IDWriteFactory* dwrite, const D2D1_RECT_F& widgetRect,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions, float scale,
    snowdesktop::NativeTooltipTextLayout& measured, D2D1_RECT_F& bounds, std::wstring& name)
{
    if (!dwrite) return false;
    const auto* region = regions.Find(regions.HoveredKey());
    if (!region || region->tooltip.empty()) return false;
    const float surfaceWidth = std::max(
        0.0f, widgetRect.right - widgetRect.left);
    const float surfaceHeight = std::max(
        0.0f, widgetRect.bottom - widgetRect.top);
    const float maximumWidth = std::max(
        1.0f, std::min(280.0f * scale, surfaceWidth - 8.0f * scale));
    const float maximumHeight = std::max(
        1.0f, std::min(192.0f * scale, surfaceHeight - 8.0f * scale));
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(snowdesktop::CreateNativeTooltipTextFormat(dwrite, &format, scale))) return false;
    const std::wstring title = Utf8ToWide(region->tooltipTitle);
    const std::wstring body = Utf8ToWide(region->tooltip);
    snowdesktop::NativeTooltipLayoutOptions options;
    options.scale = scale;
    options.centerTextInkVertically = true;
    if (FAILED(snowdesktop::MeasureNativeTooltip(dwrite,
        format.Get(), title, body, maximumWidth, maximumHeight, measured, options))) return false;
    const float width = measured.width, height = measured.height;
    const auto& shape = region->shape;
    const bool circle = shape.type ==
        snowdesktop::widget_runtime::InteractionShapeType::Circle;
    float left = circle ? shape.x - shape.radius : shape.x;
    float top = circle ? shape.y - shape.radius : shape.y;
    float right = circle ? shape.x + shape.radius : shape.x + shape.width;
    float bottom = circle ? shape.y + shape.radius : shape.y + shape.height;
    if (region->clip)
    {
        left = std::max(left, region->clip->x);
        top = std::max(top, region->clip->y);
        right = std::min(right, region->clip->x + region->clip->width);
        bottom = std::min(bottom, region->clip->y + region->clip->height);
    }
    // Anchor to the visible region, independent of where the pointer entered it.
    float x = (left + right - width) * 0.5f;
    float y = bottom + 6.0f * scale;
    if (y + height > surfaceHeight - 4.0f * scale)
        y = top - height - 6.0f * scale;
    x = std::clamp(x, 4.0f * scale, std::max(4.0f * scale, surfaceWidth - width - 4.0f * scale));
    y = std::clamp(y, 4.0f * scale, std::max(4.0f * scale, surfaceHeight - height - 4.0f * scale));
    bounds = D2D1::RectF(widgetRect.left + x, widgetRect.top + y,
        widgetRect.left + x + width, widgetRect.top + y + height);
    name = title.empty() ? body : title + L"\n" + body;
    return true;
}

void DrawWidgetViewTooltip(WidgetDrawContext* state,
    const WidgetInteractionRegions& regions, float scale)
{
    if (!state || !state->ctx || !state->dwrite) return;
    snowdesktop::NativeTooltipTextLayout measured;
    D2D1_RECT_F bounds{};
    std::wstring name;
    if (MeasureWidgetViewTooltip(state->dwrite, state->widgetRect, regions, scale, measured, bounds, name))
        snowdesktop::DrawNativeTooltip(state->ctx, bounds, measured, snowdesktop::NativeTooltipAppearance(), scale);
}

void DrawWidgetSelectOverlays(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    std::string_view focusedKey,
    float viewportHeight)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (!state || !state->ctx || !node.visible ||
        node.visibility == snowdesktop::widget_runtime::
            ViewVisibility::Hidden) return;
    WidgetViewTransformScope transformScope(state, node);
    if (node.type == ViewNodeType::Select && node.expanded)
    {
        const auto popup = snowdesktop::widget_runtime::
            ViewSelectPopupFrame(node, viewportHeight);
        if (popup.width <= 0.0f || popup.height <= 0.0f) return;
        const D2D1_RECT_F popupRect = D2D1::RectF(
            state->widgetRect.left + popup.x,
            state->widgetRect.top + popup.y,
            state->widgetRect.left + popup.x + popup.width,
            state->widgetRect.top + popup.y + popup.height);
        state->ctx->PushAxisAlignedClip(popupRect,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        IDWriteTextFormat* format = GetCachedTextFormat(state,
            node.fontSize, state->itemFontWeight, false,
            DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
        const auto palette = host.ThemePalette();
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            const auto& option = node.options[index];
            const auto frame = snowdesktop::widget_runtime::
                ViewSelectOptionFrame(node, index, viewportHeight);
            const std::string key = node.key + "/" + option.key;
            const bool enabled = option.enabled && node.enabled;
            const bool hovered = enabled && regions.IsHovered(key);
            const bool focused = enabled && key == focusedKey;
            const bool selected = option.value == node.selectedValue;
            const auto backgroundColor = focused ? palette.systemAccent
                : (selected ? MixWidgetRgb(palette.surface,
                        palette.systemAccent, 0.18f)
                    : (hovered ? palette.surfaceVariant : palette.surface));
            const auto foregroundColor = !enabled ? palette.textDisabled
                : (focused ? palette.accentText : palette.textPrimary);
            const D2D1_RECT_F rect = D2D1::RectF(
                state->widgetRect.left + frame.x,
                state->widgetRect.top + frame.y,
                state->widgetRect.left + frame.x + frame.width,
                state->widgetRect.top + frame.y + frame.height);
            if (ID2D1SolidColorBrush* background = GetCachedBrush(state,
                    static_cast<int>(backgroundColor), 1.0f))
                state->ctx->FillRectangle(rect, background);
            if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                    static_cast<int>(palette.border), 0.45f))
                state->ctx->DrawRectangle(rect, border, 1.0f);
            // Options are painted above the declarative tree, so they need
            // their own keyboard cue. Keep it distinct from the saved value.
            if (focused && frame.width > 6.0f && frame.height > 6.0f)
            {
                const auto focusRect = D2D1::RectF(rect.left + 2.0f,
                    rect.top + 2.0f, rect.right - 2.0f, rect.bottom - 2.0f);
                if (ID2D1SolidColorBrush* outer = GetCachedBrush(state,
                        0x000000, 1.0f))
                    state->ctx->DrawRectangle(focusRect, outer, 3.0f);
                if (ID2D1SolidColorBrush* inner = GetCachedBrush(state,
                        0xFFFFFF, 1.0f))
                    state->ctx->DrawRectangle(focusRect, inner, 1.0f);
            }
            const std::wstring text = Utf8ToWide(option.label);
            const bool rightToLeft = ResolveViewTextDirection(node, text) ==
                DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
            if (selected)
            {
                const float size = std::min({ 14.0f, frame.height * 0.5f,
                    std::max(0.0f, frame.width - 16.0f) });
                const float left = rightToLeft
                    ? rect.right - 10.0f - size : rect.left + 10.0f;
                const float top = (rect.top + rect.bottom - size) * 0.5f;
                if (size > 0.0f)
                {
                    if (ID2D1SolidColorBrush* mark = GetCachedBrush(state,
                            static_cast<int>(foregroundColor), 1.0f))
                    {
                        state->ctx->DrawLine(D2D1::Point2F(left,
                                top + size * 0.5f),
                            D2D1::Point2F(left + size * 0.35f, top + size),
                            mark, 2.0f);
                        state->ctx->DrawLine(D2D1::Point2F(
                                left + size * 0.35f, top + size),
                            D2D1::Point2F(left + size, top), mark, 2.0f);
                    }
                }
            }
            if (format)
            {
                if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                        static_cast<int>(foregroundColor), 1.0f))
                {
                    ComPtr<IDWriteTextLayout> layout;
                    const float width = std::max(
                        0.0f, frame.width - 40.0f);
                    if (width > 0.0f && SUCCEEDED(
                            state->dwrite->CreateTextLayout(text.data(),
                                static_cast<UINT32>(text.size()), format,
                                width, frame.height, &layout)) && layout)
                    {
                        ApplyViewTextLocaleAndDirection(
                            layout.Get(), node, text);
                        SetViewTextLayoutAlignment(
                            layout.Get(), node.textAlign);
                        layout->SetParagraphAlignment(
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        state->ctx->DrawTextLayout(D2D1::Point2F(
                            rect.left + (rightToLeft ? 10.0f : 30.0f),
                            rect.top), layout.Get(),
                            brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
        }
        if (node.scrollContentExtent > node.scrollViewportExtent)
        {
            const int viewportExtent = std::max(1,
                static_cast<int>(std::lround(popup.height)));
            const auto scrollbar = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(0, viewportExtent,
                    std::max(viewportExtent,
                        static_cast<int>(std::lround(
                            node.scrollContentExtent))),
                    viewportExtent,
                    static_cast<int>(std::lround(node.scrollOffset)),
                    CalculateWidgetCellScale(
                        state->gridCellW, state->gridCellH));
            DrawHostRect(state, popup.x + popup.width - 5.0f,
                popup.y, 3.0f, popup.height, 0xFFFFFF, 1.5f, 0.12f);
            DrawHostRect(state, popup.x + popup.width - 5.0f,
                popup.y + scrollbar.thumbStart, 3.0f,
                static_cast<float>(scrollbar.thumbEnd -
                    scrollbar.thumbStart),
                0xFFFFFF, 1.5f, 0.65f);
        }
        state->ctx->PopAxisAlignedClip();
    }
    for (const auto* child : snowdesktop::widget_runtime::
            ViewChildrenInPaintOrder(node))
        DrawWidgetSelectOverlays(state, host, *child, regions, focusedKey,
            viewportHeight);
}

bool HasLocalRefresh(const snowdesktop::widget_runtime::ViewNode& node)
{
    return node.localRefresh ||
        std::any_of(node.children.begin(), node.children.end(), HasLocalRefresh);
}

// The old tree has already supplied exit transitions and will be replaced.
// Normalize only the supported values so the regular tree equality checks
// every other property, including layout and SMTC-driven metadata.
void CollectLocalRefresh(
    snowdesktop::widget_runtime::ViewNode& previous,
    const snowdesktop::widget_runtime::ViewNode& current,
    const RECT& bounds, snowdesktop::widget_runtime::WidgetSurfacePaint& paint)
{
    using namespace snowdesktop::widget_runtime;
    if (current.key != previous.key || current.type != previous.type ||
        current.transform || current.transition ||
        current.children.size() != previous.children.size())
        return;
    for (std::size_t i = 0; i < current.children.size(); ++i)
        CollectLocalRefresh(previous.children[i], current.children[i], bounds, paint);
    if (!current.localRefresh || !previous.localRefresh || current.shadow)
        return;

    ViewRect rect = current.frame;
    float padding = 2.0f;
    if (current.type == ViewNodeType::Spectrum && current.values != previous.values)
    {
        rect = ViewNodeContentRect(current);
        const auto peak = [](const std::vector<float>& values, float minimum) {
            return values.empty() ? minimum :
                *std::max_element(values.begin(), values.end());
        };
        const float minimum = current.seriesMinimum.value_or(0.0f);
        const float maximum = current.seriesMaximum.value_or(1.0f);
        const float value = std::max(peak(previous.values, minimum),
            peak(current.values, minimum));
        const float barHeight = rect.height *
            std::clamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f);
        rect.y += rect.height - barHeight;
        rect.height = barHeight;
        previous.values = current.values;
    }
    else if (current.type == ViewNodeType::Slider)
    {
        previous.accessibilityValue = current.accessibilityValue;
        if (current.value == previous.value) return;
        padding += 8.0f + 0.5f * std::max(
            current.style.borderWidth.value_or(0.0f),
            current.disabledStyle.borderWidth.value_or(0.0f));
        previous.value = current.value;
    }
    else
    {
        if (current.type == ViewNodeType::Text && !current.textGlowRadius &&
            current.text != previous.text)
        {
            previous.text = current.text;
            paint.textChanged = true;
        }
        return;
    }
    const RECT changed{
        bounds.left + static_cast<LONG>(std::floor(rect.x - padding)),
        bounds.top + static_cast<LONG>(std::floor(rect.y - padding)),
        bounds.left + static_cast<LONG>(std::ceil(rect.x + rect.width + padding)),
        bounds.top + static_cast<LONG>(std::ceil(rect.y + rect.height + padding)),
    };
    UnionRect(&paint.damage, &paint.damage, &changed);
}
}
