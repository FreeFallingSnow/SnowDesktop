#pragma once
#include "taskbar_hook/tray_protocol.h"
#include <optional>
#include <utility>

namespace snowdesktop::tray
{
struct FocusDelivery
{
    FocusTicket ticket;
    std::string key;
    std::uint64_t foreground = 0;
};
// One outstanding user operation, never a queue of stale focus requests.
class FocusReturnTracker
{
public:
    void Arm(FocusTicket ticket, std::string key)
    { current_ = FocusDelivery{ticket, std::move(key), 0}; }
    void Cancel() { current_.reset(); }
    const FocusDelivery* Current() const { return current_ ? &*current_ : nullptr; }
    bool ObserveForeground(std::uint64_t window, DWORD process, DWORD time, bool nativeReturn)
    {
        if (!current_ || !window || static_cast<LONG>(time - current_->ticket.started) <= 0) return false;
        if (nativeReturn || FocusForegroundAllowed(current_->ticket, window, process)) return false;
        Cancel(); return true;
    }
    bool Arrive(const Notification& event, std::uint64_t epoch, std::uint64_t foreground)
    {
        if (!current_ || current_->foreground || event.operation != NIM_SETFOCUS ||
            event.epoch != epoch || event.epoch != current_->ticket.epoch ||
            event.focusSerial != current_->ticket.serial || !event.focusForeground ||
            event.focusForeground != foreground || !SameFocusIdentity(event.identity, current_->ticket.identity)) return false;
        current_->foreground = foreground;
        return true;
    }
    std::optional<FocusDelivery> Take(std::uint64_t origin, std::uint64_t serial,
        std::uint64_t epoch, std::uint64_t foreground)
    {
        if (!current_ || current_->ticket.origin != origin || current_->ticket.serial != serial) return {};
        auto value = std::move(current_);
        current_.reset();
        if (!value->foreground || value->foreground != foreground || value->ticket.epoch != epoch) return {};
        return value;
    }
private:
    std::optional<FocusDelivery> current_;
};
inline UINT FocusReturnMessage()
{ static const UINT message = RegisterWindowMessageW(L"SnowDesktop.Tray.FocusReturn.v2"); return message; }
// Called only on the origin's UI thread after consuming its single-use reply.
// Recheck at the Windows boundary: a delayed reply cannot replace a new app.
inline bool RestoreFocus(const FocusDelivery& delivery)
{
    const auto origin = reinterpret_cast<HWND>(delivery.ticket.origin);
    DWORD process = 0;
    if (!origin || !delivery.foreground || GetWindowThreadProcessId(origin, &process) != GetCurrentThreadId() ||
        process != GetCurrentProcessId() || !IsWindowVisible(origin) || !IsWindowEnabled(origin) ||
        reinterpret_cast<std::uint64_t>(GetForegroundWindow()) != delivery.foreground) return false;
    if (GetForegroundWindow() != origin && !SetForegroundWindow(origin)) return false;
    if (GetForegroundWindow() != origin) return false;
    SetFocus(origin);
    return GetFocus() == origin;
}
}
