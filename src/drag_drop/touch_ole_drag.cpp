#include "touch_ole_drag.h"

#include <utility>

namespace snowdesktop::touch_ole_drag
{
namespace
{
constexpr wchar_t relayClass[] = L"SnowDesktop.TouchDragRelay";
constexpr ULONG_PTR relayMouse = 0x53444f4c;
constexpr UINT_PTR startTimer = 1;
constexpr UINT_PTR releaseTimer = 2;
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

enum class State { Idle, Starting, Running };

struct Relay
{
    HWND owner = nullptr;
    HWND window = nullptr;
    POINT point{};
    State state = State::Idle;
    bool buttonDown = false;
    bool canceled = false;
    std::function<void(bool)> complete;

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
        state = State::Idle;
        canceled = false;
        complete = {};
    }

    void CancelStart() noexcept
    {
        const bool notify = IsWindow(owner) != FALSE;
        auto callback = std::move(complete);
        Reset();
        if (notify && callback)
            try { callback(false); } catch (...) {}
    }

    void FinishMouseInput() noexcept
    {
        ReleaseButton(true);
        if (!buttonDown)
        {
            KillTimer(window, releaseTimer);
            return;
        }
        canceled = true;
        if (const HWND capture = GetCapture()) PostMessageW(capture, WM_KEYDOWN, VK_ESCAPE, 0);
    }

    void MouseDown() noexcept
    {
        if (state != State::Starting) return;
        KillTimer(window, startTimer);
        // Keep the active window for OLE mouse capture, but remove its hit
        // region once it has received the down event.
        const HRGN emptyRegion = CreateRectRgn(0, 0, 0, 0);
        if (!emptyRegion || !SetWindowRgn(window, emptyRegion, TRUE))
        {
            if (emptyRegion) DeleteObject(emptyRegion);
            CancelStart();
            return;
        }
        if (!IsWindow(owner) || WindowFromPoint(point) == window || !SetTimer(window, releaseTimer, 40, nullptr))
        {
            CancelStart();
            return;
        }

        // The real touch has already ended. Release the mouse from the OLE
        // pump even if it never receives another WM_POINTER message.
        state = State::Running;
        auto callback = std::move(complete);
        try { callback(true); } catch (...) {}
        Reset();
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
        if (window != relay.window) return 0;
        if (wp == startTimer && relay.state == State::Starting) relay.CancelStart();
        if (wp == releaseTimer && relay.state == State::Running) relay.FinishMouseInput();
        return 0;
    case WM_LBUTTONDOWN:
        if (window == relay.window && IsRelayMouseMessage(message)) relay.MouseDown();
        return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
}

bool StartDrop(HWND owner, POINT screen, std::function<void(bool)> complete)
{
    auto& relay = Current();
    RECT clip{};
    if (relay.state != State::Idle || relay.buttonDown || !owner || !complete || GetCapture() ||
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || !GetClipCursor(&clip) || !PtInRect(&clip, screen)) return false;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = RelayProcedure;
    windowClass.hInstance = instance;
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = relayClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    relay.window = CreateWindowExW(WS_EX_TOOLWINDOW, relayClass, nullptr,
        WS_POPUP, screen.x - relayRadius, screen.y - relayRadius, 2 * relayRadius + 1, 2 * relayRadius + 1,
        nullptr, nullptr, instance, nullptr);
    if (!relay.window) return false;
    relay.owner = owner;
    relay.point = screen;
    relay.complete = std::move(complete);
    relay.state = State::Starting;

    if (!SetWindowPos(relay.window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
    {
        relay.Reset();
        return false;
    }

    // Another topmost window can keep the hit until ours is activated.
    // Verify ownership before injecting the down event.
    if (WindowFromPoint(screen) != relay.window) SetForegroundWindow(relay.window);
    if (WindowFromPoint(screen) != relay.window || !SetTimer(relay.window, startTimer, 250, nullptr) ||
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

void CancelPending() noexcept
{
    auto& relay = Current();
    if (relay.state == State::Starting) relay.CancelStart();
}

bool ShouldCancel(bool escapePressed) noexcept
{
    const auto& relay = Current();
    return relay.state == State::Running && (escapePressed || relay.canceled);
}
}
