#include "tray_menu_first_show.h"
#include "tray_appbar_position.h"
#ifdef _WIN64
#include "tray_douyin_hover.h"
#endif

#include <commctrl.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace snowdesktop::tray::first_show
{
namespace
{
constexpr UINT_PTR kHoverSubclass = 0x53444D46;
constexpr ULONGLONG kQtLayoutQuietMs = 64;
constexpr ULONGLONG kQtPresentationLimitMs = 180;
struct Watch
{
    Request request;
    HHOOK hook = nullptr;
    UINT_PTR timer = 0;
    HWND candidate = nullptr;
    HWND shadow = nullptr;
    RECT chromiumStaging{};
    UINT_PTR qtPresentationTimer = 0;
    ULONGLONG qtFirstShow = 0;
    ULONGLONG qtLastLayout = 0;
    bool qtCandidateCloaked = false;
    bool qtShadowCloaked = false;
    bool qtPresented = false;
    bool wpsLayout = false;
    bool knownChromiumLayout = false;
    wchar_t qtPrefix[80]{};
};
thread_local Watch watch;

LRESULT CALLBACK HoverProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

POINT WpsShadowOrigin(const RECT& card, const RECT& shadow)
{
    // WPS uses 1:1 horizontal and 1:2 vertical shadow padding.
    return {card.left - (shadow.right - shadow.left - (card.right - card.left)) / 2,
        card.top - (shadow.bottom - shadow.top - (card.bottom - card.top)) / 3};
}

POINT HoverOffset(const Request& request, const RECT& bounds);
bool ConvertHoverGeometry(Request& request);

void ReleaseQtPresentation(const Watch& previous)
{
    if (previous.qtPresentationTimer) KillTimer(nullptr, previous.qtPresentationTimer);
    const BOOL uncloak = FALSE;
    // Reveal the owned shadow before its owner so both become compositable
    // together when the owner's inherited cloak clears.
    if (previous.qtShadowCloaked && IsWindow(previous.shadow))
        DwmSetWindowAttribute(previous.shadow, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
    if (previous.qtCandidateCloaked && IsWindow(previous.candidate))
        DwmSetWindowAttribute(previous.candidate, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
}

void StopWatching()
{
    const auto previous = watch;
    watch = {};
    ReleaseQtPresentation(previous);
    if (previous.candidate) RemoveWindowSubclass(previous.candidate, HoverProcedure, kHoverSubclass);
    if (previous.shadow) RemoveWindowSubclass(previous.shadow, HoverProcedure, kHoverSubclass);
    if (previous.timer) KillTimer(nullptr, previous.timer);
    if (previous.hook) UnhookWindowsHookEx(previous.hook);
}

void CALLBACK PresentQt(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (timer != watch.qtPresentationTimer) return;
    const auto now = GetTickCount64();
    if (now - watch.qtFirstShow < kQtPresentationLimitMs &&
        (!IsWindowVisible(watch.candidate) || (!watch.wpsLayout && !IsWindowVisible(watch.shadow)) ||
            now - watch.qtLastLayout < kQtLayoutQuietMs)) return;
    RECT card{}, shadow{};
    if (watch.qtShadowCloaked && IsWindowVisible(watch.candidate) &&
        GetWindowRect(watch.candidate, &card) && GetWindowRect(watch.shadow, &shadow) &&
        !IsRectEmpty(&card) && !IsRectEmpty(&shadow))
    {
        constexpr UINT flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
        if (watch.wpsLayout)
        {
            // The WPS shadow may still reflect the application's original owner position.
            const auto origin = WpsShadowOrigin(card, shadow);
            if (origin.x != shadow.left || origin.y != shadow.top)
                SetWindowPos(watch.shadow, nullptr, origin.x, origin.y, 0, 0, flags);
        }
        else if (watch.qtCandidateCloaked && IsWindowVisible(watch.shadow))
        {
            // Preserve the application's settled offset between the card and its shadow.
            auto request = watch.request;
            if (ConvertHoverGeometry(request))
            {
                const auto offset = HoverOffset(request, card);
                if (offset.x || offset.y)
                {
                    const auto positions = BeginDeferWindowPos(2);
                    const auto movedCard = positions ? DeferWindowPos(positions, watch.candidate, nullptr,
                        card.left + offset.x, card.top + offset.y, 0, 0, flags) : nullptr;
                    const auto movedPair = movedCard ? DeferWindowPos(movedCard, watch.shadow, nullptr,
                        shadow.left + offset.x, shadow.top + offset.y, 0, 0, flags) : nullptr;
                    if (movedPair) EndDeferWindowPos(movedPair);
                }
            }
        }
    }
    if (timer != watch.qtPresentationTimer) return;
    const auto previous = watch;
    watch.qtPresentationTimer = 0;
    watch.qtCandidateCloaked = false;
    watch.qtShadowCloaked = false;
    watch.qtPresented = true;
    ReleaseQtPresentation(previous);
}

void StageQtPresentation(HWND window, bool& cloaked)
{
    if (watch.qtPresented || cloaked) return;
    DWORD flags = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags))) ||
        (flags & DWM_CLOAKED_APP)) return;
    if (!watch.qtPresentationTimer)
    {
        watch.qtPresentationTimer = SetTimer(nullptr, 0, 16, PresentQt);
        if (!watch.qtPresentationTimer) return;
        watch.qtFirstShow = watch.qtLastLayout = GetTickCount64();
    }
    const BOOL cloak = TRUE;
    cloaked = true;
    if (FAILED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak)))) cloaked = false;
}

