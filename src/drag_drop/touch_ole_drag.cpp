#include "touch_ole_drag.h"

#include <utility>

namespace snowdesktop::touch_ole_drag
{
namespace
{
constexpr wchar_t relayClass[] = L"SnowDesktop.TouchDragRelay";
constexpr ULONG_PTR relayMouse = 0x53444f4c;
constexpr UINT_PTR startTimer = 1;
constexpr int relayRadius = 2;

bool SendMouse(POINT point, DWORD buttons, bool move = true) noexcept
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwExtraInfo = relayMouse;
    input.mi.dwFlags = buttons;
    if (move)
    {
        const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (width <= 0 || height <= 0 || point.x < x || point.y < y ||
            point.x >= x + width || point.y >= y + height) return false;
        // Aim at the center of the pixel in the absolute input range.
        input.mi.dx = static_cast<LONG>((static_cast<LONGLONG>(point.x - x) * 65536 + 32768) / width);
        input.mi.dy = static_cast<LONG>((static_cast<LONGLONG>(point.y - y) * 65536 + 32768) / height);
        input.mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    return SendInput(1, &input, sizeof(input)) == 1;
}

enum class State { Idle, Starting, Running, FinishingContact };

struct Relay
{
    HWND owner = nullptr;
    HWND window = nullptr;
    UINT32 pointerId = 0;
    POINT point{};
    State state = State::Idle;
    bool buttonDown = false;
    bool released = false;
    bool canceled = false;
    std::function<bool(POINT)> runOle;

    ~Relay() { Reset(); }

    void ReleaseButton(bool move) noexcept
    {
        if (buttonDown && (SendMouse(point, MOUSEEVENTF_LEFTUP, move) ||
                (move && SendMouse({}, MOUSEEVENTF_LEFTUP, false)))) buttonDown = false;
    }

    void Reset() noexcept
    {
        ReleaseButton(false);
        if (const HWND old = std::exchange(window, nullptr)) DestroyWindow(old);
        owner = nullptr;
        pointerId = 0;
        state = State::Idle;
        canceled = released = false;
        runOle = {};
    }

    void StopRunning() noexcept
    {
        canceled = true;
        ReleaseButton(false);
        if (const HWND capture = GetCapture()) PostMessageW(capture, WM_KEYDOWN, VK_ESCAPE, 0);
    }

    void MouseDown() noexcept
    {
        if (state != State::Starting) return;
        KillTimer(window, startTimer);
        // Keep an active window on the OLE thread for mouse capture while
        // letting the real drop target participate in hit testing.
        const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_TRANSPARENT);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        state = State::Running;
        bool resume = false;
        try
        {
            resume = IsWindow(owner) != FALSE;
            if (resume && WindowFromPoint(point) != window && SendMouse(point, 0))
                resume = runOle(point);
        }
        catch (...) { canceled = true; }
        ReleaseButton(false);
        if (!released && !resume)
        {
            if (const HWND old = std::exchange(window, nullptr)) DestroyWindow(old);
            state = State::FinishingContact;
            runOle = {};
        }
        else Reset();
    }
};

Relay& Current() noexcept
{
    static thread_local Relay relay;
    return relay;
}

LRESULT CALLBACK RelayProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto& relay = Current();
    switch (message)
    {
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_TIMER:
        if (wp == startTimer && window == relay.window && relay.state == State::Starting) relay.Reset();
        return 0;
    case WM_LBUTTONDOWN:
        if (window == relay.window && IsRelayMouseMessage(message)) relay.MouseDown();
        return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
}

bool Start(HWND owner, UINT32 pointerId, POINT screen, std::function<bool(POINT)> runOle)
{
    auto& relay = Current();
    RECT clip{};
    if (relay.state != State::Idle || relay.buttonDown || !owner || !pointerId ||
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || !GetClipCursor(&clip) || !PtInRect(&clip, screen)) return false;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = RelayProcedure;
    windowClass.hInstance = instance;
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = relayClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    relay.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, relayClass, nullptr,
        WS_POPUP, screen.x - relayRadius, screen.y - relayRadius, 2 * relayRadius + 1, 2 * relayRadius + 1,
        nullptr, nullptr, instance, nullptr);
    if (!relay.window) return false;
    relay.owner = owner;
    relay.pointerId = pointerId;
    relay.point = screen;
    relay.runOle = std::move(runOle);
    relay.state = State::Starting;

    // The injected down must hit an owned window, never the drop target.
    if (!SetLayeredWindowAttributes(relay.window, 0, 1, LWA_ALPHA) ||
        !SetWindowPos(relay.window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) ||
        WindowFromPoint(screen) != relay.window || !SetTimer(relay.window, startTimer, 250, nullptr) ||
        !SendMouse(screen, MOUSEEVENTF_LEFTDOWN))
    {
        relay.Reset();
        return false;
    }
    relay.buttonDown = true;
    return true;
}

bool IsRelayMouseMessage(UINT message) noexcept
{
    return (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_LBUTTONDBLCLK) && GetMessageExtraInfo() == relayMouse;
}

bool Owns(HWND owner, UINT32 pointerId) noexcept
{
    const auto& relay = Current();
    return relay.state != State::Idle && relay.owner == owner && relay.pointerId == pointerId;
}

bool Update(POINT screen, bool released) noexcept
{
    auto& relay = Current();
    relay.point = screen;
    if (relay.state == State::Running)
    {
        relay.released |= released;
        if (released)
        {
            relay.ReleaseButton(true);
            if (relay.buttonDown) relay.StopRunning();
        }
        else if (!SendMouse(screen, 0)) relay.StopRunning();
    }
    else if (released)
    {
        const bool transferred = relay.state == State::FinishingContact;
        relay.Reset();
        return transferred;
    }
    return true;
}

bool Cancel() noexcept
{
    auto& relay = Current();
    if (relay.state == State::Running)
    {
        relay.released = true;
        relay.StopRunning();
        return true;
    }
    const bool transferred = relay.state == State::FinishingContact;
    relay.Reset();
    return transferred;
}

bool ShouldCancel(bool escapePressed) noexcept
{
    const auto& relay = Current();
    return relay.state == State::Running && (escapePressed || relay.canceled);
}
}
