#include "ui/input/private_pointer.h"
#include "ui/input/text_input_window.h"

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <windowsx.h>
#include <objbase.h>

namespace
{
namespace pointer = snowdesktop::private_pointer;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool Same(POINT a, POINT b) { return a.x == b.x && a.y == b.y; }

struct NativeControl
{
    HWND window = nullptr;
    RECT button{10, 10, 120, 70};
    bool hovered = false, pressed = false, badInputState = false, modal = false;
    bool* modalExit = nullptr;
    bool cancelExitsModal = false;
    std::function<void()> onModal;
    std::function<void()> onFocusLost;
    std::function<void(HWND)> onChange;
    bool focused = false;
    unsigned focusGains = 0, focusLosses = 0;
    unsigned keyDowns = 0, keyUps = 0, characterMessages = 0;
    bool ctrlAtPress = false, ctrlAtMove = false, ctrlAtRelease = false;
    bool shiftAtMove = false;
    HWND modalRow = nullptr;
    HWND forwardWindow = nullptr;
    bool embedded = false;
    POINT embeddedOrigin{};
    unsigned moves = 0, clicks = 0, doubles = 0, cancelled = 0, blocked = 0, wheels = 0, leaves = 0;
    POINT lastScreen{}, lastClient{};
    int wheel = 0;

    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* control = reinterpret_cast<NativeControl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            control = static_cast<NativeControl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(control));
        }
        if (!control) return DefWindowProcW(window, message, wp, lp);
        if (!pointer::MessageAllowed(window, message, wp, lp)) { ++control->blocked; return 0; }
        POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (message)
        {
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && control->onChange) control->onChange(reinterpret_cast<HWND>(lp));
            return 0;
        case WM_KEYDOWN:
            ++control->keyDowns;
            if (!(pointer::KeyState(static_cast<int>(wp)) & 0x8000)) control->badInputState = true;
            return 0;
        case WM_KEYUP:
            ++control->keyUps;
            if (pointer::KeyState(static_cast<int>(wp)) & 0x8000) control->badInputState = true;
            return 0;
        case WM_CHAR: ++control->characterMessages; return 0;
        case WM_SETFOCUS:
            ++control->focusGains; control->focused = true;
            if (pointer::FocusWindow() != window) control->badInputState = true;
            return 0;
        case WM_KILLFOCUS:
            ++control->focusLosses; control->focused = false;
            if (pointer::FocusWindow() == window) control->badInputState = true;
            if (control->onFocusLost) control->onFocusLost();
            return 0;
        case WM_MOUSEMOVE:
        {
            ++control->moves;
            if (wp & MK_LBUTTON) { control->ctrlAtMove = (wp & MK_CONTROL) != 0; control->shiftAtMove = (wp & MK_SHIFT) != 0; }
            if (((wp & MK_CONTROL) != 0) != ((pointer::KeyState(VK_CONTROL) & 0x8000) != 0)) control->badInputState = true;
            control->lastClient = client;
            pointer::CursorPosition(&control->lastScreen);
            POINT expected = client;
            if (control->embedded)
            { expected.x += control->embeddedOrigin.x; expected.y += control->embeddedOrigin.y; }
            else ClientToScreen(window, &expected);
            if (!Same(expected, control->lastScreen)) control->badInputState = true;
            control->hovered = PtInRect(&control->button, client) != FALSE;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            if (!pointer::TrackLeave(&tracking)) control->badInputState = true;
            if (control->forwardWindow)
            {
                POINT forwarded = control->lastScreen;
                ScreenToClient(control->forwardWindow, &forwarded);
                const pointer::ForwardScope forward(control->forwardWindow, message);
                const LPARAM position = MAKELPARAM(static_cast<short>(forwarded.x), static_cast<short>(forwarded.y));
                SendMessageW(control->forwardWindow, message, wp, position);
                // Even within the forwarding scope, the exact permit may be
                // consumed only once; nested/untagged delivery remains blocked.
                SendMessageW(control->forwardWindow, message, wp, position);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
        {
            POINT sampled{};
            if (pointer::CursorPosition(&sampled) && pointer::WindowAtPoint(sampled) == window) return 0;
            control->hovered = false; ++control->leaves; return 0;
        }
        case WM_LBUTTONDBLCLK: ++control->doubles; [[fallthrough]];
        case WM_LBUTTONDOWN:
            control->ctrlAtPress = (wp & MK_CONTROL) != 0;
            if (!(pointer::KeyState(VK_LBUTTON) & 0x8000) || !(wp & MK_LBUTTON)) control->badInputState = true;
            control->pressed = PtInRect(&control->button, client) != FALSE;
            pointer::SetPointerCapture(window);
            pointer::SetInputFocus(window);
            pointer::SetInputForeground(window);
            pointer::Cursor(LoadCursorW(nullptr, IDC_HAND));
            if (pointer::Capture() != window || pointer::FocusWindow() != window || pointer::ForegroundWindow() != window)
                control->badInputState = true;
            return 0;
        case WM_LBUTTONUP:
            control->ctrlAtRelease = (wp & MK_CONTROL) != 0 && (pointer::KeyState(VK_CONTROL) & 0x8000) != 0;
            if ((pointer::KeyState(VK_LBUTTON) & 0x8000) || (wp & MK_LBUTTON)) control->badInputState = true;
            if (control->pressed && PtInRect(&control->button, client)) ++control->clicks;
            control->pressed = false;
            pointer::ReleasePointerCapture();
            if (control->modalExit) *control->modalExit = true;
            return 0;
        case WM_MOUSEWHEEL:
            ++control->wheels; control->wheel += GET_WHEEL_DELTA_WPARAM(wp);
            pointer::CursorPosition(&control->lastScreen);
            if (!Same(client, control->lastScreen)) control->badInputState = true;
            return 0;
        case WM_CANCELMODE:
            control->pressed = false; ++control->cancelled;
            if (control->cancelExitsModal && control->modalExit) *control->modalExit = true;
            return 0;
        case WM_CAPTURECHANGED: control->pressed = false; return 0;
        case WM_RBUTTONUP:
            if (control->modal && control->modalExit)
            {
                if (control->onModal) control->onModal();
                if (control->modalRow)
                {
                    PostMessageW(control->modalRow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 30));
                    PostMessageW(control->modalRow, WM_LBUTTONUP, 0, MAKELPARAM(40, 30));
                }
                const ULONGLONG deadline = GetTickCount64() + 1500;
                while (!*control->modalExit && GetTickCount64() < deadline)
                {
                    MSG next{};
                    if (PeekMessageW(&next, nullptr, 0, 0, PM_REMOVE))
                    { TranslateMessage(&next); DispatchMessageW(&next); }
                    else Sleep(1);
                }
                if (!*control->modalExit) control->badInputState = true;
            }
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    NativeControl(int x, int y)
    {
        WNDCLASSW wc{}; wc.style = CS_DBLCLKS; wc.lpfnWndProc = Procedure;
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"SnowDesktop.PrivatePointer.Tests";
        Check(RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register native fixture");
        // A real but hidden HWND exercises the ordinary WndProc and coordinate
        // conversion without creating a visible/focused window on the user's desk.
        window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"private input fixture",
            WS_POPUP, x, y, 400, 300, nullptr, nullptr, wc.hInstance, this);
        Check(window != nullptr, "create native fixture");
    }
    ~NativeControl() { if (IsWindow(window)) DestroyWindow(window); }
    POINT Point(int x = 40, int y = 30) const
    {
        if (embedded) return {embeddedOrigin.x + x, embeddedOrigin.y + y};
        POINT point{ x, y }; Check(ClientToScreen(window, &point) != FALSE, "fixture screen geometry"); return point;
    }
};

