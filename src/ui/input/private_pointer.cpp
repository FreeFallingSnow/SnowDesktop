#include "private_pointer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <limits>
#include <utility>
#include <windowsx.h>

namespace snowdesktop::private_pointer
{
namespace
{
constexpr wchar_t DispatcherClass[] = L"SnowDesktop.PrivatePointer.Dispatch.v1";
constexpr UINT DispatchEvent = WM_APP + 71;
constexpr std::size_t MaxPending = 512, MaxTrace = 4096;

enum class Kind { Move, Down, Up, Wheel };
struct Event
{
    Kind kind = Kind::Move;
    POINT point{};
    Button button = Button::Left;
    int delta = 0;
    std::uint64_t sequence = 0;
};

struct Permit
{
    HWND window = nullptr;
    UINT message = 0;
    bool available = false;
};

struct Session;
thread_local Session* current = nullptr;
thread_local Permit permit;

double ClockMilliseconds()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ValidButton(Button button)
{
    return button == Button::Left || button == Button::Right || button == Button::Middle;
}
bool PointerMessage(UINT message)
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_NCMOUSEMOVE && message <= WM_NCXBUTTONDBLCLK) ||
        message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE ||
        message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
        message == WM_MOUSEACTIVATE || message == WM_SETCURSOR ||
        message == WM_POINTERDOWN || message == WM_POINTERUP ||
        message == WM_POINTERUPDATE || message == WM_TOUCH ||
        (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
        message == WM_IME_CHAR || message == WM_IME_COMPOSITION ||
        message == WM_IME_STARTCOMPOSITION || message == WM_IME_ENDCOMPOSITION ||
        message == WM_IME_KEYDOWN || message == WM_IME_KEYUP ||
        message == WM_INPUT;
}
bool PackedPoint(POINT point)
{
    return point.x >= std::numeric_limits<short>::min() && point.x <= std::numeric_limits<short>::max() &&
        point.y >= std::numeric_limits<short>::min() && point.y <= std::numeric_limits<short>::max();
}
LPARAM PointParameter(POINT point) { return MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)); }
WPARAM ButtonParameter(unsigned buttons)
{
    return ((buttons & 1U) ? MK_LBUTTON : 0U) |
        ((buttons & 2U) ? MK_RBUTTON : 0U) |
        ((buttons & 4U) ? MK_MBUTTON : 0U);
}

struct Session
{
    State state;
    Device::Route route;
    Device::Owns owns;
    Device::Observe observe;
    HWND dispatcher = nullptr;
    DWORD thread = 0;
    std::deque<Event> pending;
    POINT projectedPoint{};
    unsigned projectedButtons = 0, dispatchDepth = 0;
    std::uint64_t nextSequence = 0;
    bool wakePosted = false;
    double started = 0, lastClickTime = 0;
    HWND lastClickWindow = nullptr;
    POINT lastClickPoint{};
    Button lastClickButton = Button::Left;
    bool lastClickEligible = false, doublePress = false;

    bool Notify() noexcept
    {
        if (!observe) return true;
        try { observe(state); return true; }
        catch (...)
        {
            observe = {}; state.error = "private_pointer_observer_failed";
            End(); return false;
        }
    }

