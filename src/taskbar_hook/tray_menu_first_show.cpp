#include "tray_menu_first_show.h"
#include "tray_appbar_position.h"
#ifdef _WIN64
#include "tray_douyin_hover.h"
#endif

#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>

namespace snowdesktop::tray::first_show
{
namespace
{
constexpr UINT_PTR kMenuSubclass = 0x53444D46;
constexpr ULONGLONG kWpsLayoutQuietMs = 64;
constexpr ULONGLONG kWpsPresentationLimitMs = 180;
struct Watch
{
    Request request;
    HHOOK hook = nullptr;
    UINT_PTR timer = 0;
    HWND candidate = nullptr;
    HWND shadow = nullptr;
    RECT qqStaging{};
    UINT_PTR wpsPresentationTimer = 0;
    ULONGLONG wpsFirstShow = 0;
    ULONGLONG wpsLastLayout = 0;
    bool wpsCandidateCloaked = false;
    bool wpsShadowCloaked = false;
    bool wpsPresented = false;
};
thread_local Watch watch;

LRESULT CALLBACK MenuProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

POINT WpsShadowOrigin(const RECT& card, const RECT& shadow)
{
    // WPS pads its transparent surface equally on the horizontal axis.
    // The vertical padding is distributed 1:2 above and below the card.
    return {card.left - (shadow.right - shadow.left - (card.right - card.left)) / 2,
        card.top - (shadow.bottom - shadow.top - (card.bottom - card.top)) / 3};
}

void ReleaseWpsPresentation(const Watch& previous)
{
    if (previous.wpsPresentationTimer) KillTimer(nullptr, previous.wpsPresentationTimer);
    const BOOL uncloak = FALSE;
    // Reveal the owned shadow before its owner so both become compositable
    // together when the owner's inherited cloak clears.
    if (previous.wpsShadowCloaked && IsWindow(previous.shadow))
        DwmSetWindowAttribute(previous.shadow, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
    if (previous.wpsCandidateCloaked && IsWindow(previous.candidate))
        DwmSetWindowAttribute(previous.candidate, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
}

void StopWatching()
{
    const auto previous = watch;
    watch = {};
    ReleaseWpsPresentation(previous);
    if (previous.candidate) RemoveWindowSubclass(previous.candidate, MenuProcedure, kMenuSubclass);
    if (previous.shadow) RemoveWindowSubclass(previous.shadow, MenuProcedure, kMenuSubclass);
    if (previous.timer) KillTimer(nullptr, previous.timer);
    if (previous.hook) UnhookWindowsHookEx(previous.hook);
}

void CALLBACK PresentWps(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (timer != watch.wpsPresentationTimer) return;
    const auto now = GetTickCount64();
    if (now - watch.wpsFirstShow < kWpsPresentationLimitMs &&
        (!IsWindowVisible(watch.candidate) || now - watch.wpsLastLayout < kWpsLayoutQuietMs)) return;
    // Snap the shadow while it is still cloaked. It may have been positioned
    // before Qt's owner geometry was final, so use the settled rectangles.
    RECT card{}, shadow{};
    if (watch.wpsShadowCloaked && IsWindowVisible(watch.candidate) &&
        GetWindowRect(watch.candidate, &card) && GetWindowRect(watch.shadow, &shadow) &&
        !IsRectEmpty(&card) && !IsRectEmpty(&shadow))
    {
        const auto origin = WpsShadowOrigin(card, shadow);
        if (origin.x != shadow.left || origin.y != shadow.top)
            SetWindowPos(watch.shadow, nullptr, origin.x, origin.y, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
    const auto previous = watch;
    watch.wpsPresentationTimer = 0;
    watch.wpsCandidateCloaked = false;
    watch.wpsShadowCloaked = false;
    watch.wpsPresented = true;
    ReleaseWpsPresentation(previous);
}

void StageWpsPresentation(HWND window, bool& cloaked)
{
    if (watch.wpsPresented || cloaked) return;
    DWORD flags = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags))) ||
        (flags & DWM_CLOAKED_APP)) return;
    if (!watch.wpsPresentationTimer)
    {
        watch.wpsPresentationTimer = SetTimer(nullptr, 0, 16, PresentWps);
        if (!watch.wpsPresentationTimer) return;
        watch.wpsFirstShow = watch.wpsLastLayout = GetTickCount64();
    }
    const BOOL cloak = TRUE;
    cloaked = true;
    if (FAILED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak)))) cloaked = false;
}

