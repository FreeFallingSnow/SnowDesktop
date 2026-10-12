#include "desktop_passthrough_indicator.h"

#include "ui/render/native_tooltip_preferences.h"
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
    UINT dismissMessage, std::wstring hint, IDCompositionDesktopDevice* composition, IDWriteFactory* text)
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

    owner_ = owner;
    dismissMessage_ = dismissMessage;
    hint_ = std::move(hint);
    composition_ = composition;
    text_ = text;

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
    }
    // Create the complete escape surface before making any part visible.
    for (HWND edge : edges_)
        ShowWindow(edge, SW_SHOWNOACTIVATE);
    return true;
}

void DesktopPassthroughIndicator::Hide()
{
    tooltip_.Close();
    composition_.Reset(); text_.Reset();
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
    case WM_MOUSEMOVE:
        if (self)
        {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(window, &point);
            self->tooltip_.Configure(window, self->composition_.Get(), self->text_.Get(), NativeTooltipAppearance());
            self->tooltip_.SetTarget("passthrough-edge", self->hint_, {point.x, point.y, point.x + 1, point.y + 1});
        }
        return 0;
    case WM_MOUSELEAVE:
        if (self) self->tooltip_.Hide();
        return 0;
    case WM_LBUTTONDOWN:
        if (self) self->tooltip_.Hide();
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
