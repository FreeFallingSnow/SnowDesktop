#pragma once

#include "../settings_controller.h"
#include "../settings_search_index.h"

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

    LocalizeCallback localize;
    LanguageCatalogProvider languageCatalog;
    SearchInputProvider searchInput;

    /** Reconcile host-owned system state after a persisted-state reload. */
    std::function<void()> refreshExternalState;
    std::function<bool()> developerToolsVisible;
    std::function<bool()> debugVisible;

    std::wstring windowTitle = L"SnowDesktop Settings";
};

/**
 * Owns the Win32 top-level settings HWND and its full-client WinUI 3 Island.
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

    void SetWidgetSettingsService(
        widget_runtime::WidgetSettingsService* service) noexcept;
    void ApplyLanguageChange();

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
