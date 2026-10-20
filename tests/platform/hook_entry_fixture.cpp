#include <windows.h>
#ifndef HOOK_ENTRY_FIXTURE_MARKER
#define HOOK_ENTRY_FIXTURE_MARKER 1
#endif
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopTaskbarHookProc(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && lp)
    {
        const auto message = reinterpret_cast<const CWPSTRUCT*>(lp);
        if (message->message == WM_APP + 91)
            SetPropW(message->hwnd, L"SnowDesktop.Entry.Fixture.Marker", ULongToHandle(HOOK_ENTRY_FIXTURE_MARKER));
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopDockSourceCloakHook(int code, WPARAM wp, LPARAM lp)
{
    return SnowDesktopTaskbarHookProc(code, wp, lp);
}
