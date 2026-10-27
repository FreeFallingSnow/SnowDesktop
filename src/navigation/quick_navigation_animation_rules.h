#pragma once

#include "dock/dock_genie_rules.h"
#include "dock/dock_attraction_rules.h"
#include "layout/popup_motion_curve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace snowdesktop::quick_navigation_animation_rules
{
constexpr std::uint64_t kOpenDurationMs = 140;
constexpr std::uint64_t kCloseDurationMs = 110;
constexpr float kMinimumScale = 0.08f;

enum class Effect { None, Fade, Scale, Genie, Attraction };

constexpr bool IsDeformationEffect(Effect effect)
{
    return effect == Effect::Genie || effect == Effect::Attraction;
}

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
        case 4: return Effect::Attraction;
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

struct ResizeRect
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool operator==(const ResizeRect& other) const
    {
        return left == other.left && top == other.top &&
            right == other.right && bottom == other.bottom;
    }
};

class ResizeState
{
public:
    void Start(ResizeRect from, ResizeRect target, std::uint64_t now,
        bool enabled, double durationScale)
    {
        StartMotion(from, target, now, enabled,
            180.0 * std::clamp(durationScale, 0.1, 10.0));
    }

    void StartResultFollow(ResizeRect from, ResizeRect target, std::uint64_t now,
        bool enabled, double durationScale)
    {
        // Geometry follows live input, so slower presentation presets must not
        // leave it trailing repeated result arrivals. Faster presets still apply.
        StartMotion(from, target, now, enabled,
            80.0 * std::clamp(durationScale, 0.1, 1.0));
    }

private:
    void StartMotion(ResizeRect from, ResizeRect target, std::uint64_t now,
        bool enabled, double duration)
    {
        const bool continuing = animating_;
        if (continuing) Advance(now, enabled);
        durationMs_ = duration;
        from_ = current_ = from;
        target_ = target;
        started_ = now;
        const auto values = Coordinates(from);
        for (std::size_t i = 0; i < motion_.size(); ++i)
            motion_[i] = {values[i], continuing && enabled ? motion_[i].velocity : 0.0};
        animating_ = enabled && (!(from == target) || std::any_of(motion_.begin(), motion_.end(),
            [](const auto& value) { return std::abs(value.velocity) > .01; }));
        if (animating_) BuildTracks();
        else Settle();
    }

public:
    // Search results and available work areas can change during the resize.
    // Continue from the displayed frame instead of jumping to either endpoint.
    void Retarget(ResizeRect target, std::uint64_t now)
    {
        if (target == target_) return;
        Advance(now, true);
        from_ = current_;
        target_ = target;
        started_ = now;
        animating_ = !(from_ == target_) || std::any_of(motion_.begin(), motion_.end(),
            [](const auto& value) { return std::abs(value.velocity) > .01; });
        if (animating_) BuildTracks();
        else Settle();
    }

    ResizeRect Advance(std::uint64_t now, bool enabled)
    {
        if (!animating_) return current_;
        const auto elapsed = now >= started_ ? now - started_ : 0;
        if (!enabled || static_cast<double>(elapsed) >= durationMs_) { Settle(); return current_; }
        for (std::size_t i = 0; i < motion_.size(); ++i)
            motion_[i] = popup_motion_curve::Evaluate(tracks_[i], static_cast<double>(elapsed) / 1000.0);
        current_ = {static_cast<int>(std::lround(motion_[0].position)), static_cast<int>(std::lround(motion_[1].position)),
            static_cast<int>(std::lround(motion_[2].position)), static_cast<int>(std::lround(motion_[3].position))};
        return current_;
    }

