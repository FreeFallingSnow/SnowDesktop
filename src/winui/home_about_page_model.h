#pragma once

#include "../settings_route.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace snowdesktop::winui
{

enum class SettingsUpdateState : std::uint8_t
{
    Unknown,
    Checking,
    UpToDate,
    UpdateAvailable,
    ManagedByStore,
    Failed,
};

enum class SettingsBackupState : std::uint8_t
{
    Unknown,
    Empty,
    Ready,
    Running,
    Succeeded,
    Failed,
};

/** Host-owned status increment applied only to its matching settings session. */
struct HomeAboutStatusPatch
{
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::optional<std::wstring> applicationVersion;
    std::optional<std::size_t> installedWidgetCount;
    std::optional<SettingsUpdateState> updateState;
    std::optional<std::wstring> availableVersion;
    std::optional<std::wstring> updateDetail;
    std::optional<SettingsBackupState> backupState;
    std::optional<std::size_t> backupCount;
    std::optional<std::wstring> backupDetail;
};

enum class HomeAboutCommand : std::uint8_t
{
    CheckForUpdates,
    OpenProject,
    OpenLicense,
    OpenThirdPartyNotices,
};

} // namespace snowdesktop::winui
