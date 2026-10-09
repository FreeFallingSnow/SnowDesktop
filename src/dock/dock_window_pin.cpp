#include "dock_window_pin.h"

#include <dwmapi.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t kManagerClass[] = L"SnowDesktopDockWindowPinManager";
constexpr wchar_t kBorderClass[] = L"SnowDesktopDockWindowPinBorder";
constexpr wchar_t kPinProperty[] = L"SnowDesktop.DockWindowPin";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshIntervalMs = 50;
constexpr COLORREF kBorderColor = RGB(0, 120, 215);
constexpr UINT kPositionFlags =
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

bool ChangeTopmost(HWND window, bool pinned)
{
    return SetWindowPos(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, kPositionFlags) != FALSE &&
        DockWindowPin::IsPinned(window) == pinned;
}
}

struct DockWindowPin::State
{
    struct Entry
    {
        HWND target = nullptr;
        HWND border = nullptr;
        DWORD process = 0;
        DWORD thread = 0;
        RECT bounds{};
        int thickness = 0;
        int radius = -1;
        bool restoreTopmost = false;

        bool OwnsTarget() const
        {
            DWORD currentProcess = 0;
            const DWORD currentThread = GetWindowThreadProcessId(target, &currentProcess);
            return border && currentThread == thread && currentProcess == process &&
                GetPropW(target, kPinProperty) == border;
        }

        ~Entry()
        {
            if (OwnsTarget())
            {
                if (restoreTopmost && DockWindowPin::IsPinned(target))
                    ChangeTopmost(target, false);
                RemovePropW(target, kPinProperty);
            }
            if (border) DestroyWindow(border);
        }

        void UpdateBorder()
        {
            DWORD cloaked = 0;
            DwmGetWindowAttribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            RECT frame{};
            if (!IsWindowVisible(target) || IsIconic(target) || cloaked ||
                (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS,
                    &frame, sizeof(frame))) && !GetWindowRect(target, &frame)) ||
                IsRectEmpty(&frame))
            {
                ShowWindow(border, SW_HIDE);
                return;
            }

            const UINT dpi = std::max<UINT>(96, GetDpiForWindow(target));
            const int nextThickness = std::max(1, MulDiv(3, static_cast<int>(dpi), 96));
            const bool maximized = IsZoomed(target) != FALSE;
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DEFAULT;
            const bool hasCornerPreference = SUCCEEDED(DwmGetWindowAttribute(target,
                DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));
            const bool round = !maximized && hasCornerPreference &&
                (corner == DWMWCP_ROUND || corner == DWMWCP_ROUNDSMALL ||
                    (corner == DWMWCP_DEFAULT &&
                        (GetWindowLongPtrW(target, GWL_STYLE) & WS_THICKFRAME)));
            const int nextRadius = round
                ? MulDiv(corner == DWMWCP_ROUNDSMALL ? 4 : 8, static_cast<int>(dpi), 96)
                : 0;
            // Maximized windows meet the work-area edge, so place the ring
            // inside their frame instead of clipping it off-screen.
            if (!maximized) InflateRect(&frame, nextThickness, nextThickness);
            const int width = frame.right - frame.left;
            const int height = frame.bottom - frame.top;
            if (width <= nextThickness * 2 || height <= nextThickness * 2)
            {
                ShowWindow(border, SW_HIDE);
                return;
            }

            const bool shapeChanged = width != bounds.right - bounds.left ||
                height != bounds.bottom - bounds.top || thickness != nextThickness ||
                radius != nextRadius;
            if (shapeChanged)
            {
                const int outerCorner = nextRadius ? (nextRadius + nextThickness) * 2 : 0;
                HRGN outer = nextRadius
                    ? CreateRoundRectRgn(0, 0, width + 1, height + 1, outerCorner, outerCorner)
                    : CreateRectRgn(0, 0, width, height);
                HRGN inner = nextRadius
                    ? CreateRoundRectRgn(nextThickness, nextThickness,
                        width - nextThickness + 1, height - nextThickness + 1,
                        nextRadius * 2, nextRadius * 2)
                    : CreateRectRgn(nextThickness, nextThickness,
                        width - nextThickness, height - nextThickness);
                if (!outer || !inner || CombineRgn(outer, outer, inner, RGN_DIFF) == ERROR)
                {
                    if (outer) DeleteObject(outer);
                    if (inner) DeleteObject(inner);
                    ShowWindow(border, SW_HIDE);
                    return;
                }
                DeleteObject(inner);
                if (!SetWindowRgn(border, outer, FALSE))
                {
                    DeleteObject(outer);
                    ShowWindow(border, SW_HIDE);
                    return;
                }
            }

