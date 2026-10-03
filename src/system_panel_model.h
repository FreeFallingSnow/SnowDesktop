#pragma once
#include "native_ui_scene.h"
#include "status_bar.h"
#include "status_bar_input_method.h"
#include "tray_service.h"
#include "calendar_service.h"
#include "calendar_display.h"
#include "system_calendar_editor_state.h"
#include "native_date_time_picker.h"
#include "system_controls.h"
#include "system_control_feedback.h"
#include "widget_system_data_provider.h"
#include "widget_scroll_rules.h"
#include <functional>
#include <map>
#include <set>

namespace snowdesktop
{
struct SystemControlPromptState;
struct SystemCalendarActions
{
    std::function<std::vector<calendar::CalendarEvent>(const std::string&)> events;
    std::function<void()> manage;
    std::function<std::string()> today;
    std::function<std::string(const std::string&)> secondaryDate;
    std::function<std::map<std::string,std::string>(const std::string&,const std::string&)> secondaryDates;
    std::function<std::string()> secondaryRevision;
    SystemCalendarEditorActions mutations;
    // Read the shared in-memory event snapshot once for both month markers and agenda.
    std::function<std::vector<calendar::CalendarEvent>(const std::string&,const std::string&)> eventsInRange;
    std::function<std::vector<calendar::DayAnnotation>(const std::string&,const std::string&)> secondaryAnnotations;
    std::function<std::string()> secondaryCalendarId;
};
// Native EDIT children share the calendar panel's layout, draft and lifetime.
// Bounds and clip are in panel-local DIPs, after scrolling.
struct SystemCalendarInputField
{
    std::string id;
    std::wstring label, text;
    D2D1_RECT_F bounds{}, clip{};
    bool multiline=false, enabled=true;
    int limit=0;
    bool password=false;
};
struct SystemPanelInputMethodActions
{
    std::vector<status_bar_input_method::Choice> choices;
    std::function<void(const status_bar_input_method::Choice&)> select;
    std::function<void()> menu;
    std::function<void(const wchar_t*)> settings;
    bool menuAvailable = false;
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
    std::function<void()> nativeControls;
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
    // Internal task boundary; optional for deterministic offline sources.
    std::function<bool(std::uint64_t)> cancel;
    SystemPanelInputMethodActions inputMethod;
};
SystemPanelSource LiveSystemPanelSource(std::shared_ptr<widget_runtime::WidgetSystemDataProvider>);
native_ui::Palette SystemPanelPalette(const PersonalizationSettings&, bool highContrast = false);
bool IsSystemResourceAction(StatusBarAction);
class SystemPanelModel
{
public:
    SystemPanelModel(SystemPanelSource, StatusBarSettings, StatusBarAction, bool calendarStacked=false);
    ~SystemPanelModel();
    void Refresh(float availableHeight = 800, float availableWidth = 0);
    void Select(std::string page);
    bool Invoke(std::string_view id, std::optional<float> value = {});
    bool Drop(std::string_view key, D2D1_POINT_2F);
    std::optional<D2D1_RECT_F> TrayDropIndicator(std::string_view key, D2D1_POINT_2F) const;
    void Scroll(float delta);
    bool Reveal(std::string_view id);
    native_ui::InputResult HandleKey(native_ui::Input&, unsigned key, bool shift);
    void Wheel(D2D1_POINT_2F point, float notches);
    widget_scroll_rules::ScrollbarAxisGeometry ScrollbarGeometry() const;
    void DragScrollbar(int startOffset, int pointerDelta);
    void UpdateSettings(const StatusBarSettings&);
    float ScrollOffset() const { return scroll_; }
    float MaximumScroll() const { return maxScroll_; }
    D2D1_RECT_F ScrollViewport() const { return scrollViewport_; }
    void Close();
    const native_ui::Scene& View() const { return scene_; }
    const StatusBarSettings& Settings() const { return settings_; }
    const std::string& Page() const { return page_; }
    bool CalendarEditing() const;
    struct CalendarChoiceOption { std::wstring label; bool selected = false; };
    std::vector<CalendarChoiceOption> CalendarChoices(std::string_view field) const;
    bool SelectCalendarChoice(std::string_view field, std::size_t index);
    std::string CalendarFocusTarget() const;
    std::vector<SystemCalendarInputField> CalendarInputFields() const;
    bool SetCalendarInput(std::string_view id, std::wstring text);
    bool CalendarBack();
    bool CalendarEventCommand(std::string_view nodeId, bool remove, bool wholeSeries=false);
    bool CalendarEventIsSeries(std::string_view nodeId) const;
    std::vector<SystemCalendarInputField> ControlInputFields() const;
    bool SetControlInput(std::string_view id, std::wstring& text);
    std::string ControlFocusTarget() const;
    bool ControlBack();
    void CancelControlInput();
    bool BeginPowerConfirmation(std::string_view task,bool dismissOnCancel=false);
    bool TakeDismissRequest();
private:
    SystemPanelSource source_;
    StatusBarSettings settings_;
    StatusBarAction action_;
    bool calendarStacked_=false;
    bool calendarDisplaySecondary_=false;
    native_ui::Scene scene_;
    std::string page_, interface_, network_, bluetooth_, gpu_, media_, date_, month_;
    std::string calendarAnnotationKey_;
    std::string calendarDetailsRevision_;
    std::map<std::string,std::string> calendarAnnotations_;
    std::map<std::string,calendar::DayAnnotation> calendarDetails_;
    std::map<std::string,calendar::CalendarEvent> calendarEvents_;
    std::string calendarNotice_;
    std::shared_ptr<SystemCalendarEditorState> calendarEditor_;
    std::optional<calendar::CalendarSeries> calendarSeriesOriginal_;
    calendar::CalendarSeriesRule calendarRule_;
    std::string calendarMode_="single";
    bool calendarScopeSeries_=false, calendarDiscardConfirmed_=false;
    std::map<std::string,std::wstring> calendarText_;
    enum class CalendarDeleteOrigin { Editor, ContextMenu };
    CalendarDeleteOrigin calendarDeleteOrigin_=CalendarDeleteOrigin::Editor;
    bool calendarConfirmDelete_=false;
    float calendarReturnScroll_=0;
    std::optional<native_ui::DateTimePicker> calendarPicker_;
    std::vector<std::string> calendarPickerDates_;
    std::string calendarPickerField_, calendarFocus_;
    float calendarEditorScroll_=0;
    std::map<std::string,std::function<void(std::optional<float>)>> actions_;
    system_control::ControlFeedback feedback_;
    struct PendingValue { std::uint64_t task = 0; float value = 0; std::string target; };
    std::map<std::string,PendingValue> pendingValues_;
    std::map<std::string,std::string> sliderTargets_;
    std::map<std::string,std::string> valueControls_;
    struct ActionBinding { std::string group, target, indicator, radioGroup; };
    struct ControlDraft
    {
        enum class ReturnRoute { PreviousPage, ClosePanel };
        system_control::Request request;
        std::string control, passwordId;
        std::optional<ActionBinding> binding;
        std::wstring ssid, error;
        std::uint64_t task=0;
        bool hidden=false;
        ReturnRoute returnRoute=ReturnRoute::PreviousPage;
    };
    std::optional<ControlDraft> controlDraft_;
    std::uint64_t controlSerial_=0;
    struct PendingAction
    {
        std::uint64_t task = 0;
        std::uint64_t navigation = 0;
        std::string control, target, topic, collection, device, radioGroup;
    };
    std::map<std::string,ActionBinding> actionBindings_;
    // Visual bindings are rebuilt per page; in-flight guards survive navigation.
    std::map<std::string,PendingAction> pendingActions_;
    std::string invokingControl_;
    std::uint64_t navigation_ = 0;
    std::optional<widget_runtime::WidgetMediaSessionsDataSnapshot> mediaState_;
    std::set<std::string> subscriptions_;
    std::uint64_t lastStarted_ = 0;
    std::wstring error_;
    std::string errorControl_, errorGroup_, errorTarget_;
    float available_ = 800, availableWidth_ = 960, scroll_ = 0, maxScroll_ = 0, bodyStart_ = 0, bodyLeft_ = 0;
    D2D1_RECT_F scrollViewport_{};
    bool closed_ = false, scan_ = false, dismissRequested_=false;
    JsonValue Current(const char*) const;
    JsonValue Wifi() const;
    void SyncSubscriptions();
    void Start(std::string, system_control::Arguments = {}, std::string_view control = {});
    void SubmitControl(system_control::Request, const std::string& control);
    void ConfirmControl();
    void ControlForm(float&, bool inCard=false);
    void BindAction(std::string control, std::string group, std::string target, std::string indicator = {}, std::string radioGroup = {});
    bool RadioTransitionPending(const ActionBinding&) const;
    void ClearError();
    void ApplyError(float& bodyEnd);
    void PrunePendingActions();
    void ApplyPendingActions();
    void PrepareMedia();
    void OpenSettings(const wchar_t*);
    native_ui::Node& Add(std::string, native_ui::Role, D2D1_RECT_F, std::wstring = {}, std::wstring = {});
    void Command(std::string, std::function<void()>);
    void Header(std::wstring);
    void Overview(float&);
    void UnavailableControl(std::string_view, std::wstring, std::wstring, std::wstring, float&);
    void Audio(float&);
    void Brightness(float&);
    void WifiPage(float&);
    void Bluetooth(float&);
    void Power(float&);
    void Media(float&);
    void Calendar();
    void FinishStackedCalendar(float monthEnd,float agendaTop,float agendaStart,float agendaEnd,bool empty);
    void EditCalendar(calendar::CalendarEvent);
    void SwitchCalendarScope(bool wholeSeries);
    float CalendarEditor(float width);
    void OpenCalendarPicker(std::string field);
    float CalendarPicker(float width);
    void CalendarPickerCommand(std::string_view id);
    void SaveCalendar();
    void RemoveCalendar();
    void LeaveCalendarEditor(bool followSavedDate);
    void Resources();
    void Tray();
    void InputMethod();
    struct TrayDropTarget { StatusBarSettings settings; D2D1_RECT_F indicator{}; };
    std::optional<TrayDropTarget> ResolveTrayDrop(std::string_view, D2D1_POINT_2F) const;
    void Radio(std::string_view, D2D1_RECT_F, bool compact);
    void Volume(std::string_view, float&);
    void Finish(float bodyEnd, bool withMedia, float minimumBodyHeight = 0, float maximumBodyHeight = 0);
};
}
