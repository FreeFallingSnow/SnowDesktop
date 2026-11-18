#include "widget_view_text.h"

#include "widget_draw_context.h"
#include "widget_interaction_region.h"
#include "widget_view_renderer.h"
#include "common/utils.h"

#include <dwrite_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <utility>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

bool IsDeclarativeInputNode(
    snowdesktop::widget_runtime::ViewNodeType type) noexcept
{
    using snowdesktop::widget_runtime::ViewNodeType;
    return type == ViewNodeType::TextInput ||
        type == ViewNodeType::TextArea ||
        type == ViewNodeType::SearchBox ||
        type == ViewNodeType::NumberInput;
}

void SetViewTextLayoutAlignment(IDWriteTextLayout* layout,
    snowdesktop::widget_runtime::ViewTextAlignment alignment)
{
    using snowdesktop::widget_runtime::ViewTextAlignment;
    if (!layout) return;
    if (alignment == ViewTextAlignment::Center)
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else if (alignment == ViewTextAlignment::End)
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    else
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

static bool ViewLocaleDefaultsToRtl(std::string_view locale) noexcept
{
    const std::size_t separator = locale.find('-');
    std::string language(locale.substr(0, separator));
    std::transform(language.begin(), language.end(), language.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return language == "ar" || language == "ckb" || language == "dv" ||
        language == "fa" || language == "he" || language == "ps" ||
        language == "sd" || language == "ug" || language == "ur" ||
        language == "yi";
}

DWRITE_READING_DIRECTION ResolveViewTextDirection(
    const snowdesktop::widget_runtime::ViewNode& node,
    const std::wstring& text)
{
    using snowdesktop::widget_runtime::ViewTextDirection;
    if (node.textDirection == ViewTextDirection::LeftToRight)
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    if (node.textDirection == ViewTextDirection::RightToLeft)
        return DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;

    if (!text.empty())
    {
        std::vector<WORD> characterTypes(text.size());
        if (GetStringTypeW(CT_CTYPE2, text.data(),
                static_cast<int>(text.size()), characterTypes.data()))
        {
            for (const WORD type : characterTypes)
            {
                if (type == C2_RIGHTTOLEFT)
                    return DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
                if (type == C2_LEFTTORIGHT)
                    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
            }
        }
    }
    return ViewLocaleDefaultsToRtl(node.locale)
        ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
        : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}

void ApplyViewTextLocaleAndDirection(IDWriteTextLayout* layout,
    const snowdesktop::widget_runtime::ViewNode& node,
    const std::wstring& text)
{
    if (!layout) return;
    layout->SetReadingDirection(ResolveViewTextDirection(node, text));
    layout->SetFlowDirection(DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM);
    if (!node.locale.empty() && !text.empty())
    {
        const std::wstring locale = Utf8ToWide(node.locale);
        if (!locale.empty())
            layout->SetLocaleName(locale.c_str(), DWRITE_TEXT_RANGE{
                0, static_cast<UINT32>(text.size()) });
    }
}

DWRITE_WORD_WRAPPING ViewTextWrapping(
    const snowdesktop::widget_runtime::ViewNode& node) noexcept
{
    return node.textWrap ==
        snowdesktop::widget_runtime::ViewTextWrap::Wrap
        ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP;
}

static float ViewTextLineHeight(
    const snowdesktop::widget_runtime::ViewNode& node) noexcept
{
    if (node.lineHeight) return *node.lineHeight;
    float size = node.fontSize;
    for (const auto& span : node.spans)
        if (span.fontSize) size = std::max(size, *span.fontSize);
    return std::max(1.0f, size * 1.4f);
}

DWRITE_FONT_WEIGHT ViewFontWeight(
    const snowdesktop::widget_runtime::ViewNode& node,
    DWRITE_FONT_WEIGHT fallback) noexcept
{
    if (node.fontWeight != 0)
        return static_cast<DWRITE_FONT_WEIGHT>(node.fontWeight);
    return node.bold ? DWRITE_FONT_WEIGHT_BOLD : fallback;
}

void ApplyViewTextTypography(IDWriteTextLayout* layout,
    const snowdesktop::widget_runtime::ViewNode& node,
    UINT32 textLength)
{
    if (!layout || textLength == 0) return;
    const DWRITE_TEXT_RANGE range{ 0, textLength };
    if (node.fontStyle ==
        snowdesktop::widget_runtime::ViewFontStyle::Italic)
        layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
    if (node.lineHeight)
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
            *node.lineHeight, std::max(1.0f, *node.lineHeight * 0.8f));
    if (node.letterSpacing != 0.0f)
    {
        ComPtr<IDWriteTextLayout1> advanced;
        if (SUCCEEDED(layout->QueryInterface(IID_PPV_ARGS(&advanced))) &&
            advanced)
        {
            advanced->SetCharacterSpacing(node.letterSpacing * 0.5f,
                node.letterSpacing * 0.5f, 0.0f, range);
        }
    }
}

float ViewTextLayoutHeight(
    const snowdesktop::widget_runtime::ViewNode& node,
    float available) noexcept
{
    if (node.maximumLines == 0) return available;
    return std::min(available, ViewTextLineHeight(node) *
        static_cast<float>(node.maximumLines));
}

float ViewTextVerticalOffset(
    const snowdesktop::widget_runtime::ViewNode& node,
    float available, float content) noexcept
{
    using snowdesktop::widget_runtime::ViewAlignment;
    const float remaining = std::max(0.0f, available - content);
    if (node.verticalAlign == ViewAlignment::Center)
        return remaining * 0.5f;
    if (node.verticalAlign == ViewAlignment::End)
        return remaining;
    return 0.0f;
}

void SetViewTextOverflow(WidgetDrawContext* state,
    IDWriteTextFormat* format, IDWriteTextLayout* layout,
    const snowdesktop::widget_runtime::ViewNode& node)
{
    if (!state || !state->dwrite || !format || !layout ||
        node.overflowText !=
            snowdesktop::widget_runtime::ViewTextOverflow::Ellipsis)
        return;
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(state->dwrite->CreateEllipsisTrimmingSign(
            format, &ellipsis)) && ellipsis)
        layout->SetTrimming(&trimming, ellipsis.Get());
}

