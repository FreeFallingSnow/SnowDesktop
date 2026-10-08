#include "app/app.h"
#include "system/panel/system_panel_model.h"
#include "system/calendar/system_calendar_editor.h"
#include "system/controls/system_controls.h"
#include "ui/menu/modern_menu.h"
#include "system/status_bar/status_bar_view.h"
#include "system/status_bar/status_bar_shell_shortcut.h"
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
    UINT requested = 0, UINT sent = 0) noexcept try
{
    if (started < 0) return;
    wchar_t inputState[448]{};
    if (wcscmp(phase, L"queued") == 0 || wcscmp(phase, L"send-input") == 0 ||
        wcscmp(phase, L"finished") == 0)
    {
        // These are input-state snapshots only; do not synchronously query
        // foreign window text/classes or consume queue-status change bits.
        const DWORD uiThread = GetCurrentThreadId();
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
        L"StatusBarShellActivation action=%u generation=%llu phase=%ls elapsedMs=%.3f result=%ls callMs=%.3f requested=%u sent=%u%ls",
        static_cast<unsigned>(action), static_cast<unsigned long long>(generation), phase,
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - started,
        result, callMilliseconds, requested, sent, inputState);
    WriteDiagnosticLogEntry(message);
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
    if (action == Action::SystemControlCenter && !snowdesktop::StatusBarSupportsSystemQuickSettings())
        action = Action::ControlCenter;
    const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    if ((action == Action::TaskView && !statusBarTaskViewTransition_.CanBegin(now, systemTaskbarTaskViewActive_)) ||
        (snowdesktop::IsTaskViewTransitionSensitiveAction(action) && statusBarTaskViewTransition_.Busy(now)))
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
        action == Action::TaskView || action == Action::SystemCalendar ||
        action == Action::InputMethod || action == Action::InputMethodMenu || action == Action::InputMethodPanel
        ? snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() : -1;
    TraceStatusBarShellActivation(action, generation, L"queued", shortcutStarted);
    const HWND activeMenu = snowdesktop::modern_menu::ActiveRootWindow();
    if ((action == Action::Menu || action == Action::SystemMenu) &&
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
    if (!statusBar_ || exitRequested_ || !generalSettings_.statusBar.enabled || !owner ||
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
    const auto current = [this, lifetime, generation, owner, monitor] {
        const auto state = lifetime.lock();
        return state && *state == generation && !exitRequested_ &&
            generalSettings_.statusBar.enabled && statusBar_ && IsWindow(owner) &&
            IsWindowVisible(owner) && MonitorFromWindow(owner, MONITOR_DEFAULTTONULL) == monitor &&
            statusBar_->IsInteractionAvailable(monitor);
    };
    if (!current() || action == Action::None) return;
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
    const bool externalSurface = action == Action::SystemMenu || action == Action::Menu ||
        action == Action::QuickSearch || action == Action::Settings ||
        action == Action::Notifications || action == Action::SystemControlCenter ||
        action == Action::TaskView || action == Action::SystemCalendar ||
        action == Action::InputMethod || action == Action::InputMethodMenu || action == Action::InputMethodPanel;
    if (externalSurface && systemPanel_ && systemPanel_->IsOpen())
    {
        TraceStatusBarShellActivation(action, generation, L"wait-system-panel", hold->shortcutStartedMilliseconds);
        systemPanel_->CloseThen(resume, owner);
        return;
    }
    const auto chord = snowdesktop::ResolveStatusBarShellChord(action,
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
                        WriteDiagnosticLogEntry(L"StatusBar input method stage=restore-scheduler-unavailable", DiagnosticLogLevel::Error);
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
                            WriteDiagnosticLogEntry(message);
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
                            WriteDiagnosticLogEntry(message);
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
            WriteDiagnosticLogEntry(L"StatusBar input method stage=capture-scheduler-unavailable", DiagnosticLogLevel::Error);
            MessageBoxW(hwnd_, _LW("statusBar.inputMethodFailed"), _LW("statusBar.inputMethod"), MB_OK | MB_ICONINFORMATION);
        }
    }
    else if (action == Action::InputMethod || action == Action::InputMethodMenu)
    {
        const HRESULT result = statusBar_->ShowInputMethod(anchor, action == Action::InputMethodMenu);
        wchar_t message[192]{};
        swprintf_s(message, L"StatusBar input method %ls hr=0x%08lX anchor=%ld,%ld,%ld,%ld",
            action == Action::InputMethodMenu ? L"context" : L"toggle", static_cast<unsigned long>(result),
            anchor.left, anchor.top, anchor.right, anchor.bottom);
        WriteDiagnosticLogEntry(message);
        hold->shortcutFinished = true;
        TraceStatusBarShellActivation(action, generation, L"finished", hold->shortcutStartedMilliseconds,
            SUCCEEDED(result) ? L"requested" : L"failed");
        if (FAILED(result))
        {
            ShellExecuteW(owner, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL);
        }
        statusBarActivationMonitor_ = nullptr;
    }
    else if (chord.key)
    {
        if (systemPanel_) systemPanel_->Hide();
        CloseQuickNavigation();
        const HWND foreground = GetForegroundWindow();
        // Shell shortcut activation has no supported custom popup anchor. Let
        // Windows place its own surface; never move or reparent its window.
        statusBarActivationToken_ = snowdesktop::ScheduleStatusBarShellShortcut(uiAnimationScheduler_, chord, {
            [](int code) { return (GetAsyncKeyState(code) & 0x8000) != 0; },
            [this, action, generation, started = hold->shortcutStartedMilliseconds](UINT count, INPUT* input, int size) {
                TraceStatusBarShellActivation(action, generation, L"send-input-begin", started);
                const double before = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                const bool taskViewChord = action == Action::TaskView && count == 4 &&
                    !(input[0].ki.dwFlags & KEYEVENTF_KEYUP);
                if (taskViewChord && !statusBarTaskViewTransition_.Begin(before, systemTaskbarTaskViewActive_))
                    return 0u;
                const UINT sent = SendInput(count, input, size);
                if (taskViewChord && sent == 0) statusBarTaskViewTransition_.Reset();
                const double elapsed = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - before;
                TraceStatusBarShellActivation(action, generation, L"send-input", started,
                    sent == count ? L"accepted" : L"incomplete", elapsed, count, sent);
                return sent;
            },
            [this, current, foreground, action](auto token) {
                return current() && token == statusBarActivationToken_ && GetForegroundWindow() == foreground &&
                    (action != Action::TaskView || statusBarTaskViewTransition_.CanBegin(
                        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(), systemTaskbarTaskViewActive_));
            },
            [this, lifetime, generation, action, hold](auto token, snowdesktop::StatusBarShortcutResult result) {
                const wchar_t* outcome = result == snowdesktop::StatusBarShortcutResult::Sent ? L"sent" :
                    result == snowdesktop::StatusBarShortcutResult::Cancelled ? L"canceled" :
                    result == snowdesktop::StatusBarShortcutResult::TimedOut ? L"timed-out" : L"failed";
                hold->shortcutFinished = true;
                TraceStatusBarShellActivation(action, generation, L"finished", hold->shortcutStartedMilliseconds, outcome);
                const auto state = lifetime.lock();
                if (!state || *state != generation) return;
                if (token != statusBarActivationToken_) return;
                statusBarActivationToken_ = 0;
                statusBarActivationMonitor_ = nullptr;
                if (result == snowdesktop::StatusBarShortcutResult::Failed || result == snowdesktop::StatusBarShortcutResult::TimedOut)
                {
                    WriteDiagnosticLogEntry(result == snowdesktop::StatusBarShortcutResult::Failed ?
                        L"StatusBar system shortcut input failed" : L"StatusBar system shortcut modifier release timed out");
                    MessageBeep(MB_ICONWARNING);
                }
            }});
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
    else if (action == Action::Menu)
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
            statusBar_->Tray(), systemDataProvider_, statusBar_->MergedStripBounds(monitor).has_value());
    }
}

