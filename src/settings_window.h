#pragma once

#include "settings_route.h"
#include "winui/settings_window_host.h"

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>

namespace snowdesktop
{
class SettingsController;
namespace widget_runtime
{
class WidgetSettingsService;
}
}

class WidgetEngine;

/**
 * Thin compatibility facade for the application-owned WinUI settings host.
 *
 * All public entry points resolve to Open(SettingsRoute). This class owns no
 * configuration state, graphics device, ImGui context, or render loop.
 */
class SettingsWindow final
{
public:
    SettingsWindow();
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    SettingsWindow(SettingsWindow&&) = delete;
    SettingsWindow& operator=(SettingsWindow&&) = delete;

    [[nodiscard]] bool Init(
        HINSTANCE instance,
        snowdesktop::SettingsController& controller,
        snowdesktop::widget_runtime::WidgetSettingsService*
            widgetSettingsService,
        snowdesktop::winui::SettingsWindowHostOptions options = {});
    void Shutdown() noexcept;

    [[nodiscard]] bool Open(const snowdesktop::SettingsRoute& route);
    [[nodiscard]] bool Show();
    [[nodiscard]] bool ShowDockSettings();
    [[nodiscard]] bool ShowAppearanceSettings();

    void ShowWidgetEditor(
        std::size_t widgetIndex,
        const wchar_t* widgetId,
        const wchar_t* widgetName,
        const wchar_t* scriptPath);
    [[nodiscard]] bool ShowExitConfirm();
    /** Persist active page/controller edits without hiding the window. */
    [[nodiscard]] bool FlushPendingChanges();

    void SetWidgetSettingsService(
        snowdesktop::widget_runtime::WidgetSettingsService* service) noexcept;
    void SetWidgetEngine(WidgetEngine* engine);
    void RefreshWidgetsPage();
    [[nodiscard]] bool PrepareLanguageChange();
    void ApplyLanguageChange(bool widgetRuntimeReloaded);
    [[nodiscard]] bool PublishHomeAboutStatus(
        snowdesktop::winui::HomeAboutStatusPatch patch);

    [[nodiscard]] bool PreTranslateMessage(MSG* message) noexcept;
    [[nodiscard]] bool ProcessTabNavigation(MSG* message) noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;
    [[nodiscard]] bool IsHotkeyCaptureActive() const noexcept;
    void CaptureRegisteredHotkey(UINT modifiers, UINT virtualKey);

    [[nodiscard]] const std::wstring& LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