void CALLBACK Expire(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (watch.timer == timer && GetTickCount64() >= watch.request.expires) StopWatching();
}

bool HasClass(HWND window, const wchar_t* expected)
{
    wchar_t name[80]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return wcscmp(name, expected) == 0;
}

bool IsPopup(HWND window, Surface surface)
{
    if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() || GetWindow(window, GW_OWNER))
        return false;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & (WS_CHILD | WS_SYSMENU | WS_THICKFRAME)) ||
        (extended & (WS_EX_APPWINDOW | WS_EX_TRANSPARENT))) return false;
    if (surface == Surface::QQHover)
        return (extended & WS_EX_LAYERED) && HasClass(window, L"Chrome_WidgetWin_1");
    if (surface == Surface::DouyinHover)
        return (extended & WS_EX_NOACTIVATE) && (style & WS_POPUP) && HasClass(window, L"LynxMessageBox");
    if (!(extended & WS_EX_TOOLWINDOW) || (style & WS_CAPTION) == WS_CAPTION) return false;
    return HasClass(window, surface == Surface::WpsHover ? L"Qt5QWindowToolSaveBits" : L"Qt51514QWindowPopupSaveBits");
}

bool IsShadow(HWND window)
{
    return HasClass(window, L"Qt5QWindowIcon") &&
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0 &&
        IsPopup(GetWindow(window, GW_OWNER), Surface::WpsHover);
}

#ifndef _WIN64
bool IsWpsProcess()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    const auto* separator = wcsrchr(path, L'\\');
    return separator && _wcsicmp(separator + 1, L"wpscloudsvr.exe") == 0;
}
#endif

RECT ProposedBounds(HWND window, const WINDOWPOS& position)
{
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return {};
    const auto width = position.flags & SWP_NOSIZE ? bounds.right - bounds.left : position.cx;
    const auto height = position.flags & SWP_NOSIZE ? bounds.bottom - bounds.top : position.cy;
    const auto x = position.flags & SWP_NOMOVE ? bounds.left : position.x;
    const auto y = position.flags & SWP_NOMOVE ? bounds.top : position.y;
    return {x, y, x + width, y + height};
}

void SetPosition(WINDOWPOS& position, LONG x, LONG y)
{
    position.x = x;
    position.y = y;
    position.flags &= ~static_cast<UINT>(SWP_NOMOVE);
}

void PlaceMenu(const Request& request, const RECT& bounds, WINDOWPOS& position)
{
    const auto& work = request.work;
    using Coordinate = std::int64_t;
    const Coordinate width = static_cast<Coordinate>(bounds.right) - bounds.left;
    const Coordinate height = static_cast<Coordinate>(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0 || width > static_cast<Coordinate>(work.right) - work.left ||
        height > static_cast<Coordinate>(work.bottom) - work.top) return;
    const Coordinate x = bounds.left, y = bounds.top;
    constexpr Coordinate proximity = 96;
    const bool nearX = request.anchor.x >= x - proximity && request.anchor.x <= x + width + proximity;
    const bool nearY = request.anchor.y >= y - proximity && request.anchor.y <= y + height + proximity;
    Coordinate desiredX = x, desiredY = y;
    if (!nearX || !nearY)
    {
        if (!nearX) desiredX = request.anchor.x;
        const Coordinate above = request.icon.top - height, below = request.icon.bottom;
        bool preferBelow = static_cast<Coordinate>(request.icon.top) + request.icon.bottom <=
            static_cast<Coordinate>(work.top) + work.bottom;
        if (preferBelow && below + height > work.bottom && above >= work.top) preferBelow = false;
        if (!preferBelow && above < work.top && below + height <= work.bottom) preferBelow = true;
        desiredY = preferBelow ? below : above;
    }
    desiredX = std::clamp(desiredX, static_cast<Coordinate>(work.left), work.right - width);
    desiredY = std::clamp(desiredY, static_cast<Coordinate>(work.top), work.bottom - height);
    if (desiredX == x && desiredY == y) return;
    SetPosition(position, static_cast<LONG>(desiredX), static_cast<LONG>(desiredY));
}

