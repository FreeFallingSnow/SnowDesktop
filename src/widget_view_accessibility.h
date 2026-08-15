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
    ViewAccessibilityPattern patterns = ViewAccessibilityPattern::None;
    ViewRect bounds;
    std::optional<ViewRect> clip;
    std::size_t parentIndex = NoParent;
    std::vector<std::size_t> children;
    bool enabled = true;
    bool focusable = false;
    bool focused = false;
    bool offscreen = false;
    std::optional<bool> checked;
    std::optional<bool> expanded;
    std::optional<float> value;
    std::optional<float> minimum;
    std::optional<float> maximum;
    std::optional<float> step;
    bool valueReadOnly = true;
    bool rangeValueReadOnly = true;
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
