#include "settings_window.h"

#include "settings_controller.h"

#include <utility>

struct SettingsWindow::Impl
{
    HINSTANCE instance = nullptr;
    snowdesktop::SettingsController* controller = nullptr;
    snowdesktop::widget_runtime::WidgetSettingsService*
        widgetSettingsService = nullptr;
    WidgetEngine* widgetEngine = nullptr;
    snowdesktop::winui::SettingsWindowHostOptions options;
    std::unique_ptr<snowdesktop::winui::SettingsWindowHost> host;
    std::wstring lastError;

    bool EnsureInitialized()
    {
        if (host && host->IsInitialized())
            return true;
        if (!instance || !controller)
        {
            lastError = L"Settings window has not been configured";
            return false;
        }

        // A failed WinUI/XAML initialization can leave its runtime object in
        // a terminal state. Always retry with a fresh host while retaining
        // the application-lifetime dependencies and callbacks.
        auto candidate =
            std::make_unique<snowdesktop::winui::SettingsWindowHost>();
        if (!candidate->Initialize(instance, *controller,
                widgetSettingsService, options))
        {
            lastError = candidate->LastError();
            candidate->Shutdown();
            return false;
        }
        candidate->SetWidgetEngine(widgetEngine);
        lastError.clear();
        host = std::move(candidate);
        return true;
    }
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
    if (!instance)
    {
        impl_->lastError =
            L"Settings window initialization requires HINSTANCE";
        return false;
    }
    if (impl_->host)
        impl_->host->Shutdown();
    impl_->host.reset();
    impl_->instance = instance;
    impl_->controller = &controller;
    impl_->widgetSettingsService = widgetSettingsService;
    impl_->options = std::move(options);
    impl_->lastError.clear();

    // The WinUI runtime and top-level HWND are intentionally created on the
    // first Open. Besides reducing startup work, this lets every failed Open
    // retry construct a pristine XAML runtime rather than reusing a failed
    // host object.
    return true;
}

void SettingsWindow::Shutdown() noexcept
{
    if (impl_->host)
        impl_->host->Shutdown();
    impl_->host.reset();
    impl_->instance = nullptr;
    impl_->controller = nullptr;
    impl_->widgetSettingsService = nullptr;
    impl_->widgetEngine = nullptr;
    impl_->options = {};
}

bool SettingsWindow::Open(const snowdesktop::SettingsRoute& route)
{
    snowdesktop::SettingsRoute canonical =
        snowdesktop::CanonicalizeSettingsRoute(route);
    if (canonical.page == snowdesktop::SettingsPage::Home)
    {
        canonical.page = snowdesktop::SettingsPage::General;
    }
    if (!impl_->EnsureInitialized())
        return false;
    if (impl_->host->Open(canonical))
    {
        impl_->lastError.clear();
        return true;
    }

    impl_->lastError = impl_->host->LastError();
    if (impl_->lastError.empty())
        impl_->lastError = L"Settings window open failed";
    return false;
}

bool SettingsWindow::Show()
{
    return Open(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::General));
}

bool SettingsWindow::ShowDockSettings()
{
    return Open(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::Dock, "dock.enable"));
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

    impl_->host->ShowExitConfirmation([this,
                                         controller = impl_->controller](
                                         bool confirmed) {
        if (!confirmed || !controller)
            return;
        if (!FlushPendingChanges())
            return;
        snowdesktop::SettingsHostActions::Request request;
        request.action = snowdesktop::SettingsHostActions::Action::
            ExitApplication;
        (void)controller->InvokeHostAction(request);
    });
    return true;
}

bool SettingsWindow::FlushPendingChanges()
{
    return impl_->host && impl_->host->FlushPendingChanges();
}

void SettingsWindow::SetWidgetSettingsService(
    snowdesktop::widget_runtime::WidgetSettingsService* service) noexcept
{
    impl_->widgetSettingsService = service;
    if (impl_->host)
        impl_->host->SetWidgetSettingsService(service);
}

void SettingsWindow::SetWidgetEngine(WidgetEngine* engine)
{
    impl_->widgetEngine = engine;
    if (impl_->host)
        impl_->host->SetWidgetEngine(engine);
}

void SettingsWindow::RefreshWidgetsPage()
{
    if (impl_->host)
        impl_->host->RefreshWidgetsPage();
}

void SettingsWindow::RefreshGeneralRuntimeState()
{
    if (impl_->host)
        impl_->host->RefreshGeneralRuntimeState();
}

bool SettingsWindow::PrepareLanguageChange()
{
    return !impl_->host || impl_->host->PrepareLanguageChange();
}

void SettingsWindow::ApplyLanguageChange(bool widgetRuntimeReloaded)
{
    if (impl_->host)
        impl_->host->ApplyLanguageChange(widgetRuntimeReloaded);
}

bool SettingsWindow::PublishHomeAboutStatus(
    snowdesktop::winui::HomeAboutStatusPatch patch)
{
    return impl_->host &&
        impl_->host->PublishHomeAboutStatus(std::move(patch));
}

bool SettingsWindow::PreTranslateMessage(MSG* message) noexcept
{
    return impl_->host && impl_->host->PreTranslateMessage(message);
}

bool SettingsWindow::ProcessTabNavigation(MSG* message) noexcept
{
    return impl_->host && impl_->host->ProcessTabNavigation(message);
}

bool SettingsWindow::IsVisible() const noexcept
{
    return impl_->host && impl_->host->IsVisible();
}

HWND SettingsWindow::Window() const noexcept
{
    return impl_->host ? impl_->host->Window() : nullptr;
}

bool SettingsWindow::IsHotkeyCaptureActive() const noexcept
{
    return impl_->host && impl_->host->IsHotkeyCaptureActive();
}

void SettingsWindow::CaptureRegisteredHotkey(
    UINT modifiers, UINT virtualKey)
{
    if (impl_->host)
        impl_->host->CaptureRegisteredHotkey(modifiers, virtualKey);
}

const std::wstring& SettingsWindow::LastError() const noexcept
{
    return impl_->host && !impl_->host->LastError().empty()
        ? impl_->host->LastError()
        : impl_->lastError;
}
