#include "animation_settings.h"
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace snowdesktop::animation
{

namespace detail
{
std::atomic<int> mode{DefaultMode};
std::atomic<int> popupEffect{Scale};
std::atomic<int> speed{1};
std::atomic<int> frameLimit{0};
std::atomic<bool> energySaver{true};
std::atomic<bool> onBattery{false};
std::atomic<int> windowEffect{1};
std::atomic<int> dockPosition{0};
}

int NormalizeMode(int value) noexcept
{
    return value >= FollowSystem && value <= Disabled ? value : DefaultMode;
}

int NormalizePopupEffect(int value) noexcept
{
    return value >= NoEffect && value <= Scale ? value : Scale;
}

int NormalizeSpeed(int value) noexcept
{
    return value >= 0 && value <= 5 ? value : 1;
}

int NormalizeFrameLimit(int value) noexcept
{
    return value == 30 || value == 60 || value == 120 ? value : 0;
}

int NormalizeHoverEffect(int value) noexcept
{
    return value >= 0 && value <= 2 ? value : 2;
}

float NormalizeHoverScale(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 1.0f, 2.0f) : 1.28f;
}

int NormalizeLaunchEffect(int value) noexcept
{
    return value >= 0 && value <= 2 ? value : 1;
}

int NormalizeWindowEffect(int value) noexcept
{
    return value >= 0 && value <= 4 ? value : 1;
}

bool ResolveEnabled(int mode, bool systemEnabled) noexcept
{
    return NormalizeMode(mode) == AlwaysOn ||
        (NormalizeMode(mode) == FollowSystem && systemEnabled);
}

double DurationScale(int speed) noexcept
{
    // Keep the three existing stored choices and append slower playback.
    constexpr double scales[]{0.7, 1.0, 1.4, 2.0, 3.0, 4.0};
    return scales[NormalizeSpeed(speed)];
}

int ResolveFrameLimit(int requested, bool energySaverPreference,
    bool batteryPreference, bool systemEnergySaver, bool onBattery) noexcept
{
    const int limit = NormalizeFrameLimit(requested);
    const bool saving = (energySaverPreference && systemEnergySaver) ||
        (batteryPreference && onBattery);
    return saving ? (limit == 0 ? 30 : std::min(limit, 30)) : limit;
}

bool SystemAnimationsEnabled(bool refresh) noexcept
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

void SetRuntimePreferences(int mode, int popupEffect, int speed,
    int frameLimit, bool energySaver, bool onBattery, int windowEffect,
    int dockPosition) noexcept
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

bool RuntimeAnimationsEnabled() noexcept
{
    const int mode = detail::mode.load(std::memory_order_relaxed);
    return ResolveEnabled(mode, mode == FollowSystem && SystemAnimationsEnabled());
}

double RuntimeDurationScale() noexcept
{
    return DurationScale(detail::speed.load(std::memory_order_relaxed));
}

int RuntimePopupEffect() noexcept
{
    return RuntimeAnimationsEnabled()
        ? detail::popupEffect.load(std::memory_order_relaxed) : NoEffect;
}

int RuntimeWindowEffect() noexcept
{
    return detail::windowEffect.load(std::memory_order_relaxed);
}

int RuntimeDockPosition() noexcept
{
    return detail::dockPosition.load(std::memory_order_relaxed);
}

int RuntimeFrameLimit() noexcept
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
