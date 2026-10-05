#pragma once

#include <windows.h>

namespace snowdesktop::native_menu_theme
{
namespace detail
{
enum class PreferredAppMode
{
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
};

struct Api
{
    PreferredAppMode(WINAPI* setPreferredAppMode)(PreferredAppMode) = nullptr;
    bool(WINAPI* allowDarkModeForWindow)(HWND, bool) = nullptr;
    void(WINAPI* flushMenuThemes)() = nullptr;
    bool (*isHighContrast)() = nullptr;
};

// Ordinal 135 had a different signature before Windows 10 1903. Unknown
// Windows major versions also retain the standard native-menu fallback.
inline constexpr bool SupportsPreferredAppMode(DWORD major, DWORD build)
{
    return major == 10 && build >= 18362;
}

inline bool IsHighContrast()
{
    HIGHCONTRASTW contrast{ sizeof(contrast) };
    return !SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
        &contrast, 0) || (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

inline const Api& SystemApi()
{
    static const Api api = [] {
        using RtlGetVersionProc = LONG(WINAPI*)(OSVERSIONINFOW*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto getVersion = ntdll ? reinterpret_cast<RtlGetVersionProc>(
            GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (!getVersion || getVersion(&version) != 0 ||
            !SupportsPreferredAppMode(version.dwMajorVersion, version.dwBuildNumber))
            return Api{};

        // Windows exposes native dark-menu support only through these UxTheme
        // ordinals (also used by Microsoft's PowerToys/ZoomIt). Keep the system
        // DLL loaded for the lifetime of the cached function pointers.
        const HMODULE module = LoadLibraryExW(L"uxtheme.dll", nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return Api{};
        return Api{
            reinterpret_cast<PreferredAppMode(WINAPI*)(PreferredAppMode)>(
                GetProcAddress(module, MAKEINTRESOURCEA(135))),
            reinterpret_cast<bool(WINAPI*)(HWND, bool)>(
                GetProcAddress(module, MAKEINTRESOURCEA(133))),
            reinterpret_cast<void(WINAPI*)()>(
                GetProcAddress(module, MAKEINTRESOURCEA(136))),
            IsHighContrast,
        };
    }();
    return api;
}
} // namespace detail

// Shell menus are tracked on the owner's STA. PreferredAppMode is process-wide,
// so restore it (including an outer menu's mode) when this modal session ends.
// Reapplying and flushing on each opening handles Windows/software theme changes
// without retaining a stale menu-theme cache between sessions.
class ScopedTheme
{
public:
    explicit ScopedTheme(bool lightTheme, const detail::Api& api = detail::SystemApi())
        : api_(api)
    {
        if (!api_.setPreferredAppMode || !api_.allowDarkModeForWindow ||
            !api_.flushMenuThemes)
            return;
        const bool highContrast = !api_.isHighContrast || api_.isHighContrast();
        dark_ = !lightTheme && !highContrast;
        const auto mode = highContrast ? detail::PreferredAppMode::Default :
            dark_ ? detail::PreferredAppMode::ForceDark : detail::PreferredAppMode::ForceLight;
        previous_ = api_.setPreferredAppMode(mode);
        active_ = true;
        api_.flushMenuThemes();
    }

    ~ScopedTheme()
    {
        if (active_)
        {
            api_.setPreferredAppMode(previous_);
            api_.flushMenuThemes();
        }
    }

    ScopedTheme(const ScopedTheme&) = delete;
    ScopedTheme& operator=(const ScopedTheme&) = delete;

    void ApplyToWindow(HWND window) const
    {
        if (active_ && window)
            api_.allowDarkModeForWindow(window, dark_);
    }

private:
    detail::Api api_;
    detail::PreferredAppMode previous_ = detail::PreferredAppMode::Default;
    bool active_ = false;
    bool dark_ = false;
};
} // namespace snowdesktop::native_menu_theme