void DesktopApp::SyncStatusBar()
{
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
                WriteDiagnosticLogEntry(error.c_str());
                MessageBoxW(hwnd_, error.c_str(), L"SnowDesktop", MB_OK | MB_ICONERROR);
            },
            [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
                auto fillAppearance = snowdesktop::StatusBarFillAppearance(appearance);
                DrawWidgetPanelBackground(context, frame, 0,
                    D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                    D2D1::ColorF(0, 0.f), false, 0, &fillAppearance, false, 0, scale);
                 snowdesktop::DrawStatusBarEdge(context, frame, appearance, scale, generalSettings_.statusBar.position);
                brushCache_.clear(); brushCacheContext_ = nullptr;
             });
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
            return statusBarActivationMonitor_ == monitor ||
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
            for (const auto& host : persistentDockHosts_)
            {
                if (!host || !host->active || host->monitor != monitor || !host->container ||
                    !host->hwnd || !IsWindow(host->hwnd)) continue;
                state.promoted = host->promoted;
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
    statusBar_->Configure(generalSettings_.statusBar, personalizationSettings_, monitors,
        dcompDevice_.Get(), dwriteFactory_.Get(), &collectionPopupAppearance_,
        [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
            DrawWidgetPanelBackground(context, frame, appearance.cornerRadius * scale,
                D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
                false, appearance.widgetBorderWidth * scale, &appearance, false, 0, scale);
            brushCache_.clear(); brushCacheContext_ = nullptr;
        });
}
