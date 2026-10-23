#include "app/app.h"
#include "system/panel/system_panel_model.h"
#include "system/calendar/system_calendar_editor.h"
#include "system/controls/system_controls.h"
#include "ui/menu/modern_menu.h"
#include "system/status_bar/status_bar_view.h"
#include "system/status_bar/status_bar_shell_shortcut.h"
#include "system/status_bar/status_bar_shell_shortcut_relay.h"
#include "system/status_bar/status_bar_task_view_input.h"
#include "dock/dock_settings_rules.h"
#include "platform/taskbar_monitor.h"
#include "taskbar_hook/taskbar_native.h"
#include <shellscalingapi.h>
#include <powrprof.h>

namespace
{
void TraceStatusBarShellActivation(snowdesktop::StatusBarAction action,
    std::uint64_t generation, const wchar_t* phase, double started,
    const wchar_t* result = L"-", double callMilliseconds = 0,
    UINT requested = 0, UINT sent = 0, DWORD hostUiThread = 0) noexcept try
{
    if (started < 0 || !snowdesktop::diagnostics::IsLogEnabled(DiagnosticLogModule::System)) return;
    wchar_t inputState[448]{};
    if (wcscmp(phase, L"queued") == 0 || wcscmp(phase, L"send-input") == 0 ||
        wcscmp(phase, L"finished") == 0)
    {
        // These are input-state snapshots only; do not synchronously query
        // foreign window text/classes or consume queue-status change bits.
        const DWORD uiThread = hostUiThread ? hostUiThread : GetCurrentThreadId();
        const HWND foreground = GetForegroundWindow();
        const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        GUITHREADINFO uiInfo{};
        uiInfo.cbSize = sizeof(uiInfo);
        const bool uiKnown = GetGUIThreadInfo(uiThread, &uiInfo) != FALSE;
        GUITHREADINFO foregroundInfo{};
        foregroundInfo.cbSize = sizeof(foregroundInfo);
        const bool foregroundKnown = foregroundThread == uiThread ?
            (foregroundInfo = uiInfo, uiKnown) :
            (foregroundThread && GetGUIThreadInfo(foregroundThread, &foregroundInfo) != FALSE);
        swprintf_s(inputState,
            L" foreground=%p foregroundThread=%lu uiThread=%lu uiKnown=%u uiFlags=0x%08lX uiActive=%p uiFocus=%p uiCapture=%p foregroundKnown=%u foregroundFlags=0x%08lX foregroundActive=%p foregroundFocus=%p foregroundCapture=%p",
            static_cast<void*>(foreground), foregroundThread, uiThread, static_cast<unsigned>(uiKnown),
            uiInfo.flags, static_cast<void*>(uiInfo.hwndActive), static_cast<void*>(uiInfo.hwndFocus),
            static_cast<void*>(uiInfo.hwndCapture), static_cast<unsigned>(foregroundKnown),
            foregroundInfo.flags, static_cast<void*>(foregroundInfo.hwndActive),
            static_cast<void*>(foregroundInfo.hwndFocus), static_cast<void*>(foregroundInfo.hwndCapture));
    }
    wchar_t message[768]{};
    swprintf_s(message,
        L"StatusBarShellActivation action=%u generation=%llu phase=%ls elapsedMs=%.3f result=%ls callMs=%.3f requested=%u sent=%u callThread=%lu%ls",
        static_cast<unsigned>(action), static_cast<unsigned long long>(generation), phase,
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - started,
        result, callMilliseconds, requested, sent, GetCurrentThreadId(), inputState);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, message);
}
catch (...) { /* Diagnostics must not change activation or release behavior. */ }
}

struct DesktopApp::StatusBarActivationHold
{
    std::function<void()> release;
    double shortcutStartedMilliseconds = -1;
    snowdesktop::StatusBarAction shortcutAction = snowdesktop::StatusBarAction::None;
    std::uint64_t shortcutGeneration = 0;
    bool shortcutFinished = false;
    ~StatusBarActivationHold()
    {
        if (release) release();
        if (!shortcutFinished)
            TraceStatusBarShellActivation(shortcutAction, shortcutGeneration,
                L"released", shortcutStartedMilliseconds, L"without-completion");
    }
};

snowdesktop::TrayDragFeedback DesktopApp::MakeStatusBarTrayDragFeedback()
{
    return {
        [this](HWND owner, const snowdesktop::tray::Icon& icon, POINT screen, UINT size) {
            return BeginTrayDragPreview(owner, icon, screen, size);
        },
        [this](std::string_view key, POINT screen) {
            const bool bar = statusBar_ && statusBar_->PreviewTrayDrop(key, screen);
            const bool panel = systemPanel_ && systemPanel_->PreviewTrayDrop(key, screen);
            UpdateTrayDragPreview(screen, bar || panel);
        },
        [this] {
            EndTrayDragPreview();
            if (statusBar_) statusBar_->PreviewTrayDrop({}, {});
            if (systemPanel_) systemPanel_->PreviewTrayDrop({}, {});
        }};
}

void DesktopApp::CancelStatusBarActivation(HMONITOR monitor, bool immediate)
{
    if (statusBar_) statusBar_->CancelTrayMenuSession(monitor);
    if (!monitor || monitor == statusBarActivationMonitor_)
    {
        ++*statusBarActivationGeneration_;
        uiAnimationScheduler_.Cancel(statusBarActivationToken_);
        statusBarActivationToken_ = 0;
        statusBarActivationMonitor_ = nullptr;
    }
    const HWND menu = snowdesktop::modern_menu::ActiveRootWindow();
    if ((!monitor || monitor == statusBarMenuMonitor_) && statusBarMenuOwner_ &&
        menu && GetWindow(menu, GW_OWNER) == statusBarMenuOwner_)
        snowdesktop::modern_menu::DismissActive();
    if ((!monitor || monitor == statusBarQuickNavigationMonitor_) &&
        quickNavigationInvocationSource_ == QuickNavigationInvocationSource::StatusBar)
        // Cancellation also runs from WA_INACTIVE. It must not turn into an
        // explicit dismissal and reactivate the desktop before that returns.
        CloseQuickNavigation(false);
    if (systemPanel_)
    {
        if (monitor) systemPanel_->HideForMonitor(monitor, !immediate);
        else systemPanel_->Hide();
    }
}

void DesktopApp::ActivateStatusBar(snowdesktop::StatusBarAction action, HWND owner, RECT anchor)
{
    using Action = snowdesktop::StatusBarAction;
    if (action == Action::Tray && !snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Tray)) return;
    if (action == Action::SystemControlCenter && !snowdesktop::StatusBarSupportsSystemQuickSettings())
        action = Action::ControlCenter;
    const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    if (!statusBarTaskViewTransition_.AllowsAction(action, now, systemTaskbarTaskViewActive_))
    {
        TraceStatusBarShellActivation(action, *statusBarActivationGeneration_, L"ignored",
            now, L"task-view-visible-or-transition");
        return;
    }
    uiAnimationScheduler_.Cancel(statusBarActivationToken_);
    statusBarActivationToken_ = 0;
    const auto generation = ++*statusBarActivationGeneration_;
    statusBarActivationMonitor_ = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    const double shortcutStarted = action == Action::Notifications || action == Action::SystemControlCenter ||
        action == Action::TaskView || action == Action::ShowDesktop || action == Action::SystemCalendar ||
        action == Action::InputMethod || action == Action::InputMethodMenu || action == Action::InputMethodPanel || action == Action::SystemInputMethodPanel
        ? snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() : -1;
    TraceStatusBarShellActivation(action, generation, L"queued", shortcutStarted);
    const HWND activeMenu = snowdesktop::modern_menu::ActiveRootWindow();
    if ((snowdesktop::IsStatusBarContextMenuAction(action) || action == Action::SystemMenu) &&
        action == statusBarMenuAction_ && statusBarMenuMonitor_ == statusBarActivationMonitor_ &&
        statusBarMenuOwner_ && activeMenu && GetWindow(activeMenu, GW_OWNER) == statusBarMenuOwner_)
        action = Action::Dismiss;
    // The no-activate bar does not cause WM_ACTIVATE on an open surface.
    // A blank-area click must therefore explicitly dismiss it, including any
    // replacement action still waiting for a nested menu loop to unwind.
    if (action == Action::Dismiss)
    {
        quickNavigationPostCloseAction_ = {};
        DismissActiveContextMenuForPopupTransition();
        if (systemPanel_) systemPanel_->Hide();
        CloseQuickNavigation();
        statusBarActivationMonitor_ = nullptr;
        return;
    }
    if (!statusBar_ || exitRequested_ || !generalSettings_.statusBar.enabled ||
        (action == Action::ShowDesktop && !generalSettings_.statusBar.showDesktop) || !owner ||
        !IsWindow(owner) || !IsWindowVisible(owner) ||
        !statusBar_->IsInteractionAvailable(statusBarActivationMonitor_))
    {
        TraceStatusBarShellActivation(action, generation, L"rejected", shortcutStarted, L"unavailable");
        statusBarActivationMonitor_ = nullptr;
        return;
    }
    const std::weak_ptr<std::uint64_t> lifetime = statusBarActivationGeneration_;
    auto hold = std::make_shared<StatusBarActivationHold>();
    hold->shortcutStartedMilliseconds = shortcutStarted;
    hold->shortcutAction = action;
    hold->shortcutGeneration = generation;
    hold->release = [this, lifetime, generation] {
        const auto state = lifetime.lock();
        if (state && *state == generation) statusBarActivationMonitor_ = nullptr;
    };
    if (!statusBar_->PostActivation(owner, [this, lifetime, generation, action, owner, anchor, hold] {
        const auto current = lifetime.lock();
        if (!current || *current != generation) return;
        ContinueStatusBarActivation(action, owner, anchor, generation, hold);
    })) statusBarActivationMonitor_ = nullptr;
}

