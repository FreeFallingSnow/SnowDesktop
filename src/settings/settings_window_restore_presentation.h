#pragma once

#include <windows.h>

namespace snowdesktop::settings_restore_presentation
{
// Private HWND metadata shared by the settings child and the Dock host. A
// usable restored rectangle does not mean the XAML compositor has a frame yet.
inline constexpr wchar_t PendingProperty[] =
    L"SnowDesktop.Settings.RestoreFrame.Pending.v1";

inline bool IsPending(HWND window) noexcept
{
    return GetPropW(window, PendingProperty) != nullptr;
}

class PendingFrame final
{
public:
    explicit PendingFrame(HWND window) noexcept : window_(window)
    {
        SetPropW(window_, PendingProperty, reinterpret_cast<HANDLE>(this));
    }
    ~PendingFrame() { Complete(); }
    PendingFrame(const PendingFrame&) = delete;
    PendingFrame& operator=(const PendingFrame&) = delete;

    bool IsCurrent() const noexcept
    {
        return window_ && GetPropW(window_, PendingProperty) ==
            reinterpret_cast<HANDLE>(const_cast<PendingFrame*>(this));
    }
    void Complete() noexcept
    {
        // A late frame/commit from an earlier minimize must not acknowledge a
        // newer restore, or a different Island attached to the same HWND.
        if (IsCurrent()) RemovePropW(window_, PendingProperty);
        window_ = nullptr;
    }

private:
    HWND window_ = nullptr;
};
}
