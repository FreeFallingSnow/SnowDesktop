#pragma once

#include "desktop_idle_hide_preferences.h"
#include <cstdint>

namespace snowdesktop::desktop_idle_hide
{


enum class Action { None, Hide, Show };

struct Sample
{
    std::uint64_t now = 0;
    std::uint64_t inputIdleMs = 0;
    int pointerX = 0;
    int pointerY = 0;
    bool pointerValid = false;
    bool pointerOnDesktop = false;
    bool enabled = false;
    bool suspended = false;
    bool interactionBusy = false;
    bool desktopHidden = false;
    bool inputKnown = false;
    int delaySeconds = kDefaultDelaySeconds;
};

// Owns only automatic hiding. Manual visibility changes revoke that ownership.
class State
{
public:
    bool AutoHidden() const noexcept { return autoHidden_; }

    void Reset(std::uint64_t now) noexcept
    {
        autoHidden_ = false;
        initialized_ = true;
        idleFloor_ = now;
    }

    Action Update(const Sample& sample) noexcept
    {
        const bool moved = sample.pointerValid && pointerKnown_ &&
            (sample.pointerX != pointerX_ || sample.pointerY != pointerY_);
        pointerKnown_ = sample.pointerValid;
        if (pointerKnown_)
        {
            pointerX_ = sample.pointerX;
            pointerY_ = sample.pointerY;
        }
        if (!initialized_)
            Reset(sample.now);
        if (!sample.desktopHidden)
            autoHidden_ = false;
        if (!sample.enabled || sample.suspended)
        {
            const bool restore = autoHidden_ && sample.desktopHidden;
            Reset(sample.now);
            return restore ? Action::Show : Action::None;
        }
        if (moved || sample.interactionBusy)
            idleFloor_ = sample.now;
        if (sample.interactionBusy)
            return Action::None;
        if (sample.desktopHidden)
        {
            if (autoHidden_ && moved && sample.pointerOnDesktop)
            {
                autoHidden_ = false;
                return Action::Show;
            }
            return Action::None;
        }
        if (!sample.pointerValid || !sample.inputKnown)
        {
            idleFloor_ = sample.now;
            return Action::None;
        }
        // A Windows input timestamp can regress. Keep the newest activity
        // observed so far, including cursor movement and desktop interactions.
        if (sample.inputIdleMs < sample.now - idleFloor_)
            idleFloor_ = sample.now - sample.inputIdleMs;
        const auto delay = static_cast<std::uint64_t>(NormalizeDelay(sample.delaySeconds)) * 1000;
        if (sample.now - idleFloor_ >= delay)
        {
            autoHidden_ = true;
            return Action::Hide;
        }
        return Action::None;
    }

private:
    bool autoHidden_ = false;
    bool initialized_ = false;
    bool pointerKnown_ = false;
    int pointerX_ = 0;
    int pointerY_ = 0;
    std::uint64_t idleFloor_ = 0;
};
}
