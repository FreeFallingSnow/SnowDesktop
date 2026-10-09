#include "dock_window_pin.h"

#include <dwmapi.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t kManagerClass[] = L"SnowDesktopDockWindowPinManager";
constexpr wchar_t kBorderClass[] = L"SnowDesktopDockWindowPinBorder";
constexpr wchar_t kPinProperty[] = L"SnowDesktop.DockWindowPin";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshIntervalMs = 100;
constexpr UINT kRefreshMessage = WM_APP + 1;
constexpr UINT32 kBorderRgb = 0x0078D7;
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
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget;

        bool DrawBorder(const RECT& frame, int stroke, int corner)
        {
            if (!factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    factory.GetAddressOf()))) return false;
            if (!renderTarget)
            {
                const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
                if (FAILED(factory->CreateDCRenderTarget(&properties, renderTarget.GetAddressOf())))
                    return false;
            }
            const int width = frame.right - frame.left;
            const int height = frame.bottom - frame.top;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            const HDC dc = CreateCompatibleDC(nullptr);
            const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!dc || !bitmap)
            {
                if (bitmap) DeleteObject(bitmap);
                if (dc) DeleteDC(dc);
                return false;
            }
            const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
            const RECT client{0, 0, width, height};
            const float outerRadius = corner ? static_cast<float>(corner + stroke) : 0.0f;
            Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> outer, inner;
            Microsoft::WRL::ComPtr<ID2D1GeometryGroup> ring;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            const float inset = static_cast<float>(stroke);
            HRESULT result = factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(
                D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
                outerRadius, outerRadius), outer.GetAddressOf());
            if (SUCCEEDED(result)) result = factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(
                D2D1::RectF(inset, inset, static_cast<float>(width) - inset, static_cast<float>(height) - inset),
                static_cast<float>(corner), static_cast<float>(corner)), inner.GetAddressOf());
            ID2D1Geometry* geometries[]{outer.Get(), inner.Get()};
            if (SUCCEEDED(result)) result = factory->CreateGeometryGroup(
                D2D1_FILL_MODE_ALTERNATE, geometries, 2, ring.GetAddressOf());
            if (SUCCEEDED(result)) result = renderTarget->BindDC(dc, &client);
            if (SUCCEEDED(result)) result = renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(kBorderRgb, 0.5f), brush.GetAddressOf());
            if (SUCCEEDED(result))
            {
                renderTarget->BeginDraw();
                renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
                renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget->FillGeometry(ring.Get(), brush.Get());
                result = renderTarget->EndDraw();
            }
            bool updated = false;
            if (SUCCEEDED(result))
            {
                POINT destination{frame.left, frame.top};
                POINT source{};
                SIZE size{width, height};
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                updated = UpdateLayeredWindow(border, nullptr, &destination, &size,
                    dc, &source, 0, &blend, ULW_ALPHA) != FALSE;
            }
            if (result == D2DERR_RECREATE_TARGET) renderTarget.Reset();
            SelectObject(dc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(dc);
            return updated;
        }

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
            const int nextThickness = std::max(1, MulDiv(6, static_cast<int>(dpi), 96));
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
                // Leave one transparent pixel around the inner geometry so
                // the safety hole does not clip the antialiased edge.
                HRGN outer = CreateRectRgn(0, 0, width, height);
                HRGN inner = nextRadius
                    ? CreateRoundRectRgn(nextThickness + 1, nextThickness + 1,
                        width - nextThickness, height - nextThickness,
                        nextRadius * 2, nextRadius * 2)
                    : CreateRectRgn(nextThickness + 1, nextThickness + 1,
                        width - nextThickness - 1, height - nextThickness - 1);
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
                if (!DrawBorder(frame, nextThickness, nextRadius))
                {
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
            }
        }
    };

    HWND manager = nullptr;
    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<HWINEVENTHOOK> hooks;
    bool refreshQueued = false;
    bool refreshing = false;

    void QueueRefresh()
    {
        if (refreshQueued) return;
        refreshQueued = true;
        if (!PostMessageW(manager, kRefreshMessage, 0, 0)) refreshQueued = false;
    }

    static auto& ActiveHooks()
    {
        // OUTOFCONTEXT callbacks run on the registering message-loop thread.
        static thread_local std::unordered_map<HWINEVENTHOOK, State*> active;
        return active;
    }

    void Unsubscribe()
    {
        for (const auto hook : hooks)
        {
            ActiveHooks().erase(hook);
            UnhookWinEvent(hook);
        }
        hooks.clear();
    }

    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND,
        LONG object, LONG child, DWORD, DWORD)
    {
        const auto found = ActiveHooks().find(hook);
        if (found == ActiveHooks().end()) return;
        State* state = found->second;
        // WinEvent callbacks may reenter during native calls. Only enqueue a
        // coalesced update here; entry ownership is inspected in the message loop.
        if (event == EVENT_SYSTEM_FOREGROUND ||
                (object == OBJID_WINDOW && child == CHILDID_SELF))
            state->QueueRefresh();
    }

    void Subscribe()
    {
        if (!hooks.empty()) return;
        constexpr std::array<DWORD, 8> events{
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
            EVENT_OBJECT_DESTROY, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
            EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_FOREGROUND};
        for (const DWORD event : events)
        {
            const HWINEVENTHOOK hook = SetWinEventHook(event, event, nullptr, WinEventProc,
                0, 0, WINEVENT_OUTOFCONTEXT);
            if (hook)
            {
                ActiveHooks().emplace(hook, this);
                hooks.push_back(hook);
            }
        }
    }

    ~State()
    {
        Unsubscribe();
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
            // UpdateLayeredWindow owns the premultiplied surface.
            (void)dc;
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
        if (state && message == kRefreshMessage)
        {
            state->refreshQueued = false;
            state->Refresh();
            return 0;
        }
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
        if (refreshing) return;
        refreshing = true;
        std::erase_if(entries, [](const auto& entry) {
            return !entry->OwnsTarget() || !DockWindowPin::IsPinned(entry->target);
        });
        for (const auto& entry : entries) entry->UpdateBorder();
        if (entries.empty())
        {
            KillTimer(manager, kRefreshTimer);
            Unsubscribe();
        }
        refreshing = false;
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
        !SetPropW(window, kPinProperty, entry->border)) return false;
    const BOOL excluded = TRUE;
    DwmSetWindowAttribute(entry->border, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
    if (!ChangeTopmost(window, true)) return false;
    entry->restoreTopmost = true;
    if (!SetTimer(state_->manager, kRefreshTimer, kRefreshIntervalMs, nullptr)) return false;
    entry->UpdateBorder();
    state_->entries.push_back(std::move(entry));
    state_->Subscribe();
    return true;
}
