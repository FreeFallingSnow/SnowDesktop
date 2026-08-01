#include "modern_menu.h"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace
{

constexpr wchar_t kPreviewClass[] = L"SnowDesktop.ModernMenuPreview";
constexpr wchar_t kPreviewTitle[] = L"SnowDesktop Modern Menu Preview";
constexpr UINT_PTR kOpenTimer = 1;
UINT gLastCommand = 0;
snowdesktop::modern_menu::Appearance gAppearance =
    snowdesktop::modern_menu::Appearance::SystemLightBlur;

std::vector<snowdesktop::modern_menu::Item> BuildPreviewItems()
{
    using snowdesktop::modern_menu::Item;
    return {
        { 1, L"详细设置", L"\xE713", true },
        { 2, L"新增日程", L"\xE710", true },
        { 3, L"编辑日程", L"\xE70F", false },
        { 4, L"删除日程", L"\xE74D", false },
        { 0, L"", L"", false, false, true },
        { 5, L"今天", L"\xE787", true, true },
        { 6, L"前一天", L"\xE76B", true },
        { 0, L"后一天", L"\xE76C", true, false, false,
            {
                { 7, L"明天", L"\xE893", true },
                { 8, L"下周", L"\xE8D1", true },
            } },
        { 0, L"", L"", false, false, true },
        { 9, L"仅在悬停时显示\t开", L"\xE890", true },
        { 10, L"隐藏桌面时保留\t关", L"\xE9A9", true },
        { 11, L"隐私模式\t开", L"\xE72E", true },
        { 12, L"删除组件", L"\xE74D", true },
    };
}

void OpenPreviewMenu(HWND hwnd)
{
    RECT windowRect{};
    GetWindowRect(hwnd, &windowRect);
    snowdesktop::modern_menu::Options options;
    options.owner = hwnd;
    options.anchor = { windowRect.left + 48, windowRect.top + 72 };
    options.dpi = GetDpiForWindow(hwnd);
    options.lightTheme =
        gAppearance != snowdesktop::modern_menu::Appearance::SystemDarkBlur;
    options.appearance = gAppearance;
    options.iconFontFamily = L"Segoe Fluent Icons";
    const auto items = BuildPreviewItems();
    gLastCommand = snowdesktop::modern_menu::Show(items, options).command;
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK WindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(hwnd, kOpenTimer, 250, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kOpenTimer)
        {
            KillTimer(hwnd, kOpenTimer);
            OpenPreviewMenu(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        OpenPreviewMenu(hwnd);
        return 0;
    case WM_RBUTTONUP:
        gAppearance = gAppearance ==
                snowdesktop::modern_menu::Appearance::SystemLightBlur
            ? snowdesktop::modern_menu::Appearance::SystemDarkBlur
            : snowdesktop::modern_menu::Appearance::SystemLightBlur;
        OpenPreviewMenu(hwnd);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        const COLORREF previewColors[] = {
            RGB(76, 123, 206), RGB(228, 109, 118),
            RGB(83, 174, 137), RGB(238, 191, 83),
        };
        constexpr int tile = 72;
        for (int y = 0; y < client.bottom; y += tile)
        {
            for (int x = 0; x < client.right; x += tile)
            {
                RECT block{ x, y,
                    std::min(x + tile, static_cast<int>(client.right)),
                    std::min(y + tile, static_cast<int>(client.bottom)) };
                HBRUSH brush = CreateSolidBrush(previewColors[
                    ((x / tile) + (y / tile)) % _countof(previewColors)]);
                FillRect(dc, &block, brush);
                DeleteObject(brush);
            }
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        std::wstring displayText =
            L"单击重开菜单，右键切换深浅模糊\n最近命令：";
        displayText += gLastCommand == 0
            ? L"无" : std::to_wstring(gLastCommand);
        DrawTextW(dc, displayText.c_str(), -1, &client,
            DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kPreviewClass;
    if (!RegisterClassExW(&windowClass))
        return 1;

    HWND hwnd = CreateWindowExW(0, kPreviewClass, kPreviewTitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        520, 390, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
