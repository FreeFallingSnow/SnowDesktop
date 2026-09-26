#include "tray_menu_placement.h"
#include "diagnostic_log.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace snowdesktop::tray
{
void MenuPlacementSession::Arm(DWORD process, POINT anchor, RECT workArea, DWORD started)
{
    process_ = process; anchor_ = anchor; workArea_ = workArea; started_ = started;
    windows_ = {}; corrections_ = 0;
}
void MenuPlacementSession::Cancel() { process_ = 0; windows_ = {}; }
bool MenuPlacementSession::Active(DWORD now) const
{ return process_ && static_cast<DWORD>(now - started_) < kLifetimeMs; }
std::optional<POINT> MenuPlacementSession::Observe(const MenuPopupObservation& popup, DWORD now)
{
    if (!Active(now) || corrections_ >= 3 || !popup.window || popup.process != process_ ||
        static_cast<LONG>(popup.eventTime - started_) < 0) return {};
    auto existing = std::find(windows_.begin(), windows_.end(), popup.window);
    if (popup.event == EVENT_OBJECT_HIDE)
    { if (existing != windows_.end()) *existing = 0; return {}; }
    if (!popup.visible || popup.notificationWindow || !(popup.style & WS_POPUP) ||
        (popup.style & (WS_CHILD | WS_THICKFRAME)) || (popup.style & WS_CAPTION) == WS_CAPTION ||
        (popup.extendedStyle & WS_EX_APPWINDOW) ||
        (!popup.standardMenu && !(popup.owned && (popup.extendedStyle & WS_EX_TOOLWINDOW)))) return {};
    const auto width = static_cast<std::int64_t>(popup.bounds.right) - popup.bounds.left;
    const auto height = static_cast<std::int64_t>(popup.bounds.bottom) - popup.bounds.top;
    const auto workWidth = static_cast<std::int64_t>(workArea_.right) - workArea_.left;
    const auto workHeight = static_cast<std::int64_t>(workArea_.bottom) - workArea_.top;
    if (width <= 0 || height <= 0 || width > workWidth || height > workHeight) return {};
    if (popup.event == EVENT_OBJECT_SHOW && existing == windows_.end())
    {
        constexpr std::int64_t proximity = 96;
        if (anchor_.x < static_cast<std::int64_t>(popup.bounds.left) - proximity ||
            anchor_.x > static_cast<std::int64_t>(popup.bounds.right) + proximity ||
            anchor_.y < static_cast<std::int64_t>(popup.bounds.top) - proximity ||
            anchor_.y > static_cast<std::int64_t>(popup.bounds.bottom) + proximity) return {};
        existing = std::find(windows_.begin(), windows_.end(), 0);
        if (existing == windows_.end()) return {};
        *existing = popup.window;
    }
    else if (popup.event != EVENT_OBJECT_LOCATIONCHANGE || existing == windows_.end()) return {};
    const auto x = (std::clamp)(static_cast<std::int64_t>(popup.bounds.left),
        static_cast<std::int64_t>(workArea_.left), static_cast<std::int64_t>(workArea_.right) - width);
    const auto y = (std::clamp)(static_cast<std::int64_t>(popup.bounds.top),
        static_cast<std::int64_t>(workArea_.top), static_cast<std::int64_t>(workArea_.bottom) - height);
    if (x == popup.bounds.left && y == popup.bounds.top) return {};
    ++corrections_;
    return POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
}

struct MenuPlacementGuard::Impl
{
    struct Request { HWND target = nullptr; DWORD process = 0; POINT anchor{}; RECT bounds{}; DWORD started = 0; };
    std::mutex mutex;
    std::condition_variable ready;
    Request request;
    std::uint64_t requested = 0, applied = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::jthread worker;
    MenuPlacementSession session;
    HWND target = nullptr;
    HWINEVENTHOOK hook = nullptr;
    ULONGLONG expires = 0;
    static thread_local Impl* active;

    ~Impl()
    {
        worker.request_stop();
        if (stop) SetEvent(stop);
        if (worker.joinable()) worker.join();
        if (wake) CloseHandle(wake);
        if (stop) CloseHandle(stop);
    }
    void Unhook()
    {
        if (hook) UnhookWinEvent(hook);
        hook = nullptr; session.Cancel();
    }
    static void CALLBACK Event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD time)
    {
        if (!active || !window || object != OBJID_WINDOW || child != CHILDID_SELF ||
            (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_LOCATIONCHANGE && event != EVENT_OBJECT_HIDE)) return;
        std::lock_guard lock(active->mutex);
        if (active->applied != active->requested) return;
        MenuPopupObservation popup;
        popup.window = reinterpret_cast<std::uint64_t>(window); popup.event = event; popup.eventTime = time;
        popup.notificationWindow = window == active->target;
        GetWindowThreadProcessId(window, &popup.process);
        popup.style = GetWindowLongPtrW(window, GWL_STYLE); popup.extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        popup.visible = IsWindowVisible(window) != FALSE;
        wchar_t name[64]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
        popup.standardMenu = wcscmp(name, L"#32768") == 0;
        DWORD ownerProcess = 0; GetWindowThreadProcessId(GetWindow(window, GW_OWNER), &ownerProcess);
        popup.owned = ownerProcess && ownerProcess == popup.process;
        if (!GetWindowRect(window, &popup.bounds)) return;
        const auto position = active->session.Observe(popup, GetTickCount());
        if (!position) return;
        // Recheck process/style at the mutation boundary. Never activate,
        // resize, change z-order, or move an unrelated/reused main HWND.
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (process != popup.process || !IsWindowVisible(window) ||
            GetWindowLongPtrW(window, GWL_STYLE) != popup.style) return;
        if (SetWindowPos(window, nullptr, position->x, position->y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS))
            WriteDiagnosticLogEntry(L"Tray menu placement correction requested", DiagnosticLogLevel::Debug);
    }
    void Run(std::stop_token token)
    {
        active = this;
        HANDLE handles[]{stop, wake};
        while (!token.stop_requested())
        {
            if (hook && !session.Active(GetTickCount())) Unhook();
            const auto now = GetTickCount64();
            const DWORD timeout = hook ? static_cast<DWORD>(expires > now ? expires - now : 0) : INFINITE;
            const DWORD waited = MsgWaitForMultipleObjectsEx(2, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
            if (waited == WAIT_OBJECT_0 + 1)
            {
                Request next;
                std::uint64_t serial = 0;
                { std::lock_guard lock(mutex); next = request; serial = requested; }
                Unhook();
                target = next.target;
                DWORD process = 0; GetWindowThreadProcessId(next.target, &process);
                if (next.target && process && process == next.process &&
                    static_cast<DWORD>(GetTickCount() - next.started) < MenuPlacementSession::kLifetimeMs)
                {
                    MONITORINFO monitor{sizeof(monitor)};
                    if (GetMonitorInfoW(MonitorFromRect(&next.bounds, MONITOR_DEFAULTTONEAREST), &monitor))
                    {
                        session.Arm(process, next.anchor, monitor.rcWork, next.started);
                        hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, Event,
                            process, 0, WINEVENT_OUTOFCONTEXT);
                        if (!hook) session.Cancel();
                        const auto elapsed = static_cast<DWORD>(GetTickCount() - next.started);
                        expires = GetTickCount64() + (elapsed < MenuPlacementSession::kLifetimeMs ?
                            MenuPlacementSession::kLifetimeMs - elapsed : 0);
                    }
                }
                { std::lock_guard lock(mutex); applied = serial; ready.notify_all(); }
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        Unhook(); active = nullptr;
    }
};
thread_local MenuPlacementGuard::Impl* MenuPlacementGuard::Impl::active = nullptr;
MenuPlacementGuard::MenuPlacementGuard() : impl_(std::make_unique<Impl>()) {}
MenuPlacementGuard::~MenuPlacementGuard() = default;
void MenuPlacementGuard::Arm(HWND target, POINT anchor, RECT iconBounds, bool continuation)
{
    DWORD process = 0; GetWindowThreadProcessId(target, &process);
    if (!process) return;
    std::unique_lock lock(impl_->mutex);
    if (!impl_->wake || !impl_->stop) return;
    if (!impl_->worker.joinable()) impl_->worker = std::jthread([state = impl_.get()](std::stop_token token) { state->Run(token); });
    const DWORD now = GetTickCount();
    if (continuation && impl_->request.target == target && impl_->request.process == process &&
        static_cast<DWORD>(now - impl_->request.started) < MenuPlacementSession::kLifetimeMs) return;
    if (IsRectEmpty(&iconBounds)) iconBounds = {anchor.x, anchor.y, anchor.x + 1, anchor.y + 1};
    impl_->request = {target, process, anchor, iconBounds, now};
    const auto serial = ++impl_->requested;
    SetEvent(impl_->wake);
    // Install before notifying the app, so menus created by its first callback
    // are observed. Failure/timeout never blocks the original tray action.
    impl_->ready.wait_for(lock, std::chrono::milliseconds(100), [&] { return impl_->applied >= serial; });
}
void MenuPlacementGuard::Cancel()
{
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) return;
    impl_->request = {}; ++impl_->requested; SetEvent(impl_->wake);
}
}
