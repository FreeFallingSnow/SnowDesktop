#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// Host-private preferences. These do not replace the system accessibility
// information exposed to Lua packages.
namespace snowdesktop::animation
{
inline constexpr int FollowSystem = 0;
inline constexpr int AlwaysOn = 1;
inline constexpr int Disabled = 2;
inline constexpr int NoEffect = 0;
inline constexpr int Fade = 1;
inline constexpr int Scale = 2;

inline int NormalizeMode(int value) noexcept
{
    return value >= FollowSystem && value <= Disabled ? value : FollowSystem;
}
inline int NormalizePopupEffect(int value) noexcept
{
    return value >= NoEffect && value <= Scale ? value : Scale;
}
inline int NormalizeSpeed(int value) noexcept
{
    return value >= 0 && value <= 2 ? value : 1;
}
inline int NormalizeFrameLimit(int value) noexcept
{
    return value == 30 || value == 60 || value == 120 ? value : 0;
}
inline int NormalizeHoverEffect(int value) noexcept
{
    return value >= 0 && value <= 2 ? value : 2;
}
inline float NormalizeHoverScale(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 1.0f, 2.0f) : 1.28f;
}
inline int NormalizeLaunchEffect(int value) noexcept
{
    return value >= 0 && value <= 2 ? value : 1;
}
inline int NormalizeWindowEffect(int value) noexcept
{
    return value >= 0 && value <= 3 ? value : 1;
}
inline bool ResolveEnabled(int mode, bool systemEnabled) noexcept
{
    return NormalizeMode(mode) == AlwaysOn ||
        (NormalizeMode(mode) == FollowSystem && systemEnabled);
}
inline double DurationScale(int speed) noexcept
{
    return NormalizeSpeed(speed) == 0 ? 0.7 :
        NormalizeSpeed(speed) == 2 ? 1.4 : 1.0;
}
inline int ResolveFrameLimit(int requested, bool energySaverPreference,
    bool batteryPreference, bool systemEnergySaver, bool onBattery) noexcept
{
    const int limit = NormalizeFrameLimit(requested);
    const bool saving = (energySaverPreference && systemEnergySaver) ||
        (batteryPreference && onBattery);
    return saving ? (limit == 0 ? 30 : std::min(limit, 30)) : limit;
}
inline bool SystemAnimationsEnabled(bool refresh = false) noexcept
{
    static std::atomic<ULONGLONG> lastRead{0};
    static std::atomic<bool> cached{true};
    const auto now = GetTickCount64();
    const auto last = lastRead.load(std::memory_order_relaxed);
    if (!refresh && last != 0 && now - last < 250)
        return cached.load(std::memory_order_relaxed);
    ANIMATIONINFO info{sizeof(info)};
    const bool minimize = !SystemParametersInfoW(SPI_GETANIMATION,
        sizeof(info), &info, 0) || info.iMinAnimate != 0;
    BOOL enabled = TRUE;
    const bool client = !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,
        0, &enabled, 0) || enabled != FALSE;
    cached.store(minimize && client, std::memory_order_relaxed);
    lastRead.store(now, std::memory_order_relaxed);
    return minimize && client;
}

namespace detail
{
inline std::atomic<int> mode{FollowSystem};
inline std::atomic<int> popupEffect{Scale};
inline std::atomic<int> speed{1};
inline std::atomic<int> frameLimit{0};
inline std::atomic<bool> energySaver{true};
inline std::atomic<bool> onBattery{false};
inline std::atomic<int> windowEffect{1};
inline std::atomic<int> dockPosition{0};
}

inline void SetRuntimePreferences(int mode, int popupEffect, int speed,
    int frameLimit, bool energySaver, bool onBattery, int windowEffect,
    int dockPosition = 0) noexcept
{
    detail::mode.store(NormalizeMode(mode), std::memory_order_relaxed);
    detail::popupEffect.store(NormalizePopupEffect(popupEffect), std::memory_order_relaxed);
    detail::speed.store(NormalizeSpeed(speed), std::memory_order_relaxed);
    detail::frameLimit.store(NormalizeFrameLimit(frameLimit), std::memory_order_relaxed);
    detail::energySaver.store(energySaver, std::memory_order_relaxed);
    detail::onBattery.store(onBattery, std::memory_order_relaxed);
    detail::windowEffect.store(NormalizeWindowEffect(windowEffect), std::memory_order_relaxed);
    detail::dockPosition.store(std::clamp(dockPosition, 0, 3), std::memory_order_relaxed);
}
inline bool RuntimeAnimationsEnabled() noexcept
{
    const int mode = detail::mode.load(std::memory_order_relaxed);
    return ResolveEnabled(mode, mode == FollowSystem && SystemAnimationsEnabled());
}
inline double RuntimeDurationScale() noexcept
{
    return DurationScale(detail::speed.load(std::memory_order_relaxed));
}
inline int RuntimePopupEffect() noexcept
{
    return RuntimeAnimationsEnabled()
        ? detail::popupEffect.load(std::memory_order_relaxed) : NoEffect;
}
inline int RuntimeWindowEffect() noexcept
{
    return detail::windowEffect.load(std::memory_order_relaxed);
}
inline int RuntimeDockPosition() noexcept
{
    return detail::dockPosition.load(std::memory_order_relaxed);
}
inline int RuntimeFrameLimit() noexcept
{
    static std::atomic<ULONGLONG> lastRead{0};
    static std::atomic<unsigned> powerFlags{0};
    const auto now = GetTickCount64();
    const auto last = lastRead.load(std::memory_order_relaxed);
    if (last == 0 || now - last >= 1000)
    {
        SYSTEM_POWER_STATUS power{};
        unsigned flags = 0;
        if (GetSystemPowerStatus(&power))
            flags = (power.SystemStatusFlag ? 1u : 0u) |
                (power.ACLineStatus == 0 ? 2u : 0u);
        powerFlags.store(flags, std::memory_order_relaxed);
        lastRead.store(now, std::memory_order_relaxed);
    }
    const auto flags = powerFlags.load(std::memory_order_relaxed);
    return ResolveFrameLimit(detail::frameLimit.load(std::memory_order_relaxed),
        detail::energySaver.load(std::memory_order_relaxed),
        detail::onBattery.load(std::memory_order_relaxed),
        (flags & 1u) != 0, (flags & 2u) != 0);
}
}
