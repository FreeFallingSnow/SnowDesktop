#pragma once

#include <algorithm>
#include <cstdint>

namespace snowdesktop::popup_animation_rules
{
constexpr std::uint64_t kOpenDurationMs = 90;
constexpr std::uint64_t kCloseDurationMs = 90;
// The shared QPC scheduler advances directly to the current time, so a delayed
// frame never causes catch-up rendering or stretches the transition.
constexpr float kMinimumScale = 0.18f;

inline float ClampUnit(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

inline float EaseInOutSmooth(float progress)
{
    const float value = ClampUnit(progress);
    return value * value *
        (3.0f - 2.0f * value);
}

struct Visual
{
    float progress = 0.0f;
    float scale = kMinimumScale;
    bool visible = false;
};

enum class ExistingSourceAction
{
    OpenAtRequestedAnchor,
    OpenAfterExistingCloses,
    CloseExisting,
    KeepClosing,
    ReopenExisting,
};

inline ExistingSourceAction ResolveExistingSourceAction(
    bool sameSource,
    bool interactive,
    bool closingStartedByCurrentPress = false)
{
    if (!sameSource)
    {
        return closingStartedByCurrentPress
            ? ExistingSourceAction::OpenAfterExistingCloses
            : ExistingSourceAction::OpenAtRequestedAnchor;
    }
    if (interactive)
        return ExistingSourceAction::CloseExisting;
    return closingStartedByCurrentPress
        ? ExistingSourceAction::KeepClosing
        : ExistingSourceAction::ReopenExisting;
}

/**
 * @brief 判断集合按钮的双击消息是否应重放为第二次普通按下。
 *
 * Windows 会用 WM_LBUTTONDBLCLK 替代第二个 WM_LBUTTONDOWN。集合开关需要
 * 收到这次按下才能在关闭动画中立即反向；若点位由已打开弹窗占用，则仍交给
 * 弹窗原有的双击处理。
 */
constexpr bool ShouldDispatchCollectionDoubleClickPress(
    bool collectionOpenButtonHit,
    bool pointerInsideInteractivePopup)
{
    return collectionOpenButtonHit &&
        !pointerInsideInteractivePopup;
}

inline bool ShouldUsePopupItemBounds(
    bool popupSourceExists,
    bool popupInteractive)
{
    return popupSourceExists && popupInteractive;
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
        const float delta =
            static_cast<float>(elapsed) / duration;
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
        const float easedScale =
            EaseInOutSmooth(progress_);
        return {
            progress_,
            kMinimumScale +
                (1.0f - kMinimumScale) * easedScale,
            progress_ > 0.0f
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

/**
 * @brief 弹窗是否仍占据屏幕区域并遮挡下层元素。
 * 以"已打开或仍可见"为准：Open() 一经调用桌面层即开始绘制全尺寸弹窗
 * （此时 progress 可能仍为 0），关闭动画期间弹窗也仍在绘制上层，被遮挡
 * 元素的 hover/右键/双击都不得穿透；仅完全隐藏后才解除遮挡。
 */
inline bool OccludesSurface(const State& state)
{
    return state.IsInteractive() || !state.IsHidden();
}

/**
 * @brief 可见弹窗遮挡坐标点时，该输入应被弹窗消费而不是穿透到下层元素。
 * @param popupVisible    弹窗仍可见（含开/关动画）
 * @param pointInsidePopup 坐标点位于弹窗矩形内
 */
inline bool ShouldConsumePointerInsidePopup(
    bool popupVisible, bool pointInsidePopup)
{
    return popupVisible && pointInsidePopup;
}
}
