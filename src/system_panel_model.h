#pragma once
#include "native_ui_scene.h"
#include "status_bar.h"
#include "tray_service.h"
#include "calendar_service.h"
#include "system_controls.h"
#include "system_control_feedback.h"
#include "widget_system_data_provider.h"
#include <functional>
#include <map>
#include <set>

namespace snowdesktop
{
struct SystemCalendarActions
{
    std::function<std::vector<calendar::CalendarEvent>(const std::string&)> events;
    std::function<void()> manage;
    std::function<std::string()> today;
    std::function<std::string(const std::string&)> secondaryDate;
};
// All live effects live at this boundary; offline rendering supplies fixtures.
struct SystemPanelSource
{
    std::function<std::optional<system_control::Snapshot>(std::string_view)> current;
    std::function<std::uint64_t(system_control::Request)> start;
    std::function<std::vector<system_control::Completion>()> completions;
    std::function<void(std::string, std::chrono::milliseconds)> subscribe;
    std::function<void(std::string_view)> unsubscribe;
    std::function<void()> close;
    std::function<void(const wchar_t*)> settings;
    std::function<bool(system_control::Request&)> prompt;
    std::function<std::optional<widget_runtime::WidgetMediaSessionsDataSnapshot>()> media;
    std::function<std::optional<widget_runtime::WidgetMediaArtworkDataSnapshot>()> artwork;
    std::function<std::optional<widget_runtime::WidgetCpuDataSnapshot>()> cpu;
    std::function<std::optional<widget_runtime::WidgetMemoryDataSnapshot>()> memory;
    std::function<std::optional<widget_runtime::WidgetGpuDataSnapshot>()> gpu;
    std::function<std::optional<widget_runtime::WidgetNetworkTrafficDataSnapshot>()> traffic;
    std::function<std::vector<widget_runtime::WidgetResourcePoint>(std::string_view,std::string_view)> history;
    std::function<std::int64_t()> now;
    std::function<tray::Snapshot()> tray;
    std::function<void(const StatusBarSettings&)> trayChanged;
    SystemCalendarActions calendar;
};
SystemPanelSource LiveSystemPanelSource(std::shared_ptr<widget_runtime::WidgetSystemDataProvider>);
native_ui::Palette SystemPanelPalette(const PersonalizationSettings&, bool highContrast = false);
bool IsSystemResourceAction(StatusBarAction);
class SystemPanelModel
{
public:
    SystemPanelModel(SystemPanelSource, StatusBarSettings, StatusBarAction);
    ~SystemPanelModel();
    void Refresh(float availableHeight = 800);
    void Select(std::string page);
    bool Invoke(std::string_view id, std::optional<float> value = {});
    bool Drop(std::string_view key, D2D1_POINT_2F);
    void Scroll(float delta);
    void UpdateSettings(const StatusBarSettings&);
    float ScrollOffset() const { return scroll_; }
    float MaximumScroll() const { return maxScroll_; }
    D2D1_RECT_F ScrollViewport() const { return scrollViewport_; }
    void Close();
    const native_ui::Scene& View() const { return scene_; }
    const StatusBarSettings& Settings() const { return settings_; }
    const std::string& Page() const { return page_; }
private:
    SystemPanelSource source_;
    StatusBarSettings settings_;
    StatusBarAction action_;
    native_ui::Scene scene_;
    std::string page_, interface_, network_, gpu_, media_, date_, month_;
    std::map<std::string,std::function<void(std::optional<float>)>> actions_;
    system_control::ControlFeedback feedback_;
    struct PendingValue { std::uint64_t task = 0; float value = 0; std::string target; };
    std::map<std::string,PendingValue> pendingValues_;
    std::map<std::string,std::string> sliderTargets_;
    std::map<std::string,std::string> valueControls_;
    std::set<std::string> subscriptions_;
    std::uint64_t lastStarted_ = 0;
    std::wstring error_;
    float available_ = 800, scroll_ = 0, maxScroll_ = 0, bodyStart_ = 0;
    D2D1_RECT_F scrollViewport_{};
    bool closed_ = false, scan_ = false;
    JsonValue Current(const char*) const;
    JsonValue Wifi() const;
    void SyncSubscriptions();
    void Start(std::string, system_control::Arguments = {});
    void OpenSettings(const wchar_t*);
    native_ui::Node& Add(std::string, native_ui::Role, D2D1_RECT_F, std::wstring = {}, std::wstring = {});
    void Command(std::string, std::function<void()>);
    void Header(std::wstring);
    void Overview(float&);
    void Audio(float&);
    void Brightness(float&);
    void WifiPage(float&);
    void Bluetooth(float&);
    void Power(float&);
    void Media(float&);
    void Calendar();
    void Resources();
    void Tray();
    void Radio(std::string_view, D2D1_RECT_F, bool compact);
    void Volume(std::string_view, float&);
    void Footer(const wchar_t*, float&);
    void Finish(float bodyEnd, bool withMedia);
};
}
