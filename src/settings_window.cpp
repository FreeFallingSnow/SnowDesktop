#include "settings_window.h"

#include "settings_controller.h"
#include "settings_ipc_services.h"
#include "settings_process.h"
#include "widget_settings_service.h"
#include "winui/settings_ipc_backends.h"
#include "winui/settings_ipc_values.h"

#include <utility>

namespace ipc = snowdesktop::settings_ipc;

struct SettingsWindow::Impl
{
    HINSTANCE instance = nullptr;
    snowdesktop::SettingsController* controller = nullptr;
    snowdesktop::widget_runtime::WidgetSettingsService* widgetSettingsService = nullptr;
    WidgetEngine* widgetEngine = nullptr;
    snowdesktop::winui::SettingsWindowHostOptions options;
    std::unique_ptr<ipc::Channel> channel;
    std::unique_ptr<ipc::BackendServer> backends;
    ipc::SettingsProcess process;
    HWND window = nullptr;
    bool closing = false;
    std::wstring lastError;

    std::wstring LocalizedError(const char* key) const
    {
        return options.localize ? options.localize(key) : L"Settings process unavailable";
    }

    void EndSession() noexcept
    {
        try
        {
            if (options.largeIconSettings)
            {
                snowdesktop::LargeIconSettingsRequest request;
                request.action = "close";
                options.largeIconSettings(std::move(request));
            }
        }
        catch (...) {}
        window = nullptr;
        closing = false;
        if (channel) channel->Close();
        if (backends) backends->ClosePages();
        if (widgetSettingsService) widgetSettingsService->CloseAll();
        try { if (controller) (void)controller->CloseSession(); } catch (...) {}
        process.Stop();
    }
    bool EnsureInitialized()
    {
        if (channel && channel->Connected() && process.Running() && !closing) return true;
        if (!instance || !controller)
        {
            lastError = L"Settings window has not been configured";
            return false;
        }
        try
        {
            if (!channel)
            {
                channel = std::make_unique<ipc::Channel>();
                backends = std::make_unique<ipc::BackendServer>(*channel, *controller, widgetEngine, options);
                channel->Bind<void>("ui.closed", [this] { closing = true; window = nullptr; });
                channel->SetDisconnected([this] { EndSession(); });
            }
            // A normal close has acknowledged durable commits. An immediate
            // reopen can finish teardown before launching the next UI.
            if (process.Running()) { channel->Close(); EndSession(); }
            ipc::BindController(*channel, *controller, options.localize);
            if (widgetSettingsService) ipc::BindWidgetService(*channel, *widgetSettingsService);
            process.Start(*channel);
            auto [initialized, handle, error] = channel->Call<
                std::tuple<bool, std::uint64_t, std::wstring>>("ui.initialize", ipc::ExecutableIdentity());
            HWND candidate = reinterpret_cast<HWND>(handle);
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            if (!initialized || !candidate || owner != process.ProcessId())
            {
                if (!error.empty()) OutputDebugStringW(error.c_str());
                lastError = LocalizedError("settings.process.startFailed");
                channel->Close();
                EndSession();
                return false;
            }
            window = candidate;
            closing = false;
            lastError.clear();
            return true;
        }
        catch (const std::exception& error)
        {
            const std::string message(error.what());
            OutputDebugStringA(message.c_str());
            lastError = LocalizedError("settings.process.startFailed");
            if (channel) channel->Close();
            EndSession();
            return false;
        }
    }
    template<class... A> void Notify(const char* name, const A&... arguments) noexcept
    {
        try { if (channel && channel->Connected() && !closing) channel->Notify(name, arguments...); }
        catch (...) {}
    }
};

