#include "dock_window_pin.h"
#include "platform/window_support/window_support_runtime.h"
#include "platform/operation_feedback.h"
#include "theme/personalization.h"

#include <dwmapi.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t kManagerClass[] = L"SnowDesktopDockWindowPinManager";
constexpr wchar_t kBorderClass[] = L"SnowDesktopDockWindowPinBorder";
constexpr wchar_t kOperationClass[] = L"SnowDesktopDockWindowPinOperation";
constexpr wchar_t kPinProperty[] = L"SnowDesktop.DockWindowPin";
constexpr wchar_t kUndoProperty[] = L"SnowDesktop.DockWindowPinUndo";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshIntervalMs = 100;
constexpr UINT kRefreshMessage = WM_APP + 1;
constexpr UINT kPositionFlags =
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
constexpr ULONGLONG kPinConfirmationTimeoutMs = 1000;

struct BorderAppearance
{
    bool enabled = true;
    float width = 6.0f;
    UINT32 rgb = 0x0078D7;
    float opacity = 0.5f;
    bool operator==(const BorderAppearance&) const = default;
};

class ScopedPhysicalCoordinates
{
public:
    ScopedPhysicalCoordinates()
        : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~ScopedPhysicalCoordinates()
    {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    ScopedPhysicalCoordinates(const ScopedPhysicalCoordinates&) = delete;
    ScopedPhysicalCoordinates& operator=(const ScopedPhysicalCoordinates&) = delete;
private:
    DPI_AWARENESS_CONTEXT previous_;
};

bool IsTopmost(HWND window)
{
    return IsWindow(window) && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool ChangeTopmost(HWND window, bool pinned)
{
    return SetWindowPos(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, kPositionFlags) != FALSE;
}

// A cross-queue TOPMOST request may arrive after its owner has gone away.
// A preallocated message-only operation window keeps the cancellation alive
// independently of its visual and State. One target cannot acquire another
// lease meanwhile. Active entries own the record; retirement transfers it to
// that operation window until native confirmation or target destruction.
struct PinRetirement
{
    inline static std::atomic<unsigned> live{0};
    static constexpr unsigned MaximumLive = 256;
    HWND target = nullptr;
    HWND border = nullptr;
    HWND operation = nullptr;
    DWORD process = 0;
    DWORD thread = 0;
    bool retired = false;
    bool waitingForPin = false;
    bool pinInFlight = false;
    bool detached = false;
    bool completed = false;

    ~PinRetirement() { --live; }

    static std::unique_ptr<PinRetirement> Reserve()
    {
        if (live.fetch_add(1) >= MaximumLive) { --live; return {}; }
        auto* value = new (std::nothrow) PinRetirement;
        if (!value) --live;
        return std::unique_ptr<PinRetirement>(value);
    }

    bool Advance()
    {
        if (completed) return true;
        DWORD currentProcess = 0;
        const DWORD currentThread = GetWindowThreadProcessId(target, &currentProcess);
        if (currentThread != thread || currentProcess != process ||
                GetPropW(target, kUndoProperty) != operation) return true;
        const bool topmost = IsTopmost(target);
        if (waitingForPin && !topmost) return false;
        waitingForPin = false;
        if (!topmost) return true;
        ChangeTopmost(target, false);
        return !IsTopmost(target);
    }

    void Retire()
    {
        if (retired) return;
        retired = true;
        waitingForPin = pinInFlight && !IsTopmost(target);
        if (GetPropW(target, kPinProperty) == border) RemovePropW(target, kPinProperty);
    }
};
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
        UINT32 rgb = 0;
        float opacity = -1.0f;
        bool restoreTopmost = false;
        bool requestedPinned = true;
        bool requestPending = false;
        bool pinInFlight = false;
        std::unique_ptr<PinRetirement> cleanup;
        ULONGLONG requestDeadline = 0;
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget;

        bool DrawBorder(const RECT& frame, int stroke, int corner, const BorderAppearance& appearance)
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
                D2D1::ColorF(appearance.rgb, appearance.opacity), brush.GetAddressOf());
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
            return cleanup && !cleanup->retired && cleanup->border == border && IsWindow(border) &&
                currentThread == thread && currentProcess == process &&
                GetPropW(target, kPinProperty) == border;
        }

        ~Entry()
        {
            if (OwnsTarget())
            {
                if (restoreTopmost || !requestedPinned)
                    cleanup->Retire();
                else RemovePropW(target, kPinProperty);
            }
            if (!cleanup) return;
            if (cleanup->border) DestroyWindow(cleanup->border);
            if (cleanup->retired && cleanup->operation)
            {
                auto* pending = cleanup.release();
                pending->detached = true;
                if (pending->Advance()) DestroyWindow(pending->operation);
                return;
            }
            if (cleanup->operation) DestroyWindow(cleanup->operation);
        }

        bool MaintainPin(const BorderAppearance& appearance, HWND peekTarget)
        {
            if (!OwnsTarget()) return false;
            const ULONGLONG now = GetTickCount64();
            const bool topmost = IsTopmost(target);
            if (!requestedPinned)
            {
                ShowWindow(border, SW_HIDE);
                return false;
            }
            // Keep the user's request when an application resets TOPMOST during
            // movement, focus changes or restore, as PowerToys does. Only repair
            // a lost flag; refreshing a stable pin must not raise the window.
            if (!topmost)
            {
                ShowWindow(border, SW_HIDE);
                if (requestPending)
                {
                    if (now < requestDeadline) return true;
                    snowdesktop::window_support::WindowSupport().RecordFailure(
                        snowdesktop::window_support::Capability::WindowPin, target,
                        snowdesktop::window_support::Provider::InProcess, ERROR_TIMEOUT);
                    snowdesktop::operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, ERROR_TIMEOUT});
                    return false;
                }
                if (!ChangeTopmost(target, true))
                {
                    const DWORD error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
                    snowdesktop::window_support::WindowSupport().RecordFailure(
                        snowdesktop::window_support::Capability::WindowPin, target,
                        snowdesktop::window_support::Provider::InProcess, error);
                    snowdesktop::operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, error});
                    return false;
                }
                requestPending = !IsTopmost(target);
                pinInFlight = requestPending;
                cleanup->pinInFlight = pinInFlight;
                if (requestPending)
                {
                    requestDeadline = now + kPinConfirmationTimeoutMs;
                    return true;
                }
            }
            requestPending = false;
            pinInFlight = false;
            cleanup->pinInFlight = false;
            UpdateBorder(appearance, peekTarget);
            return true;
        }

        void UpdateBorder(const BorderAppearance& appearance, HWND peekTarget)
        {
            if (!appearance.enabled || appearance.opacity <= 0.0f ||
                (peekTarget && target != peekTarget))
            {
                if (IsWindowVisible(border)) ShowWindow(border, SW_HIDE);
                return;
            }
            // DWM frame bounds are physical pixels even for DPI-unaware apps.
            // Both layered-window placement and fallback queries use that space.
            const ScopedPhysicalCoordinates physicalCoordinates;
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

            const auto awareness = GetAwarenessFromDpiAwarenessContext(
                GetWindowDpiAwarenessContext(target));
            const UINT dpi = std::max<UINT>(96, GetDpiForWindow(
                awareness == DPI_AWARENESS_PER_MONITOR_AWARE ? target : border));
            const int nextThickness = std::max(1, static_cast<int>(std::lround(
                appearance.width * static_cast<float>(dpi) / 96.0f)));
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
            const bool paintChanged = rgb != appearance.rgb || opacity != appearance.opacity;
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
            }
            if (shapeChanged || paintChanged)
            {
                if (!DrawBorder(frame, nextThickness, nextRadius, appearance))
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
            if (move || shapeChanged || paintChanged || show || previous != border)
            {
                UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
                if (previous == border) flags |= SWP_NOZORDER;
                if (!shapeChanged) flags |= SWP_NOSIZE | SWP_NOREDRAW;
                if (!move) flags |= SWP_NOMOVE;
                if (!SetWindowPos(border, previous ? previous : HWND_TOPMOST,
                        frame.left, frame.top, width, height, flags))
                {
                    ShowWindow(border, SW_HIDE);
                    return;
                }
                bounds = frame;
                thickness = nextThickness;
                radius = nextRadius;
                rgb = appearance.rgb;
                opacity = appearance.opacity;
            }
        }
    };

    HWND manager = nullptr;
    BorderAppearance appearance;
    HWND peekTarget = nullptr;
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

    Entry* FindEntry(HWND window)
    {
        const auto found = std::find_if(entries.begin(), entries.end(),
            [window](const auto& entry) { return entry && entry->target == window; });
        return found == entries.end() ? nullptr : found->get();
    }

    void UpdateTrackedBorder(HWND window)
    {
        if (refreshing)
        {
            // A native call can reenter the hook while entries are being
            // updated or erased. Defer that exceptional case, not every move.
            QueueRefresh();
            return;
        }
        Entry* entry = FindEntry(window);
        if (!entry) return;
        refreshing = true;
        if (entry->OwnsTarget())
        {
            if (!entry->MaintainPin(appearance, peekTarget)) QueueRefresh();
        }
        else
            QueueRefresh();
        // Keep ownership changes and vector erasure in the manager handler.
        refreshing = false;
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

    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND window,
        LONG object, LONG child, DWORD, DWORD)
    {
        const auto found = ActiveHooks().find(hook);
        if (found == ActiveHooks().end()) return;
        State* state = found->second;
        // Focus events commonly refer to OBJID_CLIENT or a child control, not
        // OBJID_WINDOW. They can reveal a reset on any other tracked window.
        if (event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_FOCUS)
        {
            state->QueueRefresh();
            return;
        }
        if (object != OBJID_WINDOW || child != CHILDID_SELF) return;
        // Match PowerToys' motion path: update this window inside the native
        // event callback rather than behind another posted application message.
        // Border and unrelated-window motion must not trigger a full refresh.
        if (event == EVENT_OBJECT_LOCATIONCHANGE)
            state->UpdateTrackedBorder(window);
        else if (state->refreshing || state->FindEntry(window))
            state->QueueRefresh();
    }

    void Subscribe()
    {
        if (!hooks.empty()) return;
        constexpr std::array<DWORD, 9> events{
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
            EVENT_OBJECT_DESTROY, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
            EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_FOCUS};
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
        auto* cleanup = reinterpret_cast<PinRetirement*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCDESTROY && cleanup)
        {
            if (GetPropW(cleanup->target, kPinProperty) == window)
                cleanup->Retire();
            cleanup->border = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
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

    static LRESULT CALLBACK OperationProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* cleanup = reinterpret_cast<PinRetirement*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (cleanup && message == WM_TIMER && wp == kRefreshTimer && cleanup->retired && cleanup->Advance())
        {
            cleanup->completed = true;
            if (GetPropW(cleanup->target, kUndoProperty) == window)
                RemovePropW(cleanup->target, kUndoProperty);
            if (cleanup->detached) DestroyWindow(window);
            return 0;
        }
        if (message == WM_NCDESTROY && cleanup)
        {
            if (GetPropW(cleanup->target, kUndoProperty) == window)
                RemovePropW(cleanup->target, kUndoProperty);
            cleanup->operation = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            if (cleanup->detached) delete cleanup;
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
                std::pair{kManagerClass, ManagerProc}, std::pair{kBorderClass, BorderProc},
                std::pair{kOperationClass, OperationProc}})
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
        std::erase_if(entries, [this](const auto& entry) {
            return !entry->MaintainPin(appearance, peekTarget);
        });
        if (entries.empty())
        {
            if (manager) KillTimer(manager, kRefreshTimer);
            Unsubscribe();
        }
        refreshing = false;
    }
};

