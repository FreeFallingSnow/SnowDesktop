#include "status_bar.h"
#include "status_bar_appearance.h"
#include "status_bar_view.h"
#include "status_bar_interaction.h"
#include "status_bar_appbar.h"
#include "widget_system_data_provider.h"
#include "app/desktop_backdrop_compositor.h"
#include "panel_gradient_renderer.h"
#include "l10n.h"
#include "tray_service.h"
#include "diagnostic_log.h"
#include "status_bar_layout.h"
#include "status_bar_glyphs.h"
#include "status_bar_presentation.h"
#include "system_controls.h"
#include "utils.h"

#include <dcomp.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <map>
#include <sstream>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT kAppBar = WM_APP + 41, kPlace = WM_APP + 42, kFullscreen = WM_APP + 43, kForeground = WM_APP + 44;
constexpr UINT_PTR kClockTimer = 1;
std::map<HWND, bool> liveBars; // All access, including out-of-context hooks, on the UI thread.
void CALLBACK WindowEvent(HWINEVENTHOOK, DWORD event, HWND target, LONG object, LONG, DWORD, DWORD time)
{
    if (object != OBJID_WINDOW) return;
    for (auto& [window, pending] : liveBars)
    {
        if (event == EVENT_SYSTEM_FOREGROUND)
            PostMessageW(window, kForeground, reinterpret_cast<WPARAM>(target), static_cast<LPARAM>(time));
        if (!pending) pending = PostMessageW(window, kFullscreen, 0, 0) != FALSE;
    }
}
bool HighContrast()
{
    HIGHCONTRASTW info{ sizeof(info) };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(info), &info, 0) &&
        (info.dwFlags & HCF_HIGHCONTRASTON);
}
D2D1_COLOR_F SystemColor(int color)
{
    const auto value = GetSysColor(color);
    return D2D1::ColorF(GetRValue(value) / 255.f, GetGValue(value) / 255.f,
        GetBValue(value) / 255.f);
}
bool MonitorHasFullscreen(HMONITOR monitor)
{
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return false;
    struct Context { RECT monitor; bool found = false; } context{info.rcMonitor};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window)) return TRUE;
        DWORD cloaked = 0;
        (void)DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        wchar_t name[128]{};
        GetClassNameW(window, name, static_cast<int>(std::size(name)));
        const bool shell = wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
            wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0;
        RECT client{};
        if (!GetClientRect(window, &client)) return TRUE;
        POINT origin{client.left, client.top}, end{client.right, client.bottom};
        if (!ClientToScreen(window, &origin) || !ClientToScreen(window, &end)) return TRUE;
        client = {origin.x, origin.y, end.x, end.y};
        if (StatusBarFullscreenClient(client, state.monitor, true, false, cloaked != 0, shell))
        {
            state.found = true;
            return FALSE;
        }
        // EnumWindows is in Z order. A regular window covering the monitor's
        // center hides a background full-screen window after Alt+Tab. Small
        // tooltips/owned tool windows do not cancel the full-screen state.
        const POINT center{state.monitor.left + (state.monitor.right - state.monitor.left) / 2,
            state.monitor.top + (state.monitor.bottom - state.monitor.top) / 2};
        if (!shell && !cloaked && PtInRect(&client, center) &&
            !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return FALSE;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.found;
}
UINT Edge(DockPosition position)
{
    switch (position)
    {
    case DockPosition::Bottom: return ABE_BOTTOM;
    case DockPosition::Left: return ABE_LEFT;
    case DockPosition::Right: return ABE_RIGHT;
    default: return ABE_TOP;
    }
}

}