SettingsWindow::SettingsWindow() : impl_(std::make_unique<Impl>()) {}
SettingsWindow::~SettingsWindow() { Shutdown(); }
bool SettingsWindow::Init(HINSTANCE instance, snowdesktop::SettingsController& controller,
    snowdesktop::widget_runtime::WidgetSettingsService* service,
    snowdesktop::winui::SettingsWindowHostOptions options)
{
    if (!instance) return false;
    Shutdown();
    impl_->instance = instance;
    impl_->controller = &controller;
    impl_->widgetSettingsService = service;
    impl_->options = std::move(options);
    impl_->lastError.clear();
    return true;
}
void SettingsWindow::Shutdown() noexcept
{
    if (impl_->channel) impl_->channel->SetDisconnected({});
    impl_->window = nullptr;
    if (impl_->backends) impl_->backends->ClosePages();
    if (impl_->controller)
    {
        impl_->controller->SetSnapshotChangedCallback({});
        impl_->controller->SetPendingWorkCallback({});
    }
    if (impl_->widgetSettingsService)
    {
        impl_->widgetSettingsService->SetEventCallbacks({}, {});
        impl_->widgetSettingsService->CloseAll();
    }
    if (impl_->channel) impl_->channel->Close();
    impl_->process.Stop();
    impl_->backends.reset();
    impl_->channel.reset();
    impl_->instance = nullptr;
    impl_->controller = nullptr;
    impl_->widgetSettingsService = nullptr;
    impl_->widgetEngine = nullptr;
    impl_->options = {};
    impl_->closing = false;
}
bool SettingsWindow::Open(const snowdesktop::SettingsRoute& route)
{
    auto canonical = snowdesktop::CanonicalizeSettingsRoute(route);
    if (canonical.page == snowdesktop::SettingsPage::Home)
        canonical.page = snowdesktop::SettingsPage::General;
    if (!impl_->EnsureInitialized()) return false;
    AllowSetForegroundWindow(impl_->process.ProcessId());
    try
    {
        auto [opened, error] = impl_->channel->Call<std::pair<bool, std::wstring>>("ui.open", canonical);
        impl_->lastError = std::move(error);
        return opened;
    }
    catch (...) { impl_->lastError = impl_->LocalizedError("settings.process.connectionLost"); return false; }
}
bool SettingsWindow::Show()
{ return Open(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::General)); }
bool SettingsWindow::ShowDockSettings()
{ return Open(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Dock, "dock.enable")); }
bool SettingsWindow::ShowAppearanceSettings()
{ return Open(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Personalization)); }
void SettingsWindow::ShowWidgetEditor(std::size_t, const wchar_t* id, const wchar_t*, const wchar_t*)
{ if (id && *id) (void)Open(snowdesktop::SettingsRoute::ForWidget(id)); }
bool SettingsWindow::ShowExitConfirm()
{
    if (!impl_->controller || (!IsVisible() && !Show())) return false;
    try { impl_->channel->Call<void>("ui.exitConfirmation"); return true; } catch (...) { return false; }
}
bool SettingsWindow::FlushPendingChanges()
{
    if (!impl_->channel || !impl_->process.Running() || impl_->closing)
        return !impl_->controller || impl_->controller->FlushAll().Succeeded();
    try { return impl_->channel->Call<bool>("ui.flush"); } catch (...) { return false; }
}
void SettingsWindow::SetWidgetSettingsService(snowdesktop::widget_runtime::WidgetSettingsService* service) noexcept
{
    if (impl_->widgetSettingsService && impl_->widgetSettingsService != service)
        impl_->widgetSettingsService->SetEventCallbacks({}, {});
    impl_->widgetSettingsService = service;
    try { if (impl_->channel && service) ipc::BindWidgetService(*impl_->channel, *service); } catch (...) {}
}
void SettingsWindow::SetWidgetEngine(WidgetEngine* engine) { impl_->widgetEngine = engine; }
void SettingsWindow::RefreshWidgetsPage() { impl_->Notify("ui.refreshWidgets"); }
void SettingsWindow::RefreshGeneralRuntimeState() { impl_->Notify("ui.refreshGeneral"); }
bool SettingsWindow::PrepareLanguageChange()
{
    if (!impl_->channel || !impl_->process.Running() || impl_->closing) return true;
    try { return impl_->channel->Call<bool>("ui.prepareLanguage"); } catch (...) { return false; }
}
void SettingsWindow::ApplyLanguageChange(bool reloaded) { impl_->Notify("ui.applyLanguage", reloaded); }
bool SettingsWindow::PublishHomeAboutStatus(snowdesktop::winui::HomeAboutStatusPatch patch)
{
    if (!impl_->channel || !impl_->channel->Connected() || impl_->closing) return false;
    try { return impl_->channel->Call<bool>("ui.homeStatus", patch); } catch (...) { return false; }
}
bool SettingsWindow::PreTranslateMessage(MSG*) noexcept { return false; }
bool SettingsWindow::ProcessTabNavigation(MSG*) noexcept { return false; }
HWND SettingsWindow::Window() const noexcept
{
    if (!impl_->window || !impl_->process.Running() || impl_->closing) return nullptr;
    DWORD owner = 0;
    GetWindowThreadProcessId(impl_->window, &owner);
    return owner == impl_->process.ProcessId() ? impl_->window : nullptr;
}
bool SettingsWindow::IsVisible() const noexcept { const HWND window = Window(); return window && IsWindowVisible(window); }
bool SettingsWindow::IsHotkeyCaptureActive() const noexcept
{
    if (!IsVisible() || !impl_->channel || !impl_->channel->Connected()) return false;
    try { return impl_->channel->Call<bool>("ui.hotkeyCapture"); } catch (...) { return false; }
}
void SettingsWindow::CaptureRegisteredHotkey(UINT modifiers, UINT key) { impl_->Notify("ui.captureHotkey", modifiers, key); }
const std::wstring& SettingsWindow::LastError() const noexcept { return impl_->lastError; }
