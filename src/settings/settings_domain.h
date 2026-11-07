#pragma once

#include <cstdint>

namespace snowdesktop
{

enum class SettingsDomain : std::uint32_t
{
    None = 0,
    Personalization = 1u << 0,
    Dock = 1u << 1,
    Navigation = 1u << 2,
    General = 1u << 3,
    Category = 1u << 4,
    Desktop = 1u << 5,
    NativeStored =
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4),
    All =
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) |
        (1u << 5),
};

[[nodiscard]] constexpr SettingsDomain operator|(
    SettingsDomain left,
    SettingsDomain right) noexcept
{
    return static_cast<SettingsDomain>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr SettingsDomain operator&(
    SettingsDomain left,
    SettingsDomain right) noexcept
{
    return static_cast<SettingsDomain>(
        static_cast<std::uint32_t>(left) &
        static_cast<std::uint32_t>(right));
}

constexpr SettingsDomain& operator|=(
    SettingsDomain& left,
    SettingsDomain right) noexcept
{
    left = left | right;
    return left;
}

constexpr SettingsDomain& operator&=(
    SettingsDomain& left,
    SettingsDomain right) noexcept
{
    left = left & right;
    return left;
}

[[nodiscard]] constexpr SettingsDomain operator~(
    SettingsDomain value) noexcept
{
    return static_cast<SettingsDomain>(
        ~static_cast<std::uint32_t>(value));
}

[[nodiscard]] constexpr bool HasSettingsDomain(
    SettingsDomain domains,
    SettingsDomain domain) noexcept
{
    return (domains & domain) != SettingsDomain::None;
}

/**
 * How an updated value should flow beyond the immutable snapshot.
 *
 * Draft updates only mark dirty. Preview schedules a coalesced host preview.
 * Commit schedules persistence without preview. PreviewAndCommit does both,
 * which is useful for the final value of a continuous control.
 */
enum class SettingsUpdateMode : std::uint8_t
{
    Draft = 0,
    Preview = 1u << 0,
    Commit = 1u << 1,
    PreviewAndCommit = (1u << 0) | (1u << 1),
};

enum class SettingsReloadPolicy : std::uint8_t
{
    PreservePendingChanges,
    DiscardPendingChanges,
};

} // namespace snowdesktop