void Drain(pointer::Device& device)
{
    const ULONGLONG deadline = GetTickCount64() + 2500;
    while (device.Current().pendingEvents && GetTickCount64() < deadline)
    {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&message); DispatchMessageW(&message); }
        else Sleep(1);
    }
    Check(!device.Current().pendingEvents, "private event queue must complete");
}

struct RealInput
{
    POINT cursor{};
    HWND capture = GetCapture(), focus = GetFocus(), foreground = GetForegroundWindow();
    HCURSOR cursorImage = nullptr;
    std::array<BYTE, 256> keyboard{};
    RealInput()
    {
        Check(GetCursorPos(&cursor) != FALSE, "read system cursor");
        CURSORINFO info{}; info.cbSize = sizeof(info);
        Check(GetCursorInfo(&info) != FALSE, "read system cursor image"); cursorImage = info.hCursor;
        Check(GetKeyboardState(keyboard.data()) != FALSE, "read system keyboard state");
    }
    void Unchanged() const
    {
        POINT after{}; Check(GetCursorPos(&after) != FALSE && Same(after, cursor), "software input changed system cursor position");
        Check(GetCapture() == capture, "software input changed system capture");
        Check(GetFocus() == focus && GetForegroundWindow() == foreground, "software input changed system focus/foreground");
        CURSORINFO info{}; info.cbSize = sizeof(info);
        Check(GetCursorInfo(&info) != FALSE && info.hCursor == cursorImage, "software input changed system cursor image");
        std::array<BYTE, 256> keys{};
        Check(GetKeyboardState(keys.data()) != FALSE && keys == keyboard, "private input changed system keyboard state");
    }
};

