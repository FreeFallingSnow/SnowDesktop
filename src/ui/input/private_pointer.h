#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace snowdesktop::private_pointer
{
enum class Button : unsigned { Left = 1, Right = 2, Middle = 4 };

struct Sample
{
    std::uint64_t sequence = 0;
    double milliseconds = 0;
    POINT point{};
    unsigned buttons = 0;
    HWND window = nullptr;
    UINT message = 0;
    std::string phase;
};

struct State
{
    bool enabled = false, closing = false;
    RECT bounds{};
    POINT point{};
    unsigned buttons = 0;
    HWND hovered = nullptr, captured = nullptr, focused = nullptr, foreground = nullptr;
    HCURSOR cursor = nullptr;
    std::uint64_t sequence = 0;
    std::size_t pendingEvents = 0;
    std::string error;
    std::vector<Sample> trace;
};

// A session is bound to one UI thread. Events are queued to an internal HWND,
// then delivered to the host's ordinary window procedure on that same thread.
// Route and Owns must describe application surfaces, never arbitrary HWNDs.
class Device final
{
public:
    using Route = std::function<HWND(POINT)>;
    using Owns = std::function<bool(HWND)>;
    using Observe = std::function<void(const State&)>;
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    bool Begin(RECT bounds, POINT initial, Route route, Owns owns, std::string& error);
    bool Move(POINT point, std::string& error);
    bool Down(Button button, std::string& error);
    bool Up(Button button, std::string& error);
    bool Wheel(int delta, std::string& error);
    void SetObserver(Observe observe);
    void End() noexcept;
    const State& Current() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool Active() noexcept;
bool Enabled() noexcept;
bool IsDispatchingVirtualInput() noexcept;
// Call once at each application window-procedure entry. A dispatch permit is
// consumed by that exact HWND/message, so a modal loop cannot admit real input.
bool MessageAllowed(HWND window, UINT message) noexcept;

// Explicit authorization for synchronous forwarding to a second owned host.
// This is deliberately narrower than a process-wide "synthetic input" flag.
class ForwardScope final
{
public:
    ForwardScope(HWND destination, UINT message) noexcept;
    ~ForwardScope();
    ForwardScope(const ForwardScope&) = delete;
    ForwardScope& operator=(const ForwardScope&) = delete;
private:
    HWND previousWindow_ = nullptr;
    UINT previousMessage_ = 0;
    bool previousAvailable_ = false, armed_ = false;
};

// Physical input retains its existing Win32 behavior outside a private session.
// During a session these read and update only the software device's own state.
BOOL CursorPosition(LPPOINT point) noexcept;
SHORT AsyncKeyState(int key) noexcept;
SHORT KeyState(int key) noexcept;
HWND Capture() noexcept;
HWND SetPointerCapture(HWND window) noexcept;
BOOL ReleasePointerCapture() noexcept;
HWND WindowAtPoint(POINT point) noexcept;
BOOL TrackLeave(LPTRACKMOUSEEVENT tracking) noexcept;
HWND FocusWindow() noexcept;
HWND SetInputFocus(HWND window) noexcept;
HWND ForegroundWindow() noexcept;
BOOL SetInputForeground(HWND window) noexcept;
HCURSOR Cursor(HCURSOR cursor) noexcept;
}
