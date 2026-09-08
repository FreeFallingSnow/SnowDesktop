#pragma once

#include "dock_genie_rules.h"

#include <algorithm>
#include <cstdint>

namespace snowdesktop::quick_navigation_animation_rules
{
constexpr std::uint64_t kOpenDurationMs = 140;
constexpr std::uint64_t kCloseDurationMs = 110;
constexpr float kMinimumScale = 0.08f;

enum class Effect { None, Fade, Scale, Genie };

constexpr Effect ResolveEffect(bool animationsEnabled,
    bool dockSourceAndAnchorValid, int windowEffect, int popupEffect)
{
    if (!animationsEnabled)
        return Effect::None;
    if (dockSourceAndAnchorValid)
    {
        switch (windowEffect)
        {
        case 1: return Effect::Scale;
        case 2: return Effect::Fade;
        case 3: return Effect::Genie;
        default: break;
        }
    }
    switch (popupEffect)
    {
    case 1: return Effect::Fade;
    case 2: return Effect::Scale;
    default: return Effect::None;
    }
}

inline float ClampUnit(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

inline float EaseInOutSmooth(float progress)
{
    const float value = ClampUnit(progress);
    return value * value * (3.0f - 2.0f * value);
}

/**
 * @brief Return the normalized initial slope for the remaining smoothstep
 * segment when a compositor animation starts or reverses.
 */
inline float SegmentNormalizedStartSlope(
    float progress, bool opening)
{
    const float value = ClampUnit(progress);
    const float slope = opening
        ? 6.0f * value / (1.0f + 2.0f * value)
        : 6.0f * (1.0f - value) /
            (3.0f - 2.0f * value);
    return std::clamp(slope, 0.0f, 2.0f);
}

inline float ScaleCoordinate(
    float value, float anchor, float scale)
{
    return anchor + (value - anchor) * scale;
}

struct Visual
{
    float progress = 0.0f;
    float opacity = 0.0f;
    float scale = kMinimumScale;
    bool visible = false;
};

class State
{
public:
    void Configure(Effect effect, double durationScale, bool dockTiming = false)
    {
        effect_ = effect;
        dockTiming_ = dockTiming;
        durationScale_ = std::clamp(durationScale, 0.1, 10.0);
        if (effect_ == Effect::None)
        {
            if (targetVisible_)
                ShowImmediately();
            else
                ResetHidden();
        }
    }
    void Configure(bool fade, double durationScale)
    {
        Configure(fade ? Effect::Fade : Effect::Scale, durationScale);
    }
    [[nodiscard]] Effect GetEffect() const { return effect_; }
    [[nodiscard]] double DurationMilliseconds(bool opening) const
    {
        if (effect_ == Effect::None)
            return 0.0;
        if (dockTiming_)
        {
            // Match DockWindowTransition's full minimize/restore durations.
            return (effect_ == Effect::Genie ? 360.0 :
                effect_ == Effect::Fade ? 180.0 : 240.0) * durationScale_;
        }
        return (opening ? kOpenDurationMs : kCloseDurationMs) * durationScale_;
    }
    [[nodiscard]] float HiddenScale() const
    {
        return effect_ == Effect::Scale ? kMinimumScale : 1.0f;
    }
    void Open(std::uint64_t now)
    {
        if (effect_ == Effect::None)
        {
            ShowImmediately();
            return;
        }
        Advance(now);
        targetVisible_ = true;
        animating_ = progress_ < 1.0f;
        lastTick_ = now;
    }

    void Close(std::uint64_t now)
    {
        if (effect_ == Effect::None)
        {
            ResetHidden();
            return;
        }
        Advance(now);
        targetVisible_ = false;
        animating_ = progress_ > 0.0f;
        lastTick_ = now;
    }

    bool Advance(std::uint64_t now)
    {
        if (!animating_)
        {
            lastTick_ = now;
            return false;
        }

        const std::uint64_t elapsed =
            now >= lastTick_ ? now - lastTick_ : 0;
        lastTick_ = now;
        if (elapsed == 0)
            return false;

        const float duration = static_cast<float>(
            DurationMilliseconds(targetVisible_));
        const float delta = static_cast<float>(elapsed) / duration;
        const float previous = progress_;
        progress_ = ClampUnit(
            progress_ + (targetVisible_ ? delta : -delta));
        if ((targetVisible_ && progress_ >= 1.0f) ||
            (!targetVisible_ && progress_ <= 0.0f))
        {
            animating_ = false;
        }
        return progress_ != previous;
    }

    void ResetHidden()
    {
        progress_ = 0.0f;
        targetVisible_ = false;
        animating_ = false;
        lastTick_ = 0;
    }

    void ShowImmediately()
    {
        progress_ = 1.0f;
        targetVisible_ = true;
        animating_ = false;
        lastTick_ = 0;
    }

    [[nodiscard]] Visual GetVisual() const
    {
        const float eased = EaseInOutSmooth(progress_);
        return {
            progress_,
            effect_ == Effect::Genie
                ? static_cast<float>(dock_genie::Opacity(1.0 - eased)) : eased,
            effect_ == Effect::Scale
                ? kMinimumScale + (1.0f - kMinimumScale) * eased : 1.0f,
            targetVisible_ || progress_ > 0.0f
        };
    }

    [[nodiscard]] bool IsAnimating() const
    {
        return animating_;
    }

    [[nodiscard]] bool IsInteractive() const
    {
        return targetVisible_;
    }

    [[nodiscard]] bool IsClosing() const
    {
        return !targetVisible_ && progress_ > 0.0f;
    }

    [[nodiscard]] bool IsOpening() const
    {
        return targetVisible_ && animating_;
    }

    [[nodiscard]] bool IsHidden() const
    {
        return progress_ <= 0.0f;
    }

private:
    Effect effect_ = Effect::Scale;
    bool dockTiming_ = false;
    double durationScale_ = 1.0;
    float progress_ = 0.0f;
    bool targetVisible_ = false;
    bool animating_ = false;
    std::uint64_t lastTick_ = 0;
};
}