void NativeMessages()
{
    NativeControl first(-20000, -18000), second(-19000, -18000);
    std::vector<NativeControl*> windows{&first, &second};
    const auto route = [&](POINT point) -> HWND {
        for (auto* control : windows)
        {
            RECT bounds{};
            if (IsWindow(control->window) && GetWindowRect(control->window, &bounds) && PtInRect(&bounds, point)) return control->window;
        }
        return nullptr;
    };
    const auto owns = [&](HWND window) {
        for (auto* control : windows) if (control->window == window) return true;
        return false;
    };
    const RECT bounds{-30000, -30000, 30000, 30000};
    pointer::Device device; std::string error;
    Check(!device.Begin({}, {}, route, owns, error) && !pointer::Enabled(), "invalid session cannot become active");
    const RealInput real;
    unsigned observations = 0;
    device.SetObserver([&](const pointer::State&) { ++observations; });
    Check(device.Begin(bounds, first.Point(), route, owns, error), "begin private pointer");
    Drain(device);
    Check(first.hovered && first.moves == 1 && device.Current().point.x < 0, "initial move must produce real hover at negative coordinates");
    Check(!device.Move({bounds.right, 0}, error) && error == "private_pointer_outside_bounds", "exclusive monitor edge is unavailable");
    Check(device.Down(pointer::Button::Left, error), "queue down");
    Check(!device.Down(pointer::Button::Left, error), "duplicate down rejected before dispatch");
    Drain(device);
    Check(first.pressed && pointer::Capture() == first.window && device.Current().buttons == 1, "ordinary down establishes logical capture");
    Check(device.Move(second.Point(), error), "move under capture"); Drain(device);
    Check(first.moves == 2 && second.moves == 0 && device.Current().hovered == second.window, "capture routes movement while hit target changes");
    Check(device.Up(pointer::Button::Left, error), "queue captured release"); Drain(device);
    Check(first.clicks == 0 && !first.pressed && !pointer::Capture(), "release outside the button does not click");
    Check(device.Move(second.Point(), error), "move second");
    Check(device.Down(pointer::Button::Left, error) && device.Up(pointer::Button::Left, error), "click through ordinary down/up");
    Drain(device); Check(second.clicks == 1, "native button activates from private input");
    Check(device.Down(pointer::Button::Left, error) && device.Up(pointer::Button::Left, error), "second click");
    Drain(device); Check(second.doubles == 1, "native double click message is delivered");
    Check(device.Wheel(-240, error), "queue wheel"); Drain(device);
    Check(second.wheels == 1 && second.wheel == -240, "native wheel preserves screen point and delta");
    const unsigned clicks = second.clicks, moves = second.moves;
    PostMessageW(second.window, WM_MOUSEMOVE, 0, MAKELPARAM(20, 20));
    PostMessageW(second.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
    PostMessageW(second.window, WM_LBUTTONUP, 0, MAKELPARAM(20, 20));
    PostMessageW(second.window, WM_KEYDOWN, VK_SHIFT, 0);
    Check(device.Move(second.Point(), error), "drive queue to pump untagged messages"); Drain(device);
    Check(second.blocked >= 4 && second.clicks == clicks && second.moves == moves + 1, "real application input cannot contaminate private input");
    Check(pointer::KeyState(VK_SHIFT) == 0 && pointer::AsyncKeyState(VK_CONTROL) == 0, "software modifiers are independent of hardware");
    Check(device.Down(pointer::Button::Left, error), "press to cancel"); Drain(device);
    device.End();
    Check(!device.Current().enabled && !device.Current().buttons && !second.pressed && !second.hovered && !second.focused && second.cancelled == 1,
        "end cancels a held press and resampled hover without synthesizing a click");
    Check(second.clicks == clicks && observations > 6, "cancellation does not activate button and poses were observed");
    Check(!first.badInputState && !second.badInputState, "normal WndProc observes coherent private state");
    real.Unchanged();
}

void ModalQueueAndRejections()
{
    NativeControl menu(-20000, -17000), row(-19000, -17000);
    bool selected = false; menu.modal = true; menu.modalExit = row.modalExit = &selected; menu.modalRow = row.window;
    const RECT bounds{-30000, -30000, 30000, 30000};
    const auto route = [&](POINT point) { return point.x < -19500 ? menu.window : row.window; };
    const auto owns = [&](HWND window) { return window == menu.window || window == row.window; };
    pointer::Device device; std::string error;
    const RealInput real;
    Check(device.Begin(bounds, menu.Point(), route, owns, error), "begin modal fixture");
    Check(device.Down(pointer::Button::Right, error) && device.Up(pointer::Button::Right, error), "queue context press/release");
    // These are deliberately ahead of the queued private click while the right
    // release's WndProc runs its nested message loop. Its dispatch depth is >0,
    // but a consumed exact-message permit must still reject this real input.
    Check(device.Move(row.Point(), error) && device.Down(pointer::Button::Left, error) && device.Up(pointer::Button::Left, error), "queue native row selection");
    Drain(device);
    Check(selected && row.clicks == 1 && row.blocked == 2 && !menu.badInputState, "nested menu accepts only its private queued input");
    device.End(); real.Unchanged();

    Check(device.Begin(bounds, row.Point(), route, owns, error), "restart device"); Drain(device);
    auto wrongThread = std::async(std::launch::async, [&] {
        std::string threadError; return !device.Move(row.Point(), threadError);
    });
    Check(wrongThread.get(), "another UI thread cannot enqueue into a session");
    Check(device.Down(pointer::Button::Left, error), "press before target destruction"); Drain(device);
    DestroyWindow(row.window);
    Check(device.Move(menu.Point(), error), "move after captured target destroyed"); Drain(device);
    Check(device.Current().error == "private_pointer_capture_destroyed" && !device.Current().buttons && !pointer::Capture(),
        "destroyed capture is rejected and cleared without contacting another window");
    device.End();

    Check(device.Begin(bounds, menu.Point(), [](POINT) { return GetDesktopWindow(); }, [](HWND) { return true; }, error), "begin foreign-route fixture");
    Drain(device);
    Check(device.Down(pointer::Button::Left, error), "queue against foreign route"); Drain(device);
    Check(device.Current().error == "private_pointer_target_unavailable" && !device.Current().buttons,
        "broad owner callbacks cannot admit another process's window");
    device.End();

    Check(device.Begin(bounds, menu.Point(), route, owns, error), "begin observer failure fixture"); Drain(device);
    device.SetObserver([](const pointer::State&) { throw std::runtime_error("fixture observer failure"); });
    Check(device.Down(pointer::Button::Left, error), "queue observer failure"); Drain(device);
    Check(!device.Current().enabled && !device.Current().buttons && device.Current().error == "private_pointer_observer_failed",
        "observer failure cancels the session and releases only logical state");
}

void BoundedQueueAndObserverCancellation()
{
    NativeControl control(-16000, -16000);
    pointer::Device device; std::string error;
    const RECT bounds{-30000, -30000, 30000, 30000};
    const auto route = [&](POINT) { return control.window; };
    const auto owns = [&](HWND window) { return window == control.window; };
    Check(device.Begin(bounds, control.Point(), route, owns, error), "begin bounded queue fixture");
    for (unsigned index = 0; index < 511; ++index)
        Check(device.Move(control.Point(), error), "bounded queue accepts its documented capacity");
    Check(!device.Move(control.Point(), error) && error == "private_pointer_queue_full", "unbounded IPC producers cannot grow queue memory");
    device.End();
    Check(!device.Current().pendingEvents && !control.moves, "end discards queued input before any side effect");
    Check(device.Begin(bounds, control.Point(), route, owns, error), "restart with a stale queued dispatcher notification");
    Drain(device);
    Check(control.moves == 1, "stale dispatcher wake cannot replay a cancelled session's commands");
    const RealInput real;
    device.SetObserver([&](const pointer::State& state) { if (state.buttons) device.End(); });
    Check(device.Down(pointer::Button::Left, error), "queue observer-driven cancellation"); Drain(device);
    Check(!device.Current().enabled && !device.Current().buttons && !control.pressed && !control.clicks,
        "an observer cancelling before delivery cannot cause fallback physical input or click");
    real.Unchanged();
}

void InputFamiliesAndForwarding()
{
    NativeControl first(-14000, -14000), forwarded(-13000, -14000);
    first.forwardWindow = forwarded.window;
    pointer::Device device; std::string error;
    const RECT bounds{-30000, -30000, 30000, 30000};
    Check(device.Begin(bounds, first.Point(), [&](POINT) { return first.window; },
        [&](HWND window) { return window == first.window || window == forwarded.window; }, error), "begin forwarding fixture");
    Drain(device);
    Check(first.moves == 1 && forwarded.moves == 1 && forwarded.blocked == 1 && !forwarded.badInputState,
        "owned synchronous forwarding admits exactly one native delivery");
    {
        const pointer::ForwardScope outsideDispatch(first.window, WM_MOUSEMOVE);
        SendMessageW(first.window, WM_MOUSEMOVE, 0, MAKELPARAM(40, 30));
    }
    Check(first.moves == 1 && first.blocked == 1, "an idle forwarding scope cannot authorize real input");

    const UINT blockedMessages[]{WM_MOUSEHOVER, WM_NCMOUSEHOVER, WM_CONTEXTMENU,
        WM_NCPOINTERUPDATE, WM_NCPOINTERDOWN, WM_NCPOINTERUP,
        WM_POINTERUPDATE, WM_POINTERDOWN, WM_POINTERUP, WM_POINTERENTER, WM_POINTERLEAVE,
        WM_POINTERACTIVATE, WM_POINTERCAPTURECHANGED, WM_TOUCHHITTESTING,
        WM_POINTERWHEEL, WM_POINTERHWHEEL, WM_POINTERROUTEDTO, WM_POINTERROUTEDAWAY,
        WM_POINTERROUTEDRELEASED, WM_GESTURENOTIFY, WM_IME_CHAR, WM_IME_COMPOSITION,
        WM_TOUCH, WM_GESTURE, WM_INPUT};
    for (const UINT message : blockedMessages)
    {
        // No system input is injected. Handle-bearing messages have no real
        // resource here; their valid-handle disposal follows the Win32 contract
        // in production and is not claimed as a synthetic touch-device test.
        const WPARAM wp = message == WM_INPUT ? RIM_INPUTSINK : 0;
        SendMessageW(first.window, message, wp, 0);
    }
    Check(first.blocked == 1 + std::size(blockedMessages) && first.moves == 1,
        "complete pointer, hover, context, gesture and raw-input families remain private");
    device.End();
    Check(!first.hovered && !device.Current().enabled, "ending a resampled desktop hover clears its native visual state");
}

void ExplicitEmbeddedCoordinates()
{
    NativeControl proxy(-32000, -32000), ordinary(1500, 1000);
    proxy.embedded = true; proxy.embeddedOrigin = {1500, 1000};
    pointer::Device device; std::string error;
    const RECT bounds{-30000, -30000, 30000, 30000};
    const auto route = [&](POINT) { return proxy.window; };
    const auto owns = [&](HWND window) { return window == proxy.window || window == ordinary.window; };
    const RealInput real;
    Check(device.Begin(bounds, proxy.Point(), route, owns, error), "begin offscreen proxy without mapping"); Drain(device);
    Check(device.Current().error == "private_pointer_coordinate_range" && !proxy.moves,
        "unmapped offscreen proxy cannot silently wrap native 16-bit coordinates");
    device.End();

    const auto map = [&](HWND window, POINT screen, POINT& client) {
        if (window != proxy.window) { client = screen; return ScreenToClient(window, &client) != FALSE; }
        client = {screen.x - proxy.embeddedOrigin.x, screen.y - proxy.embeddedOrigin.y}; return true;
    };
    Check(device.Begin(bounds, proxy.Point(), route, owns, error, map), "begin explicitly mapped proxy"); Drain(device);
    Check(proxy.hovered && proxy.moves == 1 && !proxy.badInputState, "mapped proxy uses its visible host coordinates for real hover");
    Check(device.Down(pointer::Button::Left, error) && device.Up(pointer::Button::Left, error), "queue mapped proxy click"); Drain(device);
    Check(proxy.clicks == 1 && !proxy.badInputState && !device.Current().buttons,
        "mapped captured proxy receives coherent down/up without moving its physical HWND");
    device.End();

    Check(device.Begin(bounds, ordinary.Point(), [&](POINT) { return ordinary.window; }, owns, error,
        [](HWND, POINT, POINT&) { return false; }), "begin mapper rejection"); Drain(device);
    Check(device.Current().error == "private_pointer_coordinate_mapping_failed" && !ordinary.moves,
        "a rejecting mapper never falls back to otherwise valid ScreenToClient geometry");
    device.End();
    Check(device.Begin(bounds, ordinary.Point(), [&](POINT) { return ordinary.window; }, owns, error,
        [](HWND, POINT, POINT& client) { client = {40000, 0}; return true; }), "begin out-of-range mapper"); Drain(device);
    Check(device.Current().error == "private_pointer_coordinate_range" && !ordinary.moves,
        "a mapper cannot bypass the native coordinate range check");
    device.End(); real.Unchanged();
}

void EndNewModalWithoutMovement()
{
    NativeControl desktop(-12000, -12000), popup(-11000, -12000);
    bool popupShown = false, modalExited = false;
    desktop.modal = true; desktop.modalExit = popup.modalExit = &modalExited;
    popup.cancelExitsModal = true;
    pointer::Device device; std::string error;
    desktop.onModal = [&] { popupShown = true; device.End(); };
    const RECT bounds{-30000, -30000, 30000, 30000};
    const RealInput real;
    Check(device.Begin(bounds, desktop.Point(), [&](POINT) { return popupShown ? popup.window : desktop.window; },
        [&](HWND window) { return window == desktop.window || window == popup.window; }, error), "begin new modal cancellation fixture");
    Check(device.Down(pointer::Button::Right, error) && device.Up(pointer::Button::Right, error), "open a modal with a real right release");
    Drain(device);
    Check(popupShown && modalExited && popup.cancelled == 1 && desktop.cancelled == 1 && !desktop.badInputState,
        "end cancels a newly opened modal before another move updates hovered HWND");
    Check(!device.Current().enabled && !device.Current().closing && !device.Current().buttons,
        "ending inside the new modal unwinds the original private dispatch");
    real.Unchanged();
}

void ReentrantLogicalFocus()
{
    NativeControl first(-10000, -10000), proposed(-9000, -10000), redirected(-8000, -10000);
    pointer::Device device; std::string error;
    const RECT bounds{-30000, -30000, 30000, 30000};
    const auto route = [&](POINT) { return first.window; };
    const auto owns = [&](HWND window) { return window == first.window || window == proposed.window || window == redirected.window; };
    const RealInput real;
    Check(device.Begin(bounds, first.Point(), route, owns, error), "begin focus redirection fixture"); Drain(device);
    pointer::SetInputFocus(first.window);
    first.onFocusLost = [&] { pointer::SetInputFocus(redirected.window); };
    pointer::SetInputFocus(proposed.window);
    Check(pointer::FocusWindow() == redirected.window && first.focusGains == 1 && first.focusLosses == 1 &&
        proposed.focusGains == 0 && redirected.focusGains == 1 && redirected.focused,
        "kill-focus redirection prevents a stale set-focus notification");
    SendMessageW(redirected.window, WM_KILLFOCUS, 0, 0);
    Check(redirected.focused && redirected.blocked == 1, "physical focus notifications cannot clear the private caret");
    device.End();
    Check(redirected.focusLosses == 1 && !redirected.focused && !device.Current().enabled,
        "end sends native kill-focus before leaving private mode");

    first.onFocusLost = {};
    Check(device.Begin(bounds, first.Point(), route, owns, error), "restart reentrant-end focus fixture"); Drain(device);
    pointer::SetInputFocus(first.window);
    first.onFocusLost = [&] { device.End(); };
    pointer::SetInputFocus(proposed.window);
    Check(!device.Current().enabled && !device.Current().closing && !device.Current().focused && proposed.focusGains == 0,
        "ending during kill-focus cancels the proposed focus and closes a non-pointer dispatch");
    Check(!first.badInputState && !proposed.badInputState && !redirected.badInputState,
        "native focus notifications observe the updated logical target");
    real.Unchanged();
}

void PrivateKeyboardAndControlDrag()
{
    NativeControl control(-7000, -7000);
    pointer::Device device; std::string error;
    const RECT bounds{-30000, -30000, 30000, 30000};
    const RealInput real;
    Check(device.Begin(bounds, control.Point(), [&](POINT) { return control.window; },
        [&](HWND window) { return window == control.window; }, error), "begin keyboard and drag fixture"); Drain(device);
    Check(device.KeyDown(VK_CONTROL, error), "queue modifier without focus"); Drain(device);
    Check(device.Current().error == "private_keyboard_focus_unavailable" && !device.Current().keys[VK_CONTROL] && !control.keyDowns,
        "keyboard has no implicit desktop fallback when focus is missing");
    Check(device.KeyUp(VK_CONTROL, error), "clear rejected projected modifier"); Drain(device);
    pointer::SetInputFocus(control.window);
    Check(device.KeyDown(VK_CONTROL, error) && device.KeyDown(VK_SHIFT, error), "hold private Ctrl and Shift");
    Check(!device.KeyDown(VK_CONTROL, error), "duplicate private key down rejected");
    Check(device.Down(pointer::Button::Left, error) && device.Move(control.Point(80, 40), error) &&
        device.Up(pointer::Button::Left, error) && device.KeyUp(VK_SHIFT, error) && device.KeyUp(VK_CONTROL, error),
        "queue Ctrl drag with modifier release after mouse release");
    Drain(device);
    Check(control.ctrlAtPress && control.ctrlAtMove && control.shiftAtMove && control.ctrlAtRelease && !control.badInputState,
        "normal native press, drag and mouse-up observe held private modifiers");
    Check(!device.Current().keys[VK_CONTROL] && !device.Current().keys[VK_SHIFT] && control.keyDowns == 2 && control.keyUps == 2,
        "private keyboard releases are delivered without changing OS keys");
    const auto keyUps = control.keyUps;
    Check(device.KeyDown(VK_CONTROL, error) && device.KeyDown(VK_RETURN, error), "hold modifier and activation key before cancellation"); Drain(device);
    device.End();
    Check(control.keyUps == keyUps + 1 && !device.Current().keys[VK_CONTROL] && !device.Current().keys[VK_RETURN],
        "end clears every virtual key and only releases the required modifier");
    real.Unchanged();
}

std::wstring EditorText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied)); return text;
}

