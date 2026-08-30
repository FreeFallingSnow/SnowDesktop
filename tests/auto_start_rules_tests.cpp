#include "auto_start_rules.h"
#include "deployment_context.h"

#include <cstdlib>
#include <iostream>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
}

int main()
{
    using snowdesktop::BuildPortableAutoStartApprovalPayload;
    using snowdesktop::AutoStartOwnershipNotice;
    using snowdesktop::ClassifyAutoStartOwnershipNotice;
    using snowdesktop::DecodePortableAutoStartApprovalState;
    using snowdesktop::HasActivePortableAutoStart;
    using snowdesktop::IsPortableAutoStartApprovalActive;
    using snowdesktop::LegacyAutoStartState;
    using snowdesktop::PortableAutoStartApprovalState;
    using snowdesktop::PortableAutoStartRegistrationOwner;
    using snowdesktop::SelectAutoStartMigration;
    using snowdesktop::UnifiedAutoStartOwner;
    using snowdesktop::UnifiedAutoStartTaskState;
    using snowdesktop::deployment::PackagedAutoStartState;
    using snowdesktop::deployment::ShouldFinalizePackagedAutoStartUserEnable;

    Check(ShouldFinalizePackagedAutoStartUserEnable(
              PackagedAutoStartState::DisabledByUser,
              PackagedAutoStartState::Disabled),
        "a user-unblocked packaged task is finalized through the public API");
    Check(!ShouldFinalizePackagedAutoStartUserEnable(
               PackagedAutoStartState::Unavailable,
               PackagedAutoStartState::Disabled) &&
            !ShouldFinalizePackagedAutoStartUserEnable(
               PackagedAutoStartState::Enabled,
               PackagedAutoStartState::Disabled) &&
            !ShouldFinalizePackagedAutoStartUserEnable(
               PackagedAutoStartState::DisabledByUser,
               PackagedAutoStartState::DisabledByUser),
        "initial, app-disabled, and still-user-disabled tasks are not auto-enabled");

    Check(DecodePortableAutoStartApprovalState(0x00) ==
            PortableAutoStartApprovalState::Disabled,
        "Task Manager's zeroed marker is treated as disabled");
    Check(DecodePortableAutoStartApprovalState(0x01) ==
            PortableAutoStartApprovalState::Disabled,
        "Windows Settings marker 0x01 is treated as disabled");
    Check(DecodePortableAutoStartApprovalState(0x03) ==
            PortableAutoStartApprovalState::Disabled,
        "legacy marker 0x03 remains disabled");
    Check(DecodePortableAutoStartApprovalState(0x02) ==
            PortableAutoStartApprovalState::Enabled,
        "marker 0x02 remains enabled");
    Check(DecodePortableAutoStartApprovalState(0x7f) ==
            PortableAutoStartApprovalState::Error,
        "unknown markers remain unavailable instead of being guessed");

    constexpr auto enabledPayload =
        BuildPortableAutoStartApprovalPayload(true);
    Check(enabledPayload[0] == 0x02 && enabledPayload[4] == 0x00 &&
            enabledPayload[11] == 0x00,
        "app-side enablement writes the classic enabled approval payload");
    constexpr auto disabledPayload =
        BuildPortableAutoStartApprovalPayload(
            false, 0x0807060504030201ULL);
    Check(disabledPayload[0] == 0x03 &&
            disabledPayload[4] == 0x01 &&
            disabledPayload[5] == 0x02 &&
            disabledPayload[11] == 0x08,
        "app-side disablement writes the classic marker and FILETIME payload");

    Check(IsPortableAutoStartApprovalActive(
              PortableAutoStartApprovalState::Missing),
        "a Run registration without an approval override is active");
    Check(IsPortableAutoStartApprovalActive(
              PortableAutoStartApprovalState::Enabled),
        "an enabled approval state is active");
    Check(!IsPortableAutoStartApprovalActive(
               PortableAutoStartApprovalState::Disabled),
        "a disabled approval state is inactive");
    Check(!IsPortableAutoStartApprovalActive(
               PortableAutoStartApprovalState::Error),
        "an unavailable approval state is not reported as active");

    Check(HasActivePortableAutoStart(
              PortableAutoStartRegistrationOwner::OtherExecutable,
              PortableAutoStartApprovalState::Enabled),
        "an enabled registration owned by another executable is active");
    Check(!HasActivePortableAutoStart(
               PortableAutoStartRegistrationOwner::OtherExecutable,
               PortableAutoStartApprovalState::Disabled),
        "a disabled registration owned by another executable is not active");
    Check(!HasActivePortableAutoStart(
               PortableAutoStartRegistrationOwner::Missing,
               PortableAutoStartApprovalState::Enabled),
        "an approval entry without a Run registration is not active");
    Check(!HasActivePortableAutoStart(
               PortableAutoStartRegistrationOwner::Error,
               PortableAutoStartApprovalState::Enabled),
        "an unreadable Run registration is not reported as active");

    Check(ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Portable) ==
            AutoStartOwnershipNotice::OtherVersion &&
            ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Steam) ==
            AutoStartOwnershipNotice::OtherVersion,
        "Steam and portable startup targets owned by another deployment show the path-aware notice");
    Check(ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Packaged) ==
            AutoStartOwnershipNotice::InstalledVersion,
        "an active packaged startup target shows the installed-version notice");
    Check(ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, true,
              UnifiedAutoStartOwner::Steam) ==
            AutoStartOwnershipNotice::None &&
            ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Disabled, false,
              UnifiedAutoStartOwner::Portable) ==
            AutoStartOwnershipNotice::None &&
            ClassifyAutoStartOwnershipNotice(false,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Packaged) ==
            AutoStartOwnershipNotice::None &&
            ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Unknown) ==
            AutoStartOwnershipNotice::None,
        "current, disabled, unreadable, and unknown startup targets do not show ownership notices");

    constexpr auto noLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Portable,
        LegacyAutoStartState::Missing,
        LegacyAutoStartState::Disabled);
    Check(noLegacy.canMigrate && !noLegacy.enableUnifiedTask &&
            noLegacy.owner == UnifiedAutoStartOwner::Portable,
        "a clean machine creates a disabled task owned by the current deployment");
    constexpr auto portableLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Packaged,
        LegacyAutoStartState::Enabled,
        LegacyAutoStartState::Disabled);
    Check(portableLegacy.canMigrate &&
            portableLegacy.enableUnifiedTask &&
            portableLegacy.owner == UnifiedAutoStartOwner::Portable,
        "a sole active portable entry keeps portable ownership during migration");
    constexpr auto packagedLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Portable,
        LegacyAutoStartState::Disabled,
        LegacyAutoStartState::Enabled);
    Check(packagedLegacy.canMigrate &&
            packagedLegacy.enableUnifiedTask &&
            packagedLegacy.owner == UnifiedAutoStartOwner::Packaged,
        "a sole active packaged entry keeps packaged ownership during migration");
    constexpr auto duplicateLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Portable,
        LegacyAutoStartState::Enabled,
        LegacyAutoStartState::Enabled);
    Check(duplicateLegacy.canMigrate &&
            duplicateLegacy.enableUnifiedTask &&
            duplicateLegacy.owner == UnifiedAutoStartOwner::Portable,
        "the explicitly running deployment wins when both legacy entries are active");
    constexpr auto unknownLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Packaged,
        LegacyAutoStartState::Unavailable,
        LegacyAutoStartState::Enabled);
    Check(!unknownLegacy.canMigrate,
        "migration does not change startup when either legacy source is unreadable");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "Auto-start rule checks passed\n";
    return EXIT_SUCCESS;
}
