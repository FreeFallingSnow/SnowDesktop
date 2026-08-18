#pragma once

namespace snowdesktop::desktop_keyboard_rules
{
enum class AltF4Action
{
    PassThrough,
    RequestWindowsShutdownDialog,
    ConsumeRepeat,
};

constexpr AltF4Action ResolveAltF4Action(
    bool desktopSurfaceFocused,
    bool isF4,
    bool altDown,
    bool repeated)
{
    if (!desktopSurfaceFocused || !isF4 || !altDown)
        return AltF4Action::PassThrough;
    return repeated
        ? AltF4Action::ConsumeRepeat
        : AltF4Action::RequestWindowsShutdownDialog;
}
}