struct StatusBar::Impl
{
    struct Window
    {
        Impl& owner;
        HWND hwnd = nullptr;
        HMONITOR monitor = nullptr;
        StatusBarAppBar appbar;
        DesktopBackdropCompositor backdrop;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual2> visual;
        ComPtr<IDCompositionVisual2> contentVisual;
        ComPtr<IDCompositionSurface> backgroundSurface;
        ComPtr<IDCompositionSurface> surface;
        UINT width = 0, height = 0, dpi = 96;
        DWORD explorerPid = 0;
        bool placing = false, queued = false, fullscreen = false, failed = false, closing = false;
        bool appearanceDirty = true, paintDirty = true, painting = false;
        bool backgroundDirty = true;
        HRESULT lastPaintError = S_OK;
        std::string hoveredTray;
        HWND tooltip = nullptr;
        StatusBarTooltipState tooltipState;
        StatusBarVolumeWheel volumeWheel;
        std::uint64_t volumeTask = 0;
        std::vector<StatusBarItem> items;
        StatusBarInteraction interaction;
        bool keyboardFocusVisible = false;
        explicit Window(Impl& value) : owner(value) {}
        ~Window()
        {
            closing = true;
            if (owner.tray) owner.tray->CancelFocusReturn(hwnd);
            ClearHover(); interaction.CancelPointer();
            liveBars.erase(hwnd);
            if (owner.hidden) owner.hidden(monitor);
            if (hwnd) KillTimer(hwnd, kClockTimer);
            if (tooltip) DestroyWindow(tooltip);
            appbar.Remove();
            backdrop.Reset();
            if (hwnd) DestroyWindow(hwnd);
        }
        void QueuePlace()
        {
            if (closing || placing || queued) return;
            queued = true;
            PostMessageW(hwnd, kPlace, 0, 0);
        }
        void ClearHover()
        {
            if (tooltip)
            {
                SendMessageW(tooltip, TTM_POP, 0, 0);
                TOOLINFOW tool{sizeof(tool)}; tool.hwnd = hwnd; tool.uId = reinterpret_cast<UINT_PTR>(hwnd);
                tool.lpszText = const_cast<wchar_t*>(L"");
                SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
            }
            tooltipState.Leave();
            if (!hoveredTray.empty() && owner.tray)
            {
                POINT screen{}; GetCursorPos(&screen);
                owner.tray->Activate(hoveredTray, tray::Activation::Leave, screen);
            }
            hoveredTray.clear(); interaction.hovered.reset();
        }
        void Hide()
        {
            if (owner.tray) owner.tray->CancelFocusReturn(hwnd);
            ClearHover(); interaction.CancelPointer(); keyboardFocusVisible = false;
            if (tooltip)
            {
                SendMessageW(tooltip, TTM_POP, 0, 0);
                SetWindowPos(tooltip, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
            }
            for (const auto& item : items) if (item.icon && owner.tray)
                owner.tray->SetGeometry(item.icon->key, {});
            backdrop.HidePopupWindowPair(hwnd);
            backdrop.SetPopupTopmost(false);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
            if (owner.hidden) owner.hidden(monitor);
        }
        void CheckFullscreen()
        {
            const bool next = MonitorHasFullscreen(monitor);
            if (next == fullscreen) return;
            fullscreen = next;
            if (fullscreen) Hide();
            else if (appbar.Registered() && !failed) Show();
        }
        void Show()
        {
            if (fullscreen || failed || !appbar.Registered()) return;
            if (!IsWindowVisible(hwnd) || !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
            {
                SetWindowPos(tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                backdrop.SetPopupWindowPairZOrder(hwnd, HWND_TOPMOST, true);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (owner.appearance.glassEnabled && !HighContrast()) backdrop.ShowPopupWindowPair(hwnd);
                paintDirty = true;
            }
            Paint();
        }
        void Place()
        {
            queued = false;
            if (closing || placing) return;
            placing = true;
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info)) { placing = false; Hide(); return; }
            UINT dpiY = 96;
            if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY))) dpi = 96;
            const UINT edge = Edge(owner.settings.position);
            const bool vertical = edge == ABE_LEFT || edge == ABE_RIGHT;
            const int thickness = static_cast<int>(std::lround(
                (vertical ? 48.f : 32.f) * owner.settings.scale * dpi / 96.f));
            const bool placed = appbar.Place(hwnd, kAppBar, edge, thickness, info.rcMonitor);
            if (!placed)
            {
                if (!failed && owner.error) owner.error(_LW("statusBar.registrationFailed"));
                failed = true;
                placing = false;
                Hide();
                return;
            }
            failed = false;
            const RECT rect = appbar.Bounds();
            RECT previous{};
            GetWindowRect(hwnd, &previous);
            const bool moved = !EqualRect(&rect, &previous);
            if (moved)
            {
                ClearHover(); interaction.CancelPointer();
                SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }
            if ((moved || appearanceDirty) && owner.appearance.glassEnabled && !HighContrast())
            {
                if (!backdrop.IsAvailable()) backdrop.InitializePopup(hwnd, !fullscreen, false);
                backdrop.Reattach(hwnd);
                backdrop.BeginFrame(true);
                backdrop.AddPanel({0, 0, rect.right - rect.left, rect.bottom - rect.top},
                    0, owner.appearance.glassBlurRadius * dpi / 96.f,
                    reinterpret_cast<std::uintptr_t>(this));
                backdrop.EndFrame();
            }
            else if (appearanceDirty && (!owner.appearance.glassEnabled || HighContrast())) backdrop.Reset();
            paintDirty = paintDirty || moved || appearanceDirty;
            backgroundDirty = backgroundDirty || moved || appearanceDirty;
            appearanceDirty = false;
            placing = false;
            CheckFullscreen();
            if (!fullscreen) Show();
        }
        void BuildItems()
        {
            StatusBarSnapshot snapshot;
            wchar_t time[64]{}, date[96]{};
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, time, 64);
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, date, 96, nullptr);
            snapshot.clock = std::wstring(date) + L"   " + time;
            snapshot.cpu = owner.data->Cpu(); snapshot.memory = owner.data->Memory();
            snapshot.gpu = owner.data->Gpu(); snapshot.traffic = owner.data->NetworkTraffic();
            snapshot.network = owner.data->NetworkStatus(); snapshot.audio = owner.data->AudioOutputVolume();
            snapshot.power = owner.data->Power();
            if (owner.tray) snapshot.tray = owner.tray->Current().icons;
            items = BuildStatusBarItems(owner.settings, snapshot);
        }

        void Paint()
        {
            if (closing || painting || fullscreen || failed || !IsWindowVisible(hwnd) || !owner.composition || !owner.text) return;
            auto previousItems = std::move(items);
            BuildItems();
            if (interaction.Reconcile(previousItems, items))
            {
                ClearHover();
                for (const auto& old : previousItems) if (old.icon && owner.tray &&
                    std::none_of(items.begin(), items.end(), [&](const auto& item) {
                        return item.icon && item.icon->key == old.icon->key;
                    })) owner.tray->SetGeometry(old.icon->key, {});
            }
            const bool same = SameStatusBarContent(items, previousItems);
            if (same && !paintDirty)
            {
                for (std::size_t i = 0; i < items.size(); ++i) items[i].bounds = previousItems[i].bounds;
                return;
            }
            RECT client{};
            GetClientRect(hwnd, &client);
            if (IsRectEmpty(&client)) return;
            const UINT w = static_cast<UINT>(client.right), h = static_cast<UINT>(client.bottom);
            if (!target && FAILED(owner.composition->CreateTargetForHwnd(hwnd, FALSE, &target))) return;
            if (!visual)
            {
                if (FAILED(owner.composition->CreateVisual(&visual))) return;
                if (FAILED(owner.composition->CreateVisual(&contentVisual))) { visual.Reset(); return; }
                visual->AddVisual(contentVisual.Get(), TRUE, nullptr);
                target->SetRoot(visual.Get());
            }
            if (!surface || width != w || height != h)
            {
                surface.Reset();
                if (FAILED(owner.composition->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                    DXGI_ALPHA_MODE_PREMULTIPLIED, &surface))) return;
                width = w; height = h;
                backgroundDirty = true;
            }
            const auto& a = owner.appearance;
            const bool hc = HighContrast();
            if (backgroundDirty)
            {
                backgroundSurface.Reset();
                if (FAILED(owner.composition->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                        DXGI_ALPHA_MODE_PREMULTIPLIED, &backgroundSurface))) return;
                POINT origin{};
                ComPtr<ID2D1DeviceContext> background;
                const auto beginBackground = backgroundSurface->BeginDraw(nullptr, IID_PPV_ARGS(&background), &origin);
                if (FAILED(beginBackground)) { PaintError(beginBackground); return; }
                background->SetDpi(96, 96);
                background->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
                background->Clear(hc ? SystemColor(COLOR_WINDOW) : D2D1::ColorF(0, 0.f));
                if (!hc && owner.drawBackground) owner.drawBackground(background.Get(), client, a, dpi / 96.f * owner.settings.scale);
                background.Reset();
                const auto drawn = backgroundSurface->EndDraw();
                if (FAILED(drawn)) { PaintError(drawn); return; }
                visual->SetContent(backgroundSurface.Get());
                backgroundDirty = false;
            }
            POINT offset{};
            ComPtr<ID2D1DeviceContext> context;
            const auto begin = surface->BeginDraw(nullptr, IID_PPV_ARGS(&context), &offset);
            if (FAILED(begin)) { PaintError(begin); return; }
            painting = true;
            context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->Clear(D2D1::ColorF(0, 0.f));
            const StatusBarPalette palette{hc, SystemColor(COLOR_WINDOW), SystemColor(COLOR_WINDOWTEXT),
                SystemColor(COLOR_HIGHLIGHT), SystemColor(COLOR_HIGHLIGHTTEXT)};
            const auto contentResult = DrawStatusBarContent(context.Get(), owner.text.Get(), items, w, h,
                dpi / 96.f * owner.settings.scale, a, palette, interaction.hovered,
                keyboardFocusVisible && interaction.focused.has_value() && GetFocus() == hwnd, interaction.focused.value_or(0));
            for (const auto& item : items) if (item.icon && owner.tray)
            {
                RECT screen = item.bounds;
                if (!IsRectEmpty(&screen)) MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&screen), 2);
                owner.tray->SetGeometry(item.icon->key, screen);
            }
            context.Reset();
            const auto surfaceResult = surface->EndDraw();
            const auto result = FAILED(contentResult) ? contentResult : surfaceResult;
            painting = false;
            if (SUCCEEDED(result))
            {
                contentVisual->SetContent(surface.Get());
                const auto commit = owner.composition->Commit();
                if (FAILED(commit)) PaintError(commit);
                else { paintDirty = false; lastPaintError = S_OK; }
            }
            else PaintError(result);
        }
        void PaintError(HRESULT result)
        {
            if (lastPaintError != result)
            {
                wchar_t message[160]{};
                swprintf_s(message, L"StatusBar drawing failed hwnd=%p HRESULT=0x%08X", hwnd, static_cast<unsigned>(result));
                WriteDiagnosticLogEntry(message);
                lastPaintError = result;
            }
            paintDirty = true;
            surface.Reset();
        }
        void DismissSurfaces()
        {
            if (owner.tray) owner.tray->CancelFocusReturn();
            keyboardFocusVisible = false;
            ClearHover(); interaction.CancelPointer();
            paintDirty = true; Paint();
            const auto onActivated = owner.activate;
            if (!fullscreen && onActivated)
                onActivated(StatusBarAction::Dismiss, hwnd, appbar.Bounds());
        }
        void ActivateItem(std::optional<std::size_t> index, bool context = false)
        {
            const auto invocation = ResolveStatusBarInvocation(items, index, context);
            if (fullscreen || !invocation || !owner.activate) return;
            RECT anchor = invocation->bounds;
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
            if (invocation->isTray)
            {
                if (owner.tray)
                {
                    owner.tray->SetGeometry(invocation->trayKey, anchor);
                    owner.tray->Activate(invocation->trayKey, invocation->trayAction, {anchor.left, anchor.top}, {hwnd, hwnd});
                }
                return;
            }
            const auto action = invocation->action;
            if (owner.tray) owner.tray->CancelFocusReturn();
            ClearHover();
            paintDirty = true; Paint();
            auto onActivated = owner.activate;
            onActivated(action, hwnd, anchor);
        }
        bool TrayMouse(UINT message, POINT point)
        {
            if (!owner.tray) return false;
            for (const auto& item : items)
            {
                if (!item.icon || !PtInRect(&item.bounds, point)) continue;
                RECT geometry = item.bounds;
                MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&geometry), 2);
                owner.tray->SetGeometry(item.icon->key, geometry);
                ClientToScreen(hwnd, &point);
                const auto action = message == WM_LBUTTONDOWN ? tray::Activation::LeftDown :
                    message == WM_LBUTTONUP ? tray::Activation::LeftUp :
                    message == WM_LBUTTONDBLCLK ? tray::Activation::DoubleClick :
                    message == WM_RBUTTONDOWN ? tray::Activation::RightDown : tray::Activation::RightUp;
                owner.tray->Activate(item.icon->key, action, point, {hwnd, hwnd});
                return true;
            }
            return false;
        }
        void ReturnTrayFocus(std::uint64_t serial)
        {
            if (!owner.tray) return;
            if (fullscreen || closing || failed || !IsWindowVisible(hwnd))
            { owner.tray->CancelFocusReturn(hwnd); return; }
            const auto delivery = owner.tray->TakeFocusReturn(hwnd, serial);
            if (!delivery) return;
            Paint(); // Reconcile pending icon removal/reordering before choosing a target.
            const auto index = FindStatusBarTrayFocus(items, delivery->key);
            if (!index || !tray::RestoreFocus(*delivery)) return;
            interaction.focused = index;
            keyboardFocusVisible = delivery->ticket.keyboard != 0;
            ClearHover(); paintDirty = true; Paint();
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
            if (!self) return DefWindowProcW(window, message, wp, lp);
            if (self->closing) return DefWindowProcW(window, message, wp, lp);
            if (message == tray::FocusReturnMessage())
            { self->ReturnTrayFocus(static_cast<std::uint64_t>(wp)); return 0; }
            if (message == self->owner.taskbarCreated)
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &pid);
                if (pid && pid != self->explorerPid)
                {
                    self->explorerPid = pid;
                    self->appbar.ExplorerRestarted();
                    self->QueuePlace();
                }
                return 0;
            }
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_CONTEXTMENU)
            {
                if (DispatchStatusBarKeyboard(message, wp, lp, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                    self->interaction, self->items,
                    [&] {
                        if (self->owner.tray) self->owner.tray->CancelFocusReturn();
                        self->keyboardFocusVisible = true; self->ClearHover(); self->paintDirty = true; self->Paint();
                    },
                    [&](bool context) { self->ActivateItem(self->interaction.focused, context); },
                    [&] { self->DismissSurfaces(); })) return 0;
            }
            switch (message)
            {
            case kPlace: self->Place(); return 0;
            case kForeground:
                if (self->owner.tray) self->owner.tray->ObserveForeground(reinterpret_cast<HWND>(wp), static_cast<DWORD>(lp));
                return 0;
            case kFullscreen:
                if (auto found = liveBars.find(window); found != liveBars.end()) found->second = false;
                self->CheckFullscreen(); return 0;
            case kAppBar:
                if (wp == ABN_POSCHANGED) self->QueuePlace();
                else if (wp == ABN_FULLSCREENAPP) PostMessageW(window, kFullscreen, 0, 0);
                return 0;
            case WM_TIMER:
                if (wp == kClockTimer)
                {
                    for (const auto& completion : self->owner.data->Controls()->DrainCompletions("statusBarVolume"))
                        if (completion.id == self->volumeTask) { self->volumeTask = 0; self->volumeWheel.Reset(); }
                    self->CheckFullscreen(); self->Paint();
                }
                return 0;
            case WM_DPICHANGED:
            case WM_DISPLAYCHANGE: self->QueuePlace(); return 0;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED: self->appearanceDirty = true; self->QueuePlace(); break;
            case WM_WINDOWPOSCHANGED:
                if (!self->placing && lp &&
                    (reinterpret_cast<WINDOWPOS*>(lp)->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
                    self->appbar.Notify(ABM_WINDOWPOSCHANGED);
                break;
            case WM_ACTIVATE: self->appbar.Notify(ABM_ACTIVATE); self->paintDirty = true; self->Paint(); break;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint); self->Paint(); EndPaint(window, &paint); return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            {
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const bool doubleClick = message == WM_LBUTTONDBLCLK && self->interaction.IsDoubleClickTarget(self->items, point);
                if (self->owner.tray) self->owner.tray->CancelFocusReturn();
                if (self->interaction.Press(self->items, point, message == WM_RBUTTONDOWN) == StatusBarAction::Dismiss)
                {
                    self->DismissSurfaces();
                    return 0;
                }
                const UINT trayMessage = message == WM_LBUTTONDBLCLK && !doubleClick ? WM_LBUTTONDOWN : message;
                if (self->TrayMouse(trayMessage, point)) return 0;
                break;
            }
            case WM_RBUTTONUP:
            {
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (!self->interaction.Release(self->items, point, true).accepted) return 0;
                if (self->TrayMouse(message, point)) return 0;
                break;
            }
            case WM_LBUTTONUP:
            {
                if (self->keyboardFocusVisible)
                { self->keyboardFocusVisible = false; self->paintDirty = true; self->Paint(); }
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const auto released = self->interaction.Release(self->items, point, false);
                if (!released.accepted) return 0;
                if (self->TrayMouse(message, point)) return 0;
                if (released.item) self->ActivateItem(*released.item);
                return 0;
            }
            case WM_CONTEXTMENU:
            {
                POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                self->keyboardFocusVisible = false;
                ScreenToClient(window, &point);
                for (const auto& item : self->items)
                    if (item.icon && PtInRect(&item.bounds, point)) return 0;
                ClientToScreen(window, &point);
                if (self->owner.tray) self->owner.tray->CancelFocusReturn();
                auto onActivated = self->owner.activate;
                if (onActivated) onActivated(StatusBarAction::Menu, window, {point.x, point.y, point.x, point.y});
                return 0;
            }
            case WM_MOUSEMOVE:
            {
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                std::string hoveredKey;
                std::optional<std::size_t> hover;
                std::string tooltipKey;
                std::wstring tooltipText;
                for (const auto& item : self->items) if (PtInRect(&item.bounds, point))
                {
                    hover = static_cast<std::size_t>(&item - self->items.data());
                    tooltipText = item.icon ? (item.icon->tip.empty() ? item.icon->application : item.icon->tip) : item.tip;
                    tooltipKey = item.icon ? item.icon->key : item.key;
                    if (item.icon) hoveredKey = item.icon->key;
                    break;
                }
                if (hover != self->interaction.hovered) { self->interaction.hovered = hover; self->paintDirty = true; }
                if (hoveredKey != self->hoveredTray && self->owner.tray)
                {
                    POINT screen = point; ClientToScreen(window, &screen);
                    if (!self->hoveredTray.empty()) self->owner.tray->Activate(self->hoveredTray, tray::Activation::Leave, screen);
                    if (!hoveredKey.empty()) self->owner.tray->Activate(hoveredKey, tray::Activation::Hover, screen);
                    self->hoveredTray = std::move(hoveredKey);
                }
                if (self->tooltipState.Enter(std::move(tooltipKey), std::move(tooltipText)))
                {
                    TOOLINFOW tool{sizeof(tool)}; tool.hwnd = window; tool.uId = reinterpret_cast<UINT_PTR>(window);
                    tool.lpszText = self->tooltipState.text.data();
                    SendMessageW(self->tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
                }
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
                // A snapshot can change inside Paint. Reconcile then clears the
                // old tooltip after this event, never reinstating stale text.
                if (self->paintDirty) self->Paint();
                break;
            }
            case WM_MOUSEWHEEL:
            {
                POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(window, &point);
                for (const auto& item : self->items)
                    if (item.key == "controlCenter" && PtInRect(&item.bounds, point))
                    {
                        const float x = (point.x - item.bounds.left) / (self->dpi / 96.f * self->owner.settings.scale);
                        if (x < 32 || x >= 60) return 0;
                        const auto sample = self->owner.data->AudioOutputVolume();
                        if (!sample || !sample->available) return 0;
                        if (const auto target = self->volumeWheel.Move(GET_WHEEL_DELTA_WPARAM(wp), sample->volume))
                        {
                            system_control::Request request;
                            request.name = "audio.output.setVolume";
                            request.arguments["volume"] = std::to_string(*target);
                            self->volumeTask = self->owner.data->Controls()->Start("statusBarVolume", std::move(request));
                            if (!self->volumeTask) self->volumeWheel.Reset();
                        }
                        return 0;
                    }
                break;
            }
            case WM_MOUSELEAVE:
            case WM_CANCELMODE:
            case WM_CAPTURECHANGED:
                self->ClearHover(); self->interaction.CancelPointer();
                self->paintDirty = true; self->Paint();
                break;
            }
            return DefWindowProcW(window, message, wp, lp);
        }
    };
    std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data;
    std::shared_ptr<tray::Service> tray;
    Activate activate;
    Hidden hidden;
    Error error;
    DrawBackground drawBackground;
    StatusBarSettings settings;
    PersonalizationSettings appearance;
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDWriteFactory> text;
    std::map<std::wstring, std::unique_ptr<Window>> windows;
    std::vector<HWINEVENTHOOK> hooks;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    Impl(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> source,
        Activate action, Hidden hide, Error report, DrawBackground draw)
        : data(std::move(source)), activate(std::move(action)), hidden(std::move(hide)), error(std::move(report)), drawBackground(std::move(draw)) {}
    void Close()
    {
        for (auto hook : hooks) UnhookWinEvent(hook);
        hooks.clear();
        windows.clear();
        tray.reset();
        if (data) { data->RemoveConsumer("statusBar"); data->Controls()->RemoveConsumer("statusBarVolume"); }
    }
    void Demand(const char* topic, bool enabled, int interval = 1000)
    {
        if (!data) return;
        if (enabled) data->StartTopic("statusBar", topic, std::chrono::milliseconds(interval));
        else data->StopTopic("statusBar", topic);
    }
};

