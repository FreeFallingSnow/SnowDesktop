#pragma once

#include "widget_view_contract.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct ViewAccessibilityNode
{
    static constexpr std::size_t NoParent = static_cast<std::size_t>(-1);

    ViewNodeType sourceType = ViewNodeType::Box;
    // Host-only identity used by UI Automation. Lua still observes `key`.
    std::string semanticId;
    std::string key;
    std::string name;
    std::string role;
    std::string controlType;
    std::string valueText;
    std::string helpText;
    std::string labelledBySemanticId;
    std::string describedBySemanticId;
    std::string accessKey;
    std::string acceleratorText;
    ViewAccessibilityPattern patterns = ViewAccessibilityPattern::None;
    ViewRect bounds;
    std::optional<ViewRect> clip;
    std::size_t parentIndex = NoParent;
    std::vector<std::size_t> children;
    bool enabled = true;
    bool focusable = false;
    bool focused = false;
    bool offscreen = false;
    bool required = false;
    bool busy = false;
    bool canSelectMultiple = false;
    bool selectionRequired = false;
    AccessibilityLive live = AccessibilityLive::Off;
    int headingLevel = 0;
    std::optional<int> positionInSet;
    std::optional<int> setSize;
    std::optional<bool> checked;
    std::optional<bool> expanded;
    std::optional<float> value;
    std::optional<float> minimum;
    std::optional<float> maximum;
    std::optional<float> step;
    bool valueReadOnly = true;
    bool rangeValueReadOnly = true;
    std::optional<int> gridRow;
    std::optional<int> gridColumn;
    std::optional<int> gridRowSpan;
    std::optional<int> gridColumnSpan;
    std::optional<int> gridRowCount;
    std::optional<int> gridColumnCount;
    bool scrollHorizontal = false;
    float scrollOffset = 0.0f;
    float scrollViewportExtent = 0.0f;
    float scrollContentExtent = 0.0f;
};

bool CollectViewAccessibilityNodes(const ViewNode& root,
    std::string_view focusedKey,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error);
bool CollectInteractionAccessibilityNodes(
    const std::vector<InteractionRegion>& regions,
    float surfaceWidth, float surfaceHeight,
    std::string_view focusedKey,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error);
}
