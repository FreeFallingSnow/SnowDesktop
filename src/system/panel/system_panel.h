#pragma once
#include "system/status_bar/status_bar.h"
#include "ui/render/ui_animation_scheduler.h"
#include <dcomp.h>

namespace snowdesktop
{
struct SystemCalendarActions;
struct SystemPanelInputMethodActions;
class SystemPanel
{
public:
    using SettingsChanged=std::function<void(const StatusBarSettings&)>;
    using Background=std::function<void(ID2D1DeviceContext*,RECT,const PersonalizationSettings&,float)>;
    SystemPanel(SettingsChanged,SystemCalendarActions,std::function<bool(std::string_view,POINT)>,
        UiAnimationScheduler*,IDCompositionDesktopDevice*,IDWriteFactory*,Background);
    ~SystemPanel();
    void Show(StatusBarAction,HWND,RECT,const PersonalizationSettings&,const StatusBarSettings&,
        std::shared_ptr<tray::Service>,std::shared_ptr<widget_runtime::WidgetSystemDataProvider>, bool clockAtRight=false);
    void ShowInputMethod(SystemPanelInputMethodActions,HWND,RECT,const PersonalizationSettings&,
        const StatusBarSettings&,std::shared_ptr<widget_runtime::WidgetSystemDataProvider>);
    // Standalone system-menu confirmation: cancel/success dismisses this panel.
    void ShowPowerConfirmation(std::string task,HWND,RECT,const PersonalizationSettings&,const StatusBarSettings&,
        std::shared_ptr<widget_runtime::WidgetSystemDataProvider>);
    void Hide();
    void HideForMonitor(HMONITOR, bool animate = false);
    void CloseThen(std::function<void()>, HWND destinationOwner = nullptr);
    bool IsOpen() const;
    bool IsOpenForMonitor(HMONITOR monitor) const;
    // Includes visible cards and the active prompt/retained tray menu.
    bool ContainsPoint(POINT screen) const;
    void UpdateSettings(const StatusBarSettings&);
    bool PreTranslateMessage(MSG*);
    bool DropTrayIcon(std::string_view,POINT);
    bool PreviewTrayDrop(std::string_view,POINT);
    void SetTrayDragFeedback(TrayDragFeedback);
    void SetTrayStateChanged(std::function<void(HMONITOR,bool)>);
    void SetNativeControlsHandler(std::function<void(HWND,RECT)>);
    void SetCalendarMenuHandler(std::function<UINT(POINT,HWND,bool)>);
    void SetCalendarChoiceMenuHandler(std::function<UINT(POINT,HWND,const std::vector<std::wstring>&,std::size_t)>);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
