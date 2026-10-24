#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace snowdesktop::dock_peek
{
// Optional Windows 10/11 ABI, also used by PowerToys WindowWalker:
// https://github.com/microsoft/PowerToys/blob/115665ec3240a0c94dea3f0bfaaa6c52dcf4d019/src/modules/cmdpal/ext/Microsoft.CmdPal.Ext.WindowWalker/Helpers/NativeMethods.cs
// This is not a documented SDK contract. Missing/failed support leaves the
// ordinary thumbnail and click paths available, without restoring a source HWND.
struct Api
{
    using Activate = HRESULT(WINAPI*)(BOOL, HWND, HWND, UINT, const RECT*);
    Activate activate = nullptr;

    static Api Load() noexcept
    {
        static const Api api = [] {
            using Version = LONG(WINAPI*)(OSVERSIONINFOW*);
            const auto version = reinterpret_cast<Version>(GetProcAddress(
                GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
            OSVERSIONINFOW info{sizeof(info)};
            if (!version || version(&info) < 0 || info.dwMajorVersion != 10 ||
                info.dwBuildNumber < 17763) return Api{};
            HMODULE module = GetModuleHandleW(L"dwmapi.dll");
            if (!module) module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return Api{module ? reinterpret_cast<Activate>(
                GetProcAddress(module, MAKEINTRESOURCEA(113))) : nullptr};
        }();
        return api;
    }
};

struct WindowIdentity
{
    HWND window = nullptr;
    DWORD process = 0;
    DWORD thread = 0;

    static WindowIdentity Capture(HWND window) noexcept
    {
        WindowIdentity identity{window};
        if (window) identity.thread = GetWindowThreadProcessId(window, &identity.process);
        return identity;
    }

    bool IsCurrent() const noexcept
    {
        if (!window || !thread || !process) return false;
        const auto current = Capture(window);
        return current.thread == thread && current.process == process;
    }
};

// Own only the Peek begun by this instance. Never dismiss another tool's Peek
// merely because SnowDesktop starts or a thumbnail panel is shown.
class Session
{
public:
    explicit Session(Api api = Api::Load()) noexcept : api_(api) {}
    ~Session() { Stop(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Available() const noexcept { return api_.activate != nullptr; }
    bool Active() const noexcept { return target_.window != nullptr; }
    HWND Target() const noexcept { return target_.window; }
    bool TargetIsCurrent() const noexcept { return target_.IsCurrent(); }
    bool ForegroundUnchanged() const noexcept { return GetForegroundWindow() == foreground_; }

    bool Begin(HWND target, HWND caller) noexcept
    {
        const auto identity = WindowIdentity::Capture(target);
        if (!Available() || !identity.IsCurrent() || GetAncestor(target, GA_ROOT) != target)
            return false;
        if (target == target_.window && target_.IsCurrent()) return true;
        const HWND foreground = Active() ? foreground_ : GetForegroundWindow();
        if (FAILED(api_.activate(TRUE, target, caller, kTaskbarTrigger, nullptr)))
        {
            // A failed switch must not strand the previous window's Peek.
            Stop();
            return false;
        }
        target_ = identity;
        foreground_ = foreground;
        return true;
    }

    bool Stop() noexcept
    {
        if (!Active()) return true;
        if (FAILED(api_.activate(FALSE, nullptr, nullptr, kTaskbarTrigger, nullptr)))
            return false; // Keep ownership so the caller's cleanup timer can retry.
        target_ = {};
        foreground_ = nullptr;
        return true;
    }

private:
    static constexpr UINT kTaskbarTrigger = 3;
    Api api_;
    WindowIdentity target_;
    HWND foreground_ = nullptr;
};
}