POINT HoverOffset(const Request& request, const RECT& bounds)
{
    RECT overlap{};
    // Chromium may show the first window far outside the desktop while
    // producing its content. Preserve staging and handle its first on-screen move.
    const bool intersectsRow = IntersectRect(&overlap, &bounds, &request.row) != FALSE;
    const bool contained = bounds.left >= request.work.left && bounds.right <= request.work.right &&
        bounds.top >= request.work.top && bounds.bottom <= request.work.bottom;
    if (request.surface == Surface::DouyinHover || request.surface == Surface::WpsHover)
    {
        if (!intersectsRow && contained) return {};
    }
    else if (!intersectsRow || !IntersectRect(&overlap, &bounds, &request.work)) return {};
    const auto& work = request.work;
    const auto width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || width > work.right - work.left || height > work.bottom - work.top) return {};
    const auto x = std::clamp(bounds.left, work.left, work.right - width);
    const auto y = std::clamp(bounds.top, work.top, work.bottom - height);
    const POINT above{x, request.row.top - request.gap - height};
    const POINT below{x, request.row.bottom + request.gap};
    const POINT left{request.row.left - request.gap - width, y};
    const POINT right{request.row.right + request.gap, y};
    const std::array positions{request.preferAbove ? above : below, request.preferAbove ? below : above, left, right};
    for (const auto point : positions)
        if (point.x >= work.left && point.x <= work.right - width &&
            point.y >= work.top && point.y <= work.bottom - height)
            return {point.x - bounds.left, point.y - bounds.top};
    return {};
}

bool ConvertHoverGeometry(Request& request)
{
    // User32 passes WINDOWPOS in the window thread's coordinate space.
    // Look up the same monitor in both spaces. A window-relative point
    // transform rejects work-area corners outside the popup itself.
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!previous) return false;
    const auto monitor = MonitorFromPoint(request.anchor, MONITOR_DEFAULTTONULL);
    MONITORINFO physical{sizeof(physical)};
    const bool found = monitor && GetMonitorInfoW(monitor, &physical);
    SetThreadDpiAwarenessContext(previous);
    MONITORINFO logical{sizeof(logical)};
    if (!found || !GetMonitorInfoW(monitor, &logical)) return false;
    const auto& from = physical.rcMonitor;
    const auto& to = logical.rcMonitor;
    const LONG physicalWidth = from.right - from.left, physicalHeight = from.bottom - from.top;
    const LONG logicalWidth = to.right - to.left, logicalHeight = to.bottom - to.top;
    if (physicalWidth <= 0 || physicalHeight <= 0 || logicalWidth <= 0 || logicalHeight <= 0) return false;
    const auto x = [&](LONG value) { return to.left + MulDiv(value - from.left, logicalWidth, physicalWidth); };
    const auto y = [&](LONG value) { return to.top + MulDiv(value - from.top, logicalHeight, physicalHeight); };
    request.row = {x(request.row.left), y(request.row.top), x(request.row.right), y(request.row.bottom)};
    request.work = logical.rcWork;
    request.gap = MulDiv(request.gap, logicalHeight, physicalHeight);
    return true;
}

