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
        eventName == "contextMenu" || eventName == "change";
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
    if (region.clip && (!IsFiniteCoordinate(region.clip->x) ||
            !IsFiniteCoordinate(region.clip->y) ||
            !IsFiniteCoordinate(region.clip->width) ||
            !IsFiniteCoordinate(region.clip->height) ||
            region.clip->width <= 0.0f || region.clip->height <= 0.0f))
    {
        error = "interaction clip geometry must be finite and positive";
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
    const bool controlled = region.controlKind !=
        InteractionControlKind::None;
    if (controlled && (!region.events.contains("change") ||
            region.events.contains("click")))
    {
        error = "controlled interaction regions require change and reject click";
        return false;
    }
    if (!controlled && region.events.contains("change"))
    {
        error = "change is reserved for controlled interaction regions";
        return false;
    }
    if (region.controlKind == InteractionControlKind::Slider &&
        (!std::isfinite(region.minimum) ||
            !std::isfinite(region.maximum) ||
            !std::isfinite(region.controlValue) ||
            !std::isfinite(region.step) ||
            region.minimum >= region.maximum || region.step <= 0.0f ||
            region.step > region.maximum - region.minimum ||
            region.controlValue < region.minimum ||
            region.controlValue > region.maximum))
    {
        error = "slider interaction values are invalid";
        return false;
    }
    if (region.controlKind == InteractionControlKind::Radio &&
        region.proposedSelection.empty())
    {
        error = "radio interaction regions require a proposed selection";
        return false;
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

std::string WidgetInteractionRegions::PointerMoveTarget(
    float x, float y) const
{
    if (!pressedKey_.empty() && pressedButton_ == 1)
    {
        const InteractionRegion* pressed = Find(pressedKey_);
        if (pressed && pressed->enabled && pressed->controlKind ==
                InteractionControlKind::Slider)
            return pressedKey_;
    }
    return TargetAt(x, y);
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

std::optional<InteractionResolvedAction>
WidgetInteractionRegions::ResolveAction(
    std::string_view key, std::string_view eventName,
    float x, float y, int button) const
{
    const InteractionRegion* region = Find(key);
    if (!region) return std::nullopt;
    std::string_view resolvedName = eventName;
    const bool sliderChange = region->controlKind ==
            InteractionControlKind::Slider &&
        ((eventName == "pointerDown" && button == 1 &&
                pressedKey_ == key && pressedButton_ == 1) ||
            (eventName == "pointerMove" && pressedKey_ == key &&
                pressedButton_ == 1));
    if ((eventName == "click" && region->controlKind !=
            InteractionControlKind::None && region->controlKind !=
                InteractionControlKind::Slider) || sliderChange)
        resolvedName = "change";
    const auto action = region->events.find(resolvedName);
    if (action == region->events.end()) return std::nullopt;
    InteractionResolvedAction result;
    result.action = action->second;
    result.eventName = resolvedName;
    if (resolvedName == "change" &&
        (region->controlKind == InteractionControlKind::Toggle ||
            region->controlKind == InteractionControlKind::Checkbox))
    {
        result.previousChecked = region->checked;
        result.checked = !region->checked;
    }
    else if (resolvedName == "change" &&
        region->controlKind == InteractionControlKind::Radio)
    {
        result.previousSelection = region->currentSelection;
        result.selection = region->proposedSelection;
    }
    else if (resolvedName == "change" &&
        region->controlKind == InteractionControlKind::Slider)
    {
        const float start = region->controlLength > 0.0f
            ? region->controlStart
            : (region->vertical ? region->shape.y : region->shape.x);
        const float length = region->controlLength > 0.0f
            ? region->controlLength
            : (region->vertical ? region->shape.height :
                region->shape.width);
        float normalized = length > 0.0f
            ? ((region->vertical ? y : x) - start) / length : 0.0f;
        if (region->vertical) normalized = 1.0f - normalized;
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        const float raw = region->minimum + normalized *
            (region->maximum - region->minimum);
        float proposed = region->minimum + std::round(
            (raw - region->minimum) / region->step) * region->step;
        if (normalized <= 0.0f) proposed = region->minimum;
        if (normalized >= 1.0f) proposed = region->maximum;
        result.previousControlValue = region->controlValue;
        result.controlValue = std::clamp(
            proposed, region->minimum, region->maximum);
    }
    else if (resolvedName == "click" && region->hasExpandedProposal)
    {
        result.previousExpanded = region->expanded;
        result.expanded = !region->expanded;
    }
    return result;
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

bool WidgetInteractionRegions::FrameOpen() const noexcept
{
    return frameOpen_;
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
        if (region->enabled &&
            (!region->clip || region->clip->Contains(x, y)) &&
            region->shape.Contains(x, y))
            return &*region;
    }
    return nullptr;
}

bool WidgetInteractionRegions::ContainsKey(std::string_view key) const noexcept
{
    return key.empty() || Find(key) != nullptr;
}
}
