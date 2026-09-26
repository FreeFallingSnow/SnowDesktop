#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace snowdesktop::tray
{
struct MenuPopupObservation
{
    std::uint64_t window = 0;
    DWORD process = 0, event = 0, eventTime = 0;
    LONG_PTR style = 0, extendedStyle = 0;
    RECT bounds{};
    bool visible = false, standardMenu = false, owned = false, notificationWindow = false;
};

// Pure policy shared by the event hook and tray regressions. Only new SHOW
// events near this gesture become candidates; LOCATIONCHANGE alone cannot
// nominate an existing application window for movement.
class MenuPlacementSession
{
public:
    static constexpr DWORD kLifetimeMs = 1500;
    void Arm(DWORD process, POINT anchor, RECT workArea, DWORD started);
    void Cancel();
    bool Active(DWORD now) const;
    std::optional<POINT> Observe(const MenuPopupObservation& popup, DWORD now);
private:
    DWORD process_ = 0, started_ = 0;
    POINT anchor_{};
    RECT workArea_{};
    std::array<std::uint64_t, 4> windows_{};
    unsigned corrections_ = 0;
};

// The worker blocks when idle. A process-scoped WinEvent hook exists only for
// the short placement session; no global window enumeration or polling.
class MenuPlacementGuard
{
public:
    MenuPlacementGuard();
    ~MenuPlacementGuard();
    MenuPlacementGuard(const MenuPlacementGuard&) = delete;
    MenuPlacementGuard& operator=(const MenuPlacementGuard&) = delete;
    void Arm(HWND target, POINT anchor, RECT iconBounds, bool continuation);
    void Cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