LRESULT CALLBACK MenuProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (message == WM_NCDESTROY && window == watch.candidate)
        StopWatching();
    else if (message == WM_NCDESTROY && window == watch.shadow)
    {
        watch.shadow = nullptr;
        watch.wpsShadowCloaked = false;
        RemoveWindowSubclass(window, MenuProcedure, kMenuSubclass);
    }
    if (message == WM_WINDOWPOSCHANGED && lp && watch.request.surface == Surface::WpsHover)
    {
        const auto& position = *reinterpret_cast<const WINDOWPOS*>(lp);
        if ((position.flags & SWP_HIDEWINDOW) && window == watch.candidate)
            StopWatching();
        else if (watch.wpsPresentationTimer &&
            (position.flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) != (SWP_NOMOVE | SWP_NOSIZE))
            watch.wpsLastLayout = GetTickCount64();
    }
    if (message != WM_WINDOWPOSCHANGING || !lp)
        return DefSubclassProc(window, message, wp, lp);
    auto& position = *reinterpret_cast<WINDOWPOS*>(lp);
    if ((position.flags & SWP_HIDEWINDOW) && window == watch.candidate)
    {
        if (watch.request.surface != Surface::WpsHover) StopWatching();
        return DefSubclassProc(window, message, wp, lp);
    }
    auto request = watch.request;
    if (request.surface == Surface::WeixinMenu)
    {
        if (!(position.flags & SWP_SHOWWINDOW)) return DefSubclassProc(window, message, wp, lp);
        StopWatching();
    }
    const auto result = DefSubclassProc(window, message, wp, lp);
    // Apply after the application handles the message, before User32 commits
    // the move and show together. The menu keeps its original size and z-order.
    if (GetTickCount64() >= request.expires || (position.flags & SWP_HIDEWINDOW)) return result;
    const RECT bounds = ProposedBounds(window, position);
    if (request.surface == Surface::WeixinMenu)
    {
        if ((position.flags & SWP_SHOWWINDOW) && IsPopup(window, request.surface))
            PlaceMenu(request, bounds, position);
        return result;
    }
    if ((window != watch.candidate && window != watch.shadow) ||
        (position.flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) == (SWP_NOMOVE | SWP_NOSIZE) ||
        !ConvertHoverGeometry(request)) return result;
    if (request.surface == Surface::QQHover && !IsRectEmpty(&watch.qqStaging))
    {
        const RECT nearby{request.row.left - 512, request.row.top - 512,
            request.row.right + 512, request.row.bottom + 512};
        RECT overlap{};
        if (!IntersectRect(&overlap, &bounds, &nearby))
        {
            // Chromium/DefWindowProc can clamp a staged SHOW to the top-left
            // monitor. Keep that transaction at the application's staging
            // coordinates until it requests a position beside the tray.
            SetPosition(position, watch.qqStaging.left, watch.qqStaging.top);
            return result;
        }
        watch.qqStaging = {};
    }
    if (window == watch.shadow)
    {
        RECT ownerBounds{};
        if (GetWindowRect(watch.candidate, &ownerBounds) && !IsRectEmpty(&ownerBounds) && !IsRectEmpty(&bounds))
        {
            const auto origin = WpsShadowOrigin(ownerBounds, bounds);
            if (origin.x != bounds.left || origin.y != bounds.top)
                SetPosition(position, origin.x, origin.y);
        }
        return result;
    }
    const auto offset = HoverOffset(request, bounds);
    if (offset.x || offset.y) SetPosition(position, bounds.left + offset.x, bounds.top + offset.y);
    return result;
}