void ProductionTextInputKeyboard()
{
    namespace editor = snowdesktop::text_input;
    NativeControl owner(-6000, -6000), other(-5000, -6000);
    const HWND input = CreateWindowExW(0, editor::WindowClass(), L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 240, 44, owner.window, reinterpret_cast<HMENU>(101), GetModuleHandleW(nullptr), nullptr);
    Check(input != nullptr, "create the production native text editor");
    struct InputGuard { HWND window; ~InputGuard() { if (IsWindow(window)) DestroyWindow(window); } } inputGuard{input};
    const RECT frame{20, 30, 340, 72};
    editor::SetEmbeddedPose(input, frame, frame, true, true);
    pointer::Device device; std::string error;
    const RECT bounds{-30000, -30000, 30000, 30000};
    const auto route = [&](POINT) { return input; };
    const auto owns = [&](HWND window) { return window == owner.window || window == other.window || (window == input && editor::IsPrivatePointerHost(input)); };
    const RealInput real;
    Check(device.Begin(bounds, owner.Point(50, 45), route, owns, error,
        [](HWND window, POINT screen, POINT& client) { return editor::MapPrivatePointerPoint(window, screen, client); }), "begin production text input");
    Check(device.Down(pointer::Button::Left, error) && device.Up(pointer::Button::Left, error), "focus production editor through native mouse input"); Drain(device);
    Check(pointer::FocusWindow() == input && device.Current().error.empty(), "normal text-window handler establishes private logical focus");
    Check(!device.Text(std::wstring(1, static_cast<wchar_t>(0xd800)), error), "dangling high surrogate rejected before queueing");
    Check(!device.Text(std::wstring(1, static_cast<wchar_t>(0xdc00)), error), "unpaired low surrogate rejected before queueing");
    Check(!device.Text(std::wstring(4097, L'x'), error) && !device.Text(std::wstring(1, L'\0'), error), "oversized and null text rejected");
    Check(device.Text(L"article rename", error), "queue initial real text"); Drain(device);
    Check(EditorText(input) == L"article rename", "WM_CHAR updates the production text model");
    Check(device.KeyDown(VK_CONTROL, error) && device.KeyDown('A', error) && device.KeyUp('A', error) && device.KeyUp(VK_CONTROL, error), "select all through private Ctrl+A");
    Check(device.Text(L"搜索\U0001F60A", error), "type valid CJK and surrogate pair"); Drain(device);
    Check(EditorText(input) == L"搜索\U0001F60A", "production Ctrl+A and Unicode text replace the actual selected range");
    Check(device.KeyDown(VK_BACK, error) && device.KeyUp(VK_BACK, error), "backspace through native key handler"); Drain(device);
    Check(EditorText(input) == L"搜索", "production backspace removes the full emoji grapheme");
    Check(device.KeyDown(VK_RETURN, error) && device.KeyUp(VK_RETURN, error), "route Return to the same actual focused editor"); Drain(device);
    Check(EditorText(input) == L"搜索" && device.Current().error.empty(), "raw Return does not manufacture a character or redirect to desktop");
    Check(device.KeyDown(VK_CONTROL, error) && device.KeyDown('A', error) && device.KeyUp('A', error) && device.KeyUp(VK_CONTROL, error), "select before reentrant focus test"); Drain(device);
    owner.onChange = [&](HWND changed) { if (changed == input) pointer::SetInputFocus(other.window); };
    Check(device.Text(L"AB", error), "queue text with real EN_CHANGE focus redirection"); Drain(device);
    Check(EditorText(input) == L"A" && !other.characterMessages && device.Current().error == "private_keyboard_focus_changed",
        "text stops on a real focus change without sending remaining characters elsewhere");
    owner.onChange = {};
    pointer::SetInputFocus(input);
    Check(device.Text(L"not delivered", error), "queue characters before cancellation");
    device.End();
    Check(EditorText(input) == L"A" && !device.Current().pendingEvents && !device.Current().focused,
        "end drops queued characters and cleans the production editor focus");
    real.Unchanged();
}
}

int main()
{
    try
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Check(SUCCEEDED(apartment), "initialize text-input test STA");
        struct Apartment { ~Apartment() { CoUninitialize(); } } apartmentGuard;
        NativeMessages(); ModalQueueAndRejections(); BoundedQueueAndObserverCancellation(); InputFamiliesAndForwarding(); ExplicitEmbeddedCoordinates(); EndNewModalWithoutMovement(); ReentrantLogicalFocus();
        PrivateKeyboardAndControlDrag(); ProductionTextInputKeyboard();
        std::cout << "Private pointer native-message checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
