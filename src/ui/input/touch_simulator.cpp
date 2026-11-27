#include "touch_simulator.h"
#include "low_level_mouse_hook.h"
#include "ui/render/demo_cursor.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace snowdesktop::touch_simulation
{
namespace
{
constexpr wchar_t kWindowClass[] = L"SnowDesktop.TouchSimulator";
constexpr UINT kBegin = WM_APP;
constexpr UINT kBeginTwoContacts = WM_APP + 1;
constexpr UINT kEnd = WM_APP + 2;
constexpr UINT kPinchIn = WM_APP + 3;
constexpr UINT kPinchOut = WM_APP + 4;
constexpr UINT kTrackAim = WM_APP + 5;
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameIntervalMs = 24;
constexpr int kPinchFrames = 6;
constexpr int kPinchMinRadius = 12;
constexpr int kPinchMaxRadius = 72;
constexpr auto kContact = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
constexpr ULONG_PTR kPointerSignature = 0xFF515700;
std::atomic<HWND> touchPressTarget = nullptr;
std::mutex edgeGestureMutex;
HWND edgeGestureTarget = nullptr;
std::vector<RECT> edgeGestureZones;
using Points = std::array<POINT, 2>;

struct ButtonEvent { UINT key = 0; bool up = false; };
struct EdgeDrag { HWND target; POINT origin; };

HWND EdgeGestureTargetAt(POINT point)
{
    const std::lock_guard lock(edgeGestureMutex);
    return std::ranges::any_of(edgeGestureZones, [point](const RECT& zone) {
        return PtInRect(&zone, point);
    }) ? edgeGestureTarget : nullptr;
}

ButtonEvent MouseButton(WPARAM message, DWORD data) noexcept
{
    switch (message)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return {VK_LBUTTON, message == WM_LBUTTONUP};
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return {VK_RBUTTON, message == WM_RBUTTONUP};
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return {VK_MBUTTON, message == WM_MBUTTONUP};
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        return {static_cast<UINT>(GET_XBUTTON_WPARAM(data) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2),
            message == WM_XBUTTONUP};
    default: return {};
    }
}

bool KeyDown(UINT key) noexcept { return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0; }

UINT HeldModifiers() noexcept
{
    return (KeyDown(VK_SHIFT) ? MOD_SHIFT : 0) |
        (KeyDown(VK_CONTROL) ? MOD_CONTROL : 0) |
        (KeyDown(VK_MENU) ? MOD_ALT : 0) |
        (KeyDown(VK_LWIN) || KeyDown(VK_RWIN) ? MOD_WIN : 0);
}

RECT MonitorBounds(POINT point) noexcept
{
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor))
        return monitor.rcMonitor;
    return {0, 0, (std::max)(1, GetSystemMetrics(SM_CXSCREEN)),
        (std::max)(1, GetSystemMetrics(SM_CYSCREEN))};
}

POINT RawPosition(POINT current, const RAWMOUSE& mouse) noexcept
{
    if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
    {
        const auto add = [](LONG coordinate, LONG delta) {
            return static_cast<LONG>(std::clamp<LONGLONG>(static_cast<LONGLONG>(coordinate) + delta,
                (std::numeric_limits<LONG>::min)(), (std::numeric_limits<LONG>::max)()));
        };
        current.x = add(current.x, mouse.lLastX);
        current.y = add(current.y, mouse.lLastY);
    }
    else
    {
        const bool virtualDesktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int x = virtualDesktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        const int y = virtualDesktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        const int width = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        current = {x + MulDiv(mouse.lLastX, width - 1, 65535),
            y + MulDiv(mouse.lLastY, height - 1, 65535)};
    }
    const auto bounds = MonitorBounds(current);
    return {std::clamp(current.x, bounds.left, bounds.right - 1),
        std::clamp(current.y, bounds.top, bounds.bottom - 1)};
}

class Simulator
{
public:
    static Simulator& Current()
    {
        static Simulator simulator;
        return simulator;
    }

    bool Configure(bool enabled, UINT modifiers, UINT key)
    {
        if (!ValidBinding(modifiers, key) || (enabled && !key)) return false;
        binding_.store((modifiers << 8) | key);
        if (enabled)
            return hook_.Start(GetModuleHandleW(nullptr), Hook, Initialize, Shutdown);
        Stop();
        return true;
    }

