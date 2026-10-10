#include <windows.h>
#include <cstdlib>

namespace
{
LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_APP + 2)
    {
        if (wParam == 0) return DefWindowProcW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        if (wParam == 1) ShowWindow(window, SW_SHOWNOACTIVATE);
        if (wParam == 2) ShowWindow(window, SW_MINIMIZE);
        if (wParam == 3) ShowWindow(window, SW_MAXIMIZE);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    const HWND receiver = reinterpret_cast<HWND>(ULongToHandle(wcstoul(argv[1], nullptr, 10)));
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopMinimizeFixture32";
    type.lpfnWndProc = FixtureProc;
    if (!RegisterClassW(&type)) return 1;
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, type.lpszClassName,
        WS_OVERLAPPEDWINDOW, -20000, -20000, 240, 160, nullptr, nullptr, type.hInstance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    PostMessageW(receiver, WM_APP + 1, reinterpret_cast<WPARAM>(window), 0);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
