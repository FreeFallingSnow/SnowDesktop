#pragma once

#include "rename_controller.h"
#include <windows.h>
#include <optional>
#include <string>

// Copy identities, never Item pointers or vector indexes: a Shell refresh can
// replace/reorder the model while the second click is waiting for its timer.
struct RenameClickTarget
{
    RenameTargetKind kind = RenameTargetKind::None;
    std::wstring key;
    std::wstring surface;
    bool operator==(const RenameClickTarget&) const = default;
};

class RenameClickController
{
public:
    void Press(const RenameClickTarget& target, RECT label, POINT point,
        bool singleSelected, bool unmodified, ULONGLONG now, UINT doubleClickTime)
    {
        pressed_.reset();
        pending_.reset();
        const bool fastRepeat = target == lastTarget_ &&
            now - lastPressTime_ <= doubleClickTime;
        lastTarget_ = target;
        lastPressTime_ = now;
        if (target.kind == RenameTargetKind::None || !singleSelected ||
            !unmodified || fastRepeat || !PtInRect(&label, point))
            return;
        pressed_ = target;
        label_ = label;
        point_ = point;
    }

    void Move(POINT point, int dragX, int dragY)
    {
        if (pressed_ && (point.x - point_.x > dragX ||
                point_.x - point.x > dragX || point.y - point_.y > dragY ||
                point_.y - point.y > dragY))
            pressed_.reset();
        if (pending_ && !PtInRect(&label_, point))
            pending_.reset();
    }

    bool Release(const RenameClickTarget& target, RECT label, POINT point,
        bool eligible, ULONGLONG now, UINT doubleClickTime)
    {
        const bool arm = pressed_ && *pressed_ == target && eligible &&
            EqualRect(&label_, &label) && PtInRect(&label_, point);
        pressed_.reset();
        if (!arm) return false;
        pending_ = target;
        // Let a following WM_LBUTTONDBLCLK cancel before opening an EDIT.
        deadline_ = now + doubleClickTime;
        return true;
    }

    bool Waiting(ULONGLONG now) const { return pending_ && now < deadline_; }
    bool Active() const { return pressed_.has_value() || pending_.has_value(); }

    std::optional<RenameClickTarget> TakeReady(const RenameClickTarget& target,
        RECT label, bool eligible, ULONGLONG now)
    {
        if (Waiting(now)) return std::nullopt;
        const auto result = pending_ && *pending_ == target && eligible &&
            EqualRect(&label_, &label) ? pending_ : std::nullopt;
        Cancel();
        return result;
    }

    void Cancel()
    {
        pressed_.reset();
        pending_.reset();
        lastTarget_ = {};
        lastPressTime_ = 0;
    }

private:
    std::optional<RenameClickTarget> pressed_;
    std::optional<RenameClickTarget> pending_;
    RenameClickTarget lastTarget_;
    ULONGLONG lastPressTime_ = 0;
    ULONGLONG deadline_ = 0;
    RECT label_{};
    POINT point_{};
};