void DesktopApp::ContinueStatusBarActivation(snowdesktop::StatusBarAction action, HWND owner,
    RECT anchor, std::uint64_t generation, std::shared_ptr<StatusBarActivationHold> hold)
{
    using Action = snowdesktop::StatusBarAction;
    const auto monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    const std::weak_ptr<std::uint64_t> lifetime = statusBarActivationGeneration_;
    // Queued callbacks share the hold, including callbacks discarded by an
    // outside click. Open surfaces subsequently own their interaction lifetime.
    const auto current = [this, lifetime, generation, action, owner, monitor] {
        const auto state = lifetime.lock();
        return state && *state == generation && !exitRequested_ &&
            generalSettings_.statusBar.enabled && (action != Action::ShowDesktop || generalSettings_.statusBar.showDesktop) &&
            statusBar_ && IsWindow(owner) &&
            IsWindowVisible(owner) && MonitorFromWindow(owner, MONITOR_DEFAULTTONULL) == monitor &&
            statusBar_->IsInteractionAvailable(monitor);
    };
    if (!current() || action == Action::None || !statusBarTaskViewTransition_.AllowsAction(action,
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(), systemTaskbarTaskViewActive_)) return;
    const auto ensureSystemPanel = [this] {
        if (!systemPanel_)
            systemPanel_ = std::make_unique<snowdesktop::SystemPanel>([this](const auto& changed) {
                if (!settingsController_) return;
                auto settings = settingsController_->Snapshot()->values.general;
                // The popup owns tray and quick-control preferences. Do not overwrite
                // a concurrent settings-page edit with its old snapshot.
                settings.statusBar.pinnedTrayItems = changed.pinnedTrayItems;
                settings.statusBar.trayOrder = changed.trayOrder;
                settings.statusBar.quickControlOrder = changed.quickControlOrder;
                settings.statusBar.hiddenQuickControls = changed.hiddenQuickControls;
                settingsController_->UpdateGeneral(std::move(settings), snowdesktop::SettingsUpdateMode::PreviewAndCommit);
                uiAnimationScheduler_.ScheduleOnce(0, [this](auto) {
                    if (settingsController_) (void)settingsController_->FlushPending();
                });
            }, snowdesktop::SystemCalendarActions{
                [this](const std::string& date) {
                    return widgetEngine_ ? widgetEngine_->RuntimeCalendarEvents(date, date) :
                        std::vector<snowdesktop::calendar::CalendarEvent>{};
                },
                [this] {
                    if (systemPanel_) systemPanel_->Hide();
                    ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Calendar, "calendar.events"));
                }, {}, [this](const std::string& date) {
                    if (!widgetEngine_) return std::string{};
                    const auto& days = widgetEngine_->RuntimeCalendarAnnotations(date, date);
                    return !days.empty() && days.front().calendarAvailable ? days.front().fullDate : std::string{};
                }, [this](const std::string& from, const std::string& to) {
                    std::map<std::string, std::string> result;
                    if (widgetEngine_)
                        for (const auto& day : widgetEngine_->RuntimeCalendarAnnotations(from, to))
                            if (day.calendarAvailable && !day.fullDate.empty())
                                result.emplace(day.date, day.fullDate);
                    return result;
                }, [this] {
                    const auto& display = generalSettings_.calendarDisplay;
                    return std::string(display.enabled ? "1:" : "0:") + display.calendar;
                }, {
                    [this](const snowdesktop::calendar::CalendarEvent& draft) {
                        if(exitRequested_||!widgetEngine_)return snowdesktop::calendar::MutationResult{false,{},0,"canceled"};
                        return draft.id.empty()?widgetEngine_->RuntimeCalendarCreate(draft):
                            widgetEngine_->RuntimeCalendarUpdate(draft.id,draft.revision,draft);
                    },
                    [this](const snowdesktop::calendar::CalendarEvent& original) -> std::optional<snowdesktop::calendar::CalendarEvent> {
                        if(exitRequested_||!widgetEngine_)return {};
                        for(const auto& current:widgetEngine_->RuntimeCalendarEvents(original.date,original.date))
                            if(current.id==original.id)return current;
                        return {};
                    },
                    [this](const std::string& id){if(exitRequested_||!widgetEngine_)return snowdesktop::calendar::MutationResult{false,{},0,"canceled"};return widgetEngine_->RuntimeCalendarRemove(id);},
                    [this](const std::string& id)->std::optional<snowdesktop::calendar::CalendarSeries>{
                        return exitRequested_||!widgetEngine_?std::nullopt:widgetEngine_->RuntimeCalendarSeriesById(id);
                    },
                    [this](snowdesktop::calendar::CalendarSeries series){
                        if(exitRequested_||!widgetEngine_)return snowdesktop::calendar::MutationResult{false,{},0,"canceled"};
                        const auto id=series.id;const auto revision=series.revision;
                        return series.id.empty()?widgetEngine_->RuntimeCalendarSeriesCreate(std::move(series)):
                            widgetEngine_->RuntimeCalendarSeriesUpdate(id,revision,std::move(series));
                    },
                    [this](const std::string& id,int revision){
                        if(exitRequested_||!widgetEngine_)return snowdesktop::calendar::MutationResult{false,id,0,"canceled"};
                        return widgetEngine_->RuntimeCalendarSeriesRemove(id,revision);
                    }
                }, [this](const std::string& from,const std::string& to) {
                    return widgetEngine_?widgetEngine_->RuntimeCalendarEvents(from,to):
                        std::vector<snowdesktop::calendar::CalendarEvent>{};
                }, [this](const std::string& from,const std::string& to) {
                    return widgetEngine_?widgetEngine_->RuntimeCalendarAnnotations(from,to):
                        std::vector<snowdesktop::calendar::DayAnnotation>{};
                }, [this] {
                    return generalSettings_.calendarDisplay.calendar;
                }}, [this](std::string_view key, POINT screen) {
                    return statusBar_ && statusBar_->DropTrayIcon(key, screen);
                }, &uiAnimationScheduler_, dcompDevice_.Get(), dwriteFactory_.Get(),
                [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
                    DrawWidgetPanelBackground(context, frame, appearance.cornerRadius * scale,
                        D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                        D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
                        false, appearance.widgetBorderWidth * scale, &appearance, false, 0, scale);
                    brushCache_.clear(); brushCacheContext_ = nullptr;
                });
        systemPanel_->SetCalendarMenuHandler([this](POINT screen, HWND owner, bool series) -> UINT {
            if (exitRequested_) return 0;
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            AppendMenuW(menu, MF_STRING, 1, _LW(series ? "settings.calendar.editOccurrence" : "settings.calendar.edit"));
            if (series) AppendMenuW(menu, MF_STRING, 3, _LW("settings.calendar.editSeries"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, _LW(series ? "settings.calendar.deleteOccurrence" : "app.settings.delete"));
            if (series) AppendMenuW(menu, MF_STRING, 4, _LW("settings.calendar.deleteSeries"));
            PrepareMenuIconsForPoint(screen);
            UINT command = 0;
            try { command = ShowModernMenu(menu, screen, owner, false, false, nullptr,
                {}, {}, {}, nullptr, {}, true); }
            catch (...) { DestroyMenu(menu); ClearMenuIcons(); throw; }
            DestroyMenu(menu);
            ClearMenuIcons();
            return command;
        });
        systemPanel_->SetCalendarChoiceMenuHandler([this](POINT screen, HWND owner,
            const std::vector<std::wstring>& labels, std::size_t selected) -> UINT {
            if (exitRequested_ || labels.empty()) return 0;
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            for (std::size_t index = 0; index < labels.size(); ++index)
                AppendMenuW(menu, MF_STRING | (index == selected ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(index + 1), labels[index].c_str());
            PrepareMenuIconsForPoint(screen);
            UINT command = 0;
            try { command = ShowModernMenu(menu, screen, owner, false, false, nullptr,
                {}, {}, {}, nullptr, {}, true); }
            catch (...) { DestroyMenu(menu); ClearMenuIcons(); throw; }
            DestroyMenu(menu);
            ClearMenuIcons();
            return command;
        });
        systemPanel_->SetTrayDragFeedback(MakeStatusBarTrayDragFeedback());
        systemPanel_->SetTrayStateChanged([this](HMONITOR monitor,bool expanded) {
            if(statusBar_)statusBar_->SetTrayExpanded(monitor,expanded);
        });
        if (snowdesktop::StatusBarSupportsSystemQuickSettings())
            systemPanel_->SetNativeControlsHandler([this](HWND source, RECT location) {
                ActivateStatusBar(Action::SystemControlCenter, source, location);
            });
        else systemPanel_->SetNativeControlsHandler({});
    };
    TraceStatusBarShellActivation(action, generation, L"continue", hold->shortcutStartedMilliseconds);
    const auto resume = [this, current, generation, action, owner, anchor, hold] {
        if (!current()) return;
        statusBar_->PostActivation(owner, [this, current, generation, action, owner, anchor, hold] {
            if (current()) ContinueStatusBarActivation(action, owner, anchor, generation, hold);
        });
    };
    // A bar click can arrive inside the menu's nested message loop. Wait until
    // menu focus restoration has finished before opening another surface.
    if (HasActiveContextMenuSession())
    {
        TraceStatusBarShellActivation(action, generation, L"wait-menu", hold->shortcutStartedMilliseconds);
        DismissActiveContextMenuForPopupTransition();
        statusBarActivationToken_ = uiAnimationScheduler_.ScheduleInterval(16, [this, lifetime, current, resume](auto token) {
            if (lifetime.expired()) return;
            if (!current())
            {
                uiAnimationScheduler_.Cancel(token);
                if (token == statusBarActivationToken_) statusBarActivationToken_ = 0;
                return;
            }
            if (token != statusBarActivationToken_ || HasActiveContextMenuSession()) return;
            uiAnimationScheduler_.Cancel(token);
            statusBarActivationToken_ = 0;
            resume();
        });
        return;
    }
    if (action != Action::QuickSearch && (quickNavigationOpen_ || !quickNavigationAnimation_.IsHidden()))
    {
        TraceStatusBarShellActivation(action, generation, L"wait-quick-navigation", hold->shortcutStartedMilliseconds);
        quickNavigationPostCloseAction_ = resume;
        if (quickNavigationOpen_) CloseQuickNavigation();
        return;
    }
    if (action == Action::InputMethodPanel && systemPanel_ && systemPanel_->IsInputMethodOpen())
    {
        hold->shortcutFinished = true;
        systemPanel_->Hide();
        return;
    }
    const bool externalSurface = action == Action::SystemMenu || snowdesktop::IsStatusBarContextMenuAction(action) ||
        action == Action::QuickSearch || action == Action::Settings ||
        action == Action::Notifications || action == Action::SystemControlCenter ||
        action == Action::TaskView || action == Action::ShowDesktop || action == Action::SystemCalendar ||
        action == Action::InputMethod || action == Action::InputMethodMenu || action == Action::InputMethodPanel || action == Action::SystemInputMethodPanel;
    if (externalSurface && systemPanel_ && systemPanel_->IsOpen())
    {
        TraceStatusBarShellActivation(action, generation, L"wait-system-panel", hold->shortcutStartedMilliseconds);
        systemPanel_->CloseThen(resume, owner);
        return;
    }
    auto chord = snowdesktop::ResolveStatusBarShellChord(action,
        snowdesktop::StatusBarSupportsSystemQuickSettings(), IsClassicSystemTaskbar());
    if (action == Action::InputMethodPanel)
    {
        ensureSystemPanel();
        namespace input = snowdesktop::status_bar_input_method;
        auto capture = input::WindowsTargetCallbacks();
        capture.current = [this, current](auto token) {
            return current() && token == statusBarActivationToken_;
        };
        capture.finished = [this, current, owner, anchor, monitor, generation, hold](auto token, HRESULT result, input::Snapshot target) {
            if (!current() || token != statusBarActivationToken_) return;
            statusBarActivationToken_ = 0;
            // Panel opening/native menus can pump messages. Dispatch completion
            // through the bar HWND, outside the scheduler's active snapshot.
            statusBar_->PostActivation(owner, [this, current, owner, anchor, monitor, generation, hold, result, target] {
                if (!current()) return;
                hold->shortcutFinished = true;
                if (result != S_OK)
                {
                    MessageBoxW(hwnd_, _LW("statusBar.inputMethodFailed"), _LW("statusBar.inputMethod"), MB_OK | MB_ICONINFORMATION);
                    return;
                }
                const auto selection = input::CaptureSelection(target);
                const auto restore = [this, current, owner, monitor, hold](const input::Snapshot& original,
                    std::function<void(input::Snapshot)> next) {
                    if (!current()) return;
                    statusBarActivationMonitor_ = monitor;
                    auto callbacks = input::WindowsTargetCallbacks();
                    callbacks.current = [this, current](auto restoreToken) {
                        return current() && restoreToken == statusBarActivationToken_;
                    };
                    callbacks.finished = [this, current, owner, hold, next = std::move(next)](auto restoreToken,
                        HRESULT restored, input::Snapshot resolved) {
                        if (!current() || restoreToken != statusBarActivationToken_) return;
                        statusBarActivationToken_ = 0;
                        statusBar_->PostActivation(owner, [this, current, hold, next, restored, resolved] {
                            if (!current()) return;
                            if (restored == S_OK) next(resolved);
                            else if (restored != HRESULT_FROM_WIN32(ERROR_CANCELLED))
                                MessageBoxW(hwnd_, _LW("statusBar.inputMethodFailed"), _LW("statusBar.inputMethod"), MB_OK | MB_ICONINFORMATION);
                        });
                    };
                    statusBarActivationToken_ = input::ScheduleTarget(uiAnimationScheduler_, original, std::move(callbacks));
                    if (!statusBarActivationToken_)
                    {
                        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, L"StatusBar input method stage=restore-scheduler-unavailable", DiagnosticLogLevel::Error);
                        MessageBoxW(hwnd_, _LW("statusBar.inputMethodFailed"), _LW("statusBar.inputMethod"), MB_OK | MB_ICONINFORMATION);
                    }
                };
                snowdesktop::SystemPanelInputMethodActions actions;
                actions.choices = selection.choices;
                actions.menuAvailable = std::any_of(selection.choices.begin(), selection.choices.end(), [](const auto& choice) {
                    return choice.selected && choice.profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR;
                }) || (selection.target.layout && ImmIsIME(selection.target.layout));
                actions.select = [this, current, selection, restore, monitor](const input::Choice& choice) {
                    if (!current() || !systemPanel_) return;
                    statusBarActivationMonitor_ = monitor;
                    systemPanel_->CloseThen([this, current, selection, choice, restore] {
                        if (!current()) return;
                        restore(selection.target, [this, selection, choice](input::Snapshot resolved) {
                            auto ready = selection;
                            ready.target = resolved;
                            const HRESULT selected = input::Select(ready, choice);
                            wchar_t message[128]{};
                            swprintf_s(message, L"StatusBar input method profile selection hr=0x%08lX", static_cast<unsigned long>(selected));
                            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, message, selected != S_OK ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Info);
                            if (selected != S_OK)
                                MessageBoxW(hwnd_, _LW("statusBar.inputMethodFailed"), _LW("statusBar.inputMethod"), MB_OK | MB_ICONINFORMATION);
                        });
                    }, selection.target.foreground);
                };
                actions.menu = [this, current, selection, anchor, restore, monitor] {
                    if (!current() || !systemPanel_) return;
                    statusBarActivationMonitor_ = monitor;
                    systemPanel_->CloseThen([this, current, selection, anchor, restore] {
                        if (!current()) return;
                        restore(selection.target, [this, anchor](input::Snapshot) {
                            const HRESULT menuResult = statusBar_->ShowInputMethod(anchor, true);
                            wchar_t message[128]{};
                            swprintf_s(message, L"StatusBar input method panel menu hr=0x%08lX", static_cast<unsigned long>(menuResult));
                            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, message, FAILED(menuResult) ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Info);
                        });
                    }, selection.target.foreground);
                };
                actions.settings = [this, current, owner](const wchar_t* uri) {
                    if (!current() || !systemPanel_) return;
                    const std::wstring settingsUri(uri);
                    systemPanel_->CloseThen([this, current, owner, settingsUri] {
                        if (current()) ShellExecuteW(owner, L"open", settingsUri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }, owner);
                };
                systemPanel_->ShowInputMethod(std::move(actions), owner, anchor,
                    collectionPopupAppearance_, generalSettings_.statusBar, systemDataProvider_);
                TraceStatusBarShellActivation(snowdesktop::StatusBarAction::InputMethodPanel, generation,
                    L"finished", hold->shortcutStartedMilliseconds, L"panel-requested");
                statusBarActivationMonitor_ = nullptr;
            });
        };
        statusBarActivationToken_ = input::ScheduleTarget(uiAnimationScheduler_, std::nullopt, std::move(capture));
        if (!statusBarActivationToken_)
        {
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, L"StatusBar input method stage=capture-scheduler-unavailable", DiagnosticLogLevel::Error);
            MessageBoxW(hwnd_, _LW("statusBar.inputMethodFailed"), _LW("statusBar.inputMethod"), MB_OK | MB_ICONINFORMATION);
        }
    }
    else if (action == Action::InputMethod || action == Action::InputMethodMenu || action == Action::SystemInputMethodPanel)
    {
        const HRESULT result = statusBar_->ShowInputMethod(anchor, action == Action::InputMethodMenu,
            action == Action::SystemInputMethodPanel);
        wchar_t message[192]{};
        swprintf_s(message, L"StatusBar input method %ls hr=0x%08lX anchor=%ld,%ld,%ld,%ld",
            action == Action::SystemInputMethodPanel ? L"layout-panel" : action == Action::InputMethodMenu ? L"context" : L"toggle", static_cast<unsigned long>(result),
            anchor.left, anchor.top, anchor.right, anchor.bottom);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, message, FAILED(result) ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Info);
        hold->shortcutFinished = true;
        TraceStatusBarShellActivation(action, generation, L"finished", hold->shortcutStartedMilliseconds,
            SUCCEEDED(result) ? L"requested" : L"failed");
        if (FAILED(result))
        {
            MessageBoxW(owner, _LW("statusBar.inputMethodFailed"), _LW("statusBar.inputMethod"), MB_OK | MB_ICONINFORMATION);
        }
        statusBarActivationMonitor_ = nullptr;
    }
    else if (chord.key)
    {
        if (systemPanel_) systemPanel_->Hide();
        CloseQuickNavigation();
        const HWND foreground = GetForegroundWindow();
        const auto mouseHandoff = action == Action::TaskView ?
            snowdesktop::StatusBarTaskViewInputHandoff::Create(uiAnimationScheduler_, statusBar_->InputBounds(),
                [this, lifetime] {
                    return lifetime.expired() ? 0.0 : statusBarTaskViewTransition_.Remaining(
                        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds());
                }) : nullptr;
        if (action == Action::TaskView && !mouseHandoff)
        {
            // A delay cannot protect against a click after injection. Refuse
            // this opening if its raw-input handoff cannot be established.
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, L"StatusBar Task View input handoff unavailable; opening cancelled", DiagnosticLogLevel::Warning);
            hold->shortcutFinished = true;
            MessageBeep(MB_ICONWARNING);
            return;
        }
        using Relay = snowdesktop::StatusBarShellShortcutRelay;
        struct ShortcutRequest
        {
            std::shared_ptr<Relay> relay;
            double taskViewSendDeadline = 0;
            bool taskViewStarted = false;
        };
        const auto request = std::make_shared<ShortcutRequest>();
        const double preparationDeadline = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() + 5000;
        // The relay owns a separate foreground/focus window. Calling SendInput
        // on a worker while the desktop proxy kept focus would not isolate the
        // input destination. Shell still chooses where its own surface opens.
        statusBarActivationToken_ = uiAnimationScheduler_.ScheduleInterval(16,
            [this, lifetime, current, foreground, action, generation, chord, hold,
                mouseHandoff, request, preparationDeadline](auto token) {
                const auto state = lifetime.lock();
                if (!state) return;
                if (*state != generation || token != statusBarActivationToken_)
                {
                    if (request->relay) request->relay->Cancel();
                    uiAnimationScheduler_.Cancel(token);
                    return;
                }
                const auto finish = [&](Relay::Result result) {
                    const UINT accepted = request->relay ? request->relay->AcceptedCount() : 0;
                    // Pending cancellation can race a committed SendInput.
                    // Only a terminal result with no accepted input permits
                    // removing Task View's independent transition protection.
                    if (request->taskViewStarted && accepted == 0)
                    {
                        statusBarTaskViewTransition_.Reset();
                        if (mouseHandoff) mouseHandoff->Cancel();
                    }
                    if (mouseHandoff) mouseHandoff->ReleaseIfUnused();
                    hold->shortcutFinished = true;
                    const wchar_t* outcome = result == Relay::Result::Sent ? L"input-accepted" :
                        result == Relay::Result::Cancelled ? L"canceled" :
                        result == Relay::Result::TimedOut ? L"timed-out" : L"failed";
                    TraceStatusBarShellActivation(action, generation, L"finished",
                        hold->shortcutStartedMilliseconds, outcome, 0, chord.alt ? 6u : 4u, accepted);
                    uiAnimationScheduler_.Cancel(token);
                    if (*state != generation || token != statusBarActivationToken_) return;
                    statusBarActivationToken_ = 0;
                    statusBarActivationMonitor_ = nullptr;
                    if (result == Relay::Result::Failed || result == Relay::Result::TimedOut)
                    {
                        // Read the worker's published terminal snapshot. Its
                        // window may already be gone, so querying it here
                        // would lose the failed activation/focus evidence.
                        const auto diagnostic = request->relay ?
                            request->relay->Diagnostics() : Relay::DiagnosticSnapshot{};
                        const wchar_t* reason = request->relay ? Relay::ReasonName(diagnostic.reason) :
                            result == Relay::Result::TimedOut ? L"ui-input-release-timeout" : L"start-unavailable";
                        wchar_t message[768]{};
                        swprintf_s(message,
                            L"StatusBar system shortcut relay failure: action=%u generation=%llu reason=%s "
                            L"initialForeground=%p observedForeground=%p relayWindow=%p relayThread=%lu "
                            L"activationAttempted=%u activationReturned=%u focusChecked=%u focused=%u accepted=%u",
                            static_cast<unsigned>(action), static_cast<unsigned long long>(generation), reason,
                            static_cast<void*>(foreground), static_cast<void*>(diagnostic.observedForeground),
                            static_cast<void*>(diagnostic.relayWindow),
                            request->relay ? request->relay->ThreadId() : 0ul,
                            diagnostic.activationAttempted ? 1u : 0u, diagnostic.activationReturned ? 1u : 0u,
                            diagnostic.focusChecked ? 1u : 0u, diagnostic.focused ? 1u : 0u, accepted);
                        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, message, DiagnosticLogLevel::Warning);
                        MessageBeep(MB_ICONWARNING);
                    }
                };
                const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                if (request->relay)
                {
                    const auto result = request->relay->Poll();
                    if (result != Relay::Result::Pending) { finish(result); return; }
                    if (!current() || (request->taskViewStarted && now >= request->taskViewSendDeadline))
                        request->relay->Cancel();
                    // No join or synchronous worker request on the UI thread.
                    // Cancellation before injection publishes a terminal result;
                    // a committed injection retains its own handoff lifetime.
                    return;
                }
                if (!current() || GetForegroundWindow() != foreground ||
                    (action == Action::TaskView &&
                        !statusBarTaskViewTransition_.CanBegin(now, systemTaskbarTaskViewActive_)))
                { finish(Relay::Result::Cancelled); return; }
                if (now >= preparationDeadline) { finish(Relay::Result::TimedOut); return; }
                // Every system chord changes the foreground queue. Finish the
                // physical click before handing it to the temporary window.
                for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2,
                    VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN, static_cast<int>(chord.key)})
                    if (GetAsyncKeyState(key) & 0x8000) return;
                if (action == Action::TaskView)
                {
                    if (!statusBarTaskViewTransition_.Begin(now, systemTaskbarTaskViewActive_))
                    { finish(Relay::Result::Cancelled); return; }
                    request->taskViewStarted = true;
                    const double remaining = statusBarTaskViewTransition_.Remaining(now);
                    request->taskViewSendDeadline = now + remaining;
                    mouseHandoff->Begin(remaining);
                }
                Relay::Api api;
                // This callback runs only on the relay thread and retains no
                // DesktopApp, HWND owner, scheduler or Task View guard object.
                api.send = [action, generation, hostUiThread = GetCurrentThreadId(),
                    started = hold->shortcutStartedMilliseconds,
                    sendDeadline = request->taskViewSendDeadline](UINT count, INPUT* input, int size) {
                    TraceStatusBarShellActivation(action, generation, L"send-input-begin", started);
                    const double before = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                    const bool initialChord = count && !(input[0].ki.dwFlags & KEYEVENTF_KEYUP);
                    if (initialChord && sendDeadline > 0 && before >= sendDeadline)
                    {
                        TraceStatusBarShellActivation(action, generation, L"send-input", started,
                            L"handoff-expired", 0, count, 0, hostUiThread);
                        return 0u;
                    }
                    const UINT accepted = SendInput(count, input, size);
                    TraceStatusBarShellActivation(action, generation, L"send-input", started,
                        accepted == count ? L"accepted" : L"incomplete",
                        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - before, count, accepted,
                        hostUiThread);
                    return accepted;
                };
                request->relay = Relay::Start(foreground, chord, std::move(api));
                if (!request->relay) { finish(Relay::Result::Failed); return; }
                TraceStatusBarShellActivation(action, generation, L"relay-start",
                    hold->shortcutStartedMilliseconds);
            });
        if (!statusBarActivationToken_)
        {
            if (mouseHandoff) mouseHandoff->Cancel();
            hold->shortcutFinished = true;
            statusBarActivationMonitor_ = nullptr;
            TraceStatusBarShellActivation(action, generation, L"finished", hold->shortcutStartedMilliseconds,
                L"scheduler-unavailable");
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, L"StatusBar system shortcut relay scheduler unavailable", DiagnosticLogLevel::Error);
            MessageBeep(MB_ICONWARNING);
        }
    }
    else if (action == Action::SystemMenu)
    {
        CloseQuickNavigation();
        if (systemPanel_) systemPanel_->Hide();
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        PrepareMenuIconsForPoint({anchor.left,anchor.bottom});
        const char* labels[]{"statusBar.taskManager", "statusBar.terminal", "statusBar.systemSettings",
            "controlCenter.lock", "controlCenter.sleep", "controlCenter.hibernate", "controlCenter.restart", "controlCenter.shutdown"};
        // Fluent Regular 20 glyphs from the same embedded, pinned font as Dock.
        const wchar_t* icons[]{L"\uE49D",L"\uEE6F",L"\uF6A9",L"\uE78F",L"\uEB2D",L"\uEB2D",L"\uF13D",L"\uF60E"};
        for (UINT index = 0; index < std::size(labels); ++index)
        {
            if (index == 3) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING|(index==5&&!IsPwrHibernateAllowed()?MF_GRAYED:0), index + 1, _LW(labels[index]));
            SetMenuItemIcon(menu,index+1,icons[index],MenuIconFont::FluentRegular);
        }
        statusBarMenuMonitor_ = monitor;
        statusBarMenuOwner_ = owner;
        statusBarMenuAction_ = action;
        const UINT command = ShowModernMenu(menu, {anchor.left, anchor.bottom}, owner);
        statusBarMenuMonitor_ = nullptr;
        statusBarMenuOwner_ = nullptr;
        statusBarMenuAction_ = Action::None;
        DestroyMenu(menu);
        ClearMenuIcons();
        if (!current()) return;
        if (command >= 1 && command <= 3)
        {
            const wchar_t* target = command == 1 ? L"taskmgr.exe" : command == 2 ? L"wt.exe" : L"ms-settings:";
            auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", target, nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32 && command == 2)
                opened = reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", L"powershell.exe", nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32) MessageBeep(MB_ICONWARNING);
        }
        else if (command >= 4 && command <= 8)
        {
            const char* tasks[]{"system.power.lock", "system.power.sleep", "host.power.hibernate", "system.power.restart", "system.power.shutdown"};
            if (command >= 5)
            {
                ensureSystemPanel();
                systemPanel_->ShowPowerConfirmation(tasks[command - 4], owner, anchor,
                    collectionPopupAppearance_, generalSettings_.statusBar, systemDataProvider_);
            }
            else
            {
                snowdesktop::system_control::Request request;
                request.name = tasks[command - 4];
                if (!systemDataProvider_->Controls()->Start("statusBarVolume", std::move(request))) MessageBeep(MB_ICONWARNING);
            }
        }
    }
    else if (snowdesktop::IsStatusBarContextMenuAction(action))
    {
        if (systemPanel_) systemPanel_->Hide();
        PrepareMenuIconsForPoint({anchor.left,anchor.bottom});
        const bool merged = statusBar_ && statusBar_->MergedStripBounds(monitor).has_value();
        HMENU menu = merged ? CreateDockContextMenu(true) : CreatePopupMenu();
        if (!menu) { ClearMenuIcons(); return; }
        if (!merged)
        {
            AppendMenuW(menu, MF_STRING, 1, _LW("statusBar.menu.settings"));
            AppendMenuW(menu, MF_STRING, 2, _LW("statusBar.taskManager"));
            SetMenuItemIcon(menu,1,L"\uF6A9",MenuIconFont::FluentRegular);
            SetMenuItemIcon(menu,2,L"\uE49D",MenuIconFont::FluentRegular);
        }
        // Use the clicked item's identity for direct quick settings. All changes
        // go through the same controller as the settings window, after dismissal.
        using Settings = snowdesktop::StatusBarSettings;
        struct Flag { const char* key; bool Settings::* member; Action context; const wchar_t* glyph; };
        // Official Fluent Regular 20 glyphs from the embedded font's pinned upstream commit.
        const Flag flags[]{
            {"statusBar.menu", &Settings::menu, Action::MenuSystem, L"\uF133"}, // apps
            {"statusBar.quickSearch", &Settings::quickSearch, Action::MenuQuickSearch, L"\uF68F"}, // search
            {"statusBar.taskView", &Settings::taskView, Action::MenuTaskView, L"\uE05D"}, // app_recent
            {"statusBar.cpu", &Settings::cpu, Action::MenuCpu, L"\uE4AC"}, // developer_board
            {"statusBar.memory", &Settings::memory, Action::MenuMemory, L"\uE9F3"}, // ram
            {"statusBar.gpu", &Settings::gpu, Action::MenuGpu, L"\uF359"}, // desktop
            {"statusBar.traffic", &Settings::traffic, Action::MenuTraffic, L"\uE45F"}, // data_usage
            {"statusBar.clock", &Settings::clock, Action::MenuTime, L"\uE24E"}, // calendar_ltr
            {"statusBar.clockShowSeconds", &Settings::clockShowSeconds, Action::MenuTime, L"\uF2DD"}, // clock
            {"statusBar.notifications", &Settings::notifications, Action::MenuNotifications, L"\uF114"}, // alert
            {"statusBar.inputMethod", &Settings::inputMethod, Action::MenuInputMethod, L"\uF4B8"}, // keyboard
            {"statusBar.batteryPercentage", &Settings::batteryPercentage, Action::MenuControlCenter, L"\uF1CD"}, // battery_9_20
            {"statusBar.audioControls", &Settings::audioControls, Action::MenuControlCenter, L"\uEB42"}, // speaker_2
            {"statusBar.brightnessControls", &Settings::brightnessControls, Action::MenuControlCenter, L"\uF8A1"}, // weather_sunny
            {"statusBar.wifiControls", &Settings::wifiControls, Action::MenuControlCenter, L"\uF8AC"}, // wifi_1
            {"statusBar.bluetoothControls", &Settings::bluetoothControls, Action::MenuControlCenter, L"\uF1DE"}, // bluetooth
            {"statusBar.mediaControls", &Settings::mediaControls, Action::MenuControlCenter, L"\uF605"}, // play
            {"statusBar.powerControls", &Settings::powerControls, Action::MenuControlCenter, L"\uF60E"}}; // power
        constexpr UINT positionBase = 42000, monitorBase = 42010, scaleBase = 42020;
        constexpr UINT flagBase = 42100, clockBase = 42200, timePositionBase = 42210, controlBase = 42220, inputPanelBase = 42230,
            controlDisplayBase = 42240;
        const Settings currentSettings = generalSettings_.statusBar;
        const auto addChoice = [&](HMENU target, UINT command, const wchar_t* label, bool selected, bool disabled = false) {
            std::wstring text = label;
            if (selected) { text += L"\t"; text += _LW("statusBar.menu.current"); }
            AppendMenuW(target, MF_STRING | (disabled ? MF_GRAYED : 0), command, text.c_str());
            const wchar_t* glyph = L"\uF587"; // options_20
            if (command == positionBase) glyph = L"\uF19B"; // arrow_up_20
            else if (command == positionBase + 1) glyph = L"\uF148"; // arrow_down_20
            else if (command >= monitorBase && command < monitorBase + 3)
                glyph = command == monitorBase + 2 ? L"\uEB96" : L"\uF359"; // square_multiple / desktop_20
            else if (command >= scaleBase && command < scaleBase + 6) glyph = L"\uF8C4"; // zoom_in_20
            else if (command == clockBase || command == clockBase + 1) glyph = L"\uE24E"; // calendar_ltr_20
            else if (command == timePositionBase) glyph = L"\uE025"; // align_center_horizontal_20
            else if (command == timePositionBase + 1) glyph = L"\uE039"; // align_right_20
            SetMenuItemIcon(target, command, glyph, MenuIconFont::FluentRegular);
        };
        const auto addFlag = [&](HMENU target, std::size_t index) {
            const auto command = flagBase + static_cast<UINT>(index);
            std::wstring label = _LW(flags[index].key);
            label += L"\t";
            label += _LW(currentSettings.*(flags[index].member) ? "app.interact.shown" : "app.interact.hidden");
            AppendMenuW(target, MF_STRING, command, label.c_str());
            SetMenuItemIcon(target, command, flags[index].glyph, MenuIconFont::FluentRegular);
        };
        const auto submenu = [&](HMENU parent, const char* label) {
            const auto child = CreatePopupMenu();
            if (!child) return static_cast<HMENU>(nullptr);
            if (!AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(child), _LW(label)))
            { DestroyMenu(child); return static_cast<HMENU>(nullptr); }
            const std::string_view key(label);
            const wchar_t* glyph = L"\uF587"; // options_20
            if (key == "statusBar.position" || key == "statusBar.timeNotificationPosition")
                glyph = L"\uF8E5"; // arrow_move_20
            else if (key == "statusBar.leftItems") glyph = L"\uF133"; // apps_20
            else if (key == "statusBar.information") glyph = L"\uE45F"; // data_usage_20
            else if (key == "statusBar.timeAndNotificationArea") glyph = L"\uF2DD"; // clock_20
            else if (key == "statusBar.clockPanel") glyph = L"\uE24E"; // calendar_ltr_20
            else if (key == "settings.dock.monitor") glyph = L"\uF359"; // desktop_20
            else if (key == "statusBar.scale") glyph = L"\uF8C4"; // zoom_in_20
            SetMenuItemIcon(parent, reinterpret_cast<UINT_PTR>(child), glyph, MenuIconFont::FluentRegular);
            return child;
        };
        const auto addTimePositionChoices = [&](HMENU target) {
            if (const auto position = submenu(target, "statusBar.timeNotificationPosition"))
            {
                addChoice(position, timePositionBase, _LW("app.settings.taskbar_center"), !currentSettings.timeNotificationRightAligned);
                addChoice(position, timePositionBase + 1, _LW("app.dock.right"), currentSettings.timeNotificationRightAligned);
            }
        };
        const auto addTimeChoices = [&](HMENU target, HMENU independentTarget = nullptr) {
            if (const auto panel = submenu(target, "statusBar.clockPanel"))
            {
                addChoice(panel, clockBase, _LW("statusBar.panelCustom"), !currentSettings.clockSystemPanel);
                addChoice(panel, clockBase + 1, _LW(snowdesktop::StatusBarSupportsSystemQuickSettings() ?
                    "statusBar.panelSystemCalendar" : "statusBar.panelSystemDateTime"), currentSettings.clockSystemPanel);
            }
            if (!merged || independentTarget) addTimePositionChoices(independentTarget ? independentTarget : target);
        };
        const auto addInputChoices = [&](HMENU target) {
            if (const auto panel = submenu(target, "statusBar.inputMethodPanel"))
            {
                addChoice(panel, inputPanelBase, _LW("statusBar.panelCustom"), !currentSettings.inputMethodSystemPanel);
                addChoice(panel, inputPanelBase + 1, _LW("statusBar.panelSystemInput"), currentSettings.inputMethodSystemPanel);
            }
        };
        const auto addControlChoices = [&](HMENU target) {
            if (const auto display = submenu(target, "statusBar.controlCenterDisplay"))
            {
                const int selected = !currentSettings.controlCenter ? 2 : currentSettings.controlCenterSingleIcon ? 1 : 0;
                const char* labels[]{"app.interact.shown", "statusBar.controlCenterSingleIcon", "app.interact.hidden"};
                for (UINT index = 0; index < std::size(labels); ++index)
                    addChoice(display, controlDisplayBase + index, _LW(labels[index]), static_cast<UINT>(selected) == index);
            }
            if (snowdesktop::StatusBarSupportsSystemQuickSettings())
                if (const auto panel = submenu(target, "statusBar.controlCenterPanel"))
                {
                    addChoice(panel, controlBase, _LW("statusBar.panelCustom"), !currentSettings.controlCenterSystemPanel);
                    addChoice(panel, controlBase + 1, _LW("statusBar.panelSystemControls"), currentSettings.controlCenterSystemPanel);
                }
        };
        if (action != Action::Menu)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            // Keep quick settings scoped to the clicked control. The general
            // bar settings remain available through the settings submenu below.
            if (action == Action::MenuControlCenter) addControlChoices(menu);
            for (std::size_t index = 0; index < std::size(flags); ++index)
                if (flags[index].context == action && index < 12) addFlag(menu, index);
            if (action == Action::MenuTime) addTimeChoices(menu);
            if (action == Action::MenuNotifications && !merged) addTimePositionChoices(menu);
            if (action == Action::MenuInputMethod) addInputChoices(menu);
            if (action == Action::MenuControlCenter)
                if (const auto contents = submenu(menu, "statusBar.controlCenterContent"))
                    for (std::size_t index = 12; index < std::size(flags); ++index) addFlag(contents, index);
        }
        HMENU settingsMenu = nullptr;
        HMENU independentDock = nullptr;
        if (merged)
        {
            // The merged root has one settings popup. Reuse it, keeping active
            // settings on this level and editable standalone preferences below.
            for (int index = 0; index < GetMenuItemCount(menu) && !settingsMenu; ++index)
                settingsMenu = GetSubMenu(menu, index);
            if (settingsMenu)
            {
                const int last = GetMenuItemCount(settingsMenu) - 1;
                independentDock = GetSubMenu(settingsMenu, last);
                if (independentDock && !RemoveMenu(settingsMenu, static_cast<UINT>(last), MF_BYPOSITION))
                    independentDock = nullptr;
            }
        }
        else
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            settingsMenu = submenu(menu, "settings.nav.statusBar");
        }
        if (settingsMenu)
        {
            const HMENU positionAndAppearance = merged ? GetSubMenu(settingsMenu, 0) : settingsMenu;
            if (const auto edge = positionAndAppearance ? submenu(positionAndAppearance, "statusBar.position") : nullptr)
            {
                addChoice(edge, positionBase, _LW("app.dock.top"), currentSettings.position == DockPosition::Top);
                addChoice(edge, positionBase + 1, _LW("app.dock.bottom"), currentSettings.position == DockPosition::Bottom);
            }
            if (const auto scope = positionAndAppearance ? submenu(positionAndAppearance, "settings.dock.monitor") : nullptr)
            {
                const char* labels[]{"app.dock.first_screen", "app.dock.last_screen", "app.dock.all_screens"};
                for (UINT index = 0; index < std::size(labels); ++index)
                    addChoice(scope, monitorBase + index, _LW(labels[index]), static_cast<UINT>(currentSettings.monitorScope) == index);
            }
            HMENU independentStatus = merged ? CreatePopupMenu() : settingsMenu;
            if (const auto scale = independentStatus ? submenu(independentStatus, "statusBar.scale") : nullptr)
                for (UINT index = 0; index < 6; ++index)
                {
                    constexpr int percent[]{75, 100, 125, 150, 200, 300};
                    const auto label = std::to_wstring(percent[index]) + L"%";
                    addChoice(scale, scaleBase + index, label.c_str(), std::abs(currentSettings.scale * 100 - percent[index]) < .5f);
                }
            if (!merged) AppendMenuW(settingsMenu, MF_SEPARATOR, 0, nullptr);
            const char* labels[]{"statusBar.barItems", "statusBar.controlCenterContent"};
            const std::size_t boundaries[]{0, 12, std::size(flags)};
            for (std::size_t group = 0; group < std::size(labels); ++group)
                if (const auto section = submenu(settingsMenu, labels[group]))
                {
                    for (auto index = boundaries[group]; index < boundaries[group + 1]; ++index)
                    {
                        addFlag(section, index);
                    }
                    if (group == 0)
                    {
                        addTimeChoices(section, merged ? independentStatus : nullptr);
                        addInputChoices(section);
                        addControlChoices(section);
                    }
                }
            if (merged)
            {
                AppendMenuW(settingsMenu, MF_SEPARATOR, 0, nullptr);
                if (independentDock)
                {
                    if (!AppendMenuW(settingsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(independentDock), _LW("app.settings.dock_bar")))
                        DestroyMenu(independentDock);
                    else SetMenuItemIcon(settingsMenu, reinterpret_cast<UINT_PTR>(independentDock), L"\uF133", MenuIconFont::FluentRegular);
                }
                if (independentStatus)
                {
                    if (!AppendMenuW(settingsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(independentStatus), _LW("settings.nav.statusBar")))
                        DestroyMenu(independentStatus);
                    else SetMenuItemIcon(settingsMenu, reinterpret_cast<UINT_PTR>(independentStatus), L"\uF587", MenuIconFont::FluentRegular);
                }
            }
            AppendMenuW(settingsMenu, MF_SEPARATOR, 0, nullptr);
            const UINT detailedCommand = merged ? 41869 : 1;
            AppendMenuW(settingsMenu, MF_STRING, detailedCommand, _LW("statusBar.menu.detailed"));
            SetMenuItemIcon(settingsMenu, detailedCommand, L"\uF6A9", MenuIconFont::FluentRegular);
        }
        statusBarMenuMonitor_ = monitor;
        statusBarMenuOwner_ = owner;
        statusBarMenuAction_ = action;
        const UINT command = ShowModernMenu(menu, {anchor.left, anchor.bottom}, owner);
        statusBarMenuMonitor_ = nullptr;
        statusBarMenuOwner_ = nullptr;
        statusBarMenuAction_ = Action::None;
        DestroyMenu(menu);
        ClearMenuIcons();
        if (!current()) return;
        if (settingsController_)
        {
            auto settings = settingsController_->Snapshot()->values.general;
            auto& bar = settings.statusBar;
            bool changed = true;
            if (command >= flagBase && command < flagBase + std::size(flags))
            {
                const auto member = flags[command - flagBase].member;
                bar.*member = !(bar.*member);
            }
            else if (command == positionBase || command == positionBase + 1)
                bar.position = command == positionBase ? DockPosition::Top : DockPosition::Bottom;
            else if (command >= monitorBase && command < monitorBase + 3)
                bar.monitorScope = static_cast<DockMonitorScope>(command - monitorBase);
            else if (command >= scaleBase && command < scaleBase + 6)
            {
                constexpr float scales[]{.75f, 1.f, 1.25f, 1.5f, 2.f, 3.f};
                bar.scale = scales[command - scaleBase];
            }
            else if (command == clockBase || command == clockBase + 1) bar.clockSystemPanel = command == clockBase + 1;
            else if (command == inputPanelBase || command == inputPanelBase + 1) bar.inputMethodSystemPanel = command == inputPanelBase + 1;
            else if (command == timePositionBase || command == timePositionBase + 1)
                bar.timeNotificationRightAligned = command == timePositionBase + 1;
            else if ((command == controlBase || command == controlBase + 1) && snowdesktop::StatusBarSupportsSystemQuickSettings())
                bar.controlCenterSystemPanel = command == controlBase + 1;
            else if (command >= controlDisplayBase && command < controlDisplayBase + 3)
            {
                bar.controlCenter = command != controlDisplayBase + 2;
                bar.controlCenterSingleIcon = command == controlDisplayBase + 1;
            }
            else changed = false;
            if (changed)
            {
                if (merged && (command == positionBase || command == positionBase + 1 ||
                    (command >= monitorBase && command < monitorBase + 3)))
                {
                    // Queue both domains before the controller applies its
                    // pending snapshot, preserving fusion on every monitor.
                    auto dock = settingsController_->Snapshot()->values.dock;
                    if (command == positionBase || command == positionBase + 1) dock.position = bar.position;
                    else dock.monitorScope = bar.monitorScope;
                    settingsController_->UpdateDock(std::move(dock), snowdesktop::SettingsUpdateMode::PreviewAndCommit);
                }
                settingsController_->UpdateGeneral(std::move(settings), snowdesktop::SettingsUpdateMode::PreviewAndCommit);
                uiAnimationScheduler_.ScheduleOnce(0, [this](auto) {
                    if (settingsController_) (void)settingsController_->FlushPending();
                });
                return;
            }
        }
        ExecuteDockContextMenuCommand(command, owner);
    }
    else if (action == Action::QuickSearch)
    {
        if (systemPanel_) systemPanel_->Hide();
        if (quickNavigationOpen_) CloseQuickNavigation();
        else
        {
            statusBarQuickNavigationMonitor_ = monitor;
            OpenQuickNavigation(QuickNavigationInvocationSource::StatusBar);
        }
    }
    else if (action == Action::Settings)
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::StatusBar));
    else if (statusBar_ && statusBar_->IsInteractionAvailable(monitor))
    {
        ensureSystemPanel();
        systemPanel_->Show(action, owner, anchor, collectionPopupAppearance_, generalSettings_.statusBar,
            action == Action::Tray ? EnsureDockTrayService() : statusBar_->Tray(),
            systemDataProvider_, statusBar_->MergedStripBounds(monitor).has_value());
    }
}

