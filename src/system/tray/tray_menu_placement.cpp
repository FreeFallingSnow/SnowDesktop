#include "tray_menu_placement.h"
#include "diagnostics/diagnostic_log.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace snowdesktop::tray
{
void MenuPlacementSession::Arm(DWORD process, POINT anchor, RECT workArea, DWORD started, RECT barBounds, RECT iconBounds, bool contextGesture)
{
    process_ = process; anchor_ = anchor; workArea_ = workArea; started_ = started; iconBounds_ = iconBounds;
    contextGesture_ = contextGesture;
    // An overlay bar need not reserve rcWork. Keep its own visible edge out of
    // menu placement without excluding the overflowing tray panel itself.
    const auto barHeight = static_cast<std::int64_t>(barBounds.bottom) - barBounds.top;
    const auto workHeight = static_cast<std::int64_t>(workArea.bottom) - workArea.top;
    if (barHeight > 0 && barHeight < workHeight / 4 &&
        barBounds.left <= anchor.x && barBounds.right > anchor.x)
    {
        if (barBounds.top <= workArea.top && barBounds.bottom > workArea.top)
            workArea_.top = barBounds.bottom;
        else if (barBounds.bottom >= workArea.bottom && barBounds.top < workArea.bottom)
            workArea_.bottom = barBounds.top;
    }
    windows_ = {};
}
void MenuPlacementSession::Cancel() { process_ = 0; windows_ = {}; }
bool MenuPlacementSession::Active(DWORD now) const
{ return process_ && static_cast<DWORD>(now - started_) < kLifetimeMs; }
MenuPopupBindings MenuPlacementSession::Bindings() const
{
    MenuPopupBindings result{};
    for (std::size_t i = 0; i < windows_.size(); ++i)
        if (windows_[i].observedMenu) result[i] = windows_[i].popup;
    return result;
}
void MenuPlacementSession::RejectCorrection(std::uint64_t window)
{
    for (auto& candidate : windows_)
        if (candidate.popup.window == window) candidate.pending.reset();
}
std::optional<POINT> MenuPlacementSession::Observe(const MenuPopupObservation& popup, DWORD now)
{
    if (!Active(now) || !popup.window || popup.process != process_ ||
        static_cast<LONG>(popup.eventTime - started_) < 0) return {};
    auto existing = std::find_if(windows_.begin(), windows_.end(),
        [&](const auto& item) { return item.popup.window == popup.window; });
    if (popup.event == EVENT_OBJECT_HIDE)
    { if (existing != windows_.end()) *existing = {}; return {}; }
    // A context tool window can SHOW before becoming active. Remember that
    // bounded nomination, but require active/foreground evidence below before
    // binding or moving alternative menu shapes (no WS_POPUP / WS_THICKFRAME).
    const bool contextToolWindow = contextGesture_ && popup.notificationThread &&
        popup.thread == popup.notificationThread && (popup.extendedStyle & WS_EX_TOOLWINDOW);
    const bool activeContextMenu = contextToolWindow &&
        popup.foregroundWindow == popup.window && popup.activeWindow == popup.window;
    if (!popup.visible || popup.notificationWindow || (!(popup.style & WS_POPUP) && !contextToolWindow) ||
        (popup.style & WS_CHILD) || ((popup.style & WS_THICKFRAME) && !contextToolWindow) ||
        (popup.style & WS_CAPTION) == WS_CAPTION ||
        (popup.style & (WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) ||
        (popup.extendedStyle & (WS_EX_APPWINDOW | WS_EX_TRANSPARENT)) ||
        (!popup.standardMenu && !popup.targetRelated && !popup.strongTargetRelated && !popup.parentMenu)) return {};
    if (popup.event == EVENT_OBJECT_SHOW && existing == windows_.end())
    {
        existing = std::find_if(windows_.begin(), windows_.end(), [](const auto& item) { return !item.popup.window; });
        if (existing == windows_.end()) return {};
        *existing = {};
        existing->popup = {popup.window, popup.owner, popup.process, popup.thread, popup.style, popup.extendedStyle, popup.standardMenu};
    }
    else if ((popup.event != EVENT_OBJECT_LOCATIONCHANGE && popup.event != EVENT_OBJECT_SHOW) ||
        existing == windows_.end()) return {};
    if (existing->popup.owner != popup.owner || existing->popup.thread != popup.thread ||
        existing->popup.style != popup.style || existing->popup.extendedStyle != popup.extendedStyle)
    { *existing = {}; return {}; }
    if ((!(popup.style & WS_POPUP) || (popup.style & WS_THICKFRAME)) && !activeContextMenu) return {};
    const auto width = static_cast<std::int64_t>(popup.bounds.right) - popup.bounds.left;
    const auto height = static_cast<std::int64_t>(popup.bounds.bottom) - popup.bounds.top;
    const auto workWidth = static_cast<std::int64_t>(workArea_.right) - workArea_.left;
    const auto workHeight = static_cast<std::int64_t>(workArea_.bottom) - workArea_.top;
    if (width <= 0 || height <= 0 || workWidth <= 0 || workHeight <= 0 ||
        width > workWidth * 2 || height > workHeight * 2 ||
        (width >= workWidth && height >= workHeight)) return {};
    constexpr std::int64_t proximity = 96;
    const bool nearAnchorX = anchor_.x >= static_cast<std::int64_t>(popup.bounds.left) - proximity &&
        anchor_.x <= static_cast<std::int64_t>(popup.bounds.right) + proximity;
    const bool nearAnchorY = anchor_.y >= static_cast<std::int64_t>(popup.bounds.top) - proximity &&
        anchor_.y <= static_cast<std::int64_t>(popup.bounds.bottom) + proximity;
    const bool nearAnchor = nearAnchorX && nearAnchorY;
    // This additional evidence applies only to an explicit context-menu
    // gesture. A left-clicked borderless application window must not inherit
    // it merely because it becomes foreground on the same UI thread. The
    // caption/system controls/app-window/transparent exclusions still apply.
    const bool strongRoot = popup.strongTargetRelated || activeContextMenu;
    if (!existing->observedMenu)
    {
        const auto root = std::find_if(windows_.begin(), windows_.end(), [&](const auto& item) {
            return &item != &*existing && item.observedMenu && !item.submenu;
        });
        const auto parent = std::find_if(windows_.begin(), windows_.end(), [&](const auto& item) {
            if (&item == &*existing || !item.observedMenu) return false;
            if (item.popup.window == popup.parentMenu || item.popup.window == popup.owner) return true;
            // Native submenus may share the root's application owner instead
            // of owning each other. Require the same actual menu UI thread and
            // adjacent geometry; a second same-process menu is not sufficient.
            return popup.standardMenu && item.popup.standardMenu &&
                popup.thread == item.popup.thread && popup.owner == item.popup.owner &&
                static_cast<std::int64_t>(popup.bounds.left) <= static_cast<std::int64_t>(item.bounds.right) + proximity &&
                static_cast<std::int64_t>(popup.bounds.right) >= static_cast<std::int64_t>(item.bounds.left) - proximity &&
                static_cast<std::int64_t>(popup.bounds.top) <= static_cast<std::int64_t>(item.bounds.bottom) + proximity &&
                static_cast<std::int64_t>(popup.bounds.bottom) >= static_cast<std::int64_t>(item.bounds.top) - proximity;
        });
        if (!nearAnchor && parent == windows_.end() &&
            (root != windows_.end() || !strongRoot)) return {};
        existing->submenu = parent != windows_.end() || root != windows_.end();
        existing->observedMenu = true;
        // A fresh root already observed at this gesture is also evidence for
        // following its later layout changes. This binding never survives a
        // hide, new session or the owner/thread/style identity checks above.
        existing->anchorBound = nearAnchor || strongRoot;
    }
    existing->bounds = popup.bounds;
    if (existing->pending && existing->pending->x == popup.bounds.left &&
        existing->pending->y == popup.bounds.top) existing->pending.reset();

    std::int64_t desiredX = popup.bounds.left, desiredY = popup.bounds.top;
    const bool verticalOverflow = popup.bounds.top < workArea_.top || popup.bounds.bottom > workArea_.bottom;
    const bool haveIcon = iconBounds_.right > iconBounds_.left && iconBounds_.bottom > iconBounds_.top;
    if (!existing->submenu && ((!nearAnchor && existing->anchorBound) || (haveIcon && verticalOverflow)))
    {
        // Root menus that use the old taskbar edge must follow this gesture.
        // Preserve already reasonable near-anchor placement and never collapse
        // a submenu back on top of its root.
        if (!nearAnchorX) desiredX = anchor_.x;
        const auto iconTop = haveIcon ? static_cast<std::int64_t>(iconBounds_.top) : anchor_.y;
        const auto iconBottom = haveIcon ? static_cast<std::int64_t>(iconBounds_.bottom) : anchor_.y;
        bool below = iconTop + iconBottom <= static_cast<std::int64_t>(workArea_.top) + workArea_.bottom;
        const bool fitsBelow = iconBottom + height <= workArea_.bottom;
        const bool fitsAbove = iconTop - height >= workArea_.top;
        if (below && !fitsBelow && fitsAbove) below = false;
        else if (!below && !fitsAbove && fitsBelow) below = true;
        desiredY = below ? iconBottom : iconTop - height;
    }
    // A tall custom menu cannot fit without resizing someone else's window.
    // Preserve its size and expose the top/left choices instead of rejecting
    // all correction and leaving the complete menu above the display.
    const auto x = (std::clamp)(desiredX,
        static_cast<std::int64_t>(workArea_.left), (std::max)(static_cast<std::int64_t>(workArea_.left), static_cast<std::int64_t>(workArea_.right) - width));
    const auto y = (std::clamp)(desiredY,
        static_cast<std::int64_t>(workArea_.top), (std::max)(static_cast<std::int64_t>(workArea_.top), static_cast<std::int64_t>(workArea_.bottom) - height));
    if (x == popup.bounds.left && y == popup.bounds.top) return {};
    // SHOW/LOCATION bursts can precede delivery of SWP_ASYNCWINDOWPOS. One
    // pending destination is one request, not three consumed retries. Bounds
    // observed at that destination above acknowledge it without claiming that
    // SetWindowPos's return value proved the move.
    if (existing->pending && existing->pending->x == x && existing->pending->y == y) return {};
    if (existing->corrections >= 3) return {};
    ++existing->corrections;
    existing->pending = POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
    return existing->pending;
}

struct MenuPlacementGuard::Impl
{
    struct Request { HWND target = nullptr; DWORD process = 0; POINT anchor{}; RECT bounds{}, barBounds{}; DWORD started = 0; bool contextGesture = false; };
    std::mutex mutex;
    std::condition_variable ready;
    Request request;
    std::uint64_t requested = 0, applied = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::jthread worker;
    MenuPlacementSession session;
    MenuPopupBindings popups{};
    HWND target = nullptr;
    DWORD targetThread = 0;
    unsigned diagnosticSamples = 0;
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
        popup.thread = GetWindowThreadProcessId(window, &popup.process);
        popup.style = GetWindowLongPtrW(window, GWL_STYLE); popup.extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        popup.visible = IsWindowVisible(window) != FALSE;
        wchar_t name[64]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
        popup.standardMenu = wcscmp(name, L"#32768") == 0;
        if (wcscmp(name, L"tooltips_class32") == 0) return;
        const auto owner = GetWindow(window, GW_OWNER);
        popup.owner = reinterpret_cast<std::uint64_t>(owner);
        DWORD ownerProcess = 0; GetWindowThreadProcessId(owner, &ownerProcess);
        popup.owned = ownerProcess && ownerProcess == popup.process;
        DWORD targetProcess = 0;
        if (GetWindowThreadProcessId(active->target, &targetProcess) != active->targetThread ||
            targetProcess != popup.process) return;
        popup.notificationThread = active->targetThread;
        popup.targetRelated = popup.thread == active->targetThread && (!owner || popup.owned);
        const auto targetRoot = GetAncestor(active->target, GA_ROOTOWNER);
        // Bounded owner-chain inspection of this event's HWND only. No search
        // for or movement of existing application windows.
        auto ancestor = owner;
        for (unsigned depth = 0; ancestor && depth < 8; ++depth)
        {
            DWORD process = 0; GetWindowThreadProcessId(ancestor, &process);
            if (process != targetProcess) break;
            if (ancestor == active->target || ancestor == targetRoot)
                popup.strongTargetRelated = true;
            if (!popup.parentMenu)
                for (const auto& binding : active->popups)
                    if (binding.window == reinterpret_cast<std::uint64_t>(ancestor))
                        popup.parentMenu = binding.window;
            const auto next = GetWindow(ancestor, GW_OWNER);
            if (next == ancestor) break;
            ancestor = next;
        }
        GUITHREADINFO info{sizeof(info)};
        const bool haveGuiInfo = GetGUIThreadInfo(popup.thread, &info) != FALSE;
        popup.foregroundWindow = reinterpret_cast<std::uint64_t>(GetForegroundWindow());
        if (haveGuiInfo) popup.activeWindow = reinterpret_cast<std::uint64_t>(info.hwndActive);
        if (popup.standardMenu)
        {
            DWORD menuOwnerProcess = 0;
            if (haveGuiInfo && info.hwndMenuOwner &&
                (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) &&
                GetWindowThreadProcessId(info.hwndMenuOwner, &menuOwnerProcess) == active->targetThread &&
                menuOwnerProcess == targetProcess)
                popup.strongTargetRelated = true;
        }
        if (!GetWindowRect(window, &popup.bounds)) return;
        const auto position = active->session.Observe(popup, GetTickCount());
        active->popups = active->session.Bindings();
        if (event != EVENT_OBJECT_HIDE && active->diagnosticSamples++ < 8)
        {
            wchar_t message[768]{};
            swprintf_s(message, L"Tray menu observation pid=%lu hwnd=%p event=%lu age=%lu eventAge=%ld thread=%lu callbackThread=%lu style=0x%llX ex=0x%llX visible=%u related=%u strong=%u gui=%u active=%p foreground=%p rect=(%ld,%ld,%ld,%ld) correction=%u",
                popup.process, window, event, GetTickCount() - active->request.started,
                static_cast<LONG>(popup.eventTime - active->request.started), popup.thread, popup.notificationThread,
                static_cast<unsigned long long>(popup.style), static_cast<unsigned long long>(popup.extendedStyle),
                popup.visible ? 1u : 0u, popup.targetRelated ? 1u : 0u, popup.strongTargetRelated ? 1u : 0u,
                haveGuiInfo ? 1u : 0u, reinterpret_cast<HWND>(popup.activeWindow),
                reinterpret_cast<HWND>(popup.foregroundWindow), popup.bounds.left, popup.bounds.top,
                popup.bounds.right, popup.bounds.bottom, position ? 1u : 0u);
            WriteDiagnosticLogEntry(message, DiagnosticLogLevel::Debug);
        }
        if (!position) return;
        // Recheck process/style at the mutation boundary. Never activate,
        // resize, change z-order, or move an unrelated/reused main HWND.
        DWORD process = 0; const auto thread = GetWindowThreadProcessId(window, &process);
        if (process != popup.process || thread != popup.thread || !IsWindowVisible(window) ||
            GetWindowLongPtrW(window, GWL_STYLE) != popup.style ||
            GetWindowLongPtrW(window, GWL_EXSTYLE) != popup.extendedStyle || GetWindow(window, GW_OWNER) != owner)
        { active->session.RejectCorrection(popup.window); return; }
        if (SetWindowPos(window, nullptr, position->x, position->y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS))
            WriteDiagnosticLogEntry(L"Tray menu placement correction requested", DiagnosticLogLevel::Debug);
        else active->session.RejectCorrection(popup.window);
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
                diagnosticSamples = 0;
                DWORD process = 0; targetThread = GetWindowThreadProcessId(next.target, &process);
                if (next.target && process && process == next.process &&
                    static_cast<DWORD>(GetTickCount() - next.started) < MenuPlacementSession::kLifetimeMs)
                {
                    MONITORINFO monitor{sizeof(monitor)};
                    if (GetMonitorInfoW(MonitorFromRect(&next.bounds, MONITOR_DEFAULTTONEAREST), &monitor))
                    {
                        session.Arm(process, next.anchor, monitor.rcWork, next.started, next.barBounds, next.bounds, next.contextGesture);
                        hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, Event,
                            process, 0, WINEVENT_OUTOFCONTEXT);
                        if (!hook) session.Cancel();
                        wchar_t message[384]{};
                        swprintf_s(message, L"Tray menu session pid=%lu target=%p thread=%lu started=%lu hook=%u context=%u rect=(%ld,%ld,%ld,%ld)",
                            process, target, targetThread, next.started, hook ? 1u : 0u, next.contextGesture ? 1u : 0u,
                            next.bounds.left, next.bounds.top, next.bounds.right, next.bounds.bottom);
                        WriteDiagnosticLogEntry(message, DiagnosticLogLevel::Debug);
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
void MenuPlacementGuard::Arm(HWND target, POINT anchor, RECT iconBounds, bool continuation, RECT barBounds, bool contextGesture)
{
    DWORD process = 0; GetWindowThreadProcessId(target, &process);
    if (!process) return;
    std::unique_lock lock(impl_->mutex);
    if (!impl_->wake || !impl_->stop) return;
    if (!impl_->worker.joinable()) impl_->worker = std::jthread([state = impl_.get()](std::stop_token token) { state->Run(token); });
    const DWORD now = GetTickCount();
    if (continuation && impl_->request.target == target && impl_->request.process == process &&
        impl_->request.contextGesture == contextGesture &&
        static_cast<DWORD>(now - impl_->request.started) < MenuPlacementSession::kLifetimeMs) return;
    if (IsRectEmpty(&iconBounds)) iconBounds = {anchor.x, anchor.y, anchor.x + 1, anchor.y + 1};
    impl_->request = {target, process, anchor, iconBounds, barBounds, now, contextGesture};
    impl_->popups = {};
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
    if (impl_->request.target)
    {
        wchar_t message[192]{};
        swprintf_s(message, L"Tray menu session canceled pid=%lu age=%lu", impl_->request.process,
            GetTickCount() - impl_->request.started);
        WriteDiagnosticLogEntry(message, DiagnosticLogLevel::Debug);
    }
    impl_->request = {}; impl_->popups = {}; ++impl_->requested; SetEvent(impl_->wake);
}
MenuPopupBindings MenuPlacementGuard::Popups(HWND target) const
{
    std::lock_guard lock(impl_->mutex);
    return target && impl_->request.target == target ? impl_->popups : MenuPopupBindings{};
}
}
