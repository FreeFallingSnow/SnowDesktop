#include "widget_interaction_region.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr float kMaximumCoordinate = 1000000.0f;

bool IsFiniteCoordinate(float value) noexcept
{
    return std::isfinite(value) && std::abs(value) <= kMaximumCoordinate;
}

bool IsSupportedCursor(std::string_view cursor) noexcept
{
    return cursor.empty() || cursor == "default" || cursor == "hand" ||
        cursor == "text" || cursor == "crosshair";
}

bool IsSupportedEvent(std::string_view eventName) noexcept
{
    return eventName == "pointerEnter" || eventName == "pointerLeave" ||
        eventName == "pointerDown" || eventName == "pointerUp" ||
        eventName == "pointerMove" || eventName == "click" ||
        eventName == "doubleClick" || eventName == "wheel" ||
        eventName == "contextMenu";
}
}

bool InteractionShape::Contains(float pointX, float pointY) const noexcept
{
    if (type == InteractionShapeType::Circle)
    {
        const float dx = pointX - x;
        const float dy = pointY - y;
        return dx * dx + dy * dy <= radius * radius;
    }
    if (pointX < x || pointY < y || pointX > x + width ||
        pointY > y + height)
        return false;
    if (type != InteractionShapeType::RoundedRect || radius <= 0.0f)
        return true;

    const float effectiveRadius = std::min(
        radius, std::min(width, height) * 0.5f);
    const float nearestX = std::clamp(
        pointX, x + effectiveRadius, x + width - effectiveRadius);
    const float nearestY = std::clamp(
        pointY, y + effectiveRadius, y + height - effectiveRadius);
    const float dx = pointX - nearestX;
    const float dy = pointY - nearestY;
    return dx * dx + dy * dy <= effectiveRadius * effectiveRadius;
}

void WidgetInteractionRegions::BeginFrame()
{
    staging_.clear();
    frameOpen_ = true;
}

bool WidgetInteractionRegions::Submit(
    InteractionRegion region, std::string& error)
{
    error.clear();
    if (!frameOpen_)
    {
        error = "interaction.region may only be called during render";
        return false;
    }
    if (staging_.size() >= kMaximumRegions)
    {
        error = "interaction region limit exceeded (256)";
        return false;
    }
    if (region.key.empty() || region.key.size() > 128)
    {
        error = "interaction region key must contain 1 to 128 bytes";
        return false;
    }
    if (std::any_of(staging_.begin(), staging_.end(),
            [&region](const InteractionRegion& candidate) {
                return candidate.key == region.key;
            }))
    {
        error = "duplicate interaction region key: " + region.key;
        return false;
    }
    if (!IsSupportedCursor(region.cursor))
    {
        error = "unsupported interaction cursor";
        return false;
    }
    if (!IsFiniteCoordinate(region.shape.x) ||
        !IsFiniteCoordinate(region.shape.y))
    {
        error = "interaction region coordinates must be finite";
        return false;
    }
    if (region.shape.type == InteractionShapeType::Circle)
    {
        if (!IsFiniteCoordinate(region.shape.radius) ||
            region.shape.radius <= 0.0f)
        {
            error = "circle interaction region radius must be positive";
            return false;
        }
    }
    else if (!IsFiniteCoordinate(region.shape.width) ||
        !IsFiniteCoordinate(region.shape.height) ||
        region.shape.width <= 0.0f || region.shape.height <= 0.0f ||
        (region.shape.type == InteractionShapeType::RoundedRect &&
            (!IsFiniteCoordinate(region.shape.radius) ||
                region.shape.radius < 0.0f)))
    {
        error = "rect interaction region size must be positive";
        return false;
    }
    for (const auto& [eventName, action] : region.events)
    {
        if (!IsSupportedEvent(eventName))
        {
            error = "unsupported interaction event: " + eventName;
            return false;
        }
        if (action.id.empty() || action.id.size() > 128)
        {
            error = "interaction action id must contain 1 to 128 bytes";
            return false;
        }
    }
    staging_.push_back(std::move(region));
    return true;
}

InteractionHoverTransition WidgetInteractionRegions::CommitFrame()
{
    if (!frameOpen_) return {};
    frameOpen_ = false;
    const std::string previousHover = hoveredKey_;
    retiredHoverRegion_.reset();
    if (!previousHover.empty())
    {
        if (const InteractionRegion* previous = Find(previousHover))
            retiredHoverRegion_ = *previous;
    }
    const bool semanticSetChanged = active_ != staging_;
    active_ = std::move(staging_);
    staging_.clear();
    if (semanticSetChanged) ++generation_;

    if (!ContainsKey(pressedKey_))
    {
        pressedKey_.clear();
        pressedButton_ = -1;
        clickCandidateKey_.clear();
    }
    if (!ContainsKey(clickCandidateKey_))
        clickCandidateKey_.clear();
    const InteractionRegion* hovered = pointerKnown_
        ? HitTest(pointerX_, pointerY_) : nullptr;
    hoveredKey_ = hovered ? hovered->key : std::string{};
    return { previousHover, hoveredKey_ };
}

void WidgetInteractionRegions::AbortFrame() noexcept
{
    staging_.clear();
    frameOpen_ = false;
}

