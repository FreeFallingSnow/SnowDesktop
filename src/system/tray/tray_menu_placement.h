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
    // A direct owner chain or the notification window's UI thread, not just
    // another tool window in the same process.
    bool targetRelated = false;
    std::uint64_t owner = 0;
    DWORD thread = 0;
    // Direct notification-owner ancestry, or an OS menu loop whose actual
    // menu owner runs on the notification window's UI thread. Same PID/thread
    // alone is insufficient to re-anchor a distant popup.
    bool strongTargetRelated = false;
    std::uint64_t parentMenu = 0;
    // A context gesture may identify an ownerless custom menu by its actual
    // foreground/active HWND and notification UI thread, never PID alone.
    DWORD notificationThread = 0;
    std::uint64_t foregroundWindow = 0, activeWindow = 0;
};
struct MenuPopupBinding
{
    std::uint64_t window = 0, owner = 0;
    DWORD process = 0, thread = 0;
    LONG_PTR style = 0, extendedStyle = 0;
    bool standardMenu = false;
};
using MenuPopupBindings = std::array<MenuPopupBinding, 4>;

// Pure policy shared by the event hook and tray regressions. Only new SHOW
// events become candidates. Weakly related windows must be near the gesture;
// a strongly identified root, or the same root already bound near this
// gesture, can be re-anchored from a distant edge.
// LOCATIONCHANGE alone cannot nominate an existing window.
class MenuPlacementSession
{
public:
    static constexpr DWORD kLifetimeMs = 1500;
    void Arm(DWORD process, POINT anchor, RECT workArea, DWORD started, RECT barBounds = {}, RECT iconBounds = {}, bool contextGesture = false);
    void Cancel();
    bool Active(DWORD now) const;
    std::optional<POINT> Observe(const MenuPopupObservation& popup, DWORD now);
    void RejectCorrection(std::uint64_t window);
    MenuPopupBindings Bindings() const;
private:
    DWORD process_ = 0, started_ = 0;
    POINT anchor_{};
    RECT workArea_{}, iconBounds_{};
    bool contextGesture_ = false;
    struct Candidate
    {
        MenuPopupBinding popup;
        RECT bounds{};
        bool observedMenu = false, submenu = false, anchorBound = false;
        unsigned corrections = 0;
        std::optional<POINT> pending;
    };
    std::array<Candidate, 4> windows_{};
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
    void Arm(HWND target, POINT anchor, RECT iconBounds, bool continuation, RECT barBounds = {}, bool contextGesture = false);
    void Cancel();
    // Read-only evidence from this gesture's short hook. Callers must recheck
    // HWND identity and visibility; the hook is not kept alive by retention.
    MenuPopupBindings Popups(HWND target) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
