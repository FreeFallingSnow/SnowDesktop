#pragma once

#include "platform/auto_start_rules.h"
#include <cstdint>
#include <string>

namespace snowdesktop
{
struct AutoStartQueryResult
{
    bool packaged = false;
    bool stateKnown = false;
    bool enabled = false;
    bool taskOwnedByCurrentDeployment = false;
    UnifiedAutoStartTaskState taskStatus =
        UnifiedAutoStartTaskState::Unavailable;
    UnifiedAutoStartOwner taskOwner = UnifiedAutoStartOwner::Unknown;
    std::wstring ownerCommand;
};

enum class AutoStartApplyStatus : std::uint8_t
{
    Applied,
    ManualEnableRequired,
    BlockedByPolicy,
    PortableRegistrationConflict,
    InstalledRegistrationConflict,
    StateUnavailable,
    Failed,
};

struct AutoStartApplyResult
{
    AutoStartApplyStatus status = AutoStartApplyStatus::Failed;
    AutoStartQueryResult state;
    std::wstring message;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == AutoStartApplyStatus::Applied;
    }
};
}
