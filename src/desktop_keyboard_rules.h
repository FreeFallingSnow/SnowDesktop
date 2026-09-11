#pragma once

namespace snowdesktop::desktop_keyboard_rules
{
// Normalize only exact file-command aliases; AltGr and Win chords must not
// accidentally run clipboard or deletion commands.
constexpr unsigned int NormalizeFileCommandKey(
    unsigned int key, bool control, bool shift, bool alt, bool win)
{
    if (alt || win) return key;
    if (control && !shift && key == 'D') return 0x2E; // VK_DELETE
    if (control && !shift && key == 'R') return 0x74; // VK_F5
    if (control && !shift && key == 0x2D) return 'C'; // VK_INSERT
    if (!control && shift && key == 0x2D) return 'V';
    return key;
}

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
