#pragma once
#include "status_bar_appearance.h"
#include "merged_dock_presentation.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

struct IDCompositionDesktopDevice;
struct IDWriteFactory;
struct ID2D1DeviceContext;
namespace snowdesktop
{
namespace widget_runtime { class WidgetSystemDataProvider; }
namespace tray { class Service; struct Icon; }
// One application-owned drag surface shared with Dock. Only the captured
// source starts/ends it; both destinations independently preview a drop.
struct TrayDragFeedback
{
    std::function<bool(HWND, const tray::Icon&, POINT, UINT)> begin;
    std::function<void(std::string_view, POINT)> move;
    std::function<void()> end;
};
enum class StatusBarAction { Calendar, Tray, Network, Audio, Power, ControlCenter, Settings, Menu, QuickSearch, SystemMenu, None, Notifications, Cpu, Memory, Gpu, Traffic, Dismiss, SystemControlCenter, TaskView, SystemCalendar, InputMethod, InputMethodMenu };
struct StatusBarMonitor
{
    std::wstring id;
    HMONITOR monitor = nullptr;
    int mergedDockHeight = 0; // Physical pixels; zero means separate.
    bool reserveSpace = true; // Summon-only merged Dock keeps geometry without a Shell reservation.
};
struct StatusBarDockState
{
    bool promoted = false;
    HWND window = nullptr; // Visible merged content window, never owned by bar.
    RECT inputBounds{}; // Screen pixels, only for a visible merged Dock.
    bool interacting = false; // This monitor's bar-owned activation/menu/panel.
};
class StatusBar final
{
public:
    using Activate = std::function<void(StatusBarAction, HWND, RECT)>;
    using Hidden = std::function<void(HMONITOR)>;
    using Error = std::function<void(const std::wstring&)>;
    using DrawBackground = std::function<void(ID2D1DeviceContext*, RECT,
        const PersonalizationSettings&, float)>;
    StatusBar(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data,
        Activate activate, Hidden hidden, Error error, DrawBackground drawBackground);
    ~StatusBar();
    StatusBar(const StatusBar&) = delete;
    StatusBar& operator=(const StatusBar&) = delete;
    // UI-thread only. Monitor order is resolved by the application's page roles.
    void Configure(StatusBarSettings settings, const PersonalizationSettings& global,
        const std::vector<StatusBarMonitor>& monitors,
        IDCompositionDesktopDevice* composition, IDWriteFactory* text,
        const PersonalizationSettings* tooltipAppearance = nullptr,
        DrawBackground drawTooltipBackground = {});
    void Close();
    // Release a lost device without unregistering the AppBar or sampling demands.
    void ReleaseGraphicsResources();
    bool IsFullscreen(HMONITOR monitor) const;
    // Coalesce a UI continuation on its live bar HWND. The callback is run
    // from window dispatch, never inside the shared animation scheduler.
    bool PostActivation(HWND owner, std::function<void()> callback);
    // UI-thread summon boundary: sample before any Dock window/focus changes.
    void PrepareDockReveal(HMONITOR monitor);
    // Separate fullscreen bars dismiss from their own interaction, not Dock close.
    bool HasTemporaryReveal(HMONITOR monitor) const;
    void DismissTemporaryReveal(HMONITOR monitor);
    void SetDockStateProvider(std::function<StatusBarDockState(HMONITOR)> provider);
    // Session queries must not evaluate Dock geometry (which queries visibility).
    void SetInteractionSessionProvider(std::function<bool(HMONITOR)> provider);
    void SetMergedAppearanceProvider(std::function<PersonalizationSettings(HMONITOR)> provider);
    void RefreshDockState(HMONITOR monitor);
    // Prepare the first complete strip frame without revealing either HWND.
    bool PrepareMergedDockPresentation(HMONITOR monitor, const StatusBarDockPresentation& frame);
    void ApplyMergedDockPresentation(HMONITOR monitor, const StatusBarDockPresentation& frame);
    bool IsInteractionAvailable(HMONITOR monitor) const;
    HWND InteractionWindow(HMONITOR monitor) const;
    HWND MergedPresentationBottomWindow(HMONITOR monitor) const;
    // The Dock routes its own blank background through the same dismiss path.
    bool DismissMergedBackground(HMONITOR monitor, POINT screen);
    bool HasInteractionSession(HMONITOR monitor) const;
    // UI-thread tray menu retention; queries never notify Dock policy callbacks.
    bool HasTrayMenuSession(HMONITOR monitor) const;
    bool ContainsTrayMenuPoint(HMONITOR monitor, POINT screen) const;
    void CancelTrayMenuSession(HMONITOR monitor = nullptr);
    bool ContainsPoint(POINT screen) const;
    RECT AvailableWorkArea(HMONITOR monitor, RECT screenWorkArea) const;
    // Icon/bar sizing excludes this merged strip's own reservation. Actual
    // desktop placement still uses the Shell-constrained work area above.
    RECT MergedSizingWorkArea(HMONITOR monitor, RECT screenWorkArea) const;
    HRESULT ShowInputMethod(RECT anchor, bool context);
    std::shared_ptr<tray::Service> Tray() const;
    void SetTrayDragHandlers(std::function<void(const StatusBarSettings&)> changed,
        std::function<bool(std::string_view, POINT)> dropOutside);
    bool DropTrayIcon(std::string_view key, POINT screen);
    bool PreviewTrayDrop(std::string_view key, POINT screen);
    void SetTrayDragFeedback(TrayDragFeedback feedback);
    // Actual panel visibility, not the last click; monitor-local and UI-thread only.
    void SetTrayExpanded(HMONITOR monitor, bool expanded);
    std::optional<RECT> MergedDockArea(HMONITOR monitor) const;
    bool MergesDock(HMONITOR monitor) const;
    std::optional<RECT> MergedStripBounds(HMONITOR monitor) const;
    void SetDockChanged(std::function<void(bool geometry)> changed);
    // Application-owned window observations; must not enable taskbar effects.
    void SetSceneProvider(std::function<StatusBarSceneState(HMONITOR)> provider);
    void SetGraphicsFailureHandler(std::function<void(HRESULT)> handler);
    PersonalizationSettings AppearanceForMonitor(HMONITOR monitor) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
