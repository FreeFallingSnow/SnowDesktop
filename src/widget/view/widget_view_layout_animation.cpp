#include "widget_view_layout_animation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime::detail
{
namespace
{
const ViewTransition* LayoutTransition(const ViewNode& node) noexcept
{
    if (!node.transition || node.transition->durationMilliseconds == 0 ||
        std::find(node.transition->properties.begin(),
            node.transition->properties.end(), ViewTransitionProperty::Layout) ==
                node.transition->properties.end())
        return nullptr;
    return &*node.transition;
}

std::optional<ViewRect> TranslateRect(std::optional<ViewRect> rect,
    float x, float y) noexcept
{
    if (rect)
    {
        rect->x += x;
        rect->y += y;
    }
    return rect;
}

ViewRect InterpolateRect(const ViewRect& start, const ViewRect& target,
    float progress) noexcept
{
    return { std::lerp(start.x, target.x, progress),
        std::lerp(start.y, target.y, progress),
        std::lerp(start.width, target.width, progress),
        std::lerp(start.height, target.height, progress) };
}

std::optional<ViewRect> InterpolateRect(const std::optional<ViewRect>& start,
    const std::optional<ViewRect>& target, float progress) noexcept
{
    if (start && target) return InterpolateRect(*start, *target, progress);
    return start;
}
}

float ResolveViewTransitionProgress(ViewTransitionEasing easing,
    float progress) noexcept
{
    progress = std::clamp(progress, 0.0f, 1.0f);
    switch (easing)
    {
    case ViewTransitionEasing::EaseIn:
        return progress * progress;
    case ViewTransitionEasing::EaseOut:
        return 1.0f - (1.0f - progress) * (1.0f - progress);
    case ViewTransitionEasing::EaseInOut:
        return progress < 0.5f
            ? 2.0f * progress * progress
            : 1.0f - 2.0f * (1.0f - progress) * (1.0f - progress);
    default:
        return progress;
    }
}

std::uint32_t ScaleViewTransitionDuration(std::uint32_t duration,
    double scale) noexcept
{
    if (duration == 0) return 0;
    return static_cast<std::uint32_t>(std::clamp(
        std::round(static_cast<double>(duration) * scale), 1.0,
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

bool HasViewLayoutAnimation(const ViewNode& node) noexcept
{
    return LayoutTransition(node) ||
        std::any_of(node.children.begin(), node.children.end(),
            HasViewLayoutAnimation);
}

ViewLayoutAnimation::Geometry ViewLayoutAnimation::Capture(
    const ViewNode& node) noexcept
{
    return { node.layoutTransitionFrame,
        TranslateRect(node.clipFrame, -node.frame.x, -node.frame.y),
        TranslateRect(node.scrollbarFrame, -node.frame.x, -node.frame.y),
        node.scrollViewportExtent, node.scrollContentExtent };
}

ViewLayoutAnimation::Geometry ViewLayoutAnimation::Interpolate(
    const Entry& entry, float progress) noexcept
{
    if (progress <= 0.0f) return entry.start;
    if (progress >= 1.0f) return entry.target;
    const auto& start = entry.start;
    const auto& target = entry.target;
    return { InterpolateRect(start.layoutFrame, target.layoutFrame, progress),
        InterpolateRect(start.clip, target.clip, progress),
        InterpolateRect(start.scrollbar, target.scrollbar, progress),
        std::lerp(start.viewportExtent, target.viewportExtent, progress),
        std::lerp(start.contentExtent, target.contentExtent, progress) };
}

void ViewLayoutAnimation::Commit(const ViewNode& previous,
    const ViewNode& current, double durationScale)
{
    struct PreviousNode
    {
        const ViewNode* node;
        std::string_view parent;
    };
    std::unordered_map<std::string_view, PreviousNode> previousNodes;
    const auto index = [&](const auto& self, const ViewNode& node,
        std::string_view parent) -> void {
        if (!node.visible || node.visibility == ViewVisibility::Hidden) return;
        previousNodes.emplace(node.key, PreviousNode{ &node, parent });
        for (const auto& child : node.children) self(self, child, node.key);
    };
    index(index, previous, {});

    std::unordered_map<std::string, Entry> next;
    const auto collect = [&](const auto& self, const ViewNode& node,
        std::string_view parent, const ViewTransition* inherited) -> void {
        if (!node.visible || node.visibility == ViewVisibility::Hidden) return;
        const auto* timing = LayoutTransition(node);
        if (!timing) timing = inherited;
        const auto old = previousNodes.find(node.key);
        if (timing && old != previousNodes.end() &&
            old->second.parent == parent && old->second.node->type == node.type)
        {
            const auto target = Capture(node);
            const auto previousTarget = Capture(*old->second.node);
            const auto active = entries_.find(node.key);
            const auto& start = active != entries_.end()
                ? active->second.presented : previousTarget;
            const bool geometryChanged = !start.SameBounds(target);
            if (geometryChanged && active != entries_.end() &&
                previousTarget.SameBounds(target))
            {
                auto retained = active->second;
                retained.target = target;
                next.emplace(node.key, std::move(retained));
            }
            else if (geometryChanged)
            {
                next.emplace(node.key, Entry{ start, target, start,
                    timing->easing, ScaleViewTransitionDuration(
                        timing->durationMilliseconds, durationScale), std::nullopt });
            }
        }
        for (const auto& child : node.children)
            self(self, child, node.key, timing);
    };
    collect(collect, current, {}, nullptr);
    entries_ = std::move(next);
    painted_.reset();
}

const ViewNode& ViewLayoutAnimation::LastPainted(
    const ViewNode& committed) const noexcept
{
    return painted_ ? *painted_ : committed;
}

void ViewLayoutAnimation::Apply(const ViewNode& committed, ViewNode& painted,
    float parentOffsetX, float parentOffsetY) const
{
    const auto entry = entries_.find(committed.key);
    // Add the change in unscrolled, parent-relative layout to the committed
    // scroll position. A scroll-only update takes effect without interpolation.
    painted.frame = committed.frame;
    painted.layoutTransitionFrame = committed.layoutTransitionFrame;
    painted.frame.x += parentOffsetX;
    painted.frame.y += parentOffsetY;
    if (entry != entries_.end())
    {
        const auto& geometry = entry->second.presented;
        painted.layoutTransitionFrame = geometry.layoutFrame;
        painted.frame.x += geometry.layoutFrame.x -
            committed.layoutTransitionFrame.x;
        painted.frame.y += geometry.layoutFrame.y -
            committed.layoutTransitionFrame.y;
        painted.frame.width = geometry.layoutFrame.width;
        painted.frame.height = geometry.layoutFrame.height;
        painted.clipFrame = TranslateRect(geometry.clip,
            painted.frame.x, painted.frame.y);
        painted.scrollbarFrame = TranslateRect(geometry.scrollbar,
            painted.frame.x, painted.frame.y);
        painted.scrollViewportExtent = geometry.viewportExtent;
        painted.scrollContentExtent = geometry.contentExtent;
    }
    else
    {
        painted.clipFrame = TranslateRect(committed.clipFrame,
            parentOffsetX, parentOffsetY);
        painted.scrollbarFrame = TranslateRect(committed.scrollbarFrame,
            parentOffsetX, parentOffsetY);
        painted.scrollViewportExtent = committed.scrollViewportExtent;
        painted.scrollContentExtent = committed.scrollContentExtent;
    }
    const float offsetX = painted.frame.x - committed.frame.x;
    const float offsetY = painted.frame.y - committed.frame.y;
    for (std::size_t index = 0; index < committed.children.size(); ++index)
        Apply(committed.children[index], painted.children[index], offsetX, offsetY);
}

const ViewNode& ViewLayoutAnimation::Paint(const ViewNode& committed,
    ViewTransitionRuntime::TimePoint now)
{
    std::erase_if(entries_, [now](const auto& item) {
        const auto& entry = item.second;
        return entry.started && now - *entry.started >=
            std::chrono::milliseconds(entry.duration);
    });
    if (entries_.empty())
    {
        painted_.reset();
        return committed;
    }

    for (auto& [key, entry] : entries_)
    {
        (void)key;
        if (!entry.started) entry.started = now;
        const float elapsed = std::chrono::duration<float, std::milli>(
            now - *entry.started).count();
        entry.presented = Interpolate(entry, ResolveViewTransitionProgress(
            entry.easing, elapsed / static_cast<float>(entry.duration)));
    }
    // Reuse the descriptor tree within one commit. Only its paint geometry
    // changes on frame ticks.
    if (!painted_) painted_ = committed;
    Apply(committed, *painted_, 0.0f, 0.0f);
    return *painted_;
}
}