void CALLBACK Expire(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (watch.timer == timer && GetTickCount64() >= watch.request.expires) StopWatching();
}

bool IsQtWindow(HWND window, std::wstring_view suffix)
{
    wchar_t name[80]{};
    if (!GetClassNameW(window, name, static_cast<int>(std::size(name)))) return false;
    const std::wstring_view className(name), prefix(watch.qtPrefix);
    return !prefix.empty() && className.starts_with(prefix) && className.substr(prefix.size()) == suffix;
}

bool IsPopup(HWND window, Surface surface)
{
    if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() || GetWindow(window, GW_OWNER))
        return false;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & (WS_CHILD | WS_SYSMENU | WS_THICKFRAME)) ||
        (extended & (WS_EX_APPWINDOW | WS_EX_TRANSPARENT))) return false;
    if (surface == Surface::ChromiumHover)
        return (extended & WS_EX_LAYERED) && HasClass(window, L"Chrome_WidgetWin_1") &&
            (watch.knownChromiumLayout || (extended & WS_EX_TOPMOST));
    if (surface == Surface::DouyinHover)
        return (extended & WS_EX_NOACTIVATE) && (style & WS_POPUP) && HasClass(window, L"LynxMessageBox");
    if (!(extended & WS_EX_TOOLWINDOW) || (style & WS_CAPTION) == WS_CAPTION) return false;
    return IsQtWindow(window, L"QWindowToolSaveBits");
}

bool IsShadow(HWND window)
{
    return GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId() &&
        IsQtWindow(window, L"QWindowIcon") &&
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0 &&
        IsPopup(GetWindow(window, GW_OWNER), Surface::QtToolHover);
}

BOOL CALLBACK FindQtShadow(HWND window, LPARAM context)
{
    auto& match = *reinterpret_cast<HWND*>(context);
    if (GetWindow(window, GW_OWNER) != match || !IsShadow(window)) return TRUE;
    match = nullptr;
    return FALSE;
}

bool HasQtShadow(HWND owner)
{
    EnumThreadWindows(GetCurrentThreadId(), FindQtShadow, reinterpret_cast<LPARAM>(&owner));
    return !owner;
}

bool IsProcessImage(const wchar_t* expected)
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    const auto* separator = wcsrchr(path, L'\\');
    return separator && _wcsicmp(separator + 1, expected) == 0;
}

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

