#include "status_bar.h"
#include "status_bar_activation.h"
#include "status_bar_appearance.h"
#include "status_bar_view.h"
#include "status_bar_interaction.h"
#include "status_bar_appbar.h"
#include "widget_system_data_provider.h"
#include "app/desktop_backdrop_compositor.h"
#include "panel_gradient_renderer.h"
#include "l10n.h"
#include "tray_service.h"
#include "tray_focus.h"
#include "tray_order.h"
#include "diagnostic_log.h"
#include "status_bar_layout.h"
#include "status_bar_glyphs.h"
#include "status_bar_presentation.h"
#include "status_bar_notification.h"
#include "native_tooltip.h"
#include "system_controls.h"
#include "utils.h"

#include <dcomp.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
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
constexpr UINT kAppBar = WM_APP + 41, kPlace = WM_APP + 42, kFullscreen = WM_APP + 43, kForeground = WM_APP + 44,
    kActivate = WM_APP + 45;
constexpr UINT_PTR kClockTimer = 1, kTrayMenuTimer = 2, kPlacementTimer = 3, kInputMethodTimer = 4;
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
bool WindowClientScreenRect(HWND window, RECT& client)
{
    if (!GetClientRect(window, &client)) return false;
    POINT origin{client.left, client.top}, end{client.right, client.bottom};
    if (!ClientToScreen(window, &origin) || !ClientToScreen(window, &end)) return false;
    client = {origin.x, origin.y, end.x, end.y};
    return true;
}
std::optional<HWND> MonitorFullscreenSource(HMONITOR monitor)
{
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return std::nullopt;
    struct Context { RECT monitor; bool stopped = false; HWND window = nullptr; } context{info.rcMonitor};
    const BOOL enumerated = EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
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
        const auto style = GetWindowLongPtrW(window, GWL_STYLE), extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        const bool fullscreenCandidate = StatusBarFullscreenCandidate(style, extended, IsZoomed(window) != FALSE, shell);
        RECT client{};
        if (!WindowClientScreenRect(window, client)) return TRUE;
        if (fullscreenCandidate && StatusBarFullscreenClient(client, state.monitor, true, false, cloaked != 0, shell))
        {
            state.stopped = true;
            state.window = window;
            return FALSE;
        }
        // EnumWindows is in Z order. A regular window covering the monitor's
        // center hides a background full-screen window after Alt+Tab. Small
        // tooltips/owned tool windows do not cancel the full-screen state.
        const POINT center{state.monitor.left + (state.monitor.right - state.monitor.left) / 2,
            state.monitor.top + (state.monitor.bottom - state.monitor.top) / 2};
        if (!shell && !cloaked && PtInRect(&client, center) &&
            !(extended & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)))
        { state.stopped = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    if (!enumerated && !context.stopped) return std::nullopt;
    return context.window;
}
bool DockFullscreenSourceEligible(HWND source, DWORD expectedProcess, HMONITOR monitor)
{
    DWORD process = 0;
    if (!source || !IsWindow(source) || !GetWindowThreadProcessId(source, &process) ||
        !expectedProcess || process != expectedProcess ||
        MonitorFromWindow(source, MONITOR_DEFAULTTONULL) != monitor) return false;
    // Exclusive fullscreen often minimizes when our input proxy becomes active.
    if (IsIconic(source)) return true;
    DWORD cloaked = 0;
    (void)DwmGetWindowAttribute(source, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    MONITORINFO info{sizeof(info)};
    RECT client{};
    return GetMonitorInfoW(monitor, &info) && WindowClientScreenRect(source, client) &&
        StatusBarFullscreenCandidate(GetWindowLongPtrW(source, GWL_STYLE),
            GetWindowLongPtrW(source, GWL_EXSTYLE), IsZoomed(source) != FALSE, false) &&
        StatusBarFullscreenClient(client, info.rcMonitor, IsWindowVisible(source) != FALSE,
            false, cloaked != 0, false);
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
        ComPtr<IDCompositionEffectGroup> mergedOpacity;
        StatusBarDockPresentation mergedPresentation;
        ComPtr<IDCompositionSurface> backgroundSurface;
        ComPtr<IDCompositionSurface> surface;
        UINT width = 0, height = 0, dpi = 96;
        int mergedDockHeight = 0;
        bool reserveSpace = true, positioned = false;
        RECT placedBounds{};
        DWORD explorerPid = 0;
        bool placing = false, queued = false, fullscreen = false, failed = false, closing = false;
        bool fullscreenObserved = false; // fullscreen is the effective hide/input gate.
        bool checkingFullscreen = false;
        bool lastDockInteraction = false, lastDockPromoted = false;
        StatusBarFullscreenState fullscreenState;
        DWORD dockFullscreenProcess = 0;
        bool appearanceDirty = true, paintDirty = true, painting = false;
        bool backgroundDirty = true;
        PersonalizationSettings appearance;
        HRESULT lastPaintError = S_OK;
        status_bar_input_method::Snapshot lastInputMethod;
        std::string hoveredTray;
        NativeTooltip tooltip;
        std::optional<std::size_t> tooltipControlPart;
        std::vector<StatusBarItem> items;
        StatusBarInteraction interaction;
        bool keyboardFocusVisible = false;
        std::string pressedTray;
        std::string trayMenuKey;
        tray::MenuRetentionSession trayMenu;
        POINT trayPress{};
        bool trayDragging = false;
        bool trayExpanded = false;
        bool trayPreview = false;
        std::optional<RECT> dropIndicator;
        StatusBarActivationQueue activation;
        void EndDragFeedback()
        {
            if (std::exchange(trayPreview, false) && owner.dragFeedback.end) owner.dragFeedback.end();
        }
        explicit Window(Impl& value) : owner(value) {}
        ~Window()
        {
            closing = true;
            CancelTrayMenu();
            // Owner ends cross-window feedback before erasing this map entry.
            trayPreview = false;
            if (owner.tray) owner.tray->CancelFocusReturn(hwnd);
            ClearHover(); interaction.CancelPointer();
            liveBars.erase(hwnd);
            if (owner.hidden) owner.hidden(monitor);
            if (hwnd) { KillTimer(hwnd, kClockTimer); KillTimer(hwnd, kPlacementTimer); }
            tooltip.Close();
            appbar.Remove();
            backdrop.Reset();
            if (hwnd) DestroyWindow(hwnd);
        }
        void QueuePlace(bool settingsChanged = false)
        {
            if (closing || placing || (queued && !settingsChanged)) return;
            queued = true;
            // First presentation is immediate. Continuous settings changes
            // settle before asking Shell to resize the system work area.
            if (!positioned) PostMessageW(hwnd, kPlace, 0, 0);
            else SetTimer(hwnd, kPlacementTimer, 120, nullptr);
        }
        bool UpdateAppearance()
        {
            const auto scene = owner.sceneProvider ? owner.sceneProvider(monitor) : StatusBarSceneState{};
            const auto next = mergedDockHeight && owner.mergedAppearanceProvider
                ? owner.mergedAppearanceProvider(monitor)
                : ResolveStatusBarAppearance(owner.settings, owner.globalAppearance, scene);
            if (next == appearance) return false;
            appearance = next;
            appearanceDirty = backgroundDirty = paintDirty = true;
            ClearHover();
            // The shared strip follows Dock's appearance; refresh both HWNDs.
            if (mergedDockHeight && owner.dockChanged) owner.dockChanged(false);
            return true;
        }
        void ClearHover()
        {
            tooltip.Hide();
            tooltipControlPart.reset();
            if (!hoveredTray.empty() && owner.tray)
            {
                POINT screen{}; GetCursorPos(&screen);
                owner.tray->Activate(hoveredTray, tray::Activation::Leave, screen);
            }
            hoveredTray.clear(); interaction.hovered.reset();
        }
        void SyncTooltip(bool immediate = false)
        {
            if (!InputAvailable() || GetCapture() || trayMenu.tracker.Armed() ||
                !interaction.hovered || *interaction.hovered >= items.size())
            { tooltip.Hide(); return; }
            const auto& item = items[*interaction.hovered];
            auto body = item.icon ? (item.icon->tip.empty() ? item.icon->application : item.icon->tip) : item.tip;
            auto key = item.icon ? "tray/" + item.icon->key : "bar/" + item.key;
            RECT anchor = item.bounds;
            // Compact hover targets must not pull the tooltip into the taller
            // merged strip. Place owned surfaces beyond its outer edge.
            if (mergedDockHeight) { anchor.top = 0; anchor.bottom = static_cast<LONG>(height); }
            if (item.key == "controlCenter" && tooltipControlPart)
            {
                const auto part = *tooltipControlPart;
                key += "/" + std::to_string(part); body = item.controlTips[part];
                const float scale = dpi / 96.f * (mergedDockHeight ? 1.f : owner.settings.scale);
                if (part == 0) anchor.right = std::min(anchor.right, anchor.left + static_cast<LONG>(std::lround(32 * scale)));
                else if (part == 1)
                {
                    anchor.right = std::min(anchor.right, anchor.left + static_cast<LONG>(std::lround(60 * scale)));
                    anchor.left += static_cast<LONG>(std::lround(32 * scale));
                }
                else anchor.left += static_cast<LONG>(std::lround(60 * scale));
                if (part == 1 && owner.volumeWheel.pending)
                    body = std::wstring(_LW("statusBar.volume")) + L"  " +
                        std::to_wstring(static_cast<int>(std::lround(*owner.volumeWheel.pending * 100))) +
                        L"%";
            }
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
            tooltip.SetTarget(std::move(key), std::move(body), anchor,
                owner.settings.position == DockPosition::Bottom ? NativeTooltipPlacement::Above : NativeTooltipPlacement::Below,
                immediate);
        }
        void Hide(bool notify = true)
        {
            activation.Cancel();
            CancelTrayMenu();
            EndDragFeedback(); dropIndicator.reset();
            pressedTray.clear(); trayDragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (owner.tray) owner.tray->CancelFocusReturn(hwnd);
            ClearHover(); interaction.CancelPointer(); keyboardFocusVisible = false;
            paintDirty = true;
            for (const auto& item : items) if (item.icon && owner.tray)
                owner.tray->SetGeometry(item.icon->key, {});
            backdrop.HidePopupWindowPair(hwnd);
            backdrop.SetPopupTopmost(false);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
            if (notify && owner.hidden) owner.hidden(monitor);
        }
        void CheckFullscreen()
        {
            if (closing || checkingFullscreen) return;
            checkingFullscreen = true;
            struct CheckScope { bool& active; ~CheckScope() { active = false; } } checkScope{checkingFullscreen};
            const auto observed = MonitorFullscreenSource(monitor);
            const auto dock = owner.dockStateProvider ? owner.dockStateProvider(monitor) : StatusBarDockState{};
            const bool previousObservation = fullscreenObserved;
            DWORD foregroundProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
            fullscreenObserved = fullscreenState.Observe(observed, dock.promoted || dock.interacting,
                foregroundProcess == GetCurrentProcessId(),
                DockFullscreenSourceEligible(fullscreenState.DockSource(), dockFullscreenProcess, monitor));
            if (mergedDockHeight)
            {
                // The Dock alone owns presentation. Observations may ask it
                // to recompute policy, but may never restore this HWND.
                const bool changed = previousObservation != fullscreenObserved ||
                    lastDockInteraction != dock.interacting || lastDockPromoted != dock.promoted;
                lastDockInteraction = dock.interacting; lastDockPromoted = dock.promoted;
                if (changed && owner.dockChanged) owner.dockChanged(false);
                return;
            }
            const bool next = fullscreenState.ShouldHide(dock.interacting);
            const HWND source = fullscreenState.Source();
            const bool changed = next != fullscreen;
            fullscreen = next;
            if (changed)
            {
                wchar_t name[128]{}, message[256]{};if (source) GetClassNameW(source,name,static_cast<int>(std::size(name)));
                swprintf_s(message,L"StatusBar fullscreen monitor=%p bar=%p hidden=%d source=%p class=%s",monitor,hwnd,fullscreen,source,name);
                WriteDiagnosticLogEntry(message);
            }
            const bool shown = IsWindowVisible(hwnd) != FALSE;
            const bool topmost = (GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST) != 0;
            // Shell/Dock restacks can reveal a popup without a new fullscreen
            // transition. Reconcile physical visibility as well as state.
            if (fullscreen) { if (changed || shown || topmost) Hide(); }
            else if (appbar.Registered() && !failed && (changed || !shown || !topmost)) Show();
        }
        bool OwnsPointer(POINT clientPoint) const
        {
            RECT client{}, dockBounds{};
            GetClientRect(hwnd, &client);
            if (mergedDockHeight && owner.dockStateProvider)
            {
                dockBounds = owner.dockStateProvider(monitor).inputBounds;
                if (!IsRectEmpty(&dockBounds))
                    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&dockBounds), 2);
            }
            return StatusBarOwnsPointer(InputAvailable(),
                client, dockBounds, clientPoint);
        }
        bool InputAvailable() const
        {
            return !closing && !fullscreen && !failed && IsWindowVisible(hwnd) &&
                (!mergedDockHeight || mergedPresentation.inputEnabled);
        }
        void ApplyDockPose(const StatusBarDockPresentation& frame)
        {
            if (!mergedDockHeight || !visual || !owner.composition) return;
            if (!mergedOpacity) owner.composition->CreateEffectGroup(&mergedOpacity);
            if (mergedOpacity)
            {
                mergedOpacity->SetOpacity(frame.opacity);
                visual->SetEffect(mergedOpacity.Get());
            }
            visual->SetOffsetX(frame.offsetX);
            visual->SetOffsetY(frame.offsetY);
            backdrop.SetVisualOpacity(frame.opacity);
            backdrop.SetVisualTranslation(frame.offsetX, frame.offsetY);
        }
        bool PrepareDockPresentation(const StatusBarDockPresentation& frame)
        {
            if (!mergedDockHeight || closing || failed || !positioned) return false;
            if (!frame.visible) return true;
            const bool windowHidden = !IsWindowVisible(hwnd);
            if (windowHidden) paintDirty = true;
            if (paintDirty || !surface || !backgroundSurface) Paint(true);
            if (paintDirty || backgroundDirty || !surface || !backgroundSurface || !visual || !owner.composition)
                return false;
            if (windowHidden)
            {
                ApplyDockPose(frame);
                const auto result = owner.composition->Commit();
                if (FAILED(result)) { PaintError(result); return false; }
            }
            return true;
        }
        void ApplyDockPresentation(const StatusBarDockPresentation& frame)
        {
            if (!mergedDockHeight || closing) return;
            if (mergedPresentation.inputEnabled && !frame.inputEnabled)
            {
                CancelTrayMenu();
                EndDragFeedback(); ClearHover(); interaction.CancelPointer(); keyboardFocusVisible = false;
                pressedTray.clear(); trayDragging = false; if (GetCapture() == hwnd) ReleaseCapture();
                if (owner.tray) owner.tray->CancelFocusReturn(hwnd);
                paintDirty = true;
            }
            mergedPresentation = frame;
            fullscreen = !frame.visible;
            if (!frame.visible || failed || !positioned)
            {
                if (IsWindowVisible(hwnd)) Hide(false);
                return;
            }
            if (!IsWindowVisible(hwnd)) Show();
            else if (paintDirty) Paint();
            ApplyDockPose(mergedPresentation);
            backdrop.SetPopupWindowPairZOrder(hwnd, frame.insertAfter, frame.topmost);
            backdrop.SetVisible(appearance.glassEnabled && !HighContrast());
            backdrop.CommitVisualChanges();
        }
        void Show()
        {
            if (fullscreen || failed || !positioned) return;
            // Fill retained content and the full strip before any native show.
            // This also covers startup or a device reset with animations off.
            Paint(true);
            if (paintDirty || !surface || !backgroundSurface) return;
            if (!IsWindowVisible(hwnd) || (!mergedDockHeight && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)))
            {
                // Restoration must stay beneath the actual merged Dock and
                // its owned menu, even when that pair is already topmost.
                backdrop.SetPopupWindowPairZOrder(hwnd, mergedDockHeight ? mergedPresentation.insertAfter : HWND_TOPMOST,
                    mergedDockHeight ? mergedPresentation.topmost : true);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (appearance.glassEnabled && !HighContrast()) backdrop.ShowPopupWindowPair(hwnd);
            }
            SyncTooltip();
        }
        void Place()
        {
            KillTimer(hwnd, kPlacementTimer);
            queued = false;
            if (closing || placing) return;
            placing = true;
            UpdateAppearance();
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info)) { placing = false; Hide(); return; }
            const UINT previousDpi=dpi;
            UINT dpiY = 96;
            if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY))) dpi = 96;
            if(dpi!=previousDpi){surface.Reset();backgroundSurface.Reset();appearanceDirty=backgroundDirty=paintDirty=true;}
            const UINT edge = Edge(owner.settings.position);
            const bool vertical = edge == ABE_LEFT || edge == ABE_RIGHT;
            const int thickness = mergedDockHeight > 0 ? mergedDockHeight : static_cast<int>(std::lround(
                (vertical ? 48.f : 32.f) * owner.settings.scale * dpi / 96.f));
            bool placed = false;
            if (reserveSpace)
            {
                placed = appbar.Place(hwnd, kAppBar, edge, thickness, info.rcMonitor);
                if (placed) placedBounds = appbar.Bounds();
            }
            else
            {
                appbar.Remove(); placedBounds = info.rcMonitor;
                StatusBarAppBar::SetThickness(placedBounds, edge, thickness); placed = true;
            }
            positioned = placed;
            if (!placed)
            {
                if (!failed && owner.error) owner.error(_LW("statusBar.registrationFailed"));
                failed = true;
                placing = false;
                Hide();
                return;
            }
            failed = false;
            const RECT rect = placedBounds;
            RECT previous{};
            GetWindowRect(hwnd, &previous);
            const bool moved = !EqualRect(&rect, &previous);
            if (moved)
            {
                EndDragFeedback();pressedTray.clear();trayDragging=false;if(GetCapture()==hwnd)ReleaseCapture();
                ClearHover(); interaction.CancelPointer();
                const BOOL placedWindow=SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
                wchar_t message[320]{};
                swprintf_s(message,L"StatusBar placement monitor=%p bar=%p dpi=%u bounds=%ld,%ld,%ld,%ld positioned=%d error=%lu",monitor,hwnd,dpi,
                    rect.left,rect.top,rect.right,rect.bottom,placedWindow,placedWindow?0:GetLastError());
                WriteDiagnosticLogEntry(message);
            }
            if ((moved || appearanceDirty) && appearance.glassEnabled && !HighContrast())
            {
                if (!backdrop.IsAvailable()) backdrop.InitializePopup(hwnd, !fullscreen, false);
                backdrop.Reattach(hwnd);
                backdrop.BeginFrame(true);
                backdrop.AddPanel({0, 0, rect.right - rect.left, rect.bottom - rect.top},
                    0, appearance.glassBlurRadius * dpi / 96.f,
                    reinterpret_cast<std::uintptr_t>(this));
                backdrop.EndFrame();
            }
            else if (appearanceDirty && (!appearance.glassEnabled || HighContrast())) backdrop.Reset();
            paintDirty = paintDirty || moved || appearanceDirty;
            backgroundDirty = backgroundDirty || moved || appearanceDirty;
            appearanceDirty = false;
            placing = false;
            CheckFullscreen();
            // InitializePopup starts hidden. Updating glass while the bar
            // itself is visible must reveal its new helper after checking the
            // current fullscreen state, not the state before this placement.
            if (mergedDockHeight) ApplyDockPresentation(mergedPresentation);
            else
            {
                backdrop.SetPopupTopmost(!fullscreen);
                backdrop.SetVisible(!fullscreen && appearance.glassEnabled && !HighContrast());
                if (!fullscreen) Show();
            }
            if (moved && owner.dockChanged) owner.dockChanged(true);
        }
        void BuildItems()
        {
            StatusBarSnapshot snapshot;
            wchar_t time[64]{}, date[96]{};
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, time, 64);
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, date, 96, nullptr);
            snapshot.clock = std::wstring(date) + L"   " + time;
            snapshot.cpu = owner.data->Cpu(); snapshot.memory = owner.data->Memory();
            snapshot.gpu = owner.data->Gpu(true); snapshot.traffic = owner.data->NetworkTraffic();
            snapshot.network = owner.data->NetworkStatus(); snapshot.audio = owner.data->AudioOutputVolume();
            snapshot.wifi = owner.data->Controls()->Current("network.wifi");
            snapshot.notifications = status_bar_notification::Current();
            if (owner.volumeWheel.task && (!snapshot.audio || !snapshot.audio->available ||
                owner.volumeWheel.endpoint != snapshot.audio->endpointId))
            {
                owner.data->Controls()->Cancel(owner.volumeWheel.task);
                owner.volumeWheel.Reset();
            }
            snapshot.power = owner.data->Power();
            if (owner.tray) snapshot.tray = owner.tray->Current().icons;
            snapshot.trayExpanded = trayExpanded;
            if (owner.inputMethod) snapshot.inputMethod = owner.inputMethod->Current();
            items = BuildStatusBarItems(owner.settings, snapshot);
        }

        void Paint(bool prepareHidden = false)
        {
            if (closing || painting || failed || (!prepareHidden && (fullscreen || !IsWindowVisible(hwnd))) ||
                !owner.composition || !owner.text) return;
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
                SyncTooltip();
                return;
            }
            RECT client{};
            GetClientRect(hwnd, &client);
            if (IsRectEmpty(&client)) return;
            const UINT w = static_cast<UINT>(client.right), h = static_cast<UINT>(client.bottom);
            if (!target) { const auto hr=owner.composition->CreateTargetForHwnd(hwnd,FALSE,&target);if(FAILED(hr)){PaintError(hr);return;} }
            if (!visual)
            {
                auto hr=owner.composition->CreateVisual(&visual);
                if (SUCCEEDED(hr)) hr=owner.composition->CreateVisual(&contentVisual);
                if (SUCCEEDED(hr)) hr=visual->AddVisual(contentVisual.Get(),TRUE,nullptr);
                if (SUCCEEDED(hr)) hr=target->SetRoot(visual.Get());
                if (FAILED(hr)) { visual.Reset();contentVisual.Reset();PaintError(hr);return; }
            }
            if (!surface || width != w || height != h)
            {
                surface.Reset();
                const auto hr=owner.composition->CreateSurface(w,h,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&surface);
                if(FAILED(hr)){PaintError(hr);return;}
                width = w; height = h;
                backgroundDirty = true;
            }
            const auto& a = appearance;
            const bool hc = HighContrast();
            if (backgroundDirty)
            {
                backgroundSurface.Reset();
                const auto hr=owner.composition->CreateSurface(w,h,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&backgroundSurface);
                if(FAILED(hr)){PaintError(hr);return;}
                POINT origin{};
                ComPtr<ID2D1DeviceContext> background;
                const auto beginBackground = backgroundSurface->BeginDraw(nullptr, IID_PPV_ARGS(&background), &origin);
                if (FAILED(beginBackground)) { PaintError(beginBackground); return; }
                background->SetDpi(96, 96);
                background->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
                background->Clear(hc ? SystemColor(COLOR_WINDOW) : D2D1::ColorF(0, 0.f));
                if (!hc && owner.drawBackground) owner.drawBackground(background.Get(), client, a, dpi / 96.f * (mergedDockHeight ? 1.f : owner.settings.scale));
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
                dpi / 96.f * (mergedDockHeight ? 1.f : owner.settings.scale), a, palette, interaction.hovered,
                keyboardFocusVisible && interaction.focused.has_value() && GetFocus() == hwnd,
                interaction.focused.value_or(0), mergedDockHeight > 0);
            if (dropIndicator)
            {
                ComPtr<ID2D1SolidColorBrush> brush;
                if (SUCCEEDED(context->CreateSolidColorBrush(hc ? palette.highlight :
                    D2D1::ColorF(a.contentTheme == 1 ? 0x202020 : 0xf4f4f4), &brush)))
                {
                    const auto& r = *dropIndicator;
                    const auto rect = D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                        static_cast<float>(r.right), static_cast<float>(r.bottom));
                    if (r.right-r.left <= MulDiv(3,static_cast<int>(dpi),96)) context->FillRectangle(rect,brush.Get());
                    else context->DrawRoundedRectangle(D2D1::RoundedRect(rect,4.f,4.f),brush.Get(),2.f);
                }
            }
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
                ApplyDockPose(mergedPresentation);
                const auto commit = owner.composition->Commit();
                if (FAILED(commit)) PaintError(commit);
                else { paintDirty = false; lastPaintError = S_OK; SyncTooltip(); }
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
            if (owner.graphicsFailure) owner.graphicsFailure(result);
        }
        bool CancelTrayMenu()
        {
            const bool armed = trayMenu.tracker.Armed();
            trayMenu.Reset(); trayMenuKey.clear();
            if (hwnd) KillTimer(hwnd, kTrayMenuTimer);
            return armed;
        }
        bool ArmTrayMenu(const std::string& key)
        {
            if (!owner.tray) return CancelTrayMenu();
            const auto snapshot = owner.tray->Current();
            for (const auto& icon : snapshot.icons)
            {
                if (icon.key != key || (icon.state & NIS_HIDDEN)) continue;
                const HWND callback = reinterpret_cast<HWND>(icon.identity.window);
                // Down/up are one gesture: keep any already-observed menu owner.
                if (trayMenu.tracker.Armed() && trayMenuKey == key && trayMenu.target == callback &&
                    trayMenu.process == icon.identity.process && trayMenu.BelongsToTarget(callback) &&
                    GetWindowThreadProcessId(callback, nullptr) == trayMenu.targetThread) return false;
                const bool changed = CancelTrayMenu();
                // NOACTIVATE leaves the previous foreground in place while the
                // asynchronous callback starts. It is only the bounded discovery
                // origin; the real focus-return ticket still targets this bar.
                const HWND foreground = GetForegroundWindow();
                trayMenu.Arm(callback, icon.identity.process, foreground ? foreground : hwnd, hwnd);
                if (trayMenu.tracker.Armed())
                {
                    trayMenuKey = key;
                    SetTimer(hwnd, kTrayMenuTimer, 50, nullptr);
                    return true;
                }
                return changed;
            }
            return CancelTrayMenu();
        }
        bool ActiveTrayMenu()
        {
            // Called by dockStateProvider: never notify/reenter its owner here.
            if (!trayMenu.tracker.Armed()) return false;
            if (!InputAvailable() || !owner.tray || !FindStatusBarTrayFocus(items, trayMenuKey))
            { CancelTrayMenu(); return false; }
            return trayMenu.Active(GetForegroundWindow(), owner.tray->MenuPopups(trayMenu.target));
        }
        void PollTrayMenu()
        {
            const bool armed = trayMenu.tracker.Armed();
            ActiveTrayMenu();
            if (!trayMenu.tracker.Armed() || trayMenu.tracker.ObservedMenu())
                KillTimer(hwnd, kTrayMenuTimer);
            // Discovery polling stops after observation. Existing foreground and
            // clock dispatch, plus the Dock's policy queries, track menu closure.
            if (armed && !trayMenu.tracker.Armed() && owner.dockChanged) owner.dockChanged(false);
        }
        void ActivateTray(const std::string& key, tray::Activation action, POINT point, bool leftClick = false)
        {
            if (!owner.tray) return;
            // The app callback may synchronously open a foreign menu loop.
            // Cancel the current tooltip before dispatch, then keep it hidden
            // throughout this gesture's discovery/retention session.
            tooltip.Hide();
            bool changed = ArmTrayMenu(key);
            bool accepted = false;
            if (leftClick) accepted = owner.tray->Activate(key, tray::Activation::LeftDown, point, {hwnd, hwnd});
            accepted = owner.tray->Activate(key, action, point, {hwnd, hwnd}) || accepted;
            if (!accepted) changed = CancelTrayMenu() || changed;
            if (changed && owner.dockChanged) owner.dockChanged(false);
        }
        void DismissSurfaces()
        {
            CancelTrayMenu();
            if (owner.tray) owner.tray->CancelFocusReturn();
            keyboardFocusVisible = false;
            ClearHover(); interaction.CancelPointer();
            paintDirty = true; Paint();
            const auto onActivated = owner.activate;
            if (InputAvailable() && onActivated)
                onActivated(StatusBarAction::Dismiss, hwnd, placedBounds);
        }
        void ActivateItem(std::optional<std::size_t> index, bool context = false)
        {
            const auto invocation = ResolveStatusBarInvocation(items, index, context);
            if (!InputAvailable() || !invocation || !owner.activate) return;
            RECT anchor = invocation->bounds;
            if (mergedDockHeight && !invocation->isTray)
            { anchor.top = 0; anchor.bottom = static_cast<LONG>(height); }
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
            if (invocation->isTray)
            {
                if (owner.tray)
                {
                    owner.tray->SetGeometry(invocation->trayKey, anchor);
                    ActivateTray(invocation->trayKey, invocation->trayAction, {anchor.left, anchor.top});
                }
                return;
            }
            const auto action = invocation->action;
            CancelTrayMenu();
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
                if (message == WM_LBUTTONDOWN)
                {
                    pressedTray = item.icon->key; trayPress = point; trayDragging = false;
                    SetCapture(hwnd);
                    if (CancelTrayMenu() && owner.dockChanged) owner.dockChanged(false);
                    return true;
                }
                const auto action = message == WM_LBUTTONDOWN ? tray::Activation::LeftDown :
                    message == WM_LBUTTONUP ? tray::Activation::LeftUp :
                    message == WM_LBUTTONDBLCLK ? tray::Activation::DoubleClick :
                    message == WM_RBUTTONDOWN ? tray::Activation::RightDown : tray::Activation::RightUp;
                ActivateTray(item.icon->key, action, point);
                return true;
            }
            return false;
        }
        void ReturnTrayFocus(std::uint64_t serial)
        {
            if (!owner.tray) return;
            if (!InputAvailable())
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
            if (!self->InputAvailable() &&
                ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_CONTEXTMENU ||
                    message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
                    message == WM_SYSKEYDOWN || message == WM_SYSKEYUP || message == WM_SYSCHAR)) return 0;
            if (message == tray::FocusReturnMessage())
            { self->ReturnTrayFocus(static_cast<std::uint64_t>(wp)); return 0; }
            if (message == self->owner.taskbarCreated)
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &pid);
                if (pid && pid != self->explorerPid)
                {
                    const bool canceledMenu = self->CancelTrayMenu();
                    self->explorerPid = pid;
                    self->appbar.ExplorerRestarted();
                    self->QueuePlace();
                    if (canceledMenu && self->owner.dockChanged) self->owner.dockChanged(false);
                }
                return 0;
            }
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_CONTEXTMENU)
            {
                if (DispatchStatusBarKeyboard(message, wp, lp, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                    self->interaction, self->items,
                    [&] {
                        if (self->owner.tray) self->owner.tray->CancelFocusReturn();
                        const auto focused = self->interaction.focused;
                        const bool sameTray = focused && *focused < self->items.size() && self->items[*focused].icon &&
                            self->items[*focused].icon->key == self->trayMenuKey;
                        const bool canceledMenu = !sameTray && self->CancelTrayMenu();
                        self->keyboardFocusVisible = true; self->ClearHover(); self->paintDirty = true; self->Paint();
                        if (canceledMenu && self->owner.dockChanged) self->owner.dockChanged(false);
                    },
                    [&](bool context) { self->ActivateItem(self->interaction.focused, context); },
                    [&] {
                        self->fullscreenState.DismissReveal();
                        PostMessageW(window, kFullscreen, 0, 0);
                        self->DismissSurfaces();
                    })) return 0;
            }
            switch (message)
            {
            case kActivate:
            {
                auto callback = self->activation.Take();
                if (callback && self->InputAvailable()) callback();
                // Activation may destroy the bar or enter a modal window loop.
                return 0;
            }
            case kPlace: self->Place(); return 0;
            case kForeground:
                if (self->owner.tray) self->owner.tray->ObserveForeground(reinterpret_cast<HWND>(wp), static_cast<DWORD>(lp));
                if (self->UpdateAppearance()) self->QueuePlace();
                self->PollTrayMenu();
                return 0;
            case kFullscreen:
                if (auto found = liveBars.find(window); found != liveBars.end()) found->second = false;
                self->CheckFullscreen();
                if (self->UpdateAppearance()) self->QueuePlace();
                return 0;
            case kAppBar:
                if (wp == ABN_POSCHANGED) self->QueuePlace();
                else if (wp == ABN_FULLSCREENAPP) PostMessageW(window, kFullscreen, 0, 0);
                return 0;
            case WM_TIMER:
                if (wp == kClockTimer)
                {
                    // One service consumer and one pending audio target across
                    // all bars: the first monitor must not drain another's result.
                    for (const auto& completion : self->owner.data->Controls()->DrainCompletions("statusBarVolume"))
                        self->owner.volumeWheel.Complete(completion.id);
                    self->CheckFullscreen();
                    if (self->UpdateAppearance()) self->QueuePlace();
                    else self->Paint();
                    self->PollTrayMenu();
                }
                else if (wp == kInputMethodTimer && self->owner.inputMethod)
                {
                    const auto input = self->owner.inputMethod->Current();
                    if (input != self->lastInputMethod)
                    {
                        self->lastInputMethod = input;
                        self->Paint();
                    }
                }
                else if (wp == kTrayMenuTimer) self->PollTrayMenu();
                else if (wp == kPlacementTimer) self->Place();
                return 0;
            case WM_DPICHANGED:
            case WM_DISPLAYCHANGE:
                // A display handoff can discard compositor contents even when
                // Shell approves the same rectangle. Do not skip that repaint
                // just because the sampled labels and bounds are unchanged.
                self->surface.Reset();self->backgroundSurface.Reset();
                self->appearanceDirty=self->backgroundDirty=self->paintDirty=true;
                self->QueuePlace();return 0;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED: self->appearanceDirty = true; self->QueuePlace(); break;
            case WM_WINDOWPOSCHANGED:
                if (!self->placing && lp &&
                    (reinterpret_cast<WINDOWPOS*>(lp)->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
                    self->appbar.Notify(ABM_WINDOWPOSCHANGED);
                break;
            case WM_WINDOWPOSCHANGING:
                if (lp)
                {
                    auto& position = *reinterpret_cast<WINDOWPOS*>(lp);
                    ConstrainHiddenStatusBarPosition(self->fullscreen, position);
                    // Shell can raise a registered AppBar when another window
                    // gains focus without activating the bar itself. Its opaque
                    // surface must remain behind the merged Dock icon HWND.
                    // Correct this request in place: do not send another Shell
                    // notification or recursively reposition the native pair.
                    if (self->mergedDockHeight && self->mergedPresentation.visible &&
                        IsWindow(self->mergedPresentation.insertAfter) &&
                        !(position.flags & (SWP_NOZORDER | SWP_HIDEWINDOW)))
                        position.hwndInsertAfter = self->mergedPresentation.insertAfter;
                }
                break;
            case WM_NCHITTEST:
            {
                POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(window, &point);
                return self->OwnsPointer(point) ? HTCLIENT : HTTRANSPARENT;
            }
            case WM_ACTIVATE:
                self->appbar.Notify(ABM_ACTIVATE);
                // Activation can raise the AppBar above the independent icon
                // HWND. Restore the pair immediately, not on the next hover.
                if (self->mergedDockHeight && self->mergedPresentation.visible &&
                    self->mergedPresentation.insertAfter && IsWindowVisible(window))
                    self->backdrop.SetPopupWindowPairZOrder(window,
                        self->mergedPresentation.insertAfter, true);
                self->paintDirty = true; self->Paint(); break;
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
                self->ClearHover();
                if (self->owner.tray) self->owner.tray->CancelFocusReturn();
                if (self->interaction.Press(self->items, point, message == WM_RBUTTONDOWN) == StatusBarAction::Dismiss)
                {
                    self->DismissSurfaces();
                    return 0;
                }
                const UINT trayMessage = message == WM_LBUTTONDBLCLK && !doubleClick ? WM_LBUTTONDOWN : message;
                if (self->TrayMouse(trayMessage, point)) return 0;
                if (self->CancelTrayMenu() && self->owner.dockChanged) self->owner.dockChanged(false);
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
                if (!self->pressedTray.empty())
                {
                    const auto key = std::exchange(self->pressedTray, {});
                    const bool dragging = std::exchange(self->trayDragging, false);
                    POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ClientToScreen(window, &screen);
                    self->EndDragFeedback(); ReleaseCapture(); self->interaction.CancelPointer();
                    if (dragging)
                    {
                        if (!self->owner.Drop(key, screen) && self->owner.dropOutside) self->owner.dropOutside(key, screen);
                    }
                    else if (self->owner.tray && std::any_of(self->items.begin(), self->items.end(), [&](const auto& item) {
                        return item.icon && item.icon->key == key && PtInRect(&item.bounds, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                    }))
                    {
                        self->ActivateTray(key, tray::Activation::LeftUp, screen, true);
                    }
                    return 0;
                }
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
                self->CancelTrayMenu();
                if (self->owner.tray) self->owner.tray->CancelFocusReturn();
                self->ClearHover();
                auto onActivated = self->owner.activate;
                if (onActivated) onActivated(StatusBarAction::Menu, window, {point.x, point.y, point.x, point.y});
                return 0;
            }
            case WM_MOUSEMOVE:
            {
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (!self->pressedTray.empty())
                {
                    POINT screen = point; ClientToScreen(window, &screen);
                    const LONG threshold = MulDiv(8, static_cast<int>(self->dpi), 96);
                    if (std::abs(screen.x - self->trayPress.x) >= threshold ||
                        std::abs(screen.y - self->trayPress.y) >= threshold)
                    {
                        if (!self->trayDragging)
                        {
                            self->trayDragging = true; self->ClearHover();
                            if (self->owner.dragFeedback.begin)
                                for (const auto& item : self->items) if (item.icon && item.icon->key == self->pressedTray)
                                {
                                    self->trayPreview = true;
                                    self->owner.dragFeedback.begin(window,*item.icon,screen,
                                        static_cast<UINT>(std::lround(24*self->dpi/96.f*(self->mergedDockHeight ? 1.f : self->owner.settings.scale))));
                                    break;
                                }
                        }
                    }
                    if (self->trayDragging)
                    {
                        if (self->owner.dragFeedback.move) self->owner.dragFeedback.move(self->pressedTray,screen);
                        SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));
                    }
                    return 0;
                }
                std::string hoveredKey;
                std::optional<std::size_t> hover;
                self->tooltipControlPart.reset();
                for (const auto& item : self->items) if (PtInRect(&item.bounds, point))
                {
                    hover = static_cast<std::size_t>(&item - self->items.data());
                    if (item.key == "controlCenter")
                    {
                        const auto part = StatusBarControlPart((point.x - item.bounds.left) /
                            (self->dpi / 96.f * (self->mergedDockHeight ? 1.f : self->owner.settings.scale)));
                        self->tooltipControlPart = part;
                    }
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
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
                // A snapshot can change inside Paint. Reconcile then clears the
                // old tooltip after this event, never reinstating stale text.
                if (self->paintDirty) self->Paint();
                else self->SyncTooltip();
                break;
            }
            case WM_MOUSEWHEEL:
            {
                POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(window, &point);
                for (const auto& item : self->items)
                    if (item.key == "controlCenter" && PtInRect(&item.bounds, point))
                    {
                        const float x = (point.x - item.bounds.left) / (self->dpi / 96.f * (self->mergedDockHeight ? 1.f : self->owner.settings.scale));
                        if (StatusBarControlPart(x) != 1) return 0;
                        const bool canceledMenu = self->CancelTrayMenu();
                        const auto sample = self->owner.data->AudioOutputVolume();
                        if (!sample || !sample->available)
                        {
                            if (canceledMenu && self->owner.dockChanged) self->owner.dockChanged(false);
                            return 0;
                        }
                        auto& wheel = self->owner.volumeWheel;
                        if (wheel.task && wheel.endpoint != sample->endpointId)
                        { self->owner.data->Controls()->Cancel(wheel.task); wheel.Reset(); }
                        if (const auto target = wheel.Move(GET_WHEEL_DELTA_WPARAM(wp), sample->volume))
                        {
                            system_control::Request request;
                            request.name = "audio.output.setVolume";
                            request.arguments["volume"] = std::to_string(*target);
                            wheel.Started(self->owner.data->Controls()->Start("statusBarVolume", std::move(request)), sample->endpointId);
                            self->interaction.hovered = static_cast<std::size_t>(&item - self->items.data());
                            self->tooltipControlPart = 1;
                            self->SyncTooltip(true);
                            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
                        }
                        if (canceledMenu && self->owner.dockChanged) self->owner.dockChanged(false);
                        return 0;
                    }
                break;
            }
            case WM_MOUSELEAVE:
                if (!self->pressedTray.empty()) return 0;
                [[fallthrough]];
            case WM_CANCELMODE:
            case WM_CAPTURECHANGED:
                self->EndDragFeedback();
                self->pressedTray.clear(); self->trayDragging = false;
                if (message == WM_CANCELMODE && GetCapture() == window) ReleaseCapture();
                self->ClearHover(); self->interaction.CancelPointer();
                self->paintDirty = true; self->Paint();
                break;
            }
            return DefWindowProcW(window, message, wp, lp);
        }
    };
    std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data;
    std::shared_ptr<tray::Service> tray;
    std::unique_ptr<status_bar_input_method::Service> inputMethod;
    Activate activate;
    Hidden hidden;
    Error error;
    DrawBackground drawBackground;
    DrawBackground drawTooltipBackground;
    std::function<void(const StatusBarSettings&)> trayChanged;
    std::function<bool(std::string_view, POINT)> dropOutside;
    TrayDragFeedback dragFeedback;
    std::function<void(bool)> dockChanged;
    std::function<StatusBarDockState(HMONITOR)> dockStateProvider;
    std::function<bool(HMONITOR)> interactionSessionProvider;
    std::function<PersonalizationSettings(HMONITOR)> mergedAppearanceProvider;
    std::function<StatusBarSceneState(HMONITOR)> sceneProvider;
    std::function<void(HRESULT)> graphicsFailure;
    StatusBarSettings settings;
    PersonalizationSettings globalAppearance;
    PersonalizationSettings tooltipAppearance;
    StatusBarVolumeWheel volumeWheel;
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDWriteFactory> text;
    std::map<std::wstring, std::unique_ptr<Window>> windows;
    bool removingWindows = false;
    std::vector<HWINEVENTHOOK> hooks;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    struct DropTarget { Window* window; bool pinned; std::string before; RECT indicator; };
    std::optional<DropTarget> FindDrop(std::string_view key, POINT point)
    {
        if (removingWindows || !tray || key.empty()) return {};
        for (const auto& [id, window] : windows)
        {
            if (!window || !window->InputAvailable()) continue;
            RECT bounds{}; GetWindowRect(window->hwnd, &bounds);
            if (!PtInRect(&bounds, point)) continue;
            POINT local = point; ScreenToClient(window->hwnd, &local);
            const LONG inset = MulDiv(5,static_cast<int>(window->dpi),96);
            const LONG stroke = (std::max)(2,MulDiv(2,static_cast<int>(window->dpi),96));
            const auto snapshot=tray->Current();auto visible=window->items;
            std::erase_if(visible,[&](const auto& item){return item.icon&&std::none_of(snapshot.icons.begin(),snapshot.icons.end(),[&](const auto& icon){
                return icon.key==item.icon->key&&!(icon.state&NIS_HIDDEN)&&!tray::DuplicatesControlCenter(icon)&&!icon.persistentKey.empty()&&
                    std::find(settings.pinnedTrayItems.begin(),settings.pinnedTrayItems.end(),icon.persistentKey)!=settings.pinnedTrayItems.end();
            });});
            const auto target=ResolveStatusBarTrayDrop(visible,local,inset,stroke);
            if(!target)return {};
            auto probe=settings;
            if (!tray::PlaceIcon(probe,snapshot,key,target->pinned,target->before)) return {};
            return DropTarget{window.get(),target->pinned,target->before,target->indicator};
        }
        return {};
    }
    bool PreviewDrop(std::string_view key,POINT point)
    {
        if(removingWindows)return false;
        const auto target=FindDrop(key,point);
        for(const auto& [id,window]:windows)
        {
            if(!window)continue;
            const std::optional<RECT> next=target&&target->window==window.get()?std::optional(target->indicator):std::nullopt;
            if(next.has_value()!=window->dropIndicator.has_value() ||
                (next&&!EqualRect(&*next,&*window->dropIndicator)))
            {window->dropIndicator=next;window->paintDirty=true;window->Paint();}
        }
        return target.has_value();
    }
    bool Drop(std::string_view key,POINT point)
    {
        const auto target=FindDrop(key,point);
        if(target&&tray::PlaceIcon(settings,tray->Current(),key,target->pinned,target->before))
        {const auto value=settings;const auto callback=trayChanged;if(callback)callback(value);return true;}
        return false;
    }
    Impl(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> source,
        Activate action, Hidden hide, Error report, DrawBackground draw)
        : data(std::move(source)), activate(std::move(action)), hidden(std::move(hide)), error(std::move(report)), drawBackground(std::move(draw)) {}
    void Close()
    {
        for (auto hook : hooks) UnhookWinEvent(hook);
        hooks.clear();
        for (const auto& [id,window] : windows) if(window) window->EndDragFeedback();
        // A panel-origin drag can end from Window::~Window -> hidden. Its
        // cross-surface cleanup must not iterate a map during clear/erase.
        removingWindows=true;
        windows.clear();
        removingWindows=false;
        tray.reset();
        inputMethod.reset();
        volumeWheel.Reset();
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
void StatusBar::ReleaseGraphicsResources()
{
    auto& self = *impl_;
    for (auto& [id, window] : self.windows)
    {
        (void)id;
        if (!window) continue;
        window->Hide();
        window->tooltip.Close();
        window->backdrop.Reset();
        if (window->target) (void)window->target->SetRoot(nullptr);
        window->surface.Reset(); window->backgroundSurface.Reset();
        window->contentVisual.Reset(); window->visual.Reset(); window->mergedOpacity.Reset(); window->target.Reset();
        window->appearanceDirty = window->backgroundDirty = window->paintDirty = true;
    }
    self.composition.Reset(); self.text.Reset();
}
HRESULT StatusBar::ShowInputMethod(RECT anchor)
{
    return impl_->settings.enabled && impl_->settings.inputMethod && impl_->inputMethod ?
        impl_->inputMethod->Show(anchor) : E_UNEXPECTED;
}
std::shared_ptr<tray::Service> StatusBar::Tray() const { return impl_->tray; }
void StatusBar::SetTrayDragHandlers(std::function<void(const StatusBarSettings&)> changed,
    std::function<bool(std::string_view, POINT)> dropOutside)
{ impl_->trayChanged = std::move(changed); impl_->dropOutside = std::move(dropOutside); }
bool StatusBar::DropTrayIcon(std::string_view key, POINT screen) { return impl_->Drop(key, screen); }
bool StatusBar::PreviewTrayDrop(std::string_view key, POINT screen) { return impl_->PreviewDrop(key,screen); }
void StatusBar::SetTrayDragFeedback(TrayDragFeedback feedback) { impl_->dragFeedback=std::move(feedback); }
void StatusBar::SetTrayExpanded(HMONITOR monitor, bool expanded)
{
    if (impl_->removingWindows) return;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->monitor != monitor || window->trayExpanded == expanded) continue;
        window->trayExpanded = expanded;
        window->paintDirty = true;
        if (window->hwnd) InvalidateRect(window->hwnd, nullptr, FALSE);
    }
}
void StatusBar::SetDockChanged(std::function<void(bool)> changed) { impl_->dockChanged = std::move(changed); }
void StatusBar::SetDockStateProvider(std::function<StatusBarDockState(HMONITOR)> provider)
{ impl_->dockStateProvider = std::move(provider); }
void StatusBar::SetInteractionSessionProvider(std::function<bool(HMONITOR)> provider)
{ impl_->interactionSessionProvider = std::move(provider); }
void StatusBar::SetMergedAppearanceProvider(std::function<PersonalizationSettings(HMONITOR)> provider)
{ impl_->mergedAppearanceProvider = std::move(provider); }
void StatusBar::RefreshDockState(HMONITOR monitor)
{
    if (impl_->removingWindows) return;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor) window->CheckFullscreen();
    }
}
void StatusBar::ApplyMergedDockPresentation(HMONITOR monitor, const StatusBarDockPresentation& frame)
{
    if (impl_->removingWindows) return;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor) window->ApplyDockPresentation(frame);
    }
}
bool StatusBar::PrepareMergedDockPresentation(HMONITOR monitor, const StatusBarDockPresentation& frame)
{
    if (impl_->removingWindows) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor) return window->PrepareDockPresentation(frame);
    }
    return false;
}
HWND StatusBar::MergedPresentationBottomWindow(HMONITOR monitor) const
{
    if (impl_->removingWindows) return nullptr;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->monitor != monitor || !window->mergedDockHeight ||
            !IsWindowVisible(window->hwnd)) continue;
        const HWND next = GetWindow(window->hwnd, GW_HWNDNEXT);
        return window->backdrop.IsBackdropWindow(next) ? next : window->hwnd;
    }
    return nullptr;
}
HWND StatusBar::InteractionWindow(HMONITOR monitor) const
{
    if (impl_->removingWindows) return nullptr;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor && window->InputAvailable()) return window->hwnd;
    }
    return nullptr;
}
bool StatusBar::DismissMergedBackground(HMONITOR monitor, POINT screen)
{
    if (impl_->removingWindows) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->monitor != monitor || !window->mergedDockHeight ||
            !window->InputAvailable() || !PtInRect(&window->placedBounds, screen)) continue;
        POINT local = screen;
        ScreenToClient(window->hwnd, &local);
        if (HitTestStatusBarItems(window->items, local)) return false;
        window->DismissSurfaces();
        return true;
    }
    return false;
}
bool StatusBar::IsInteractionAvailable(HMONITOR monitor) const
{
    if (impl_->removingWindows) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor)
            return window->InputAvailable();
    }
    return false;
}
bool StatusBar::HasInteractionSession(HMONITOR monitor) const
{
    return !impl_->removingWindows && impl_->interactionSessionProvider &&
        impl_->interactionSessionProvider(monitor);
}
bool StatusBar::HasTrayMenuSession(HMONITOR monitor) const
{
    if (impl_->removingWindows) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor) return window->ActiveTrayMenu();
    }
    return false;
}
bool StatusBar::ContainsTrayMenuPoint(HMONITOR monitor, POINT screen) const
{
    if (impl_->removingWindows) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor)
            return window->ActiveTrayMenu() && window->trayMenu.ContainsPoint(screen);
    }
    return false;
}
void StatusBar::CancelTrayMenuSession(HMONITOR monitor)
{
    if (impl_->removingWindows) return;
    bool changed = false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && (!monitor || window->monitor == monitor)) changed = window->CancelTrayMenu() || changed;
    }
    // Clear every requested window before invoking policy, which can query us.
    if (changed && impl_->dockChanged) impl_->dockChanged(false);
}
bool StatusBar::ContainsPoint(POINT screen) const
{
    if (impl_->removingWindows) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->closing) continue;
        auto local = screen; ScreenToClient(window->hwnd, &local);
        if (window->OwnsPointer(local)) return true;
    }
    return false;
}
RECT StatusBar::AvailableWorkArea(HMONITOR monitor, RECT area) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor && window->appbar.Registered())
            return ConstrainStatusBarWorkArea(area, window->appbar.Bounds(), window->appbar.Edge());
    }
    return area;
}
RECT StatusBar::MergedSizingWorkArea(HMONITOR monitor, RECT area) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor && window->mergedDockHeight)
            return window->appbar.RestoreWorkArea(area);
    }
    return area;
}
void StatusBar::PrepareDockReveal(HMONITOR monitor)
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->monitor != monitor) continue;
        window->CheckFullscreen();
        window->fullscreenState.BeginDockReveal(!window->mergedDockHeight);
        window->dockFullscreenProcess = 0;
        if (const HWND source = window->fullscreenState.DockSource())
            GetWindowThreadProcessId(source, &window->dockFullscreenProcess);
        // The independent bar can now accept the whole down/up/open sequence,
        // even if Dock's own hover or launch session ends in the meantime.
        if (!window->mergedDockHeight) window->CheckFullscreen();
    }
}
bool StatusBar::HasTemporaryReveal(HMONITOR monitor) const
{
    if (impl_->removingWindows) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor && !window->mergedDockHeight)
            return window->fullscreenState.Revealed();
    }
    return false;
}
void StatusBar::DismissTemporaryReveal(HMONITOR monitor)
{
    if (impl_->removingWindows) return;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->monitor != monitor || !window->fullscreenState.Revealed()) continue;
        window->fullscreenState.DismissReveal();
        // Let the existing popup close path consume the outside down first.
        PostMessageW(window->hwnd, kFullscreen, 0, 0);
    }
}
void StatusBar::SetGraphicsFailureHandler(std::function<void(HRESULT)> handler) { impl_->graphicsFailure = std::move(handler); }
void StatusBar::SetSceneProvider(std::function<StatusBarSceneState(HMONITOR)> provider)
{
    impl_->sceneProvider = std::move(provider);
    for (auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->UpdateAppearance()) window->QueuePlace();
    }
}
PersonalizationSettings StatusBar::AppearanceForMonitor(HMONITOR monitor) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor) return window->appearance;
    }
    return ResolveStatusBarAppearance(impl_->settings.theme, impl_->globalAppearance);
}
bool StatusBar::MergesDock(HMONITOR monitor) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor && window->mergedDockHeight) return true;
    }
    return false;
}
std::optional<RECT> StatusBar::MergedDockArea(HMONITOR monitor) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->monitor != monitor || !window->mergedDockHeight || window->failed ||
            !window->positioned) continue;
        const auto bounds = window->placedBounds;
        auto center = MergedStatusBarCenter(bounds.right - bounds.left, bounds.bottom - bounds.top,
            window->dpi / 96.f * impl_->settings.scale);
        OffsetRect(&center, bounds.left, bounds.top); return center;
    }
    return {};
}
std::optional<RECT> StatusBar::MergedStripBounds(HMONITOR monitor) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor && window->mergedDockHeight &&
            !window->failed && window->positioned) return window->placedBounds;
    }
    return {};
}
bool StatusBar::IsFullscreen(HMONITOR monitor) const
{
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (window && window->monitor == monitor) return window->fullscreenObserved;
    }
    return false;
}
bool StatusBar::PostActivation(HWND owner, std::function<void()> callback)
{
    if (impl_->removingWindows || !owner || !callback) return false;
    for (const auto& [id, window] : impl_->windows)
    {
        (void)id;
        if (!window || window->hwnd != owner || !window->InputAvailable()) continue;
        return window->activation.Post(owner, kActivate, std::move(callback));
    }
    return false;
}
void StatusBar::Configure(StatusBarSettings settings, const PersonalizationSettings& global,
    const std::vector<StatusBarMonitor>& monitors,
    IDCompositionDesktopDevice* composition, IDWriteFactory* text,
    const PersonalizationSettings* tooltipAppearance, DrawBackground drawTooltipBackground)
{
    NormalizeStatusBarSettings(settings);
    auto& self = *impl_;
    const bool changed = settings != self.settings || global != self.globalAppearance;
    self.settings = std::move(settings);
    self.globalAppearance = global;
    self.tooltipAppearance = tooltipAppearance ? *tooltipAppearance : global;
    self.drawTooltipBackground = std::move(drawTooltipBackground);
    if (self.composition.Get() != composition)
        ReleaseGraphicsResources();
    self.composition = composition; self.text = text;
    if (!self.settings.enabled || monitors.empty()) { self.Close(); return; }
    if (!self.tray) self.tray = std::make_shared<tray::Service>();
    if (self.settings.inputMethod && !self.inputMethod)
        self.inputMethod = std::make_unique<status_bar_input_method::Service>();
    else if (!self.settings.inputMethod) self.inputMethod.reset();
    self.Demand("system.cpu", self.settings.cpu);
    self.Demand("system.memory", self.settings.memory);
    self.Demand("system.gpu", self.settings.gpu);
    self.Demand("system.network.traffic", self.settings.traffic);
    self.Demand("system.network.status", true, 3000);
    self.Demand("network.wifi", true, 3000);
    self.Demand("system.power", true, 3000);
    self.Demand("audio.output.volume", true);
    for(const auto& [id,window]:self.windows)
        if(window&&std::none_of(monitors.begin(),monitors.end(),[&](const auto& monitor){return monitor.id==id;}))
            window->EndDragFeedback();
    self.PreviewDrop({},{});
    self.removingWindows=true;
    std::erase_if(self.windows, [&](const auto& entry) {
        return std::none_of(monitors.begin(), monitors.end(), [&](const auto& monitor) { return monitor.id == entry.first; });
    });
    self.removingWindows=false;
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
            GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &window->explorerPid);
            SetTimer(window->hwnd, kClockTimer, 1000, nullptr);
        }
        if (self.settings.inputMethod) SetTimer(window->hwnd, kInputMethodTimer, 200, nullptr);
        else KillTimer(window->hwnd, kInputMethodTimer);
        if(window->monitor!=monitor.monitor)
        {window->Hide();window->fullscreenState={};window->dockFullscreenProcess=0;
         window->surface.Reset();window->backgroundSurface.Reset();window->appearanceDirty=window->backgroundDirty=window->paintDirty=true;}
        window->monitor = monitor.monitor;
        window->tooltip.Configure(window->hwnd, composition, text, self.tooltipAppearance, self.drawTooltipBackground);
        const bool mergedChanged = window->mergedDockHeight != monitor.mergedDockHeight || window->reserveSpace != monitor.reserveSpace;
        const bool mergedModeChanged = (window->mergedDockHeight > 0) != (monitor.mergedDockHeight > 0);
        if (mergedChanged || changed)
        {
            window->EndDragFeedback(); window->ClearHover(); window->interaction.CancelPointer();
            window->pressedTray.clear(); window->trayDragging = false;
            if (GetCapture() == window->hwnd) ReleaseCapture();
        }
        window->mergedDockHeight = monitor.mergedDockHeight;
        window->reserveSpace = monitor.reserveSpace;
        if (mergedModeChanged)
        {
            window->fullscreenState.DismissReveal();
            window->mergedPresentation = {};
            if (monitor.mergedDockHeight) { window->fullscreen = true; window->Hide(false); }
            else
            {
                if (window->visual) { window->visual->SetOffsetX(0.f); window->visual->SetOffsetY(0.f); window->visual->SetEffect(nullptr); }
                window->backdrop.SetVisualOpacity(1.f);
                window->backdrop.SetVisualTranslation(0.f, 0.f);
                const RECT frame{0, 0, static_cast<LONG>(window->width), static_cast<LONG>(window->height)};
                window->backdrop.SetPanelTransform(reinterpret_cast<std::uintptr_t>(window.get()),
                    D2D1::Matrix4x4F(), frame);
            }
        }
        window->UpdateAppearance();
        window->appearanceDirty = window->appearanceDirty || changed || mergedChanged;
        window->paintDirty = window->paintDirty || changed || mergedChanged;
        if (mergedChanged && self.dockChanged) self.dockChanged(true);
        window->QueuePlace(changed || mergedChanged);
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
