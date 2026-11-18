#include "widget_view_transition_validation.h"
#include "widget_view_tree.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace snowdesktop::widget_runtime
{
namespace
{
bool IsValidTransitionDuration(std::uint32_t milliseconds) noexcept
{
    return milliseconds >= 1 && milliseconds <= 2000;
}

bool IsSupportedTransitionEasing(ViewTransitionEasing easing) noexcept
{
    switch (easing)
    {
    case ViewTransitionEasing::Linear:
    case ViewTransitionEasing::EaseIn:
    case ViewTransitionEasing::EaseOut:
    case ViewTransitionEasing::EaseInOut:
        return true;
    default:
        return false;
    }
}

bool ValidateTransition(const ViewTransition& transition, std::string& error)
{
    if (!IsValidTransitionDuration(transition.durationMilliseconds) ||
        transition.properties.empty() || transition.properties.size() > 4)
    {
        error = "view transition must contain 1 to 4 properties and a duration from 1 to 2000ms";
        return false;
    }

    std::array<bool, 6> seen{};
    for (const auto property : transition.properties)
    {
        const auto index = static_cast<std::size_t>(property);
        if (index >= seen.size() || seen[index])
        {
            error = "view transition properties must be supported and unique";
            return false;
        }
        seen[index] = true;
    }

    if (!IsSupportedTransitionEasing(transition.easing))
    {
        error = "view transition easing is unsupported";
        return false;
    }
    return true;
}

bool ValidatePresenceTransition(const std::optional<ViewPresenceTransition>& presence,
    std::string_view name, std::string& error)
{
    if (!presence) return true;

    const auto& opacity = presence->opacity;
    if (!IsValidTransitionDuration(presence->durationMilliseconds) ||
        (!opacity && !presence->transform) ||
        (opacity && (!std::isfinite(*opacity) || *opacity < 0.0f || *opacity > 1.0f)))
    {
        error = "view " + std::string(name) +
            " requires bounded opacity or transform and a duration from 1 to 2000ms";
        return false;
    }
    if (!IsSupportedTransitionEasing(presence->easing))
    {
        error = "view " + std::string(name) + " easing is unsupported";
        return false;
    }
    return true;
}
}

bool ValidateViewNodeTransitions(const ViewNode& node, std::string& error)
{
    return (!node.transition || ValidateTransition(*node.transition, error)) &&
        ValidatePresenceTransition(node.enterTransition, "enterTransition", error) &&
        ValidatePresenceTransition(node.exitTransition, "exitTransition", error);
}
}
