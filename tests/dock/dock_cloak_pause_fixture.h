#pragma once

#include <windows.h>
#include <string>

namespace dock_cloak_fixture
{
inline constexpr UINT PauseMessage = WM_APP + 7;

inline std::wstring EventName(DWORD process, DWORD ticket, const wchar_t* suffix)
{
    return L"Local\\SnowDesktopCloakPause-" + std::to_wstring(process) + L"-" +
        std::to_wstring(ticket) + suffix;
}

// Shared only by the x64/x86 child fixtures. The parent owns all assertions and
// controls this bounded pause using events, without touching a user's window.
inline LRESULT Pause(WPARAM process, LPARAM ticket)
{
    const HANDLE entered = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        EventName(static_cast<DWORD>(process), static_cast<DWORD>(ticket), L"-entered").c_str());
    const HANDLE resume = OpenEventW(SYNCHRONIZE, FALSE,
        EventName(static_cast<DWORD>(process), static_cast<DWORD>(ticket), L"-resume").c_str());
    if (entered && resume)
    {
        SetEvent(entered);
        WaitForSingleObject(resume, 5000);
    }
    if (entered) CloseHandle(entered);
    if (resume) CloseHandle(resume);
    return 0;
}
}
