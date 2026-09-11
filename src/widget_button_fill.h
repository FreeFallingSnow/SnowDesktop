#pragma once

#include <cstdint>
#include <optional>

namespace snowdesktop::widget_runtime
{
struct WidgetButtonFill
{
    std::uint32_t color;
    float alpha;
};

inline WidgetButtonFill ResolveWidgetButtonFill(
    std::optional<std::uint32_t> background, std::uint32_t foreground,
    bool enabled, bool hovered, bool pressed) noexcept
{
    if (background) return {*background, 1.0f};
    return {foreground, enabled && pressed ? 0.24f :
        enabled && hovered ? 0.18f : 0.12f};
}
}
