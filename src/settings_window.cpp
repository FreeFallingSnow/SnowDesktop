#include "settings_window.h"

#include "settings_controller.h"

#include <utility>

struct SettingsWindow::Impl
{
    snowdesktop::SettingsController* controller = nullptr;
    snowdesktop::winui::SettingsWindowHost host;
};

SettingsWindow::SettingsWindow()
    : impl_(std::make_unique<Impl>())
{
}

SettingsWindow::~SettingsWindow()
{
    Shutdown();
}

bool SettingsWindow::Init(
    HINSTANCE instance,
    snowdesktop::SettingsController& controller,
    snowdesktop::widget_runtime::WidgetSettingsService* widgetSettingsService,
    snowdesktop::winui::SettingsWindowHostOptions options)
{
    impl_->controller = &controller;
    if (impl_->host.Initialize(instance, controller, widgetSettingsService,
            std::move(options)))
    {
        return true;
    }
    impl_->controller = nullptr;
    return false;
}

void SettingsWindow::Shutdown() noexcept
{
    impl_->host.Shutdown();
    impl_->controller = nullptr;
}

bool SettingsWindow::Open(const snowdesktop::SettingsRoute& route)
{
    return impl_->host.Open(route);
}

bool SettingsWindow::Show()
{
    return Open(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::Home));
}

bool SettingsWindow::ShowDockSettings()
{
    return Open(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::DockAndTaskbar));
}

bool SettingsWindow::ShowAppearanceSettings()
{
    return Open(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::Personalization));
}

void SettingsWindow::ShowWidgetEditor(
    std::size_t,
    const wchar_t* widgetId,
    const wchar_t*,
    const wchar_t*)
{
    if (widgetId && *widgetId)
        (void)Open(snowdesktop::SettingsRoute::ForWidget(widgetId));
}

bool SettingsWindow::ShowExitConfirm()
{
    if (!impl_->controller)
        return false;
    if (!IsVisible() && !Show())
        return false;

    impl_->host.ShowExitConfirmation([controller = impl_->controller](
                                         bool confirmed) {
        if (!confirmed || !controller)
            return;
        snowdesktop::SettingsHostActions::Request request;
        request.action = snowdesktop::SettingsHostActions::Action::
            ExitApplication;
        (void)controller->InvokeHostAction(request);
    });
    return true;
}

void SettingsWindow::SetWidgetSettingsService(
    snowdesktop::widget_runtime::WidgetSettingsService* service) noexcept
{
    impl_->host.SetWidgetSettingsService(service);
}

void SettingsWindow::ApplyLanguageChange()
{
    impl_->host.ApplyLanguageChange();
}

bool SettingsWindow::PreTranslateMessage(MSG* message) noexcept
{
    return impl_->host.PreTranslateMessage(message);
}

bool SettingsWindow::ProcessTabNavigation(MSG* message) noexcept
{
    return impl_->host.ProcessTabNavigation(message);
}

bool SettingsWindow::IsVisible() const noexcept
{
    return impl_->host.IsVisible();
}

bool SettingsWindow::IsHotkeyCaptureActive() const noexcept
{
    return impl_->host.IsHotkeyCaptureActive();
}

void SettingsWindow::CaptureRegisteredHotkey(
    UINT modifiers, UINT virtualKey)
{
    impl_->host.CaptureRegisteredHotkey(modifiers, virtualKey);
}

const std::wstring& SettingsWindow::LastError() const noexcept
{
    return impl_->host.LastError();
}