DockWindowPin::DockWindowPin() = default;
DockWindowPin::~DockWindowPin() { Clear(); }

bool DockWindowPin::IsPinned(HWND window)
{
    return IsWindow(window) &&
        (IsWindow(static_cast<HWND>(GetPropW(window, kPinProperty))) || IsTopmost(window));
}

void DockWindowPin::Refresh()
{
    if (state_) state_->Refresh();
}

void DockWindowPin::SetPeekTarget(HWND window)
{
    snowdesktop::window_support::WindowSupport().SetPinPeek(window);
    if (!state_ || state_->peekTarget == window) return;
    state_->peekTarget = window;
    state_->Refresh();
}

void DockWindowPin::SetAppearance(const PersonalizationSettings& settings)
{
    auto normalized = settings;
    NormalizeWindowPinBorderAppearance(normalized);
    snowdesktop::window_support::WindowSupport().SetPinAppearance({
        normalized.windowPinBorderEnabled ? 1u : 0u, normalized.windowPinBorderWidth,
        normalized.windowPinBorderR, normalized.windowPinBorderG, normalized.windowPinBorderB,
        normalized.windowPinBorderOpacity});
    const auto channel = [](float value) { return static_cast<UINT32>(std::lround(value * 255.0f)); };
    const BorderAppearance appearance{normalized.windowPinBorderEnabled, normalized.windowPinBorderWidth,
        (channel(normalized.windowPinBorderR) << 16) | (channel(normalized.windowPinBorderG) << 8) |
            channel(normalized.windowPinBorderB), normalized.windowPinBorderOpacity};
    if (!state_) state_ = std::make_unique<State>();
    if (state_->appearance == appearance) return;
    state_->appearance = appearance;
    state_->Refresh();
}

