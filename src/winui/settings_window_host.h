#pragma once

#include "windows_compat.h"
#include "settings/settings_fwd.h"
#include "settings_window_host_options.h"

#include <functional>
#include <memory>
#include <string>

namespace snowdesktop::widget_runtime
{
class IWidgetSettingsService;
}

class WidgetEngine;

namespace snowdesktop::winui
{

/**
 * Owns the Win32 top-level settings HWND and its full-client WinUI 3 Island.
 * AppWindowTitleBar extends XAML into the caption while its platform default
 * drag region owns non-client input. Windows continues to own the caption
 * buttons, maximize/Snap behavior and their accessibility providers.
 *
 * The host does not own SettingsController or WidgetSettingsService. All
 * methods are STA-thread affine except callbacks that only enqueue immutable
 * service notifications to the settings DispatcherQueue.
 */
class SettingsWindowHost final
{
public:
    SettingsWindowHost();
    ~SettingsWindowHost();

    SettingsWindowHost(const SettingsWindowHost&) = delete;
    SettingsWindowHost& operator=(const SettingsWindowHost&) = delete;
    SettingsWindowHost(SettingsWindowHost&&) = delete;
    SettingsWindowHost& operator=(SettingsWindowHost&&) = delete;

    [[nodiscard]] bool Initialize(
        HINSTANCE instance,
        ISettingsController& controller,
        widget_runtime::IWidgetSettingsService* widgetSettingsService,
        SettingsWindowHostOptions options = {});
    void Shutdown() noexcept;

    /** Reload a hidden session, open exactly one route, then activate it. */
    [[nodiscard]] bool Open(const SettingsRoute& route);

    /** Flush all values and close the view session before hiding the HWND. */
    [[nodiscard]] bool Hide();

    /**
     * Flush both the active declarative component editor and controller
     * domains without closing the session. Used before exit/restart.
     */
    [[nodiscard]] bool FlushPendingChanges();

    void SetWidgetSettingsService(
        widget_runtime::IWidgetSettingsService* service) noexcept;
    /** Attach/detach the application-lifetime component engine. */
    void SetWidgetEngine(WidgetEngine* engine);
    /** Re-capture the component page after an external subscription change. */
    void RefreshWidgetsPage();
    /** Re-query General-page state owned by the application runtime. */
    void RefreshGeneralRuntimeState();
    /** Commit the active component editor before its runtime generation changes. */
    [[nodiscard]] bool PrepareLanguageChange();
    /** Rebind localized component schema after the runtime reload completes. */
    void ApplyLanguageChange(bool widgetRuntimeReloaded);
    [[nodiscard]] bool PublishHomeAboutStatus(
        HomeAboutStatusPatch patch);

    [[nodiscard]] bool PreTranslateMessage(MSG* message) noexcept;
    [[nodiscard]] bool ProcessTabNavigation(MSG* message) noexcept;

    [[nodiscard]] bool IsHotkeyCaptureActive() const noexcept;
    void CaptureRegisteredHotkey(UINT modifiers, UINT virtualKey);

    void ShowExitConfirmation(std::function<void(bool)> completed);

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;
    [[nodiscard]] HWND Window() const noexcept;
    [[nodiscard]] const std::wstring& LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