    bool Owned(HWND window) const noexcept
    {
        if (!window || !IsWindow(window) || !owns) return false;
        DWORD process = 0;
        const DWORD ownerThread = GetWindowThreadProcessId(window, &process);
        if (ownerThread != thread || process != GetCurrentProcessId()) return false;
        try { return owns(window); } catch (...) { return false; }
    }
    HWND Hit(POINT point) const noexcept
    {
        if (!route || !PtInRect(&state.bounds, point)) return nullptr;
        try { const HWND window = route(point); return Owned(window) ? window : nullptr; }
        catch (...) { return nullptr; }
    }
    void Record(HWND window, UINT message, const char* phase)
    {
        state.trace.push_back({state.sequence, std::max(0.0, ClockMilliseconds() - started),
            state.point, state.buttons, window, message, phase});
        if (state.trace.size() > MaxTrace) state.trace.erase(state.trace.begin());
    }
    LRESULT Deliver(HWND window, UINT message, WPARAM wp = 0, LPARAM lp = 0) noexcept
    {
        if (!state.enabled || current != this || !Owned(window)) return 0;
        const Permit previous = permit;
        permit = {window, message, true};
        ++dispatchDepth;
        const LRESULT result = SendMessageW(window, message, wp, lp);
        --dispatchDepth;
        permit = previous;
        return result;
    }
    void Deactivate() noexcept
    {
        state.enabled = false;
        state.closing = false;
        state.buttons = 0;
        state.captured = state.hovered = state.focused = state.foreground = nullptr;
        state.cursor = nullptr;
        if (current == this) { current = nullptr; permit = {}; }
    }
    void End() noexcept
    {
        if (!state.enabled || state.closing) return;
        state.closing = true;
        pending.clear(); state.pendingEvents = 0;
        projectedButtons = state.buttons = 0;
        const HWND captured = state.captured, hovered = state.hovered;
        state.captured = state.hovered = nullptr;
        // A cancellation is never a button-up: an unfinished press must not
        // accidentally launch a file or commit a drag while the session ends.
        Deliver(captured, WM_CANCELMODE);
        Deliver(captured, WM_CAPTURECHANGED);
        if (hovered != captured) Deliver(hovered, WM_CANCELMODE);
        Deliver(hovered, WM_MOUSELEAVE);
        state.focused = state.foreground = nullptr;
        try { Record(nullptr, 0, "ended"); } catch (...) {}
        if (!dispatchDepth) Deactivate();
        Notify();
    }
    bool Queue(Event event, std::string& error)
    {
        error.clear();
        if (!state.enabled || state.closing || current != this || thread != GetCurrentThreadId())
        { error = "private_pointer_not_active"; return false; }
        if (pending.size() >= MaxPending) { error = "private_pointer_queue_full"; return false; }
        event.sequence = ++nextSequence;
        pending.push_back(event);
        if (!wakePosted)
        {
            if (!PostMessageW(dispatcher, DispatchEvent, 0, 0))
            { pending.pop_back(); error = "private_pointer_dispatch_unavailable"; return false; }
            wakePosted = true;
        }
        state.pendingEvents = pending.size();
        return true;
    }
    bool Destination(HWND& destination, POINT& client)
    {
        if (state.captured && !Owned(state.captured))
        {
            state.captured = nullptr; state.buttons = projectedButtons = 0;
            state.error = "private_pointer_capture_destroyed";
            return false;
        }
        destination = state.captured ? state.captured : state.hovered;
        if (!Owned(destination)) { state.error = "private_pointer_target_unavailable"; return false; }
        client = state.point;
        if (!ScreenToClient(destination, &client) || !PackedPoint(client))
        { state.error = "private_pointer_coordinate_range"; return false; }
        return true;
    }
    void Process(const Event& event)
    {
        state.sequence = event.sequence;
        state.error.clear();
        state.point = event.point;
        const HWND hit = Hit(event.point);
        if (hit != state.hovered)
        {
            const HWND previous = state.hovered;
            state.hovered = hit;
            if (Owned(previous)) Deliver(previous, WM_MOUSELEAVE);
        }
        if (!state.enabled || state.closing) return;
        HWND destination = nullptr;
        POINT client{};
        if (!Destination(destination, client))
        {
            // Moving through an uncovered part of the selected monitor is
            // valid; pressing there is explicitly unavailable.
            if (event.kind == Kind::Move && !state.captured && !state.hovered) state.error.clear();
            Record(nullptr, 0, state.error.empty() ? "moving" : "rejected");
            return;
        }
        UINT message = WM_MOUSEMOVE;
        WPARAM wp = ButtonParameter(state.buttons);
        LPARAM lp = PointParameter(client);
        const char* phase = "moving";
        if (event.kind == Kind::Down)
        {
            const unsigned bit = static_cast<unsigned>(event.button);
            if (state.buttons & bit) { state.error = "private_pointer_button_already_down"; Record(destination, 0, "rejected"); return; }
            state.buttons |= bit; wp = ButtonParameter(state.buttons); phase = "pressed";
            const double now = ClockMilliseconds();
            doublePress = lastClickEligible && lastClickButton == event.button && lastClickWindow == destination &&
                now - lastClickTime <= GetDoubleClickTime() &&
                std::abs(static_cast<long long>(state.point.x) - lastClickPoint.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                std::abs(static_cast<long long>(state.point.y) - lastClickPoint.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2 &&
                (GetClassLongPtrW(destination, GCL_STYLE) & CS_DBLCLKS) != 0;
            message = event.button == Button::Left ? (doublePress ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN) :
                event.button == Button::Right ? (doublePress ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN) :
                    (doublePress ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN);
            lastClickEligible = false;
            state.focused = state.foreground = destination;
        }
        else if (event.kind == Kind::Up)
        {
            const unsigned bit = static_cast<unsigned>(event.button);
            if (!(state.buttons & bit)) { state.error = "private_pointer_button_not_down"; Record(destination, 0, "rejected"); return; }
            state.buttons &= ~bit; wp = ButtonParameter(state.buttons); phase = "released";
            message = event.button == Button::Left ? WM_LBUTTONUP : event.button == Button::Right ? WM_RBUTTONUP : WM_MBUTTONUP;
            lastClickEligible = !doublePress;
            lastClickTime = ClockMilliseconds(); lastClickPoint = state.point;
            lastClickButton = event.button; lastClickWindow = destination;
        }
        else if (event.kind == Kind::Wheel)
        {
            if (!PackedPoint(state.point)) { state.error = "private_pointer_coordinate_range"; Record(destination, 0, "rejected"); return; }
            message = WM_MOUSEWHEEL; phase = "scroll";
            wp = MAKEWPARAM(static_cast<WORD>(ButtonParameter(state.buttons)), static_cast<short>(event.delta));
            lp = PointParameter(state.point);
        }
        Record(destination, message, phase);
        // Notify at delivery time, before a target can enter a modal loop.
        // The cursor pose therefore cannot wait for that menu to close.
        if (!Notify() || !state.enabled || state.closing || current != this) return;
        Deliver(destination, message, wp, lp);
    }
    void Dispatch() noexcept
    {
        wakePosted = false;
        if (!state.enabled || state.closing || pending.empty()) return;
        const Event event = pending.front(); pending.pop_front();
        state.pendingEvents = pending.size();
        // Schedule the following event before calling a window procedure:
        // a menu may run a nested loop until that later event selects a row.
        if (!pending.empty())
        {
            wakePosted = PostMessageW(dispatcher, DispatchEvent, 0, 0) != FALSE;
            if (!wakePosted) { state.error = "private_pointer_dispatch_unavailable"; End(); return; }
        }
        try { Process(event); }
        catch (...) { state.error = "private_pointer_dispatch_failed"; End(); }
        if (state.closing && !dispatchDepth) Deactivate();
        Notify();
    }
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* session = reinterpret_cast<Session*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            session = static_cast<Session*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(session));
        }
        if (session && message == DispatchEvent) { session->Dispatch(); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }
};
}

