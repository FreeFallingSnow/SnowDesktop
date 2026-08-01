#include "modern_menu.h"
#include "resource.h"

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <vector>

namespace
{

constexpr wchar_t kPreviewClass[] = L"SnowDesktop.ModernMenuPreview";
constexpr wchar_t kPreviewTitle[] = L"SnowDesktop Modern Menu Preview";
constexpr UINT_PTR kOpenTimer = 1;
UINT gLastCommand = 0;
HANDLE gFluentFontHandle = nullptr;
snowdesktop::modern_menu::Appearance gAppearance =
    snowdesktop::modern_menu::Appearance::SystemDarkBlur;
UINT gPreviewDpi = USER_DEFAULT_SCREEN_DPI;

std::vector<snowdesktop::modern_menu::Item> BuildPreviewItems()
{
    using snowdesktop::modern_menu::Item;
    std::vector<Item> items{
        { 1, L"剪切", L"\uF33A", true },
        { 2, L"复制", L"\uF32B", true },
        { 15, L"新建", L"\uF10C", true },
        { 3, L"重命名", L"\U000F0A39", true },
        { 4, L"编辑", L"\uF3DD", true },
        { 5, L"删除", L"\uF34C", false },
        { 0, L"", L"", false, false, true },
        { 6, L"详细设置", L"\uF6A9", true },
        { 7, L"今天", L"\uF21D", true, true },
        { 8, L"前一天", L"\uF15B", true },
        { 0, L"后一天", L"\uF181", true, false, false,
            {
                { 9, L"明天", L"\uF21D", true },
                { 10, L"下周", L"\uF181", true },
            } },
        { 0, L"", L"", false, false, true },
        { 11, L"仅在悬停时显示\t开", L"\uE5F2", true },
        { 12, L"隐藏桌面时保留\t关", L"\uE5F5", true },
        { 13, L"隐私模式\t开", L"\uE78F", true },
        { 14, L"删除组件", L"\uF34C", true },
    };
    items[0].quickAction = true;
    items[0].quickIcon = snowdesktop::MenuQuickIcon::Cut;
    items[1].quickAction = true;
    items[1].quickIcon = snowdesktop::MenuQuickIcon::Copy;
    items[2].quickAction = true;
    items[2].quickIcon = snowdesktop::MenuQuickIcon::NewItem;
    items[3].quickAction = true;
    items[3].quickIcon = snowdesktop::MenuQuickIcon::Rename;
    items[4].quickAction = true;
    items[4].quickIcon = snowdesktop::MenuQuickIcon::Edit;
    items[5].quickAction = true;
    items[5].quickIcon = snowdesktop::MenuQuickIcon::Delete;
    return items;
}

void OpenPreviewMenu(HWND hwnd)
{
    RECT windowRect{};
    GetWindowRect(hwnd, &windowRect);
    snowdesktop::modern_menu::Options options;
    options.owner = hwnd;
    options.anchor = { windowRect.left + 48, windowRect.top + 72 };
    // Keep the default preview at 96 DPI so low-resolution rasterization can
    // be inspected even when the development monitor uses display scaling.
    options.dpi = gPreviewDpi;
    options.lightTheme =
        gAppearance != snowdesktop::modern_menu::Appearance::SystemDarkBlur;
    options.appearance = gAppearance;
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
    case WM_MBUTTONUP:
    {
        constexpr UINT previewDpis[] = { 96, 120, 144, 192 };
        const auto current = std::find(std::begin(previewDpis),
            std::end(previewDpis), gPreviewDpi);
        const size_t next = current == std::end(previewDpis)
            ? 0
            : (static_cast<size_t>(current - std::begin(previewDpis)) + 1) %
                std::size(previewDpis);
        gPreviewDpi = previewDpis[next];
        OpenPreviewMenu(hwnd);
        return 0;
    }
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
            L"单击重开，右键切换深浅，中键切换 DPI\n当前 DPI：";
        displayText += std::to_wstring(gPreviewDpi);
        displayText += L"  最近命令：";
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
    HRSRC resource = FindResourceW(instance,
        MAKEINTRESOURCEW(IDR_FLUENT_REGULAR_FONT), RT_RCDATA);
    HGLOBAL resourceHandle = resource
        ? LoadResource(instance, resource) : nullptr;
    void* fontData = resourceHandle ? LockResource(resourceHandle) : nullptr;
    const DWORD fontSize = resource
        ? SizeofResource(instance, resource) : 0;
    if (fontData && fontSize > 0)
    {
        DWORD fontCount = 0;
        gFluentFontHandle = AddFontMemResourceEx(
            fontData, fontSize, nullptr, &fontCount);
    }
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
    if (gFluentFontHandle)
        RemoveFontMemResourceEx(gFluentFontHandle);
    return static_cast<int>(message.wParam);
}
