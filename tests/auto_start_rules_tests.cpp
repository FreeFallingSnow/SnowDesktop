#include "auto_start_rules.h"

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
    using snowdesktop::DecodePortableAutoStartApprovalState;
    using snowdesktop::HasActivePortableAutoStart;
    using snowdesktop::IsPortableAutoStartApprovalActive;
    using snowdesktop::PortableAutoStartApprovalState;
    using snowdesktop::PortableAutoStartRegistrationOwner;

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

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "Auto-start rule checks passed\n";
    return EXIT_SUCCESS;
}
