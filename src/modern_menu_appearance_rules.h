#pragma once

#include "modern_menu.h"

namespace snowdesktop::modern_menu::appearance_rules
{

inline constexpr unsigned long kWindows11MinimumBuild = 22000;

inline constexpr bool IsWindows11OrGreater(
    unsigned long majorVersion, unsigned long buildNumber)
{
    return majorVersion > 10 ||
        (majorVersion == 10 && buildNumber >= kWindows11MinimumBuild);
}

inline constexpr Appearance ResolveForWindows(
    Appearance requested, bool lightTheme,
    unsigned long majorVersion, unsigned long buildNumber)
{
    if (requested != Appearance::FollowSystem)
        return requested;
    if (IsWindows11OrGreater(majorVersion, buildNumber))
        return Appearance::FollowSystem;
    return lightTheme ? Appearance::OpaqueLight : Appearance::OpaqueDark;
}

inline Appearance ResolveForCurrentWindows(
    Appearance requested, bool lightTheme)
{
    struct WindowsVersion
    {
        DWORD major = 0;
        DWORD build = 0;
    };
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
    return ResolveForWindows(
        requested, lightTheme, current.major, current.build);
}

inline constexpr bool IsLightTheme(
    Appearance appearance, bool followSystemLightTheme)
{
    if (appearance == Appearance::SystemLightBlur ||
        appearance == Appearance::OpaqueLight)
    {
        return true;
    }
    if (appearance == Appearance::SystemDarkBlur ||
        appearance == Appearance::OpaqueDark)
    {
        return false;
    }
    return followSystemLightTheme;
}

inline constexpr bool UsesSystemBlur(Appearance appearance)
{
    return appearance == Appearance::FollowSystem ||
        appearance == Appearance::SystemLightBlur ||
        appearance == Appearance::SystemDarkBlur;
}

} // namespace snowdesktop::modern_menu::appearance_rules
