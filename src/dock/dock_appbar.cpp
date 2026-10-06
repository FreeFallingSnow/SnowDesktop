#include "dock_appbar.h"
#include "system/status_bar/status_bar_appbar.h"
#include <algorithm>

namespace snowdesktop
{
namespace
{
constexpr UINT kShellCallback = WM_APP + 1;
constexpr UINT_PTR kPlacementTimer = 1;
constexpr UINT kPlacementDelay = 120;
constexpr wchar_t kWindowClass[] = L"SnowDesktop.DockAppBar";
}
struct DockAppBars::Window
{
    DockAppBars& owner;
    HWND hwnd = nullptr;
    StatusBarAppBar appbar;
    DockAppBarReservation desired;
    DockAppBarReservation applied;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    bool placing = false, closing = false, queued = false;

    explicit Window(DockAppBars& value) : owner(value) {}
    ~Window()
    {
        closing = true;
        if (hwnd) KillTimer(hwnd, kPlacementTimer);
        appbar.Remove();
        if (hwnd) DestroyWindow(hwnd);
    }
    bool Create()
    {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = Procedure;
        cls.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            kWindowClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance, this) != nullptr;
    }
    void Queue(bool restart = false)
    {
        if (closing || placing || !hwnd || (queued && !restart)) return;
        queued = SetTimer(hwnd, kPlacementTimer, kPlacementDelay, nullptr) != 0;
    }
    void Place()
    {
        KillTimer(hwnd, kPlacementTimer);
        queued = false;
        if (closing || placing) return;
        placing = true;
        const RECT previous = appbar.Bounds();
        const bool registered = appbar.Registered();
        if (appbar.Place(hwnd, kShellCallback, desired.edge, desired.thickness, desired.bounds))
        {
            applied = desired;
            const RECT rect = appbar.Bounds();
            RECT windowBounds{};
            if (!GetWindowRect(hwnd, &windowBounds) || !EqualRect(&windowBounds, &rect))
                SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_HIDEWINDOW);
        }
        placing = false;
        const RECT current = appbar.Bounds();
        if ((registered != appbar.Registered() || !EqualRect(&previous, &current)) && owner.changed_)
            owner.changed_();
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self || self->closing) return DefWindowProcW(window, message, wp, lp);
        if (message == self->taskbarCreated)
        {
            self->appbar.ExplorerRestarted(); self->Queue(); return 0;
        }
        if (message == kShellCallback)
        {
            if (wp == ABN_POSCHANGED) self->Queue();
            return 0;
        }
        if (message == WM_TIMER && wp == kPlacementTimer) { self->Place(); return 0; }
        if (message == WM_DISPLAYCHANGE) self->Queue();
        if (message == WM_WINDOWPOSCHANGED && !self->placing) self->appbar.Notify(ABM_WINDOWPOSCHANGED);
        return DefWindowProcW(window, message, wp, lp);
    }
};

DockAppBars::DockAppBars(std::function<void()> changed) : changed_(std::move(changed)) {}
DockAppBars::~DockAppBars() { changed_ = {}; windows_.clear(); }
void DockAppBars::Configure(const std::vector<DockAppBarReservation>& reservations)
{
    bool removed = false;
    std::erase_if(windows_, [&](const auto& window) {
        const bool erase = std::none_of(reservations.begin(), reservations.end(), [&](const auto& request) {
            return request.monitor == window->desired.monitor;
        });
        removed = removed || erase;
        return erase;
    });
    for (const auto& request : reservations)
    {
        if (!request.monitor || request.thickness <= 0 || request.edge > ABE_BOTTOM || IsRectEmpty(&request.bounds)) continue;
        auto it = std::find_if(windows_.begin(), windows_.end(), [&](const auto& window) {
            return window->desired.monitor == request.monitor;
        });
        if (it == windows_.end())
        {
            auto window = std::make_unique<Window>(*this);
            window->desired = request;
            if (!window->Create()) continue;
            window->Queue();
            windows_.push_back(std::move(window));
        }
        else if (!((*it)->desired == request))
        {
            (*it)->desired = request;
            (*it)->Queue(true);
        }
        else if (!(*it)->appbar.Registered() || !((*it)->applied == request))
            (*it)->Queue(); // A later layout may retry a transient Shell rejection.
    }
    if (removed && changed_) changed_();
}
RECT DockAppBars::RestoreWorkArea(HMONITOR monitor, RECT work) const
{
    for (const auto& window : windows_)
    {
        if (window->applied.monitor != monitor || !window->appbar.Registered()) continue;
        const RECT reserved = window->appbar.Bounds();
        work = RestoreAppBarWorkArea(work, reserved, window->appbar.Edge());
    }
    return work;
}
std::optional<RECT> DockAppBars::Approved(const DockAppBarReservation& request) const
{
    for (const auto& window : windows_)
        if (window->applied == request && window->appbar.Registered()) return window->appbar.Bounds();
    return {};
}
}
