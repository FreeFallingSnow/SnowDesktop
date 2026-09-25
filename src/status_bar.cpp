#include "status_bar.h"
#include "status_bar_appbar.h"
#include "widget_system_data_provider.h"
#include "app/desktop_backdrop_compositor.h"
#include "panel_gradient_renderer.h"
#include "l10n.h"
#include "tray_service.h"
#include "diagnostic_log.h"
#include "status_bar_layout.h"

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
constexpr UINT kAppBar = WM_APP + 41, kPlace = WM_APP + 42, kFullscreen = WM_APP + 43;
constexpr UINT_PTR kClockTimer = 1;
std::map<HWND, bool> liveBars; // All access, including out-of-context hooks, on the UI thread.
void CALLBACK WindowEvent(HWINEVENTHOOK, DWORD, HWND, LONG object, LONG, DWORD, DWORD)
{
    if (object != OBJID_WINDOW) return;
    for (auto& [window, pending] : liveBars)
        if (!pending) pending = PostMessageW(window, kFullscreen, 0, 0) != FALSE;
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
std::wstring Percent(double value)
{
    return std::to_wstring(static_cast<int>(std::lround(value))) + L"%";
}
}

struct StatusBar::Impl
{
    struct Item
    {
        std::wstring text;
        StatusBarAction action;
        RECT bounds{};
        std::optional<tray::Icon> icon;
    };
    struct Window
    {
        Impl& owner;
        HWND hwnd = nullptr;
        HMONITOR monitor = nullptr;
        StatusBarAppBar appbar;
        DesktopBackdropCompositor backdrop;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual2> visual;
        ComPtr<IDCompositionSurface> surface;
        UINT width = 0, height = 0, dpi = 96;
        DWORD explorerPid = 0;
        bool placing = false, queued = false, fullscreen = false, failed = false, closing = false;
        bool appearanceDirty = true, paintDirty = true, painting = false;
        HRESULT lastPaintError = S_OK;
        std::string hoveredTray;
        HWND tooltip = nullptr;
        std::wstring tooltipText;
        std::vector<Item> items;
        std::size_t focused = 0;
        explicit Window(Impl& value) : owner(value) {}
        ~Window()
        {
            closing = true;
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
        void Hide()
        {
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
                SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
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
            appearanceDirty = false;
            placing = false;
            CheckFullscreen();
            if (!fullscreen) Show();
        }
        void BuildItems()
        {
            items.clear();
            const auto& s = owner.settings;
            const auto add = [&](std::wstring text, StatusBarAction action) {
                items.push_back({std::move(text), action, {}, {}});
            };
            add(L"SnowDesktop", StatusBarAction::Menu);
            if (s.clock)
            {
                wchar_t time[64]{}, date[96]{};
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, time, 64);
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, date, 96, nullptr);
                add(std::wstring(date) + L"   " + time, StatusBarAction::Calendar);
            }
            if (s.cpu)
            {
                const auto value = owner.data->Cpu();
                add(L"CPU " + (value && value->available && !value->warmingUp ? Percent(value->usagePercent) : L"—"), StatusBarAction::ControlCenter);
            }
            if (s.memory)
            {
                const auto value = owner.data->Memory();
                add(_LW("statusBar.memory") + std::wstring(L" ") + (value && value->available && value->totalBytes ?
                    Percent(100. * value->usedBytes / value->totalBytes) : L"—"), StatusBarAction::ControlCenter);
            }
            if (s.gpu)
            {
                const auto value = owner.data->Gpu();
                double maximum = 0;
                if (value) for (const auto& adapter : value->adapters) maximum = std::max(maximum, adapter.usagePercent);
                add(L"GPU " + (value && value->available && !value->warmingUp ? Percent(maximum) : L"—"), StatusBarAction::ControlCenter);
            }
            if (s.traffic)
            {
                const auto value = owner.data->NetworkTraffic();
                add(value && value->available && !value->warmingUp ?
                    L"↓ " + std::to_wstring(value->downloadBytesPerSecond / 1024) +
                    L" ↑ " + std::to_wstring(value->uploadBytesPerSecond / 1024) + L" KiB/s" : L"↓ — ↑ —", StatusBarAction::Network);
            }
            if (s.network) add(_LW("statusBar.network"), StatusBarAction::Network);
            if (s.volume)
            {
                const auto value = owner.data->AudioOutputVolume();
                add(_LW("statusBar.volume") + std::wstring(L" ") + (value && value->available ?
                    (value->muted ? _LW("statusBar.muted") : Percent(value->volume * 100.)) : L"—"), StatusBarAction::Audio);
            }
            if (s.battery)
            {
                const auto value = owner.data->Power();
                if (value && value->available) add(Percent(value->batteryPercent), StatusBarAction::Power);
            }
            if (s.tray)
            {
                if (owner.tray)
                {
                    auto icons = owner.tray->Current().icons;
                    const auto rank = [&](const tray::Icon& icon) {
                        return std::find(s.trayOrder.begin(), s.trayOrder.end(), icon.persistentKey) - s.trayOrder.begin();
                    };
                    std::stable_sort(icons.begin(), icons.end(), [&](const auto& left, const auto& right) { return rank(left) < rank(right); });
                    for (auto& icon : icons)
                        if (!(icon.state & NIS_HIDDEN) && !icon.persistentKey.empty() &&
                            std::find(s.pinnedTrayItems.begin(), s.pinnedTrayItems.end(), icon.persistentKey) != s.pinnedTrayItems.end())
                            items.push_back({icon.tip, StatusBarAction::Tray, {}, std::move(icon)});
                }
                add(L"⌃", StatusBarAction::Tray);
            }
            if (s.controlCenter) add(L"☷", StatusBarAction::ControlCenter);
        }
        void Paint()
        {
            if (closing || painting || fullscreen || failed || !IsWindowVisible(hwnd) || !owner.composition || !owner.text) return;
            auto previousItems = std::move(items);
            BuildItems();
            const bool same = previousItems.size() == items.size() && std::equal(items.begin(), items.end(), previousItems.begin(),
                [](const auto& a, const auto& b) {
                    return a.text == b.text && a.action == b.action && a.icon.has_value() == b.icon.has_value() &&
                        (!a.icon || (a.icon->key == b.icon->key && a.icon->width == b.icon->width &&
                            a.icon->height == b.icon->height && a.icon->pixels == b.icon->pixels));
                });
            if (same && !paintDirty) { items = std::move(previousItems); return; }
            RECT client{};
            GetClientRect(hwnd, &client);
            if (IsRectEmpty(&client)) return;
            const UINT w = static_cast<UINT>(client.right), h = static_cast<UINT>(client.bottom);
            if (!target && FAILED(owner.composition->CreateTargetForHwnd(hwnd, FALSE, &target))) return;
            if (!visual)
            {
                if (FAILED(owner.composition->CreateVisual(&visual))) return;
                target->SetRoot(visual.Get());
            }
            if (!surface || width != w || height != h)
            {
                surface.Reset();
                if (FAILED(owner.composition->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                    DXGI_ALPHA_MODE_PREMULTIPLIED, &surface))) return;
                width = w; height = h;
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
            const auto& a = owner.appearance;
            const bool hc = HighContrast();
            context->Clear(hc ? SystemColor(COLOR_WINDOW) : D2D1::ColorF(0, 0.f));
            if (!hc && owner.drawBackground) owner.drawBackground(context.Get(), client, a, dpi / 96.f * owner.settings.scale);
            ComPtr<ID2D1SolidColorBrush> brush;
            context->CreateSolidColorBrush(hc ? SystemColor(COLOR_WINDOWTEXT) :
                D2D1::ColorF(a.contentTheme == 1 ? 0x202020 : 0xf4f4f4), &brush);
            ComPtr<IDWriteTextFormat> format;
            const float scale = dpi / 96.f * owner.settings.scale;
            owner.text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f * scale, L"", &format);
            if (format && brush)
            {
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const float padding = 8.f * scale;
                const auto extentOf = [&](const Item& item) {
                    if (item.icon) return 28.f * scale;
                    ComPtr<IDWriteTextLayout> layout;
                    DWRITE_TEXT_METRICS metrics{};
                    if (SUCCEEDED(owner.text->CreateTextLayout(item.text.c_str(), static_cast<UINT32>(item.text.size()),
                            format.Get(), 2000.f, static_cast<float>(h), &layout))) layout->GetMetrics(&metrics);
                    return std::clamp(metrics.widthIncludingTrailingWhitespace + 20.f * scale, 30.f * scale, 260.f * scale);
                };
                LONG centerWidth = 0, leftWidth = 0;
                std::vector<LONG> rightWidths;
                for (const auto& item : items)
                    if (item.action == StatusBarAction::Calendar) centerWidth = static_cast<LONG>(std::ceil(extentOf(item)));
                    else if (item.action == StatusBarAction::Menu) leftWidth = static_cast<LONG>(std::ceil(extentOf(item)));
                    else rightWidths.push_back(static_cast<LONG>(std::ceil(extentOf(item))));
                const auto bounds = StatusBarHorizontalLayout(static_cast<LONG>(w), static_cast<LONG>(h),
                    static_cast<LONG>(padding), leftWidth, centerWidth, rightWidths);
                std::size_t rightIndex = 2;
                for (auto& item : items)
                {
                    const bool left = item.action == StatusBarAction::Menu, center = item.action == StatusBarAction::Calendar;
                    item.bounds = bounds[left ? 0 : center ? 1 : rightIndex++];
                    if (IsRectEmpty(&item.bounds))
                    {
                        if (item.icon && owner.tray) owner.tray->SetGeometry(item.icon->key, {});
                        continue;
                    }
                    const auto rect = D2D1::RectF(static_cast<float>(item.bounds.left), 0,
                        static_cast<float>(item.bounds.right), static_cast<float>(h));
                    if (item.icon)
                    {
                        const auto& icon = *item.icon;
                        ComPtr<ID2D1Bitmap> bitmap;
                        if (!icon.pixels.empty() && SUCCEEDED(context->CreateBitmap(D2D1::SizeU(icon.width, icon.height),
                                icon.pixels.data(), icon.width * 4,
                                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bitmap)))
                        {
                            const float size = 18.f * scale;
                            const float iconLeft = (rect.left + rect.right - size) / 2, iconTop = (rect.top + rect.bottom - size) / 2;
                            context->DrawBitmap(bitmap.Get(), D2D1::RectF(iconLeft, iconTop, iconLeft + size, iconTop + size));
                        }
                        RECT screen = item.bounds;
                        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&screen), 2);
                        owner.tray->SetGeometry(icon.key, screen);
                    }
                    else context->DrawText(item.text.c_str(), static_cast<UINT32>(item.text.size()), format.Get(), rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    if (GetFocus() == hwnd && &item == &items[std::min(focused, items.size() - 1)])
                        context->DrawRectangle(rect, brush.Get(), 1.f);
                }
            }
            context.Reset();
            const auto result = surface->EndDraw();
            painting = false;
            if (SUCCEEDED(result))
            {
                visual->SetContent(surface.Get());
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
        void ActivateItem(std::size_t index)
        {
            if (fullscreen || index >= items.size() || !owner.activate) return;
            RECT anchor = items[index].bounds;
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
            if (items[index].icon && owner.tray)
            {
                owner.tray->SetGeometry(items[index].icon->key, anchor);
                owner.tray->Activate(items[index].icon->key, tray::Activation::Keyboard, {anchor.left, anchor.top});
                return;
            }
            auto onActivated = owner.activate;
            onActivated(items[index].action, hwnd, anchor);
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
                owner.tray->Activate(item.icon->key, action, point);
                return true;
            }
            return false;
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
            switch (message)
            {
            case kPlace: self->Place(); return 0;
            case kFullscreen:
                if (auto found = liveBars.find(window); found != liveBars.end()) found->second = false;
                self->CheckFullscreen(); return 0;
            case kAppBar:
                if (wp == ABN_POSCHANGED) self->QueuePlace();
                else if (wp == ABN_FULLSCREENAPP) PostMessageW(window, kFullscreen, 0, 0);
                return 0;
            case WM_TIMER:
                if (wp == kClockTimer) { self->CheckFullscreen(); self->Paint(); }
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
            case WM_RBUTTONUP:
                if (self->TrayMouse(message, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) return 0;
                break;
            case WM_LBUTTONUP:
            {
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (self->TrayMouse(message, point)) return 0;
                for (std::size_t index = 0; index < self->items.size(); ++index)
                    if (PtInRect(&self->items[index].bounds, point)) { self->ActivateItem(index); break; }
                return 0;
            }
            case WM_CONTEXTMENU:
            {
                POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (point.x != -1 || point.y != -1)
                {
                    ScreenToClient(window, &point);
                    for (const auto& item : self->items)
                        if (item.icon && PtInRect(&item.bounds, point)) return 0;
                }
                if (point.x == -1 && point.y == -1) point = {self->appbar.Bounds().left, self->appbar.Bounds().bottom};
                else ClientToScreen(window, &point);
                auto onActivated = self->owner.activate;
                if (onActivated) onActivated(StatusBarAction::Menu, window, {point.x, point.y, point.x, point.y});
                return 0;
            }
            case WM_MOUSEMOVE:
            {
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                std::string hovered;
                self->tooltipText.clear();
                for (const auto& item : self->items) if (PtInRect(&item.bounds, point))
                {
                    self->tooltipText = item.icon ? (item.icon->tip.empty() ? item.icon->application : item.icon->tip) : item.text;
                    if (item.icon) hovered = item.icon->key;
                    break;
                }
                if (hovered != self->hoveredTray && self->owner.tray)
                {
                    POINT screen = point; ClientToScreen(window, &screen);
                    if (!self->hoveredTray.empty()) self->owner.tray->Activate(self->hoveredTray, tray::Activation::Leave, screen);
                    if (!hovered.empty()) self->owner.tray->Activate(hovered, tray::Activation::Hover, screen);
                    self->hoveredTray = std::move(hovered);
                }
                TOOLINFOW tool{sizeof(tool)}; tool.hwnd = window; tool.uId = reinterpret_cast<UINT_PTR>(window);
                SetWindowPos(self->tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                tool.lpszText = self->tooltipText.data();
                SendMessageW(self->tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
                break;
            }
            case WM_MOUSELEAVE:
                if (!self->hoveredTray.empty() && self->owner.tray) self->owner.tray->Activate(self->hoveredTray, tray::Activation::Leave, {});
                self->hoveredTray.clear();
                SendMessageW(self->tooltip, TTM_POP, 0, 0);
                break;
            case WM_KEYDOWN:
                if (wp == VK_RETURN || wp == VK_SPACE) { self->ActivateItem(self->focused); return 0; }
                else if (!self->items.empty() && (wp == VK_RIGHT || wp == VK_DOWN || wp == VK_TAB))
                    self->focused = (self->focused + 1) % self->items.size();
                else if (!self->items.empty() && (wp == VK_LEFT || wp == VK_UP))
                    self->focused = (self->focused + self->items.size() - 1) % self->items.size();
                self->paintDirty = true; self->Paint(); return 0;
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
        if (data) data->RemoveConsumer("statusBar");
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
        if (window->monitor == monitor) return window->fullscreen;
    }
    return false;
}
void StatusBar::Configure(StatusBarSettings settings, const PersonalizationSettings& global,
    const std::vector<StatusBarMonitor>& monitors,
    IDCompositionDesktopDevice* composition, IDWriteFactory* text)
{
    NormalizeStatusBarSettings(settings);
    auto& self = *impl_;
    const auto appearance = ResolveSurfaceTheme(settings.theme, global, 0, false);
    const bool changed = settings != self.settings || appearance != self.appearance;
    self.settings = std::move(settings);
    self.appearance = appearance;
    if (self.composition.Get() != composition)
    {
        for (auto& [id, window] : self.windows)
        {
            (void)id;
            window->surface.Reset(); window->visual.Reset(); window->target.Reset();
            window->paintDirty = true;
        }
    }
    self.composition = composition; self.text = text;
    if (!self.settings.enabled || monitors.empty()) { self.Close(); return; }
    if (self.settings.tray && !self.tray) self.tray = std::make_shared<tray::Service>();
    if (!self.settings.tray) self.tray.reset();
    self.Demand("system.cpu", self.settings.cpu);
    self.Demand("system.memory", self.settings.memory);
    self.Demand("system.gpu", self.settings.gpu);
    self.Demand("system.network.traffic", self.settings.traffic);
    self.Demand("system.network.status", self.settings.network, 3000);
    self.Demand("system.power", self.settings.battery, 10000);
    self.Demand("audio.output.volume", self.settings.volume);
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
            tool.uId = reinterpret_cast<UINT_PTR>(window->hwnd); tool.lpszText = window->tooltipText.data();
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
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)) self.hooks.push_back(hook);
}
}