static bool ApplyStyledTextSpanTypography(WidgetDrawContext* state,
    IDWriteTextLayout* layout,
    const snowdesktop::widget_runtime::ViewTextSpan& span,
    const DWRITE_TEXT_RANGE& range)
{
    using snowdesktop::widget_runtime::ViewIconFont;
    if (!state || !layout) return false;
    if (span.icon)
    {
        const bool fontAwesome =
            span.iconFont == ViewIconFont::FontAwesome;
        IDWriteTextFormat* iconFormat = GetCachedTextFormat(state,
            span.fontSize.value_or(14.0f), DWRITE_FONT_WEIGHT_NORMAL,
            false, DWRITE_WORD_WRAPPING_NO_WRAP, fontAwesome,
            false, !fontAwesome);
        if (!iconFormat) return false;
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(iconFormat->GetFontCollection(&collection)) ||
            !collection)
            return false;
        const UINT32 familyLength =
            iconFormat->GetFontFamilyNameLength();
        if (familyLength == 0 || familyLength > 128) return false;
        std::wstring family(familyLength + 1, L'\0');
        if (FAILED(iconFormat->GetFontFamilyName(
                family.data(), familyLength + 1)))
            return false;
        family.resize(familyLength);
        if (FAILED(layout->SetFontCollection(collection.Get(), range)) ||
            FAILED(layout->SetFontFamilyName(family.c_str(), range)) ||
            FAILED(layout->SetFontWeight(
                DWRITE_FONT_WEIGHT_NORMAL, range)) ||
            FAILED(layout->SetFontStyle(
                DWRITE_FONT_STYLE_NORMAL, range)) ||
            FAILED(layout->SetFontStretch(
                DWRITE_FONT_STRETCH_NORMAL, range)))
            return false;
    }
    if (span.fontSize && FAILED(layout->SetFontSize(
            *span.fontSize, range)))
        return false;
    if (span.bold && FAILED(layout->SetFontWeight(
            DWRITE_FONT_WEIGHT_BOLD, range)))
        return false;
    if (span.italic && FAILED(layout->SetFontStyle(
            DWRITE_FONT_STYLE_ITALIC, range)))
        return false;
    if (span.underline && FAILED(layout->SetUnderline(TRUE, range)))
        return false;
    if (span.strikethrough && FAILED(
            layout->SetStrikethrough(TRUE, range)))
        return false;
    return true;
}