void DesktopApp::SyncStatusBar()
{
    const bool dockTrayRequired = generalSettings_.dockEnabled &&
        std::any_of(dockSettings_.trayApplications.begin(), dockSettings_.trayApplications.end(),
            [](const auto& rule) { return rule.enabled; });
    const bool barTrayRequired = generalSettings_.statusBar.enabled && generalSettings_.statusBar.tray;
    if (snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Tray) &&
        (dockTrayRequired || barTrayRequired)) EnsureDockTrayService();
    else dockTrayService_.reset();
    if (!systemDataProvider_ || !dcompDevice_ || !dwriteFactory_) return;
    if (systemPanel_) systemPanel_->UpdateSettings(generalSettings_.statusBar);
    if (!generalSettings_.statusBar.enabled)
    {
        CancelStatusBarActivation();
        statusBar_.reset();
        return;
    }
    if (!statusBar_)
    {
        statusBar_ = std::make_unique<snowdesktop::StatusBar>(systemDataProvider_,
            [this](snowdesktop::StatusBarAction action, HWND owner, RECT anchor) {
                // Sample once at the input boundary. Deferred activation must
                // not reinterpret either an ordinary click or a Ctrl click.
                action = snowdesktop::ResolveStatusBarClick(action, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                    generalSettings_.statusBar, snowdesktop::StatusBarSupportsSystemQuickSettings());
                ActivateStatusBar(action, owner, anchor);
            },
            [this](HMONITOR monitor) { CancelStatusBarActivation(monitor); },
            [this](const std::wstring& error) {
                WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, error.c_str(), DiagnosticLogLevel::Error);
                MessageBoxW(hwnd_, error.c_str(), L"SnowDesktop", MB_OK | MB_ICONERROR);
            },
            [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
                auto fillAppearance = snowdesktop::StatusBarFillAppearance(appearance);
                DrawWidgetPanelBackground(context, frame, 0,
                    D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                    D2D1::ColorF(0, 0.f), false, 0, &fillAppearance, false, 0, scale);
                brushCache_.clear(); brushCacheContext_ = nullptr;
             }, dockTrayService_);
        statusBar_->SetPointerPressed([this] {
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
            InvalidatePersistentDockHosts();
            InvalidateFloatingPopupWindow();
        });
        statusBar_->SetTrayDragHandlers([this](const auto& changed) {
            if (!settingsController_) return;
            auto settings = settingsController_->Snapshot()->values.general;
            settings.statusBar.pinnedTrayItems = changed.pinnedTrayItems;
            settings.statusBar.trayOrder = changed.trayOrder;
            settingsController_->UpdateGeneral(std::move(settings), snowdesktop::SettingsUpdateMode::PreviewAndCommit);
            uiAnimationScheduler_.ScheduleOnce(0, [this](auto) {
                if (settingsController_) (void)settingsController_->FlushPending();
            });
        }, [this](std::string_view key, POINT screen) {
            return systemPanel_ && systemPanel_->DropTrayIcon(key, screen);
        });
        statusBar_->SetDockChanged([this](bool geometry) {
            if (exitRequested_) return;
            if (geometry) ScheduleDisplayTopologyRefresh();
            else
            {
                UpdatePersistentDockHostVisibility();
                InvalidateDockRects();
            }
        });
        statusBar_->SetInteractionSessionProvider([this](HMONITOR monitor) {
            const HWND menu = snowdesktop::modern_menu::ActiveRootWindow();
            return IsDockStartMenuActive(monitor) || statusBarActivationMonitor_ == monitor ||
                (statusBarMenuMonitor_ == monitor && statusBarMenuOwner_ && menu &&
                    GetWindow(menu, GW_OWNER) == statusBarMenuOwner_) ||
                (statusBarQuickNavigationMonitor_ == monitor &&
                    quickNavigationInvocationSource_ == QuickNavigationInvocationSource::StatusBar &&
                    (quickNavigationOpen_ || !quickNavigationAnimation_.IsHidden())) ||
                (systemPanel_ && systemPanel_->IsOpenForMonitor(monitor)) ||
                (statusBar_ && statusBar_->HasTrayMenuSession(monitor));
        });
        statusBar_->SetDockStateProvider([this](HMONITOR monitor) {
            snowdesktop::StatusBarDockState state;
            state.interacting = statusBar_->HasInteractionSession(monitor);
            state.quickSearch = generalSettings_.dockEnabled && dockSettings_.showQuickSearchButton;
            for (const auto& host : persistentDockHosts_)
            {
                if (!host || !host->active || host->monitor != monitor || !host->container ||
                    !host->hwnd || !IsWindow(host->hwnd)) continue;
                state.promoted = host->promoted;
                if (dockSettings_.edgeAttached && !host->container->IsMergedWithStatusBar() &&
                    IsWindowVisible(host->hwnd) && IsDockContainerInteractionVisible(host->container))
                {
                    state.adjacentDockBounds = host->container->GetVisualPanelBounds(lastMousePoint_);
                    OffsetRect(&state.adjacentDockBounds, virtualLeft_, virtualTop_);
                }
                if (host->container->IsMergedWithStatusBar() && IsWindowVisible(host->hwnd))
                {
                    state.window = host->hwnd;
                    state.inputBounds = host->container->GetInteractiveBounds();
                    OffsetRect(&state.inputBounds, virtualLeft_, virtualTop_);
                }
                break;
            }
            return state;
        });
        statusBar_->SetMergedAppearanceProvider([this](HMONITOR) { return CurrentDockAppearance(); });
        statusBar_->SetTrayDragFeedback(MakeStatusBarTrayDragFeedback());
        statusBar_->SetGraphicsFailureHandler([this](HRESULT error) {
            (void)RequestGraphicsDeviceRecovery(L"StatusBar", error);
        });
        statusBar_->SetSceneProvider([this](HMONITOR monitor) {
            snowdesktop::StatusBarSceneState scene;
            const auto& settings = generalSettings_.statusBar;
            if (!settings.noWindow.enabled && !settings.maximizedWindow.enabled)
                return scene;
            // StartDockForegroundMonitor already owns these observations even
            // when Dock/taskbar styling is disabled. The taskbar's own timer
            // maintains the cache while its controls are active. Otherwise the
            // first bar consumes dirty observations for all monitors together.
            // Never enable the Explorer appearance hook to obtain this state.
            const DWORD now = GetTickCount();
            const bool dirty = systemTaskbarWindowStateChangedTick_.load() != systemTaskbarWindowStateObservedTick_;
            const bool taskbarObserverActive = IsSystemTaskbarHookRequired(dockSettings_);
            if (systemTaskbarWindowScanTick_ == 0 ||
                (systemTaskbarMonitorWindowStates_.empty() && now - systemTaskbarWindowScanTick_ >= 1500) ||
                (!taskbarObserverActive &&
                    ((dirty && now - systemTaskbarWindowScanTick_ >= 250) ||
                        now - systemTaskbarWindowScanTick_ >= 1500)))
            {
                RefreshSystemTaskbarWindowState();
                systemTaskbarWindowScanTick_ = now;
            }
            // Empty entries are published only after successful window and
            // monitor observations. A missing entry remains unknown/default.
            if (const auto found = systemTaskbarMonitorWindowStates_.find(monitor);
                found != systemTaskbarMonitorWindowStates_.end())
            {
                scene.noWindow = !found->second.visible && !found->second.maximized;
                scene.maximizedWindow = found->second.maximized;
            }
            return scene;
        });
    }
    auto order = BuildMonitorRenderOrder();
    if (order.size() > 1)
    {
        if (generalSettings_.statusBar.monitorScope == DockMonitorScope::First)
            order.erase(order.begin() + 1, order.end());
        else if (generalSettings_.statusBar.monitorScope == DockMonitorScope::Last)
            order.erase(order.begin(), order.end() - 1);
    }
    std::vector<snowdesktop::StatusBarMonitor> monitors;
    auto dockOrder = BuildMonitorRenderOrder();
    if (dockOrder.size() > 1)
    {
        if (dockSettings_.monitorScope == DockMonitorScope::First) dockOrder.resize(1);
        else if (dockSettings_.monitorScope == DockMonitorScope::Last) dockOrder.erase(dockOrder.begin(), dockOrder.end() - 1);
    }
    for (const auto index : order)
    {
        const auto& page = gridPages_[index];
        RECT screen = page.bounds;
        OffsetRect(&screen, virtualLeft_, virtualTop_);
        if (auto monitor = MonitorFromRect(&screen, MONITOR_DEFAULTTONULL))
        {
            int mergedHeight = 0;
            if (generalSettings_.dockEnabled && dockSettings_.edgeAttached &&
                dockSettings_.position == generalSettings_.statusBar.position &&
                std::find(dockOrder.begin(), dockOrder.end(), index) != dockOrder.end())
            {
                UINT dpiX = 96, dpiY = 96;
                if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
                mergedHeight = MulDiv(std::clamp(dockSettings_.mergedBarHeight, 32, 96), static_cast<int>(dpiX), 96);
            }
            monitors.push_back({page.monitorId, monitor, mergedHeight,
            snowdesktop::ReserveStatusBarSpace(mergedHeight != 0, dockSettings_.showOnlyWhenSummoned)});
        }
    }
    statusBar_->SetPresentationEnabled(!desktopStartupPresentationPending_);
    auto effectiveStatusBar = generalSettings_.statusBar;
    if (!snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Tray)) effectiveStatusBar.tray = false;
    statusBar_->Configure(effectiveStatusBar, personalizationSettings_, monitors,
        dcompDevice_.Get(), dwriteFactory_.Get(), &collectionPopupAppearance_,
        [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
            DrawWidgetPanelBackground(context, frame, appearance.cornerRadius * scale,
                D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
                false, appearance.widgetBorderWidth * scale, &appearance, false, 0, scale);
            brushCache_.clear(); brushCacheContext_ = nullptr;
        });
}
