#pragma once

#include "widget_view_tree.h"

#include <dwrite.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
class WidgetDrawContext;
class WidgetInteractionRegions;
class WidgetViewHost;

bool IsDeclarativeInputNode(ViewNodeType type) noexcept;
void SetViewTextLayoutAlignment(IDWriteTextLayout* layout, ViewTextAlignment alignment);
DWRITE_READING_DIRECTION ResolveViewTextDirection(
    const ViewNode& node, const std::wstring& text);
void ApplyViewTextLocaleAndDirection(IDWriteTextLayout* layout,
    const ViewNode& node, const std::wstring& text);
DWRITE_WORD_WRAPPING ViewTextWrapping(const ViewNode& node) noexcept;
DWRITE_FONT_WEIGHT ViewFontWeight(const ViewNode& node, DWRITE_FONT_WEIGHT fallback) noexcept;
void ApplyViewTextTypography(IDWriteTextLayout* layout, const ViewNode& node, UINT32 count);
float ViewTextLayoutHeight(const ViewNode& node, float contentHeight) noexcept;
float ViewTextVerticalOffset(const ViewNode& node, float contentHeight, float actualHeight) noexcept;
void SetViewTextOverflow(WidgetDrawContext* state, IDWriteTextFormat* format,
    IDWriteTextLayout* layout, const ViewNode& node);
void DrawWidgetStyledText(WidgetDrawContext* state, const WidgetViewHost& host,
    const ViewNode& node, const ViewStyle& style, float opacity,
    const WidgetInteractionRegions& regions, const ViewThemePalette& palette);
bool CollectStyledTextActionRegions(WidgetDrawContext* state, const WidgetViewHost& host,
    const ViewNode& root, const ViewNode& node, std::vector<InteractionRegion>& regions,
    const std::optional<ViewRect>& inheritedClip, std::string& error);
}