    [[nodiscard]] bool IsAnimating() const { return animating_; }
    [[nodiscard]] std::array<double, 4> Velocity() const
    { return {motion_[0].velocity, motion_[1].velocity, motion_[2].velocity, motion_[3].velocity}; }
    void Stop() { animating_ = false; for (auto& value : motion_) value.velocity = 0.0; }
    [[nodiscard]] ResizeRect Envelope() const
    {
        auto low = Coordinates(from_), high = low;
        const auto target = Coordinates(target_);
        for (std::size_t i = 0; i < tracks_.size(); ++i)
        {
            low[i] = std::min(low[i], target[i]); high[i] = std::max(high[i], target[i]);
            // Turning points are explicit segment boundaries. Include inertia
            // in the retained host so a rapid reversal cannot clip the panel.
            for (const auto& segment : tracks_[i].segments)
                for (const double value : {segment.constant, segment.EvaluateLocal(segment.durationSeconds).position})
                { low[i] = std::min(low[i], value); high[i] = std::max(high[i], value); }
        }
        return {static_cast<int>(std::floor(low[0])), static_cast<int>(std::floor(low[1])),
            static_cast<int>(std::ceil(high[2])), static_cast<int>(std::ceil(high[3]))};
    }

private:
    static std::array<double, 4> Coordinates(ResizeRect rect)
    { return {static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right), static_cast<double>(rect.bottom)}; }
    void Settle()
    {
        current_ = target_; animating_ = false;
        const auto target = Coordinates(target_);
        for (std::size_t i = 0; i < motion_.size(); ++i)
        { motion_[i] = {target[i], 0.0}; tracks_[i].segments.clear(); }
    }
    void BuildTracks()
    {
        const auto target = Coordinates(target_);
        for (std::size_t i = 0; i < tracks_.size(); ++i)
        {
            // Fit in normalized coordinates: the error remains subpixel for
            // large desktop rectangles without generating excessive segments.
            const double factor = std::max({1.0, std::abs(motion_[i].position - target[i]),
                std::abs(motion_[i].velocity) * durationMs_ / 1000.0});
            tracks_[i] = popup_motion_curve::Build(
                {(motion_[i].position - target[i]) / factor, motion_[i].velocity / factor},
                0.0, 6.638352067994 / (durationMs_ * .00075), durationMs_);
            tracks_[i].target = target[i];
            for (auto& segment : tracks_[i].segments)
            {
                segment.constant = target[i] + factor * segment.constant;
                segment.linear *= factor; segment.quadratic *= factor; segment.cubic *= factor;
            }
        }
    }
    std::array<popup_motion_curve::Sample, 4> motion_{};
    std::array<popup_motion_curve::Timeline, 4> tracks_{};
    ResizeRect from_{};
    ResizeRect target_{};
    ResizeRect current_{};
    std::uint64_t started_ = 0;
    double durationMs_ = 180.0;
    bool animating_ = false;
};

