#pragma once

#include <algorithm>
#include <cstdint>

namespace snowdesktop::quick_navigation_animation_rules
{
constexpr std::uint64_t kOpenDurationMs = 140;
constexpr std::uint64_t kCloseDurationMs = 110;
constexpr unsigned int kFrameIntervalMs = 8;
constexpr float kMinimumScale = 0.08f;

inline float ClampUnit(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

inline float EaseInOutSmooth(float progress)
{
    const float value = ClampUnit(progress);
    return value * value * (3.0f - 2.0f * value);
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

enum class AnchorMode
{
    Pointer,
    DockSearch,
};

constexpr bool ShouldRefreshCloseAnchor(
    AnchorMode mode)
{
    return mode == AnchorMode::Pointer;
}

class State
{
public:
    void Open(std::uint64_t now)
    {
        Advance(now);
        targetVisible_ = true;
        animating_ = progress_ < 1.0f;
        lastTick_ = now;
    }

    void Close(std::uint64_t now)
    {
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
            targetVisible_ ? kOpenDurationMs : kCloseDurationMs);
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
            eased,
            kMinimumScale + (1.0f - kMinimumScale) * eased,
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

    [[nodiscard]] bool IsHidden() const
    {
        return progress_ <= 0.0f;
    }

private:
    float progress_ = 0.0f;
    bool targetVisible_ = false;
    bool animating_ = false;
    std::uint64_t lastTick_ = 0;
};
}
