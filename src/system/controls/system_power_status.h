#pragma once
#include <cstdint>
#include <optional>

namespace snowdesktop
{
// Internal interpretation of SYSTEM_POWER_STATUS. Unknown is not false, and
// reserved/out-of-range bytes must never manufacture a full, charging battery.
struct SystemPowerStatus
{
    std::optional<bool> batteryPresent;
    std::optional<bool> onAC;
    std::optional<bool> charging;
    std::optional<unsigned> batteryPercent;
    bool saver = false;
};

inline SystemPowerStatus DecodeSystemPowerStatus(std::uint8_t flags,
    std::uint8_t ac, std::uint8_t percent, std::uint8_t saver)
{
    SystemPowerStatus result;
    if (ac <= 1) result.onAC = ac == 1;
    result.saver = saver == 1;
    // 255 includes every flag bit but explicitly means unknown. Test it first.
    if (flags == 255) return result;
    result.batteryPresent = (flags & 128) == 0;
    if (!*result.batteryPresent) return result;
    result.charging = (flags & 8) != 0;
    if (percent <= 100) result.batteryPercent = percent;
    return result;
}
}
