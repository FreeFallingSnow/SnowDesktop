#pragma once

#include "animation_preferences.h"

#include <atomic>

namespace snowdesktop::animation
{

namespace detail
{
extern std::atomic<int> mode;
extern std::atomic<int> popupEffect;
extern std::atomic<int> speed;
extern std::atomic<int> frameLimit;
extern std::atomic<bool> energySaver;
extern std::atomic<bool> onBattery;
extern std::atomic<int> windowEffect;
extern std::atomic<int> dockPosition;
}

bool SystemAnimationsEnabled(bool refresh = false) noexcept;
void SetRuntimePreferences(int mode, int popupEffect, int speed,
    int frameLimit, bool energySaver, bool onBattery, int windowEffect,
    int dockPosition = 0) noexcept;
bool RuntimeAnimationsEnabled() noexcept;
double RuntimeDurationScale() noexcept;
int RuntimePopupEffect() noexcept;
int RuntimeWindowEffect() noexcept;
int RuntimeDockPosition() noexcept;
int RuntimeFrameLimit() noexcept;
}