InteractionHoverTransition WidgetInteractionRegions::UpdateHover(
    float x, float y)
{
    pointerKnown_ = true;
    retiredHoverRegion_.reset();
    pointerX_ = x;
    pointerY_ = y;
    const std::string previous = hoveredKey_;
    const InteractionRegion* hovered = HitTest(x, y);
    hoveredKey_ = hovered ? hovered->key : std::string{};
    return { previous, hoveredKey_ };
}

InteractionHoverTransition WidgetInteractionRegions::ClearHover()
{
    pointerKnown_ = false;
    retiredHoverRegion_.reset();
    const std::string previous = hoveredKey_;
    hoveredKey_.clear();
    return { previous, {} };
}

InteractionPointerResult WidgetInteractionRegions::PointerDown(
    float x, float y, int button)
{
    UpdateHover(x, y);
    pressedKey_ = hoveredKey_;
    pressedButton_ = button;
    clickCandidateKey_.clear();
    return { pressedKey_, {} };
}

InteractionPointerResult WidgetInteractionRegions::PointerUp(
    float x, float y, int button)
{
    UpdateHover(x, y);
    InteractionPointerResult result;
    result.targetKey = hoveredKey_;
    if (button == pressedButton_ && !pressedKey_.empty() &&
        pressedKey_ == hoveredKey_)
    {
        clickCandidateKey_ = pressedKey_;
        result.clickTargetKey = clickCandidateKey_;
    }
    else
        clickCandidateKey_.clear();
    pressedKey_.clear();
    pressedButton_ = -1;
    return result;
}

std::string WidgetInteractionRegions::ConsumeClickTarget(float x, float y)
{
    UpdateHover(x, y);
    std::string result;
    if (!clickCandidateKey_.empty() &&
        clickCandidateKey_ == hoveredKey_ && ContainsKey(clickCandidateKey_))
        result = clickCandidateKey_;
    clickCandidateKey_.clear();
    return result;
}

std::string WidgetInteractionRegions::TargetAt(float x, float y) const
{
    const InteractionRegion* target = HitTest(x, y);
    return target ? target->key : std::string{};
}

const InteractionRegion* WidgetInteractionRegions::Find(
    std::string_view key) const noexcept
{
    const auto found = std::find_if(active_.begin(), active_.end(),
        [key](const InteractionRegion& candidate) {
            return candidate.key == key;
        });
    return found == active_.end() ? nullptr : &*found;
}

const InteractionAction* WidgetInteractionRegions::FindAction(
    std::string_view key, std::string_view eventName) const noexcept
{
    const InteractionRegion* region = Find(key);
    if (!region) return nullptr;
    const auto action = region->events.find(eventName);
    return action == region->events.end() ? nullptr : &action->second;
}

const InteractionAction* WidgetInteractionRegions::FindTransitionAction(
    std::string_view key, std::string_view eventName) const noexcept
{
    if (const InteractionAction* action = FindAction(key, eventName))
        return action;
    if (!retiredHoverRegion_ || retiredHoverRegion_->key != key)
        return nullptr;
    const auto action = retiredHoverRegion_->events.find(eventName);
    return action == retiredHoverRegion_->events.end()
        ? nullptr : &action->second;
}

const InteractionAction* WidgetInteractionRegions::ActionAt(
    float x, float y, std::string_view eventName,
    std::string* targetKey) const noexcept
{
    const InteractionRegion* region = HitTest(x, y);
    if (!region) return nullptr;
    if (targetKey) *targetKey = region->key;
    const auto action = region->events.find(eventName);
    return action == region->events.end() ? nullptr : &action->second;
}

bool WidgetInteractionRegions::IsHovered(std::string_view key) const noexcept
{
    return !key.empty() && hoveredKey_ == key;
}

bool WidgetInteractionRegions::IsPressed(std::string_view key) const noexcept
{
    return !key.empty() && pressedKey_ == key;
}

const std::string& WidgetInteractionRegions::HoveredKey() const noexcept
{
    return hoveredKey_;
}

const std::string& WidgetInteractionRegions::PressedKey() const noexcept
{
    return pressedKey_;
}

bool WidgetInteractionRegions::LastPointer(float& x, float& y) const noexcept
{
    if (!pointerKnown_) return false;
    x = pointerX_;
    y = pointerY_;
    return true;
}

std::uint64_t WidgetInteractionRegions::Generation() const noexcept
{
    return generation_;
}

std::string WidgetInteractionRegions::CursorAt(float x, float y) const
{
    const InteractionRegion* region = HitTest(x, y);
    return region ? region->cursor : std::string{};
}

bool WidgetInteractionRegions::Empty() const noexcept
{
    return active_.empty();
}

void WidgetInteractionRegions::Reset() noexcept
{
    active_.clear();
    staging_.clear();
    frameOpen_ = false;
    pointerKnown_ = false;
    hoveredKey_.clear();
    pressedKey_.clear();
    clickCandidateKey_.clear();
    retiredHoverRegion_.reset();
    pressedButton_ = -1;
    ++generation_;
}

const InteractionRegion* WidgetInteractionRegions::HitTest(
    float x, float y) const noexcept
{
    for (auto region = active_.rbegin(); region != active_.rend(); ++region)
    {
        if (region->enabled && region->shape.Contains(x, y))
            return &*region;
    }
    return nullptr;
}

bool WidgetInteractionRegions::ContainsKey(std::string_view key) const noexcept
{
    return key.empty() || Find(key) != nullptr;
}
}