POINT HoverOffset(const Request& request, const RECT& bounds)
{
    RECT overlap{};
    // Chromium may show the first window far outside the desktop while
    // producing its content. Preserve staging and handle its first on-screen move.
    const bool intersectsRow = IntersectRect(&overlap, &bounds, &request.row) != FALSE;
    const bool contained = bounds.left >= request.work.left && bounds.right <= request.work.right &&
        bounds.top >= request.work.top && bounds.bottom <= request.work.bottom;
    if (request.surface == Surface::DouyinHover || request.surface == Surface::QtToolHover)
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

LRESULT CALLBACK HoverProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (message == WM_NCDESTROY && window == watch.candidate)
        StopWatching();
    else if (message == WM_NCDESTROY && window == watch.shadow)
    {
        watch.shadow = nullptr;
        watch.qtShadowCloaked = false;
        RemoveWindowSubclass(window, HoverProcedure, kHoverSubclass);
    }
    if (message == WM_WINDOWPOSCHANGED && lp && watch.request.surface == Surface::QtToolHover)
    {
        const auto& position = *reinterpret_cast<const WINDOWPOS*>(lp);
        if ((position.flags & SWP_HIDEWINDOW) && window == watch.candidate)
            StopWatching();
        else if (watch.qtPresentationTimer &&
            (position.flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) != (SWP_NOMOVE | SWP_NOSIZE))
            watch.qtLastLayout = GetTickCount64();
    }
    if (message != WM_WINDOWPOSCHANGING || !lp)
        return DefSubclassProc(window, message, wp, lp);
    auto& position = *reinterpret_cast<WINDOWPOS*>(lp);
    if ((position.flags & SWP_HIDEWINDOW) && window == watch.candidate)
    {
        if (watch.request.surface != Surface::QtToolHover) StopWatching();
        return DefSubclassProc(window, message, wp, lp);
    }
    auto request = watch.request;
    const auto result = DefSubclassProc(window, message, wp, lp);
    // Apply after the application handles the message, before User32 commits
    // the move and show together. The card keeps its original size and z-order.
    if (GetTickCount64() >= request.expires || (position.flags & SWP_HIDEWINDOW)) return result;
    if (request.surface == Surface::QtToolHover && !watch.wpsLayout) return result;
    const RECT bounds = ProposedBounds(window, position);
    if ((window != watch.candidate && window != watch.shadow) ||
        (position.flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) == (SWP_NOMOVE | SWP_NOSIZE) ||
        !ConvertHoverGeometry(request)) return result;
    if (request.surface == Surface::ChromiumHover && !IsRectEmpty(&watch.chromiumStaging))
    {
        const RECT nearby{request.row.left - 512, request.row.top - 512,
            request.row.right + 512, request.row.bottom + 512};
        RECT overlap{};
        if (!IntersectRect(&overlap, &bounds, &nearby))
        {
            // Chromium/DefWindowProc can clamp a staged SHOW to the top-left
            // monitor. Keep that transaction at the application's staging
            // coordinates until it requests a position beside the tray.
            SetPosition(position, watch.chromiumStaging.left, watch.chromiumStaging.top);
            return result;
        }
        watch.chromiumStaging = {};
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

LRESULT CALLBACK WatchHover(int code, WPARAM wp, LPARAM lp)
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
            const bool movingChromium = watch.knownChromiumLayout && IsWindowVisible(message.hwnd) &&
                !(position.flags & SWP_NOMOVE);
            if (!watch.candidate && (showing || movingChromium) && IsPopup(message.hwnd, watch.request.surface))
            {
                const auto bounds = ProposedBounds(message.hwnd, position);
                const RECT vicinity{watch.request.icon.left - 512, watch.request.icon.top - 512,
                    watch.request.icon.right + 512, watch.request.icon.bottom + 512};
                RECT overlap{};
                const bool chromiumCard = watch.request.surface != Surface::ChromiumHover ||
                    (!IsRectEmpty(&bounds) && bounds.right - bounds.left <= 640 && bounds.bottom - bounds.top <= 480 &&
                        ((bounds.right < -8000 && bounds.bottom < -8000) || IntersectRect(&overlap, &bounds, &vicinity)));
                const bool qtCard = watch.request.surface != Surface::QtToolHover ||
                    watch.wpsLayout || HasQtShadow(message.hwnd);
                if (chromiumCard && qtCard && SetWindowSubclass(message.hwnd, HoverProcedure, kHoverSubclass, 0))
                {
                    watch.candidate = message.hwnd;
#ifdef _WIN64
                    if (watch.request.surface == Surface::DouyinHover) douyin::Bind(message.hwnd);
#endif
                    if (watch.request.surface == Surface::ChromiumHover && bounds.right < -8000 && bounds.bottom < -8000)
                        watch.chromiumStaging = bounds;
                }
            }
            if (!watch.shadow && showing && watch.request.surface == Surface::QtToolHover && IsShadow(message.hwnd))
            {
                const auto owner = GetWindow(message.hwnd, GW_OWNER);
                if (!watch.candidate && (watch.wpsLayout || !IsWindowVisible(owner)) &&
                    SetWindowSubclass(owner, HoverProcedure, kHoverSubclass, 0))
                    watch.candidate = owner;
                if (owner == watch.candidate && SetWindowSubclass(message.hwnd, HoverProcedure, kHoverSubclass, 0))
                    watch.shadow = message.hwnd;
            }
            if (showing && watch.request.surface == Surface::QtToolHover)
            {
                if (message.hwnd == watch.candidate)
                    StageQtPresentation(message.hwnd, watch.qtCandidateCloaked);
                else if (message.hwnd == watch.shadow)
                    StageQtPresentation(message.hwnd, watch.qtShadowCloaked);
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
        (request.surface != Surface::AppBarOnly && IsRectEmpty(&request.row)))
        return;
    // The query session is independent of window classification and may be
    // used by any tray client. Its answer expires even without a popup.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&WatchHover), &pinned)) return;
    const bool appbarArmed = appbar::Arm(request, static_cast<DWORD>(message.wParam));
    if (request.surface == Surface::AppBarOnly)
    {
        if (appbarArmed) InterlockedExchange(&view->armed, 1);
        return;
    }
    wchar_t qtPrefix[80]{};
    bool wpsLayout = false;
    bool knownChromiumLayout = false;
    switch (request.surface)
    {
    case Surface::ChromiumHover:
        if (!HasClass(message.hwnd, kElectronTrayClass)) return;
        knownChromiumLayout = IsProcessImage(L"QQ.exe");
        break;
    case Surface::QtToolHover:
        wpsLayout = IsProcessImage(L"wpscloudsvr.exe");
        if (wpsLayout)
            StringCchCopyW(qtPrefix, std::size(qtPrefix), L"Qt5");
        else if (!ReadQt5TrayPrefix(message.hwnd, qtPrefix))
            return;
        break;
#ifdef _WIN64
    case Surface::DouyinHover:
        if (!HasClass(message.hwnd, L"TRAY_CLASS")) return;
        break;
#endif
    default:
        return;
    }

    StopWatching();
    watch.request = request;
    watch.wpsLayout = wpsLayout;
    watch.knownChromiumLayout = knownChromiumLayout;
    StringCchCopyW(watch.qtPrefix, std::size(watch.qtPrefix), qtPrefix);
    watch.hook = SetWindowsHookExW(WH_CALLWNDPROC, WatchHover, nullptr, GetCurrentThreadId());
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
