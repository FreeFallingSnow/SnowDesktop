#pragma once
#include "status_bar.h"
#include "ui_animation_scheduler.h"
#include <dcomp.h>

namespace snowdesktop
{
struct SystemCalendarActions;
class SystemPanel
{
public:
    using SettingsChanged=std::function<void(const StatusBarSettings&)>;
    using Background=std::function<void(ID2D1DeviceContext*,RECT,const PersonalizationSettings&,float)>;
    SystemPanel(SettingsChanged,SystemCalendarActions,std::function<bool(std::string_view,POINT)>,
        UiAnimationScheduler*,IDCompositionDesktopDevice*,IDWriteFactory*,Background);
    ~SystemPanel();
    void Show(StatusBarAction,HWND,RECT,const PersonalizationSettings&,const StatusBarSettings&,
        std::shared_ptr<tray::Service>,std::shared_ptr<widget_runtime::WidgetSystemDataProvider>);
    void Hide();
    void HideForMonitor(HMONITOR);
    void CloseThen(std::function<void()>);
    bool IsOpen() const;
    void UpdateSettings(const StatusBarSettings&);
    bool PreTranslateMessage(MSG*);
    bool DropTrayIcon(std::string_view,POINT);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