struct Device::Impl { Session session; };
Device::Device() : impl_(std::make_unique<Impl>()) {}
Device::~Device()
{
    End();
    if (impl_->session.dispatcher) DestroyWindow(impl_->session.dispatcher);
}
const State& Device::Current() const noexcept { return impl_->session.state; }
bool Device::Begin(RECT bounds, POINT initial, Route route, Owns owns, std::string& error)
{
    error.clear(); auto& session = impl_->session;
    if (current || session.state.enabled) { error = "private_pointer_busy"; return false; }
    if (IsRectEmpty(&bounds) || !PtInRect(&bounds, initial) || !route || !owns)
    { error = "invalid_private_pointer_session"; return false; }
    const DWORD thread = GetCurrentThreadId();
    if (session.thread && session.thread != thread) { error = "private_pointer_wrong_thread"; return false; }
    if (!session.dispatcher)
    {
        WNDCLASSW wc{}; wc.lpfnWndProc = Session::WindowProcedure;
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = DispatcherClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        { error = "private_pointer_dispatch_unavailable"; return false; }
        session.dispatcher = CreateWindowExW(0, DispatcherClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, wc.hInstance, &session);
        if (!session.dispatcher) { error = "private_pointer_dispatch_unavailable"; return false; }
    }
    session.state = {}; session.state.enabled = true; session.state.bounds = bounds; session.state.point = initial;
    session.route = std::move(route); session.owns = std::move(owns); session.thread = thread;
    session.projectedPoint = initial; session.projectedButtons = 0;
    session.pending.clear(); session.nextSequence = 0; session.lastClickEligible = false;
    session.started = ClockMilliseconds(); current = &session;
    if (!session.Queue({Kind::Move, initial}, error)) { session.End(); return false; }
    return true;
}
bool Device::Move(POINT point, std::string& error)
{
    auto& session = impl_->session;
    if (!PtInRect(&session.state.bounds, point)) { error = "private_pointer_outside_bounds"; return false; }
    if (!session.Queue({Kind::Move, point}, error)) return false;
    session.projectedPoint = point; return true;
}
bool Device::Down(Button button, std::string& error)
{
    auto& session = impl_->session; const unsigned bit = static_cast<unsigned>(button);
    if (!ValidButton(button)) { error = "invalid_private_pointer_button"; return false; }
    if (session.projectedButtons & bit) { error = "private_pointer_button_already_down"; return false; }
    if (!session.Queue({Kind::Down, session.projectedPoint, button}, error)) return false;
    session.projectedButtons |= bit; return true;
}
bool Device::Up(Button button, std::string& error)
{
    auto& session = impl_->session; const unsigned bit = static_cast<unsigned>(button);
    if (!ValidButton(button)) { error = "invalid_private_pointer_button"; return false; }
    if (!(session.projectedButtons & bit)) { error = "private_pointer_button_not_down"; return false; }
    if (!session.Queue({Kind::Up, session.projectedPoint, button}, error)) return false;
    session.projectedButtons &= ~bit; return true;
}
bool Device::Wheel(int delta, std::string& error)
{
    if (!delta || delta < -32768 || delta > 32767) { error = "invalid_private_pointer_wheel"; return false; }
    return impl_->session.Queue({Kind::Wheel, impl_->session.projectedPoint, Button::Left, delta}, error);
}
void Device::End() noexcept
{
    auto& session = impl_->session;
    if (!session.thread || session.thread == GetCurrentThreadId()) session.End();
}
void Device::SetObserver(Observe observe) { impl_->session.observe = std::move(observe); }