    bool Enabled() const noexcept { return static_cast<bool>(hook_); }
    void Stop() noexcept { hook_.Stop(); }

private:
    ~Simulator() { Stop(); }

    static bool Initialize() noexcept
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        if (!InitializeTouchInjection(2, TOUCH_FEEDBACK_DEFAULT)) return false;
        auto& self = Current();
        self.window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, nullptr,
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!self.window_) return false;
        const RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_INPUTSINK, self.window_};
        if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
        {
            try
            {
                self.aimOverlay_.emplace();
                return true;
            }
            catch (...)
            {
                const RAWINPUTDEVICE remove{0x01, 0x02, RIDEV_REMOVE, nullptr};
                RegisterRawInputDevices(&remove, 1, sizeof(remove));
            }
        }
        DestroyWindow(self.window_);
        self.window_ = nullptr;
        return false;
    }

    static void Shutdown() noexcept
    {
        auto& self = Current();
        self.End(true);
        self.blockedButtons_ = 0;
        self.aimActive_ = false;
        self.paintedAim_.reset();
        self.aimOverlay_.reset();
        const RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        DestroyWindow(self.window_);
        self.window_ = nullptr;
    }

    static LRESULT CALLBACK Hook(int code, WPARAM message, LPARAM data) noexcept
    {
        if (code == HC_ACTION)
        {
            const auto& mouse = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            // Compatibility mouse messages generated by touch or pen use this signature.
            if (!(mouse.flags & LLMHF_INJECTED) &&
                (mouse.dwExtraInfo & 0xFFFFFF00) != kPointerSignature &&
                Current().OnMouse(message, mouse)) return 1;
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    bool OnMouse(WPARAM message, const MSLLHOOKSTRUCT& mouse) noexcept
    {
        const auto button = MouseButton(message, mouse.mouseData);
        const UINT bit = button.key ? 1u << button.key : 0;
        if (button.up)
        {
            // A release of a button pressed before conversion belongs to the original app.
            if (!(blockedButtons_ & bit)) return false;
            blockedButtons_ &= ~bit;
            if (button.key == VK_LBUTTON) Post(kEnd);
            return true;
        }

        const UINT binding = binding_.load();
        const UINT key = binding & 0xff;
        const bool triggerHeld = key && (IsMouseBinding(key)
            ? button.key == key || (blockedButtons_ & (1u << key))
            : KeyDown(key));
        if (!triggerHeld && !blockedButtons_) return false;
        const UINT required = (binding >> 8) | KeyModifier(key);
        const UINT held = HeldModifiers();
        const bool bound = triggerHeld && (held & required) == required;
        if ((!bound && !blockedButtons_) || RecordingBinding()) return false;
        if (!aimActive_)
        {
            if (!PostMessageW(window_, kTrackAim, 0, 0)) return false;
            if (!GetPhysicalCursorPos(&aim_)) aim_ = mouse.pt;
            dragPoint_ = aim_;
            aimActive_ = true;
        }
        if (message == WM_LBUTTONDOWN)
        {
            const UINT second = !(required & MOD_CONTROL) ? MOD_CONTROL :
                !(required & MOD_SHIFT) ? MOD_SHIFT : !(required & MOD_ALT) ? MOD_ALT : 0;
            if (!bound || !Post((held & second) ? kBeginTwoContacts : kBegin)) return false;
        }
        if (message == WM_MOUSEWHEEL && bound && !(blockedButtons_ & (1u << VK_LBUTTON)))
            Post(GET_WHEEL_DELTA_WPARAM(mouse.mouseData) < 0 ? kPinchIn : kPinchOut);
        blockedButtons_ |= bit;
        return true;
    }

    bool Post(UINT message) const noexcept
    {
        return PostMessageW(window_, message, static_cast<WPARAM>(aim_.x), static_cast<LPARAM>(aim_.y)) != FALSE;
    }

    static bool RecordingBinding() noexcept
    {
        const HWND foreground = GetForegroundWindow();
        return foreground && GetPropW(foreground, BindingCaptureProperty);
    }

    void OnRawInput(HRAWINPUT handle) noexcept
    {
        if (!aimActive_) return;
        RAWINPUT raw{};
        UINT bytes = sizeof(raw);
        if (GetRawInputData(handle, RID_INPUT, &raw, &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
            raw.header.dwType != RIM_TYPEMOUSE || !raw.header.hDevice) return;
        const auto& mouse = raw.data.mouse;
        if ((mouse.ulExtraInformation & 0xFFFFFF00) == kPointerSignature) return;
        aim_ = RawPosition(aim_, mouse);
        dragPoint_ = aim_;
    }

    void UpdateAim() noexcept
    {
        if (aimActive_ && !blockedButtons_)
        {
            const UINT binding = binding_.load();
            const UINT key = binding & 0xff;
            const UINT required = (binding >> 8) | KeyModifier(key);
            aimActive_ = key && !IsMouseBinding(key) && KeyDown(key) &&
                (HeldModifiers() & required) == required && !RecordingBinding();
        }
        if (!aimActive_)
        {
            if (paintedAim_) aimOverlay_->Hide();
            paintedAim_.reset();
            if (!count_) KillTimer(window_, kFrameTimer);
            return;
        }
        if (paintedAim_ && paintedAim_->x == aim_.x && paintedAim_->y == aim_.y) return;
        demo_cursor::Pose pose;
        pose.x = static_cast<float>(aim_.x);
        pose.y = static_cast<float>(aim_.y);
        pose.scale = 0.75f;
        if (aimOverlay_->Show(pose)) paintedAim_ = aim_;
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept
    {
        auto& self = Current();
        const POINT point{static_cast<LONG>(wp), static_cast<LONG>(lp)};
        switch (message)
        {
        case kTrackAim:
            if (SetTimer(window, kFrameTimer, kFrameIntervalMs, nullptr)) self.UpdateAim();
            else self.aimActive_ = false;
            return 0;
        case WM_INPUT:
            self.OnRawInput(reinterpret_cast<HRAWINPUT>(lp));
            return DefWindowProcW(window, message, wp, lp);
        case kBegin: case kBeginTwoContacts:
            self.BeginDrag(point, message == kBeginTwoContacts);
            return 0;
        case kEnd:
            self.EndDrag(point);
            return 0;
        case kPinchIn: case kPinchOut:
            self.BeginPinch(point, message == kPinchIn);
            return 0;
        case WM_TIMER:
            if (wp == kFrameTimer)
            {
                self.Frame();
                self.UpdateAim();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            self.End(true);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    bool Send(const Points& points, POINTER_FLAGS flags) const noexcept
    {
        std::array<POINTER_TOUCH_INFO, 2> contacts{};
        for (UINT i = 0; i < count_; ++i)
        {
            auto& pointer = contacts[i].pointerInfo;
            pointer.pointerType = PT_TOUCH;
            pointer.pointerId = i + 1;
            pointer.ptPixelLocation = points[i];
            pointer.pointerFlags = flags;
        }
        if (InjectTouchInput(count_, contacts.data())) return true;
        if (GetLastError() != ERROR_NOT_READY) return false;
        // Consecutive frames may land in the same 0.1 ms window.
        Sleep(1);
        return InjectTouchInput(count_, contacts.data()) != FALSE;
    }

    void Begin(const Points& points, UINT count) noexcept
    {
        count_ = count;
        if (!Send(points, POINTER_FLAG_DOWN | kContact))
        {
            count_ = 0;
            return;
        }
        sentPoints_ = points;
        if (!SetTimer(window_, kFrameTimer, kFrameIntervalMs, nullptr)) End(true);
    }

    void BeginDrag(POINT point, bool twoContacts) noexcept
    {
        End(true);
        if (!twoContacts)
        {
            if (const HWND target = EdgeGestureTargetAt(point))
            {
                // Reserve the whole contact before Windows can recognize a Shell swipe.
                edgeDrag_ = EdgeDrag{target, point};
                return;
            }
        }
        Points points{point, point};
        if (twoContacts)
        {
            const auto bounds = MonitorBounds(point);
            points[0].x = point.x - bounds.left > 20 ? point.x - 20 :
                (std::min)(point.x + 20, bounds.right - 1);
        }
        const DWORD pressedAt = GetTickCount();
        Begin(points, twoContacts ? 2 : 1);
        // Pointer-aware apps can consume injected touch without emitting a mouse press.
        if (const HWND target = touchPressTarget.load(); target && count_ == 1)
            PostMessageW(target, TouchPressMessage, pressedAt, MAKELPARAM(point.x, point.y));
    }

    void EndDrag(POINT point) noexcept
    {
        if (const auto edge = std::exchange(edgeDrag_, std::nullopt))
        {
            PostMessageW(edge->target, EdgeSwipeMessage,
                MAKEWPARAM(edge->origin.x, edge->origin.y), MAKELPARAM(point.x, point.y));
            return;
        }
        if (count_ && !pinching_)
        {
            dragPoint_ = point;
            Frame();
            End(false);
        }
    }

    Points PinchPoints(int radius) const noexcept
    {
        return {POINT{(std::max)(pinchCenter_.x - radius, pinchBounds_.left), pinchCenter_.y},
            POINT{(std::min)(pinchCenter_.x + radius, pinchBounds_.right - 1), pinchCenter_.y}};
    }

    void BeginPinch(POINT point, bool inward) noexcept
    {
        if (count_)
        {
            if (pinching_) queuedPinch_ = PinchRequest{point, inward};
            return;
        }
        pinchCenter_ = point;
        pinchBounds_ = MonitorBounds(point);
        pinchStart_ = inward ? kPinchMaxRadius : kPinchMinRadius;
        pinchEnd_ = inward ? kPinchMinRadius : kPinchMaxRadius;
        pinchFrame_ = 0;
        Begin(PinchPoints(pinchStart_), 2);
        pinching_ = count_ != 0;
    }

    void Frame() noexcept
    {
        if (!count_) return;
        Points points = sentPoints_;
        if (pinching_)
        {
            ++pinchFrame_;
            points = PinchPoints(pinchStart_ + (pinchEnd_ - pinchStart_) * pinchFrame_ / kPinchFrames);
        }
        else points[count_ - 1] = dragPoint_;
        if (!Send(points, POINTER_FLAG_UPDATE | kContact))
        {
            End(true);
            return;
        }
        sentPoints_ = points;
        if (pinching_ && pinchFrame_ == kPinchFrames)
        {
            End(false);
            if (queuedPinch_)
            {
                const auto next = *queuedPinch_;
                queuedPinch_.reset();
                BeginPinch(next.point, next.inward);
            }
        }
    }

    void End(bool cancelled) noexcept
    {
        if (cancelled)
        {
            queuedPinch_.reset();
            edgeDrag_.reset();
        }
        if (count_)
        {
            if (!aimActive_) KillTimer(window_, kFrameTimer);
            Send(sentPoints_, POINTER_FLAG_UP | (cancelled ? POINTER_FLAG_CANCELED : 0));
        }
        count_ = 0;
        pinching_ = false;
    }

    LowLevelMouseHook hook_;
    std::atomic<UINT> binding_{DefaultKey};
    HWND window_ = nullptr;
    UINT blockedButtons_ = 0;
    bool aimActive_ = false;
    POINT aim_{};
    std::optional<POINT> paintedAim_;
    std::optional<demo_cursor::Overlay> aimOverlay_;
    UINT count_ = 0;
    Points sentPoints_{};
    POINT dragPoint_{};
    std::optional<EdgeDrag> edgeDrag_;
    bool pinching_ = false;
    POINT pinchCenter_{};
    RECT pinchBounds_{};
    int pinchStart_ = 0;
    int pinchEnd_ = 0;
    int pinchFrame_ = 0;
    struct PinchRequest { POINT point; bool inward; };
    std::optional<PinchRequest> queuedPinch_;
};
}

bool Configure(bool enabled, UINT modifiers, UINT key) { return Simulator::Current().Configure(enabled, modifiers, key); }
void SetEdgeGestureTargets(HWND window, std::vector<RECT> zones)
{
    const std::lock_guard lock(edgeGestureMutex);
    edgeGestureTarget = window;
    edgeGestureZones = std::move(zones);
}
void ReleaseEdgeGestureAt(POINT point)
{
    const std::lock_guard lock(edgeGestureMutex);
    std::erase_if(edgeGestureZones, [point](const RECT& zone) { return PtInRect(&zone, point); });
}
bool Enabled() noexcept { return Simulator::Current().Enabled(); }
void SetTouchPressTarget(HWND window) noexcept { touchPressTarget.store(window); }
void Stop() noexcept
{
    SetTouchPressTarget(nullptr);
    Simulator::Current().Stop();
}
}