class State
{
public:
    void Configure(Effect effect, double durationScale, bool dockTiming = false,
        bool responsive = false)
    {
        const bool nextResponsive = responsive && !dockTiming &&
            (effect == Effect::Scale || effect == Effect::Fade);
        if (nextResponsive && !responsive_)
            motion_ = {EaseInOutSmooth(progress_), 0.0};
        if (!nextResponsive && responsive_)
        {
            const double reveal = ClampUnit(static_cast<float>(motion_.position));
            progress_ = static_cast<float>(0.5 - std::sin(std::asin(1.0 - 2.0 * reveal) / 3.0));
        }
        const bool changed = nextResponsive != responsive_;
        responsive_ = nextResponsive;
        effect_ = effect;
        dockTiming_ = dockTiming;
        durationScale_ = std::clamp(durationScale, 0.1, 10.0);
        if (responsive_ && changed && animating_) StartResponsiveSegment();
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
    [[nodiscard]] bool UsesResponsiveMotion() const { return responsive_; }
    [[nodiscard]] double Velocity() const { return motion_.velocity; }
    [[nodiscard]] popup_motion_curve::Timeline RemainingTimeline() const
    { return popup_motion_curve::Remainder(timeline_, elapsed_); }
    [[nodiscard]] popup_motion_curve::Timeline RemainingScaleTimeline() const
    {
        auto result = RemainingTimeline();
        const double offset = HiddenScale(), factor = 1.0 - offset;
        result.target = offset + factor * result.target;
        for (auto& segment : result.segments)
        {
            segment.constant = offset + factor * segment.constant;
            segment.linear *= factor; segment.quadratic *= factor; segment.cubic *= factor;
        }
        return result;
    }
    [[nodiscard]] double DurationMilliseconds(bool opening) const
    {
        if (effect_ == Effect::None)
            return 0.0;
        if (responsive_)
            return std::ceil((opening ? 160.0 : 110.0) * durationScale_);
        if (dockTiming_)
        {
            // Match DockWindowTransition's full minimize/restore durations.
            return (effect_ == Effect::Attraction ? 420.0 :
                effect_ == Effect::Genie ? 360.0 :
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
        if (responsive_ && targetVisible_ && animating_) return;
        targetVisible_ = true;
        if (responsive_) { StartResponsiveSegment(); lastTick_ = now; return; }
        animating_ = progress_ < 1.0f;
        lastTick_ = now;
    }

    void Close(std::uint64_t now)
    {
        resize_.Stop();
        if (effect_ == Effect::None)
        {
            ResetHidden();
            return;
        }
        Advance(now);
        if (responsive_ && !targetVisible_ && animating_) return;
        targetVisible_ = false;
        if (responsive_) { StartResponsiveSegment(); lastTick_ = now; return; }
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

        if (responsive_)
        {
            elapsed_ += static_cast<double>(elapsed);
            motion_ = popup_motion_curve::Evaluate(timeline_, elapsed_ / 1000.0);
            if (elapsed_ >= timeline_.durationMilliseconds)
            {
                motion_ = {targetVisible_ ? 1.0 : 0.0, 0.0};
                animating_ = false;
            }
            progress_ = ClampUnit(static_cast<float>(motion_.position));
            return true;
        }

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
        resize_ = {};
        motion_ = {}; timeline_ = {}; elapsed_ = 0.0;
        progress_ = 0.0f;
        targetVisible_ = false;
        animating_ = false;
        lastTick_ = 0;
    }

    void ShowImmediately()
    {
        motion_ = {1.0, 0.0}; timeline_ = {}; elapsed_ = 0.0;
        progress_ = 1.0f;
        targetVisible_ = true;
        animating_ = false;
        lastTick_ = 0;
    }

    [[nodiscard]] Visual GetVisual() const
    {
        const float eased = responsive_ ? static_cast<float>(motion_.position) : EaseInOutSmooth(progress_);
        return {
            progress_,
            effect_ == Effect::Attraction
                ? static_cast<float>(dock_attraction::Opacity(1.0 - eased))
                : effect_ == Effect::Genie
                ? static_cast<float>(dock_genie::Opacity(1.0 - eased)) : ClampUnit(eased),
            effect_ == Effect::Scale
                ? kMinimumScale + (1.0f - kMinimumScale) * eased : 1.0f,
            targetVisible_ || progress_ > 0.0f || (responsive_ && animating_)
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
        return !targetVisible_ && (progress_ > 0.0f || (responsive_ && animating_));
    }

    [[nodiscard]] bool IsOpening() const
    {
        return targetVisible_ && animating_;
    }

    [[nodiscard]] bool IsHidden() const
    {
        return progress_ <= 0.0f && (!responsive_ || !animating_);
    }

    [[nodiscard]] ResizeState& Resize() { return resize_; }
    [[nodiscard]] const ResizeState& Resize() const { return resize_; }

private:
    void StartResponsiveSegment()
    {
        const double target = targetVisible_ ? 1.0 : 0.0;
        const double duration = popup_motion_curve::AtRest(motion_, target)
            ? 0.0 : DurationMilliseconds(targetVisible_);
        elapsed_ = 0.0;
        const double frequency = popup_motion_curve::TargetBoundedFrequency(motion_, target,
            duration > 0.0 ? (targetVisible_ ? 6.638352067994 : 3.0) / (duration * 0.00075) : 0.0);
        timeline_ = popup_motion_curve::Build(motion_, target,
            frequency, duration);
        animating_ = duration > 0.0;
        if (!animating_) motion_ = {target, 0.0};
        progress_ = ClampUnit(static_cast<float>(motion_.position));
    }
    bool responsive_ = false;
    popup_motion_curve::Sample motion_;
    popup_motion_curve::Timeline timeline_;
    double elapsed_ = 0.0;
    ResizeState resize_;
    Effect effect_ = Effect::Scale;
    bool dockTiming_ = false;
    double durationScale_ = 1.0;
    float progress_ = 0.0f;
    bool targetVisible_ = false;
    bool animating_ = false;
    std::uint64_t lastTick_ = 0;
};
}