bool Active() noexcept { return current && current->state.enabled; }
bool Enabled() noexcept { return Active(); }
bool IsDispatchingVirtualInput() noexcept { return Active() && current->dispatchDepth != 0; }
bool MessageAllowed(HWND window, UINT message) noexcept
{
    if (!Active() || !PointerMessage(message)) return true;
    if (permit.available && permit.window == window && permit.message == message)
    { permit.available = false; return true; }
    return false;
}
ForwardScope::ForwardScope(HWND destination, UINT message) noexcept
{
    if (!IsDispatchingVirtualInput() || !current->Owned(destination)) return;
    previousWindow_ = permit.window; previousMessage_ = permit.message; previousAvailable_ = permit.available;
    permit = {destination, message, true}; armed_ = true;
}
ForwardScope::~ForwardScope()
{
    if (armed_) permit = {previousWindow_, previousMessage_, previousAvailable_};
}
BOOL CursorPosition(LPPOINT point) noexcept
{
    if (!Active()) return GetCursorPos(point);
    if (!point) return FALSE;
    *point = current->state.point; return TRUE;
}
SHORT KeyState(int key) noexcept
{
    if (!Active()) return GetKeyState(key);
    const unsigned bit = key == VK_LBUTTON ? 1U : key == VK_RBUTTON ? 2U : key == VK_MBUTTON ? 4U : 0U;
    return static_cast<SHORT>(bit && (current->state.buttons & bit) ? 0x8000 : 0);
}
SHORT AsyncKeyState(int key) noexcept
{
    if (!Active()) return GetAsyncKeyState(key);
    if (GetSystemMetrics(SM_SWAPBUTTON))
    {
        if (key == VK_LBUTTON) key = VK_RBUTTON;
        else if (key == VK_RBUTTON) key = VK_LBUTTON;
    }
    return KeyState(key);
}
HWND Capture() noexcept
{
    if (!Active()) return GetCapture();
    return current->Owned(current->state.captured) ? current->state.captured : nullptr;
}
HWND SetPointerCapture(HWND window) noexcept
{
    if (!Active()) return SetCapture(window);
    const HWND previous = Capture();
    if (!current->Owned(window)) return previous;
    current->state.captured = window;
    if (previous != window) current->Deliver(previous, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(window));
    return previous;
}
BOOL ReleasePointerCapture() noexcept
{
    if (!Active()) return ReleaseCapture();
    const HWND previous = Capture(); current->state.captured = nullptr;
    current->Deliver(previous, WM_CAPTURECHANGED); return TRUE;
}
HWND WindowAtPoint(POINT point) noexcept { return Active() ? current->Hit(point) : WindowFromPoint(point); }
BOOL TrackLeave(LPTRACKMOUSEEVENT tracking) noexcept
{
    if (!Active()) return TrackMouseEvent(tracking);
    return tracking && tracking->cbSize == sizeof(TRACKMOUSEEVENT) && current->Owned(tracking->hwndTrack);
}
HWND FocusWindow() noexcept
{
    if (!Active()) return GetFocus();
    return current->Owned(current->state.focused) ? current->state.focused : nullptr;
}
HWND SetInputFocus(HWND window) noexcept
{
    if (!Active()) return SetFocus(window);
    const HWND previous = FocusWindow();
    if (!window || current->Owned(window)) current->state.focused = window;
    return previous;
}
HWND ForegroundWindow() noexcept
{
    if (!Active()) return GetForegroundWindow();
    return current->Owned(current->state.foreground) ? current->state.foreground : nullptr;
}
BOOL SetInputForeground(HWND window) noexcept
{
    if (!Active()) return SetForegroundWindow(window);
    if (!current->Owned(window)) return FALSE;
    current->state.foreground = window; return TRUE;
}
HCURSOR Cursor(HCURSOR cursor) noexcept
{
    if (!Active()) return SetCursor(cursor);
    const auto previous = current->state.cursor; current->state.cursor = cursor; return previous;
}
}
