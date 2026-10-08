#pragma once

#include "common/constants.h"
#include "settings/general_settings.h"

namespace snowdesktop
{

// The persistent control window owns this shortcut, including while the
// software desktop is hidden or its Explorer-backed windows are recreated.
class SoftwareDesktopHotkeyRegistration final
{
public:
    SoftwareDesktopHotkeyRegistration() = default;
    SoftwareDesktopHotkeyRegistration(const SoftwareDesktopHotkeyRegistration&) = delete;
    SoftwareDesktopHotkeyRegistration& operator=(const SoftwareDesktopHotkeyRegistration&) = delete;
    ~SoftwareDesktopHotkeyRegistration() { Unregister(); }

    void Apply(HWND owner, const GeneralSettings& settings)
    {
        Unregister();
        if (!settings.softwareDesktopHotkeyEnabled || !IsWindow(owner) ||
            settings.softwareDesktopHotkeyVirtualKey == 0 ||
            settings.softwareDesktopHotkeyVirtualKey > 0xff)
            return;
        const UINT modifiers = settings.softwareDesktopHotkeyModifiers &
            (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
        if (RegisterHotKey(owner, kSoftwareDesktopHotkeyId,
                modifiers | MOD_NOREPEAT, settings.softwareDesktopHotkeyVirtualKey))
        {
            owner_ = owner;
            modifiers_ = modifiers;
            virtualKey_ = settings.softwareDesktopHotkeyVirtualKey;
        }
    }

    void Unregister() noexcept
    {
        if (owner_ && IsWindow(owner_))
            UnregisterHotKey(owner_, kSoftwareDesktopHotkeyId);
        owner_ = nullptr;
        modifiers_ = virtualKey_ = 0;
    }

    bool Matches(UINT modifiers, UINT virtualKey) const noexcept
    {
        return IsRegistered() && virtualKey == virtualKey_ &&
            (modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) == modifiers_;
    }

    bool IsRegistered() const noexcept { return owner_ && IsWindow(owner_); }

private:
    HWND owner_ = nullptr;
    UINT modifiers_ = 0;
    UINT virtualKey_ = 0;
};

// SnowDesktop has one application/control window per process. Keep the
// registration independent of the replaceable desktop presentation windows.
inline SoftwareDesktopHotkeyRegistration& SoftwareDesktopHotkey()
{
    static SoftwareDesktopHotkeyRegistration registration;
    return registration;
}

} // namespace snowdesktop
