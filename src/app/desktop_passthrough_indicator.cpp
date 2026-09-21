#include "desktop_passthrough_indicator.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

namespace snowdesktop
{
namespace
{
constexpr wchar_t kEdgeClass[] = L"SnowDesktopPassthroughEdge";

struct MonitorEdgeBounds
{
    std::vector<RECT> edges;
    bool complete = true;
};

BOOL CALLBACK CollectMonitorEdges(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto& bounds = *reinterpret_cast<MonitorEdgeBounds*>(data);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
    {
        bounds.complete = false;
        return FALSE;
    }
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        dpiX = 96;
    const auto edges = desktop_passthrough_rules::EdgeBounds(info.rcMonitor, dpiX);
    bounds.edges.insert(bounds.edges.end(), edges.begin(), edges.end());
    return TRUE;
}
}

bool DesktopPassthroughIndicator::Show(HINSTANCE instance, HWND owner,
    UINT dismissMessage, std::wstring hint)
{
    Hide();
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_HIGHLIGHT);
    windowClass.lpszClassName = kEdgeClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    MonitorEdgeBounds bounds;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitorEdges,
            reinterpret_cast<LPARAM>(&bounds)) || !bounds.complete || bounds.edges.empty())
        return false;

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls))
        return false;
    owner_ = owner;
    dismissMessage_ = dismissMessage;
    hint_ = std::move(hint);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        owner, nullptr, instance, nullptr);
    if (!tooltip_)
    {
        Hide();
        return false;
    }
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 420);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 200);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

    for (const RECT& rect : bounds.edges)
    {
        HWND edge = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kEdgeClass, hint_.c_str(), WS_POPUP,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            owner, nullptr, instance, this);
        if (!edge)
        {
            Hide();
            return false;
        }
        edges_.push_back(edge);
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = edge;
        tool.uId = 1;
        tool.lpszText = hint_.data();
        GetClientRect(edge, &tool.rect);
        if (!SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
        {
            Hide();
            return false;
        }
    }
    // Create the complete escape surface before making any part visible.
    for (HWND edge : edges_)
        ShowWindow(edge, SW_SHOWNOACTIVATE);
    return true;
}

void DesktopPassthroughIndicator::Hide()
{
    // Remove tooltip subclasses before destroying the tools they refer to.
    if (tooltip_)
        DestroyWindow(tooltip_);
    tooltip_ = nullptr;
    for (HWND edge : edges_)
        DestroyWindow(edge);
    edges_.clear();
    hint_.clear();
    owner_ = nullptr;
    dismissMessage_ = 0;
}

bool DesktopPassthroughIndicator::OwnsWindow(HWND window) const
{
    return std::find(edges_.begin(), edges_.end(), window) != edges_.end();
}

LRESULT CALLBACK DesktopPassthroughIndicator::WindowProc(HWND window,
    UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<DesktopPassthroughIndicator*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<DesktopPassthroughIndicator*>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, GetSysColorBrush(COLOR_HIGHLIGHT));
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SYSCOLORCHANGE:
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == window)
        {
            ReleaseCapture();
            RECT client{};
            GetClientRect(window, &client);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (self && PtInRect(&client, point))
                PostMessageW(self->owner_, self->dismissMessage_,
                    reinterpret_cast<WPARAM>(window), 0);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}
