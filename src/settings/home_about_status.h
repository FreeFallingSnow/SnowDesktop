#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::settings
{
enum class SettingsBackupState : std::uint8_t
{
    Unknown,
    Empty,
    Ready,
    Running,
    Succeeded,
    Failed,
};

struct DebugEnvironmentSummary
{
    std::wstring id;
    std::wstring name;
    bool operator==(const DebugEnvironmentSummary&) const = default;
};

/** Host-owned Home/About/Debug status for one matching settings session. */
struct HomeAboutStatusPatch
{
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::optional<std::wstring> applicationVersion;
    std::optional<std::size_t> installedWidgetCount;
    /** Deployment ownership controls whether the Store update button exists. */
    std::optional<bool> packaged;
    std::optional<SettingsBackupState> backupState;
    std::optional<std::size_t> backupCount;
    std::optional<std::wstring> backupDetail;
    /** Session-only scheduler state used by the conditional Debug page. */
    std::optional<bool> animationDiagnosticsEnabled;
    std::optional<bool> temporaryInitializationEnabled;
    std::optional<bool> debugProfileEnabled;
    std::optional<std::wstring> debugDataDirectory;
    std::optional<std::wstring> debugDesktopDirectory;
    std::optional<std::wstring> animationDiagnosticsStatus;
    std::optional<bool> usageGuideExpanded;
    std::optional<std::vector<DebugEnvironmentSummary>> debugEnvironments;
    std::optional<std::wstring> debugEnvironmentId;
    std::optional<std::wstring> debugEnvironmentName;
    std::optional<bool> touchSimulationActive;
};

// Shared by requested snapshots and unsolicited host publications. This
// stream is independent of persisted settings/controller revisions.
class HomeAboutStatusSequence
{
public:
    HomeAboutStatusPatch Next(std::uint64_t generation) noexcept
    {
        HomeAboutStatusPatch patch;
        patch.generation = generation;
        patch.revision = ++revision_;
        return patch;
    }
private:
    std::uint64_t revision_ = 0;
};

} // namespace snowdesktop::settings