LRESULT CALLBACK WatchMenu(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && lp && watch.hook)
    {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lp);
        if (GetTickCount64() >= watch.request.expires)
            StopWatching();
        // The hook identifies the pending show. The window procedure below
        // owns the actual edit to the writable WINDOWPOS.
        else if (message.message == WM_WINDOWPOSCHANGING && message.lParam &&
            GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<UINT_PTR>(watch.request.receiver)), nullptr) == GetCurrentThreadId())
        {
            const auto& position = *reinterpret_cast<const WINDOWPOS*>(message.lParam);
            const bool showing = (position.flags & SWP_SHOWWINDOW) != 0;
            const bool movingQq = watch.request.surface == Surface::QQHover && IsWindowVisible(message.hwnd) &&
                !(position.flags & SWP_NOMOVE);
            if (!watch.candidate && (showing || movingQq) && IsPopup(message.hwnd, watch.request.surface))
            {
                const auto bounds = ProposedBounds(message.hwnd, position);
                const RECT vicinity{watch.request.icon.left - 512, watch.request.icon.top - 512,
                    watch.request.icon.right + 512, watch.request.icon.bottom + 512};
                RECT overlap{};
                const bool qqCard = watch.request.surface != Surface::QQHover ||
                    (!IsRectEmpty(&bounds) && bounds.right - bounds.left <= 640 && bounds.bottom - bounds.top <= 480 &&
                        ((bounds.right < -8000 && bounds.bottom < -8000) || IntersectRect(&overlap, &bounds, &vicinity)));
                if (qqCard && SetWindowSubclass(message.hwnd, MenuProcedure, kMenuSubclass, 0))
                {
                    watch.candidate = message.hwnd;
#ifdef _WIN64
                    if (watch.request.surface == Surface::DouyinHover) douyin::Bind(message.hwnd);
#endif
                    if (watch.request.surface == Surface::QQHover && bounds.right < -8000 && bounds.bottom < -8000)
                        watch.qqStaging = bounds;
                }
            }
            if (!watch.shadow && showing && watch.request.surface == Surface::WpsHover && IsShadow(message.hwnd))
            {
                const auto owner = GetWindow(message.hwnd, GW_OWNER);
                if (!watch.candidate && SetWindowSubclass(owner, MenuProcedure, kMenuSubclass, 0))
                    watch.candidate = owner;
                if (owner == watch.candidate && SetWindowSubclass(message.hwnd, MenuProcedure, kMenuSubclass, 0))
                    watch.shadow = message.hwnd;
            }
            if (showing && watch.request.surface == Surface::WpsHover)
            {
                if (message.hwnd == watch.candidate)
                    StageWpsPresentation(message.hwnd, watch.wpsCandidateCloaked);
                else if (message.hwnd == watch.shadow)
                    StageWpsPresentation(message.hwnd, watch.wpsShadowCloaked);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}

void Arm(const CWPSTRUCT& message)
{
    if (!message.wParam || !message.lParam ||
        GetWindowThreadProcessId(message.hwnd, nullptr) != GetCurrentThreadId()) return;
    const auto mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE,
        RequestName(static_cast<DWORD>(message.wParam), static_cast<ULONG_PTR>(message.lParam)).c_str());
    if (!mapping) return;
    auto* view = static_cast<Request*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Request)));
    CloseHandle(mapping);
    if (!view) return;
    struct Unmap { Request* view; ~Unmap() { UnmapViewOfFile(view); } } unmap{view};
    const auto request = *view;
    const auto now = GetTickCount64();
    if (request.receiver != reinterpret_cast<UINT_PTR>(message.hwnd) ||
        request.process != GetCurrentProcessId() ||
        request.expires <= now || request.expires - now > kLifetimeMs ||
        IsRectEmpty(&request.icon) || !PtInRect(&request.icon, request.anchor) || IsRectEmpty(&request.work) ||
        (request.surface != Surface::WeixinMenu && request.surface != Surface::AppBarOnly && IsRectEmpty(&request.row)))
        return;
    // The query session is independent of window classification and may be
    // used by any tray client. Its answer expires even without a popup.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&WatchMenu), &pinned)) return;
    const bool appbarArmed = appbar::Arm(request, static_cast<DWORD>(message.wParam));
    if (request.surface == Surface::AppBarOnly)
    {
        if (appbarArmed) InterlockedExchange(&view->armed, 1);
        return;
    }
#ifdef _WIN64
    if (request.surface == Surface::WeixinMenu)
    {
        if (!IsWeixinReceiver(message.hwnd)) return;
    }
    else if (request.surface == Surface::QQHover)
    {
        if (!HasClass(message.hwnd, L"Electron_NotifyIconHostWindow")) return;
    }
    else if (request.surface != Surface::DouyinHover || !HasClass(message.hwnd, L"TRAY_CLASS"))
        return;
#else
    if (request.surface != Surface::WpsHover || !IsWpsProcess()) return;
#endif

    StopWatching();
    watch.request = request;
    watch.hook = SetWindowsHookExW(WH_CALLWNDPROC, WatchMenu, nullptr, GetCurrentThreadId());
    if (watch.hook) watch.timer = SetTimer(nullptr, 0, kLifetimeMs, Expire);
    if (!watch.timer)
    {
        StopWatching();
        return;
    }
#ifdef _WIN64
    // Other client builds can still benefit from placing the card in view.
    if (request.surface == Surface::DouyinHover) douyin::Arm(request, static_cast<DWORD>(message.wParam));
#endif
    InterlockedExchange(&view->armed, 1);
}
}
