#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    unsigned virtualKey = 0;
    std::uint16_t codeUnit = 0;
};

struct State
{
    bool enabled = false, closing = false;
    RECT bounds{};
    POINT point{};
    unsigned buttons = 0;
    std::array<bool, 256> keys{};
    HWND hovered = nullptr, captured = nullptr, focused = nullptr, foreground = nullptr;
    HCURSOR cursor = nullptr;
    std::uint64_t sequence = 0;
    std::size_t pendingEvents = 0;
    std::string error;
    std::vector<Sample> trace;
};

// Optional internal, value-only diagnostics. They do not authorize input or
// change MessageAllowed. A record may be missing on overflow; check failed.
enum class DeliveryObservationKind { begin, permit_consumed, permit_rejected, returned };
struct DeliveryIdentity
{
    // Session and delivery numbers are unique only during the owner thread's
    // lifetime in this process; thread IDs can be reused. A nonzero forward
    // identifies one ForwardScope within this session.
    std::uint64_t session = 0, event = 0, delivery = 0;
    bool forwarded = false;
    DWORD ownerThread = 0;
    std::uint64_t forward = 0;
    // True when the originating Deliver is the primary queued event. A
    // ForwardScope inherits this origin but forwarded=true distinguishes
    // that secondary call. Cleanup and other secondary Deliver calls use
    // false and carry no queued-event number.
    bool queuedEvent = false;
};
struct DeliveryObservation
{
    // HWND and message parameters are copied numeric values only; messages
    // may carry handles or pointers whose lifetimes are not extended here.
    DeliveryObservationKind kind;
    DeliveryIdentity identity;
    HWND window = nullptr;
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    LRESULT result = 0;
};
struct DeliveryObservationLog
{
    DWORD ownerThread = 0;
    std::uint64_t session = 0;
    bool failed = false;
    std::vector<DeliveryObservation> entries;
    // Capacity of the live log at opt-in, preserved when entries are copied.
    std::optional<std::size_t> recordedCapacity;
};
struct QueuedEventIdentity
{
    DWORD ownerThread = 0;
    std::uint64_t session = 0, event = 0;
};

// A session is bound to one UI thread. Device and its observation log must
// outlive every synchronous Deliver and ForwardScope originating from it.
// Events are queued to an internal HWND,
// then delivered to the host's ordinary window procedure on that same thread.
// Route and Owns must describe application surfaces, never arbitrary HWNDs.
class Device final
{
public:
    using Route = std::function<HWND(POINT)>;
    using Owns = std::function<bool(HWND)>;
    using Observe = std::function<void(const State&)>;
    // Embedded, offscreen text proxies use their visible host's coordinates.
    // A supplied mapper owns all approved targets; failure never falls back.
    using MapPoint = std::function<bool(HWND, POINT screen, POINT& client)>;
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    bool Begin(RECT bounds, POINT initial, Route route, Owns owns, std::string& error, MapPoint map = {});
    bool Move(POINT point, std::string& error);
    bool Down(Button button, std::string& error);
    bool Up(Button button, std::string& error);
    bool Wheel(int delta, std::string& error);
    // Key events and UTF-16 WM_CHAR data share the same ordered queue. They
    // require an exact current owned focus; text never follows a focus change.
    // Text is nonempty, at most 4096 code units, with valid surrogate pairs.
    // A queued release is delivered only after its down reached a native host.
    bool KeyDown(unsigned virtualKey, std::string& error);
    bool KeyUp(unsigned virtualKey, std::string& error);
    bool Text(std::wstring text, std::string& error);
    void SetObserver(Observe observe);
    // Opt in before Begin; no callback is invoked from production dispatch.
    // Records remain owned by this Device and are readable on its UI thread
    // until the next Begin, opt-in or destruction. Capacity overflow is explicit.
    // A session started on another thread yields no log on that thread; a
    // zero session in an old log is not evidence of an observed session.
    bool EnableDeliveryObservation(std::size_t capacity) noexcept;
    const DeliveryObservationLog* DeliveryObservations() const noexcept;
    // Read immediately after a successful Begin or input call and before
    // pumping messages. False means the single pending event cannot be bound.
    // Never use an older value to interpret a failed enqueue call.
    bool LastQueuedEventIdentity(QueuedEventIdentity& identity) const noexcept;
    // Cancel each owned capture/hover/hit/focus host without activating releases.
    void End() noexcept;
    const State& Current() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool Active() noexcept;
bool Enabled() noexcept;
bool IsDispatchingVirtualInput() noexcept;
// Snapshot the current nested delivery identity at a WndProc boundary. Zero
// means unavailable and never proves a message was admitted.
DeliveryIdentity CurrentDeliveryIdentity() noexcept;
// Call once at each application window-procedure entry. A dispatch permit is
// consumed by that exact HWND/message, so a modal loop cannot admit real input.
// Rejected WM_INPUT/WM_TOUCH/WM_GESTURE resources are disposed here. A false
// result must be dropped immediately, without forwarding or DefWindowProc.
bool MessageAllowed(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept;

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
    bool previousAvailable_ = false, previousForwarded_ = false, armed_ = false;
    std::uint64_t previousForward_ = 0;
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
