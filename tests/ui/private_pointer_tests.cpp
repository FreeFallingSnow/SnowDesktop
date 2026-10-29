#include "ui/input/private_pointer.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windowsx.h>

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
    HWND modalRow = nullptr;
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
        if (!pointer::MessageAllowed(window, message)) { ++control->blocked; return 0; }
        POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (message)
        {
        case WM_MOUSEMOVE:
        {
            ++control->moves;
            control->lastClient = client;
            pointer::CursorPosition(&control->lastScreen);
            POINT expected = client;
            ClientToScreen(window, &expected);
            if (!Same(expected, control->lastScreen)) control->badInputState = true;
            control->hovered = PtInRect(&control->button, client) != FALSE;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            if (!pointer::TrackLeave(&tracking)) control->badInputState = true;
            return 0;
        }
        case WM_MOUSELEAVE: control->hovered = false; ++control->leaves; return 0;
        case WM_LBUTTONDBLCLK: ++control->doubles; [[fallthrough]];
        case WM_LBUTTONDOWN:
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
        case WM_CANCELMODE: control->pressed = false; ++control->cancelled; return 0;
        case WM_CAPTURECHANGED: control->pressed = false; return 0;
        case WM_RBUTTONUP:
            if (control->modal && control->modalExit)
            {
                PostMessageW(control->modalRow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 30));
                PostMessageW(control->modalRow, WM_LBUTTONUP, 0, MAKELPARAM(40, 30));
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
    RealInput()
    {
        Check(GetCursorPos(&cursor) != FALSE, "read system cursor");
        CURSORINFO info{}; info.cbSize = sizeof(info);
        Check(GetCursorInfo(&info) != FALSE, "read system cursor image"); cursorImage = info.hCursor;
    }
    void Unchanged() const
    {
        POINT after{}; Check(GetCursorPos(&after) != FALSE && Same(after, cursor), "software input changed system cursor position");
        Check(GetCapture() == capture, "software input changed system capture");
        Check(GetFocus() == focus && GetForegroundWindow() == foreground, "software input changed system focus/foreground");
        CURSORINFO info{}; info.cbSize = sizeof(info);
        Check(GetCursorInfo(&info) != FALSE && info.hCursor == cursorImage, "software input changed system cursor image");
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
    Check(!device.Current().enabled && !device.Current().buttons && !second.pressed && second.cancelled == 1,
        "end cancels a held press without synthesizing a click");
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
}

int main()
{
    try
    {
        NativeMessages(); ModalQueueAndRejections(); BoundedQueueAndObserverCancellation();
        std::cout << "Private pointer native-message checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
