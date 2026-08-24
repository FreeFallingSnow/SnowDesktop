#pragma once

#include "../settings_controller.h"
#include "../settings_search_index.h"
#include "backup_data_page_backend.h"
#include "general_page_presenter.h"
#include "home_about_page_model.h"
#include "widgets_page_backend.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
class WidgetSettingsService;
}

class WidgetEngine;

namespace snowdesktop::winui
{

/** Runtime dependencies supplied by DesktopApp to the WinUI settings host. */
struct SettingsWindowHostOptions
{
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;
    using LanguageCatalogProvider = std::function<
        std::vector<std::pair<std::string, std::wstring>>() >;
    using SearchInputProvider =
        std::function<SettingsSearchIndexInput()>;
    using HomeAboutStatusProvider = std::function<HomeAboutStatusPatch(
        std::uint64_t generation,
        std::uint64_t revision)>;

    LocalizeCallback localize;
    LanguageCatalogProvider languageCatalog;
    SearchInputProvider searchInput;
    HomeAboutStatusProvider homeAboutStatus;
    /** Runtime-only ownership warnings for the Windows auto-start setting. */
    std::function<GeneralStartupConflict()> startupConflict;

    /** Ensure a persisted component instance is loaded before its declarative
     * settings session is created. The application owns the instance-to-
     * package lookup needed by WidgetEngine::EnsureWidgetLoaded. */
    std::function<bool(std::wstring_view instanceId)>
        ensureWidgetSettingsInstance;

    /** Application-owned seams used by the page backends. UI ownership,
     * dispatch and snapshot publication are supplied by SettingsWindowHost. */
    WidgetsPageBackendOptions widgetsPage;
    BackupDataPageBackendOptions backupDataPage;

    /** Reconcile host-owned system state after a persisted-state reload. */
    std::function<void()> refreshExternalState;
    std::function<bool()> developerToolsVisible;
    std::function<bool()> debugVisible;

    std::wstring windowTitle = L"SnowDesktop Settings";
};

/**
 * Owns the Win32 top-level settings HWND and its full-client WinUI 3 Island.
 * WS_OVERLAPPEDWINDOW deliberately retains the native system caption buttons;
 * the Island never extends or substitutes the non-client title bar.
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
        SettingsController& controller,
        widget_runtime::WidgetSettingsService* widgetSettingsService,
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
        widget_runtime::WidgetSettingsService* service) noexcept;
    /** Attach/detach the application-lifetime component engine. */
    void SetWidgetEngine(WidgetEngine* engine);
    /** Re-capture the component page after an external subscription change. */
    void RefreshWidgetsPage();
    void ApplyLanguageChange();
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