void DrawWidgetStyledText(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    const snowdesktop::widget_runtime::ViewThemePalette& palette)
{
    if (!state || !state->ctx || !state->dwrite || node.spans.empty())
        return;
    std::optional<std::wstring> privateFontPath;
    if (!node.fontResourceName.empty())
        privateFontPath = host.ResolveFont(node.fontResourceName);
    IDWriteTextFormat* format = node.fontResourceName.empty() ||
            privateFontPath
        ? GetCachedTextFormat(state, node.fontSize,
            ViewFontWeight(node, state->itemFontWeight),
            false, ViewTextWrapping(node), false, false, false,
            privateFontPath ? &*privateFontPath : nullptr)
        : nullptr;
    if (!format) return;

    std::wstring text;
    std::vector<std::pair<const snowdesktop::widget_runtime::ViewTextSpan*,
        DWRITE_TEXT_RANGE>> ranges;
    ranges.reserve(node.spans.size());
    for (const auto& span : node.spans)
    {
        const std::wstring part = Utf8ToWide(span.text);
        const UINT32 start = static_cast<UINT32>(text.size());
        text += part;
        ranges.emplace_back(&span,
            DWRITE_TEXT_RANGE{ start, static_cast<UINT32>(part.size()) });
    }
    if (text.empty()) return;
    const auto content = snowdesktop::widget_runtime::
        ViewNodeContentRect(node);
    const float width = content.width;
    const float height = content.height;
    const float layoutHeight = ViewTextLayoutHeight(node, height);
    ComPtr<IDWriteTextLayout> layout;
    if (width <= 0.0f || layoutHeight <= 0.0f || FAILED(
            state->dwrite->CreateTextLayout(text.data(),
                static_cast<UINT32>(text.size()), format, width, layoutHeight,
                &layout)) || !layout)
        return;
    ApplyViewTextLocaleAndDirection(layout.Get(), node, text);
    SetViewTextLayoutAlignment(layout.Get(), node.textAlign);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    SetViewTextOverflow(state, format, layout.Get(), node);
    ApplyViewTextTypography(layout.Get(), node,
        static_cast<UINT32>(text.size()));
    for (const auto& [span, range] : ranges)
    {
        const std::string targetKey = span->key.empty()
            ? std::string{} : node.key + "/" + span->key;
        const bool hovered = !targetKey.empty() &&
            regions.IsHovered(targetKey);
        const bool pressed = !targetKey.empty() &&
            regions.IsPressed(targetKey);
        if (!ApplyStyledTextSpanTypography(
                state, layout.Get(), *span, range))
            return;
        if (!span->underline && !span->events.empty() && hovered)
            layout->SetUnderline(TRUE, range);
        std::optional<std::uint32_t> foreground =
            snowdesktop::widget_runtime::ResolveViewThemeColor(
                span->foreground, span->foregroundToken, palette);
        if (pressed &&
            (span->pressedForeground || span->pressedForegroundToken))
            foreground = snowdesktop::widget_runtime::ResolveViewThemeColor(
                span->pressedForeground, span->pressedForegroundToken,
                palette);
        else if (hovered &&
            (span->hoverForeground || span->hoverForegroundToken))
            foreground = snowdesktop::widget_runtime::ResolveViewThemeColor(
                span->hoverForeground, span->hoverForegroundToken, palette);
        else if (!foreground && !span->events.empty())
            foreground = palette.systemAccent;
        if (foreground)
        {
            if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                    static_cast<int>(*foreground), opacity))
                layout->SetDrawingEffect(brush, range);
        }
    }
    ID2D1SolidColorBrush* brush = GetCachedBrush(state,
        static_cast<int>(style.foreground.value_or(0xFFFFFF)), opacity);
    if (brush)
    {
        DWRITE_TEXT_METRICS metrics{};
        const float contentHeight = SUCCEEDED(layout->GetMetrics(&metrics))
            ? std::min(layoutHeight, metrics.height) : layoutHeight;
        state->ctx->DrawTextLayout(D2D1::Point2F(
            state->widgetRect.left + content.x,
            state->widgetRect.top + content.y +
                ViewTextVerticalOffset(node, height, contentHeight)),
            layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

struct StyledTextHitLayout
{
    ComPtr<IDWriteTextLayout> layout;
    std::vector<std::pair<const snowdesktop::widget_runtime::ViewTextSpan*,
        DWRITE_TEXT_RANGE>> ranges;
    snowdesktop::widget_runtime::ViewRect content;
    float originY = 0.0f;
};

static bool BuildStyledTextHitLayout(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& node,
    StyledTextHitLayout& result, std::string& error)
{
    result = {};
    if (!state || !state->dwrite || node.spans.empty())
    {
        error = "styledText interaction layout is unavailable";
        return false;
    }
    std::optional<std::wstring> privateFontPath;
    if (!node.fontResourceName.empty())
        privateFontPath = host.ResolveFont(node.fontResourceName);
    IDWriteTextFormat* format = node.fontResourceName.empty() ||
            privateFontPath
        ? GetCachedTextFormat(state, node.fontSize,
            ViewFontWeight(node, state->itemFontWeight),
            false, ViewTextWrapping(node), false, false, false,
            privateFontPath ? &*privateFontPath : nullptr)
        : nullptr;
    if (!format)
    {
        error = "styledText interaction font is unavailable";
        return false;
    }

    std::wstring text;
    result.ranges.reserve(node.spans.size());
    for (const auto& span : node.spans)
    {
        const std::wstring part = Utf8ToWide(span.text);
        const UINT32 start = static_cast<UINT32>(text.size());
        text += part;
        result.ranges.emplace_back(&span,
            DWRITE_TEXT_RANGE{ start, static_cast<UINT32>(part.size()) });
    }
    result.content = snowdesktop::widget_runtime::ViewNodeContentRect(node);
    const float layoutHeight = ViewTextLayoutHeight(
        node, result.content.height);
    if (text.empty() || result.content.width <= 0.0f ||
        layoutHeight <= 0.0f || FAILED(state->dwrite->CreateTextLayout(
            text.data(), static_cast<UINT32>(text.size()), format,
            result.content.width, layoutHeight, &result.layout)) ||
        !result.layout)
    {
        error = "styledText interaction layout could not be created";
        return false;
    }
    ApplyViewTextLocaleAndDirection(result.layout.Get(), node, text);
    SetViewTextLayoutAlignment(result.layout.Get(), node.textAlign);
    result.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    SetViewTextOverflow(state, format, result.layout.Get(), node);
    ApplyViewTextTypography(result.layout.Get(), node,
        static_cast<UINT32>(text.size()));
    for (const auto& [span, range] : result.ranges)
    {
        if (!ApplyStyledTextSpanTypography(
                state, result.layout.Get(), *span, range))
        {
            error = "styledText inline icon font is unavailable";
            return false;
        }
    }
    DWRITE_TEXT_METRICS metrics{};
    const float contentHeight =
        SUCCEEDED(result.layout->GetMetrics(&metrics))
        ? std::min(layoutHeight, metrics.height) : layoutHeight;
    result.originY = result.content.y + ViewTextVerticalOffset(
        node, result.content.height, contentHeight);
    return true;
}

static std::optional<snowdesktop::widget_runtime::ViewRect>
IntersectStyledTextHitRect(
    const snowdesktop::widget_runtime::ViewRect& first,
    const snowdesktop::widget_runtime::ViewRect& second)
{
    const float left = std::max(first.x, second.x);
    const float top = std::max(first.y, second.y);
    const float right = std::min(first.x + first.width,
        second.x + second.width);
    const float bottom = std::min(first.y + first.height,
        second.y + second.height);
    if (right <= left || bottom <= top) return std::nullopt;
    return snowdesktop::widget_runtime::ViewRect{
        left, top, right - left, bottom - top };
}

bool CollectStyledTextActionRegions(WidgetDrawContext* state, const WidgetViewHost& host,
    const snowdesktop::widget_runtime::ViewNode& root,
    const snowdesktop::widget_runtime::ViewNode& node,
    std::vector<snowdesktop::widget_runtime::InteractionRegion>& regions,
    const std::optional<snowdesktop::widget_runtime::ViewRect>& inheritedClip,
    std::string& error)
{
    using namespace snowdesktop::widget_runtime;
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return true;

    if (node.type == ViewNodeType::StyledText)
    {
        const bool hasTargets = std::any_of(node.spans.begin(),
            node.spans.end(), [](const ViewTextSpan& span) {
                return !span.key.empty();
            });
        if (hasTargets)
        {
            StyledTextHitLayout hitLayout;
            if (!BuildStyledTextHitLayout(state, host, node, hitLayout, error))
                return false;
            const ViewRect contentClip = hitLayout.content;
            const std::optional<ViewRect> effectiveClip = contentClip;
            if (effectiveClip)
            {
                for (const auto& [span, range] : hitLayout.ranges)
                {
                    if (span->key.empty() || range.length == 0) continue;
                    UINT32 metricCount = 0;
                    const HRESULT countResult = hitLayout.layout->
                        HitTestTextRange(range.startPosition, range.length,
                            hitLayout.content.x, hitLayout.originY,
                            nullptr, 0, &metricCount);
                    if (FAILED(countResult) &&
                        countResult != E_NOT_SUFFICIENT_BUFFER)
                    {
                        error = "styledText span hit testing failed";
                        return false;
                    }
                    if (metricCount == 0) continue;
                    if (metricCount > WidgetInteractionRegions::
                            kMaximumHitFragments)
                    {
                        error = "styledText span hit fragment limit exceeded (64)";
                        return false;
                    }
                    std::vector<DWRITE_HIT_TEST_METRICS> metrics(metricCount);
                    UINT32 written = 0;
                    if (FAILED(hitLayout.layout->HitTestTextRange(
                            range.startPosition, range.length,
                            hitLayout.content.x, hitLayout.originY,
                            metrics.data(), metricCount, &written)))
                    {
                        error = "styledText span hit testing failed";
                        return false;
                    }

                    InteractionRegion region;
                    region.key = node.key + "/" + span->key;
                    region.cursor = span->cursor.empty()
                        ? (!span->events.empty() ? "hand" : "default")
                        : span->cursor;
                    region.tooltipTitle = span->tooltipTitle;
                    region.tooltip = span->tooltip;
                    region.events = span->events;
                    region.accessibilityRole = span->events.empty()
                        ? "text" : "link";
                    region.accessibilityLabel =
                        span->accessibilityLabel.empty()
                        ? span->text : span->accessibilityLabel;
                    region.enabled = node.enabled;
                    if (span->events.contains("click") ||
                        span->events.contains("keyDown") ||
                        span->events.contains("keyUp"))
                        region.focusable = true;
                    if (inheritedClip)
                        region.clip = InteractionClipRect{
                            inheritedClip->x, inheritedClip->y,
                            inheritedClip->width, inheritedClip->height };

                    bool first = true;
                    ViewRect unionRect{};
                    for (UINT32 index = 0; index < written; ++index)
                    {
                        const auto& metric = metrics[index];
                        const auto clipped = IntersectStyledTextHitRect(
                            ViewRect{ metric.left, metric.top,
                                metric.width, metric.height },
                            *effectiveClip);
                        if (!clipped) continue;
                        InteractionShape fragment;
                        fragment.type = InteractionShapeType::Rect;
                        fragment.x = clipped->x;
                        fragment.y = clipped->y;
                        fragment.width = clipped->width;
                        fragment.height = clipped->height;
                        region.hitFragments.push_back(fragment);
                        if (first)
                        {
                            unionRect = *clipped;
                            first = false;
                        }
                        else
                        {
                            const float left = std::min(
                                unionRect.x, clipped->x);
                            const float top = std::min(
                                unionRect.y, clipped->y);
                            const float right = std::max(
                                unionRect.x + unionRect.width,
                                clipped->x + clipped->width);
                            const float bottom = std::max(
                                unionRect.y + unionRect.height,
                                clipped->y + clipped->height);
                            unionRect = { left, top,
                                right - left, bottom - top };
                        }
                    }
                    if (first) continue;
                    region.shape.type = InteractionShapeType::Rect;
                    region.shape.x = unionRect.x;
                    region.shape.y = unionRect.y;
                    region.shape.width = unionRect.width;
                    region.shape.height = unionRect.height;
                    if (regions.size() >=
                        WidgetInteractionRegions::kMaximumRegions)
                    {
                        error = "view interaction region limit exceeded (256)";
                        return false;
                    }
                    ApplyViewTransform(root, region);
                    regions.push_back(std::move(region));
                }
            }
        }
    }

    std::optional<ViewRect> childClip = inheritedClip;
    if (node.clipFrame)
    {
        childClip = inheritedClip
            ? IntersectStyledTextHitRect(*inheritedClip, *node.clipFrame)
            : node.clipFrame;
    }
    for (const ViewNode* child : ViewChildrenInPaintOrder(node))
        if (!CollectStyledTextActionRegions(
                state, host, root, *child, regions, childClip, error)) return false;
    return true;
}
}