StatusBar::StatusBar(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data,
    Activate activate, Hidden hidden, Error error, DrawBackground drawBackground)
    : impl_(std::make_unique<Impl>(std::move(data), std::move(activate), std::move(hidden), std::move(error), std::move(drawBackground))) {}
StatusBar::~StatusBar() { Close(); }
void StatusBar::Close() { impl_->Close(); }
std::shared_ptr<tray::Service> StatusBar::Tray() const { return impl_->tray; }
bool StatusBar::IsFullscreen(HMONITOR monitor) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor) return window->fullscreen;
    }
    return false;
}
void StatusBar::Configure(StatusBarSettings settings, const PersonalizationSettings& global,
    const std::vector<StatusBarMonitor>& monitors,
    IDCompositionDesktopDevice* composition, IDWriteFactory* text)
{
    NormalizeStatusBarSettings(settings);
    auto& self = *impl_;
    const auto appearance = ResolveStatusBarAppearance(settings.theme, global);
    const bool changed = settings != self.settings || appearance != self.appearance;
    self.settings = std::move(settings);
    self.appearance = appearance;
    if (self.composition.Get() != composition)
    {
        for (auto& [id, window] : self.windows)
        {
            (void)id;
            if (!window) continue;
            window->surface.Reset(); window->backgroundSurface.Reset();
            window->contentVisual.Reset(); window->visual.Reset(); window->target.Reset();
            window->backgroundDirty = true;
            window->paintDirty = true;
        }
    }
    self.composition = composition; self.text = text;
    if (!self.settings.enabled || monitors.empty()) { self.Close(); return; }
    if (!self.tray) self.tray = std::make_shared<tray::Service>();
    self.Demand("system.cpu", self.settings.cpu);
    self.Demand("system.memory", self.settings.memory);
    self.Demand("system.gpu", self.settings.gpu);
    self.Demand("system.network.traffic", self.settings.traffic);
    self.Demand("system.network.status", true, 3000);
    self.Demand("system.power", true, 3000);
    self.Demand("audio.output.volume", true);
    std::erase_if(self.windows, [&](const auto& entry) {
        return std::none_of(monitors.begin(), monitors.end(), [&](const auto& monitor) { return monitor.id == entry.first; });
    });
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = Impl::Window::Procedure;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"SnowDesktop.StatusBar";
    RegisterClassExW(&cls);
    for (const auto& monitor : monitors)
    {
        auto& window = self.windows[monitor.id];
        if (!window)
        {
            window = std::make_unique<Impl::Window>(self);
            window->monitor = monitor.monitor;
            window->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                cls.lpszClassName, _LW("settings.nav.statusBar"), WS_POPUP,
                0, 0, 1, 1, nullptr, nullptr, cls.hInstance, window.get());
            if (!window->hwnd)
            {
                if (self.error) self.error(_LW("statusBar.registrationFailed"));
                window.reset();
                continue;
            }
            liveBars.emplace(window->hwnd, false);
            window->tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                window->hwnd, nullptr, cls.hInstance, nullptr);
            TOOLINFOW tool{sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tool.hwnd = window->hwnd;
            tool.uId = reinterpret_cast<UINT_PTR>(window->hwnd); tool.lpszText = window->tooltipState.text.data();
            SendMessageW(window->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &window->explorerPid);
            SetTimer(window->hwnd, kClockTimer, 1000, nullptr);
        }
        window->monitor = monitor.monitor;
        window->appearanceDirty = window->appearanceDirty || changed;
        window->paintDirty = window->paintDirty || changed;
        window->QueuePlace();
    }
    if (self.hooks.empty())
        for (const auto range : {std::pair{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                std::pair{EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
                std::pair{EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE},
                std::pair{EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
                std::pair{EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}})
            if (auto hook = SetWinEventHook(range.first, range.second, nullptr, WindowEvent, 0, 0,
                    WINEVENT_OUTOFCONTEXT | (range.first == EVENT_SYSTEM_FOREGROUND ? 0 : WINEVENT_SKIPOWNPROCESS)))
                self.hooks.push_back(hook);
}
}