            // Keep the ring immediately above its window, below other topmost
            // windows. It never raises the application or takes its focus.
            const HWND previous = GetWindow(target, GW_HWNDPREV);
            const bool move = !EqualRect(&bounds, &frame);
            const bool show = !IsWindowVisible(border);
            if (move || shapeChanged || show || previous != border)
            {
                UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
                if (previous == border) flags |= SWP_NOZORDER;
                if (!SetWindowPos(border, previous ? previous : HWND_TOPMOST,
                        frame.left, frame.top, width, height, flags))
                {
                    ShowWindow(border, SW_HIDE);
                    return;
                }
                bounds = frame;
                thickness = nextThickness;
                radius = nextRadius;
                if (shapeChanged || show) InvalidateRect(border, nullptr, FALSE);
            }
        }
    };

    HWND manager = nullptr;
    std::vector<std::unique_ptr<Entry>> entries;

    ~State()
    {
        entries.clear();
        if (manager) DestroyWindow(manager);
    }

    static LRESULT CALLBACK BorderProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            if (dc)
            {
                RECT client{};
                GetClientRect(window, &client);
                HBRUSH brush = CreateSolidBrush(kBorderColor);
                FillRect(dc, &client, brush);
                DeleteObject(brush);
            }
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    static LRESULT CALLBACK ManagerProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state && message == WM_TIMER && wp == kRefreshTimer)
        {
            state->Refresh();
            return 0;
        }
        if (message == WM_NCDESTROY) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wp, lp);
    }

    bool Initialize()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        for (const auto& registration : {
                std::pair{kManagerClass, ManagerProc}, std::pair{kBorderClass, BorderProc}})
        {
            WNDCLASSEXW cls{sizeof(cls)};
            cls.hInstance = instance;
            cls.lpszClassName = registration.first;
            cls.lpfnWndProc = registration.second;
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
        }
        manager = CreateWindowExW(0, kManagerClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, instance, this);
        return manager != nullptr;
    }

    void Refresh()
    {
        std::erase_if(entries, [](const auto& entry) {
            return !entry->OwnsTarget() || !DockWindowPin::IsPinned(entry->target);
        });
        for (const auto& entry : entries) entry->UpdateBorder();
        if (entries.empty()) KillTimer(manager, kRefreshTimer);
    }
};

DockWindowPin::DockWindowPin() = default;
DockWindowPin::~DockWindowPin() = default;

bool DockWindowPin::IsPinned(HWND window)
{
    return IsWindow(window) && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

void DockWindowPin::Refresh()
{
    if (state_) state_->Refresh();
}

void DockWindowPin::Clear()
{
    state_.reset();
}

bool DockWindowPin::Toggle(HWND window)
{
    if (!IsWindow(window) || GetAncestor(window, GA_ROOT) != window) return false;
    Refresh();
    if (IsPinned(window))
    {
        if (!ChangeTopmost(window, false)) return false;
        Refresh();
        return true;
    }
    if (!state_)
    {
        auto state = std::make_unique<State>();
        if (!state->Initialize()) return false;
        state_ = std::move(state);
    }

    auto entry = std::make_unique<State::Entry>();
    entry->target = window;
    entry->thread = GetWindowThreadProcessId(window, &entry->process);
    entry->border = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        kBorderClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!entry->border || GetPropW(window, kPinProperty) ||
        !SetLayeredWindowAttributes(entry->border, 0, 255, LWA_ALPHA) ||
        !SetPropW(window, kPinProperty, entry->border)) return false;
    const BOOL excluded = TRUE;
    DwmSetWindowAttribute(entry->border, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
    if (!ChangeTopmost(window, true)) return false;
    entry->restoreTopmost = true;
    if (!SetTimer(state_->manager, kRefreshTimer, kRefreshIntervalMs, nullptr)) return false;
    entry->UpdateBorder();
    state_->entries.push_back(std::move(entry));
    return true;
}