void DockWindowPin::Clear()
{
    snowdesktop::window_support::protocol::Request request;
    request.command = snowdesktop::window_support::protocol::Command::ClearPins;
    snowdesktop::window_support::WindowSupport().Submit(request);
    state_.reset();
}

bool DockWindowPin::Toggle(HWND window)
{
    const ScopedPhysicalCoordinates physicalCoordinates;
    if (!IsWindow(window) || GetAncestor(window, GA_ROOT) != window) return false;
    const auto route = snowdesktop::window_support::WindowSupport().Resolve(
        snowdesktop::window_support::Capability::WindowPin, window);
    if (route.provider == snowdesktop::window_support::Provider::Helper)
    {
        snowdesktop::window_support::protocol::Request request;
        request.command = snowdesktop::window_support::protocol::Command::TogglePin;
        request.target = snowdesktop::window_support::protocol::Capture(window);
        return snowdesktop::window_support::WindowSupport().Submit(request);
    }
    if (!route.Available())
    {
        if (snowdesktop::window_support::WindowSupport().RequestPinAuthorization(window)) return true;
        snowdesktop::operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, ERROR_NOT_SUPPORTED});
        return false;
    }
    const bool accepted = [&]() -> bool {
    if (state_)
    {
        if (auto* entry = state_->FindEntry(window); entry && entry->OwnsTarget())
        {
            // Retire the request before refreshing so explicit cancellation
            // cannot be mistaken for another application resetting TOPMOST.
            const bool wasRefreshing = std::exchange(state_->refreshing, true);
            if (!ChangeTopmost(window, false))
            {
                state_->refreshing = wasRefreshing;
                return false;
            }
            entry->restoreTopmost = false;
            entry->requestedPinned = false;
            entry->requestPending = true;
            entry->requestDeadline = GetTickCount64() + kPinConfirmationTimeoutMs;
            state_->refreshing = wasRefreshing;
            Refresh();
            return true;
        }
    }
    Refresh();
    // A retired request must be observed and undone before a new request can
    // target the same native window; an old inverse must not undo a new pin.
    if (const HWND retiringBorder = static_cast<HWND>(GetPropW(window, kUndoProperty)))
    {
        const HWND border = static_cast<HWND>(GetPropW(window, kPinProperty));
        if (!IsWindow(retiringBorder) && (!border || !IsWindow(border)))
        {
            // A crashed helper cannot run its retirement window. Only reclaim
            // our stale markers after both owning HWNDs have disappeared.
            RemovePropW(window, kUndoProperty);
            if (border) RemovePropW(window, kPinProperty);
        }
        else
        {
        DWORD ownerProcess = 0;
        wchar_t className[64]{};
        if (GetWindowThreadProcessId(retiringBorder, &ownerProcess) != GetCurrentThreadId() ||
            ownerProcess != GetCurrentProcessId() ||
            !GetClassNameW(retiringBorder, className, static_cast<int>(std::size(className))) ||
            lstrcmpW(className, kOperationClass) != 0) { SetLastError(ERROR_BUSY); return false; }
        auto* cleanup = reinterpret_cast<PinRetirement*>(
            GetWindowLongPtrW(retiringBorder, GWLP_USERDATA));
        if (!cleanup || cleanup->target != window || cleanup->operation != retiringBorder ||
            !cleanup->retired || !cleanup->Advance()) { SetLastError(ERROR_BUSY); return false; }
        DestroyWindow(retiringBorder);
        }
    }
    if (IsTopmost(window))
    {
        if (!ChangeTopmost(window, false)) return false;
        Refresh();
        return true;
    }
    if (!state_)
    {
        state_ = std::make_unique<State>();
    }
    if (!state_->manager && !state_->Initialize()) return false;

    auto entry = std::make_unique<State::Entry>();
    auto cleanup = PinRetirement::Reserve();
    if (!cleanup) return false;
    entry->target = window;
    entry->thread = GetWindowThreadProcessId(window, &entry->process);
    RECT initialFrame{};
    GetWindowRect(window, &initialFrame);
    entry->border = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        kBorderClass, L"", WS_POPUP, initialFrame.left, initialFrame.top, 1, 1, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!entry->border) return false;
    cleanup->target = window;
    cleanup->border = entry->border;
    cleanup->process = entry->process;
    cleanup->thread = entry->thread;
    SetWindowLongPtrW(entry->border, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(cleanup.get()));
    entry->cleanup = std::move(cleanup);
    entry->cleanup->operation = CreateWindowExW(0, kOperationClass, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!entry->cleanup->operation) return false;
    SetWindowLongPtrW(entry->cleanup->operation, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(entry->cleanup.get()));
    if (!SetTimer(entry->cleanup->operation, kRefreshTimer, kRefreshIntervalMs, nullptr) ||
        GetPropW(window, kUndoProperty) || GetPropW(window, kPinProperty) ||
        !SetPropW(window, kUndoProperty, entry->cleanup->operation) ||
        !SetPropW(window, kPinProperty, entry->border)) return false;
    const BOOL excluded = TRUE;
    DwmSetWindowAttribute(entry->border, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
    if (!ChangeTopmost(window, true)) return false;
    entry->restoreTopmost = true;
    entry->requestPending = !IsTopmost(window);
    entry->pinInFlight = entry->requestPending;
    entry->cleanup->pinInFlight = entry->pinInFlight;
    entry->requestDeadline = GetTickCount64() + kPinConfirmationTimeoutMs;
    if (!SetTimer(state_->manager, kRefreshTimer, kRefreshIntervalMs, nullptr)) return false;
    entry->UpdateBorder(state_->appearance, state_->peekTarget);
    state_->entries.push_back(std::move(entry));
    state_->Subscribe();
    return true;
    }();
    if (accepted) return true;
    const DWORD error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    if (error == ERROR_BUSY) return false;
    auto& support = snowdesktop::window_support::WindowSupport();
    support.RecordFailure(snowdesktop::window_support::Capability::WindowPin, window,
        snowdesktop::window_support::Provider::InProcess, error);
    if (support.Resolve(snowdesktop::window_support::Capability::WindowPin, window).provider ==
            snowdesktop::window_support::Provider::Helper)
    {
        snowdesktop::window_support::protocol::Request request;
        request.command = snowdesktop::window_support::protocol::Command::TogglePin;
        request.target = snowdesktop::window_support::protocol::Capture(window);
        if (support.Submit(request)) return true;
    }
    snowdesktop::operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, error});
    SetLastError(error);
    return false;
}
