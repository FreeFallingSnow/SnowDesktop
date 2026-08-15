#pragma once

#include "widget_view_tree.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct ViewNodeContract
{
    ViewNodeType type = ViewNodeType::Box;
    std::string_view name;
    std::string_view category;
    std::string_view feature;
    std::string_view defaultAccessibilityRole;
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
}
