#pragma once

#include "modern_menu.h"

namespace snowdesktop::modern_menu::appearance_rules
{

inline constexpr unsigned long kWindows11MinimumBuild = 22000;

inline constexpr bool IsWin10Style(Appearance appearance)
{
    return appearance == Appearance::Win10Light ||
        appearance == Appearance::Win10Dark ||
        appearance == Appearance::Win10FollowSystem;
}

inline constexpr int PanelPaddingDip(Appearance appearance)
{
    return IsWin10Style(appearance) ? 3 : kSubmenuPanelPaddingDip;
}

inline constexpr int PanelRadiusDip(Appearance appearance)
{
    return IsWin10Style(appearance) ? 4 : 8;
}

inline constexpr int CompanionPanelRadiusDip(Appearance appearance)
{
    return IsWin10Style(appearance) ? PanelRadiusDip(appearance) : 10;
}

inline constexpr unsigned BlurPanelAlpha(bool lightTheme)
{
    return lightTheme ? 52u : 58u;
}

inline constexpr unsigned BlurTintAlpha(bool lightTheme)
{
    return lightTheme ? 0x48u : 0x50u;
}

inline constexpr bool IsWindows11OrGreater(
    unsigned long majorVersion, unsigned long buildNumber)
{
    return majorVersion > 10 ||
        (majorVersion == 10 && buildNumber >= kWindows11MinimumBuild);
}

inline constexpr Appearance DefaultForWindows(
    unsigned long majorVersion, unsigned long buildNumber)
{
    return IsWindows11OrGreater(majorVersion, buildNumber)
        ? Appearance::FollowSystem : Appearance::Win10FollowSystem;
}

inline constexpr Appearance LegacyFollowForWindows(
    unsigned long majorVersion, unsigned long buildNumber)
{
    return IsWindows11OrGreater(majorVersion, buildNumber)
        ? Appearance::FollowSystem : Appearance::OpaqueFollowSystem;
}

struct WindowsVersion
{
    DWORD major = 0;
    DWORD build = 0;
};

inline WindowsVersion CurrentWindowsVersion()
{
    static const WindowsVersion current = [] {
        using RtlGetVersionProc = LONG(WINAPI*)(OSVERSIONINFOW*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rtlGetVersion = ntdll
            ? reinterpret_cast<RtlGetVersionProc>(
                GetProcAddress(ntdll, "RtlGetVersion"))
            : nullptr;
        if (!rtlGetVersion)
            return WindowsVersion{};

        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtlGetVersion(&version) != 0)
            return WindowsVersion{};
        return WindowsVersion{
            version.dwMajorVersion, version.dwBuildNumber };
    }();
    return current;
}

inline Appearance DefaultForCurrentWindows()
{
    const auto version = CurrentWindowsVersion();
    return DefaultForWindows(version.major, version.build);
}

inline Appearance LegacyFollowForCurrentWindows()
{
    const auto version = CurrentWindowsVersion();
    return LegacyFollowForWindows(version.major, version.build);
}

inline constexpr Appearance ResolveForCurrentWindows(
    Appearance requested, bool lightTheme)
{
    if (requested == Appearance::OpaqueFollowSystem)
        return lightTheme ? Appearance::OpaqueLight : Appearance::OpaqueDark;
    if (requested == Appearance::FollowSystem)
        return lightTheme ? Appearance::SystemLightBlur : Appearance::SystemDarkBlur;
    if (requested == Appearance::Win10FollowSystem)
        return lightTheme ? Appearance::Win10Light : Appearance::Win10Dark;
    return requested;
}

inline constexpr bool IsLightTheme(
    Appearance appearance, bool followSystemLightTheme)
{
    if (appearance == Appearance::SystemLightBlur ||
        appearance == Appearance::OpaqueLight ||
        appearance == Appearance::Win10Light)
    {
        return true;
    }
    if (appearance == Appearance::SystemDarkBlur ||
        appearance == Appearance::OpaqueDark ||
        appearance == Appearance::Win10Dark)
    {
        return false;
    }
    return followSystemLightTheme;
}

inline bool IsLightThemeForCurrentWindows(Appearance appearance)
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
            &value, &size) != ERROR_SUCCESS)
        value = 1;
    return IsLightTheme(appearance, value != 0);
}

inline constexpr bool UsesSystemBlur(Appearance appearance)
{
    return appearance == Appearance::FollowSystem ||
        appearance == Appearance::SystemLightBlur ||
        appearance == Appearance::SystemDarkBlur;
}

} // namespace snowdesktop::modern_menu::appearance_rules
