#pragma once

namespace snowdesktop::animation
{
inline constexpr int FollowSystem = 0;
inline constexpr int AlwaysOn = 1;
inline constexpr int Disabled = 2;
inline constexpr int DefaultMode = AlwaysOn;
inline constexpr int NoEffect = 0;
inline constexpr int Fade = 1;
inline constexpr int Scale = 2;

int NormalizeMode(int value) noexcept;
int NormalizePopupEffect(int value) noexcept;
int NormalizeSpeed(int value) noexcept;
int NormalizeFrameLimit(int value) noexcept;
int NormalizeHoverEffect(int value) noexcept;
float NormalizeHoverScale(float value) noexcept;
int NormalizeLaunchEffect(int value) noexcept;
int NormalizeWindowEffect(int value) noexcept;
bool ResolveEnabled(int mode, bool systemEnabled) noexcept;
double DurationScale(int speed) noexcept;
int ResolveFrameLimit(int requested, bool energySaverPreference,
    bool batteryPreference, bool systemEnergySaver, bool onBattery) noexcept;
}
