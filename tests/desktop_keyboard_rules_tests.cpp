#include "desktop_keyboard_rules.h"

#include <iostream>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}

int main()
{
    using snowdesktop::desktop_keyboard_rules::AltF4Action;
    using snowdesktop::desktop_keyboard_rules::ResolveAltF4Action;

    Check(ResolveAltF4Action(true, true, true, false) ==
            AltF4Action::RequestWindowsShutdownDialog,
        "Alt+F4 on the desktop requests the Windows shutdown dialog");
    Check(ResolveAltF4Action(true, true, true, true) ==
            AltF4Action::ConsumeRepeat,
        "repeated desktop Alt+F4 input is consumed without another request");
    Check(ResolveAltF4Action(false, true, true, false) ==
            AltF4Action::PassThrough,
        "Alt+F4 outside the desktop keeps its normal window behavior");
    Check(ResolveAltF4Action(true, false, true, false) ==
            AltF4Action::PassThrough,
        "other Alt shortcuts continue through normal dispatch");
    Check(ResolveAltF4Action(true, true, false, false) ==
            AltF4Action::PassThrough,
        "F4 without Alt continues through normal dispatch");

    if (failures == 0)
        std::cout << "All desktop keyboard rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
