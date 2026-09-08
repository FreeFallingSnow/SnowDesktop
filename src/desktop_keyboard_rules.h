#pragma once

namespace snowdesktop::desktop_keyboard_rules
{
constexpr bool IsForegroundFocusReady(
    bool foregroundFocusKnown,
    bool foregroundFocusMatchesTarget,
    bool requireForegroundTarget,
    bool foregroundMatchesTarget)
{
    return foregroundFocusKnown &&
        foregroundFocusMatchesTarget &&
        (!requireForegroundTarget || foregroundMatchesTarget);
}

constexpr bool ShouldAttachForegroundInputQueue(
    unsigned long currentThread,
    unsigned long foregroundThread,
    bool foregroundFocusReady)
{
    return !foregroundFocusReady &&
        currentThread != 0 && foregroundThread != 0 &&
        currentThread != foregroundThread;
}

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
