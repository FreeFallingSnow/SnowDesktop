#pragma once
#include <windows.h>

namespace snowdesktop::taskbar_hook::native
{
inline constexpr wchar_t kAttachedProperty[] = L"SnowDesktop.Taskbar.Native.v11";
// Private host/Explorer handshake. Unlike WM_CONTEXTMENU, these messages do
// not ask Explorer to open the taskbar's own menu or change keyboard focus.
inline constexpr wchar_t kBeginMenuAccess[] = L"SnowDesktop.Taskbar.BeginMenuAccess.v1";
inline constexpr wchar_t kCancelMenuAccess[] = L"SnowDesktop.Taskbar.CancelMenuAccess.v1";

class MenuAccess final
{
public:
    HRESULT Begin(HWND taskbar)
    {
        if (!taskbar || !GetPropW(taskbar, kAttachedProperty)) return S_FALSE;
        window_ = taskbar;
        static volatile LONG sequence = 0;
        token_ = static_cast<DWORD>(InterlockedIncrement(&sequence));
        if (!token_) token_ = static_cast<DWORD>(InterlockedIncrement(&sequence));
        DWORD_PTR accepted = 0;
        SetLastError(ERROR_SUCCESS);
        if (!SendMessageTimeoutW(window_, RegisterWindowMessageW(kBeginMenuAccess),
            GetCurrentProcessId(), static_cast<LPARAM>(token_),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 300, &accepted))
        {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_TIMEOUT);
        }
        return accepted ? S_OK : E_FAIL;
    }
    // The hook now follows the actual menu until it closes. If the provider
    // silently opens nothing, the existing 1500 ms discovery budget expires.
    void HandOff() noexcept { window_ = nullptr; }
    ~MenuAccess()
    {
        if (window_) PostMessageW(window_, RegisterWindowMessageW(kCancelMenuAccess),
            GetCurrentProcessId(), static_cast<LPARAM>(token_));
    }
    MenuAccess() = default;
    MenuAccess(const MenuAccess&) = delete;
    MenuAccess& operator=(const MenuAccess&) = delete;
private:
    HWND window_ = nullptr;
    DWORD token_ = 0;
};
}
