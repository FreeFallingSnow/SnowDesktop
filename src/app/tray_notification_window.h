#pragma once

#include <windows.h>
#include <string>

namespace snowdesktop::tray_notification
{
inline constexpr wchar_t PortableApplicationId[] = L"SnowDesktop";

// Preserve the package's identity when available; portable notifications use
// one stable identity across restarts and upgrades.
std::wstring ApplicationId();

// applicationsRoot is HKCU\Software\Classes\AppUserModelId in production.
// Only display metadata is written, never notification permission settings.
bool RegisterApplication(HKEY applicationsRoot, const std::wstring& applicationId,
    const std::wstring& iconPath);

// Give Shell an explicit notification identity before adding the tray icon.
// A caption alone still allows NotifyIconGeneratedAumid identifiers to leak
// into Windows notification menus. Keep message routing on the legacy window.
// The caller owns the returned hidden window and must destroy it.
HWND CreateOwnerWindow(HWND callbackWindow);
}
