#pragma once

#include "widget_view_tree.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class ViewAccessibilityPattern : std::uint32_t
{
    None = 0,
    Invoke = 1u << 0,
    Toggle = 1u << 1,
    Selection = 1u << 2,
    SelectionItem = 1u << 3,
    RangeValue = 1u << 4,
    Value = 1u << 5,
    ExpandCollapse = 1u << 6,
    Scroll = 1u << 7,
    Grid = 1u << 8,
    GridItem = 1u << 9,
};

constexpr ViewAccessibilityPattern operator|(
    ViewAccessibilityPattern left,
    ViewAccessibilityPattern right) noexcept
{
    return static_cast<ViewAccessibilityPattern>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

constexpr bool HasViewAccessibilityPattern(
    ViewAccessibilityPattern value,
    ViewAccessibilityPattern pattern) noexcept
{
    return (static_cast<std::uint32_t>(value) &
        static_cast<std::uint32_t>(pattern)) != 0;
}

struct ViewNodeContract
{
    ViewNodeType type = ViewNodeType::Box;
    std::string_view name;
    std::string_view category;
    std::string_view feature;
    std::string_view defaultAccessibilityRole;
    std::string_view uiaControlType;
    ViewAccessibilityPattern uiaPatterns =
        ViewAccessibilityPattern::None;
    bool keyboardFocusable = false;
};

enum class ViewEventPayloadKind : std::uint8_t
{
    Pointer,
    Wheel,
    Key,
    Action,
    Change,
    SelectionChange,
    Focus,
    Submit,
    ScrollEnd,
};

struct ViewEventContract
{
    std::string_view name;
    ViewEventPayloadKind payload = ViewEventPayloadKind::Action;
};

std::span<const ViewNodeContract> ViewNodeContracts() noexcept;
const ViewNodeContract* FindViewNodeContract(
    ViewNodeType type) noexcept;
const ViewNodeContract* FindViewNodeContract(
    std::string_view name) noexcept;
std::optional<ViewNodeType> FindViewNodeType(
    std::string_view name) noexcept;

std::span<const std::string_view> ViewNodePropertyNames() noexcept;
bool IsKnownViewNodeProperty(std::string_view property) noexcept;
bool ViewNodeAllowsProperty(
    ViewNodeType type, std::string_view property) noexcept;
bool ViewNodeRequiresProperty(
    ViewNodeType type, std::string_view property) noexcept;
std::vector<std::string_view> ViewNodeAllowedProperties(
    ViewNodeType type);
std::vector<std::string_view> ViewNodeRequiredProperties(
    ViewNodeType type);

std::span<const ViewEventContract> ViewEventContracts() noexcept;
bool IsKnownViewEvent(std::string_view event) noexcept;
bool ViewNodeAllowsEvent(
    ViewNodeType type, std::string_view event) noexcept;
std::vector<std::string_view> ViewNodeAllowedEvents(
    ViewNodeType type);
}
