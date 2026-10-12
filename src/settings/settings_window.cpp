#include "settings_window.h"
#include "diagnostics/diagnostic_log.h"
#include "common/utils.h"

#include "settings_controller.h"
#include "settings_ipc_services.h"
#include "settings_process.h"
#include "widget/settings/widget_settings_service.h"
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
    bool opening = false;
    snowdesktop::SettingsRoute openingRoute;
    OpenCompleted openCompleted;
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
    void CompleteOpen(bool opened)
    {
        opening = false;
        auto completed = std::exchange(openCompleted, {});
        if (completed) completed(opened);
    }
    void FailOpen(std::exception_ptr error)
    {
        std::wstring detail;
        try { if (error) std::rethrow_exception(error); }
        catch (const std::exception& failure) { detail = Utf8ToWide(failure.what()); }
        catch (...) {}
        if (!detail.empty()) WriteDiagnosticLogEntry(detail.c_str(), DiagnosticLogLevel::Error);
        lastError = LocalizedError("settings.process.startFailed") + (detail.empty() ? L"" : L"\n" + detail);
        EndSession();
        CompleteOpen(false);
    }
    void StartOpen()
    {
        AllowSetForegroundWindow(process.ProcessId());
        channel->RequestAsync("ui.open", ipc::Pack(openingRoute),
            [this](ipc::Bytes reply, std::exception_ptr error) {
                if (error) { FailOpen(error); return; }
                try
                {
                    auto [opened, message] = ipc::Unpack<std::pair<bool, std::wstring>>(reply);
                    lastError = std::move(message);
                    CompleteOpen(opened);
                }
                catch (...) { FailOpen(std::current_exception()); }
            });
    }
    void EnsureInitialized()
    {
        if (channel && channel->Connected() && process.Running() && !closing && window)
        {
            StartOpen();
            return;
        }
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
        channel->RequestAsync("ui.initialize", ipc::Pack(ipc::ExecutableIdentity()),
            [this](ipc::Bytes reply, std::exception_ptr failure) {
                if (failure) { FailOpen(failure); return; }
                try
                {
                    auto [initialized, handle, error] = ipc::Unpack<
                        std::tuple<bool, std::uint64_t, std::wstring>>(reply);
                    HWND candidate = reinterpret_cast<HWND>(handle);
                    DWORD owner = 0;
                    GetWindowThreadProcessId(candidate, &owner);
                    if (!initialized || !candidate || owner != process.ProcessId())
                    {
                        if (!error.empty()) WriteDiagnosticLogEntry(error.c_str(), DiagnosticLogLevel::Error);
                        lastError = LocalizedError("settings.process.startFailed") + (error.empty() ? L"" : L"\n" + error);
                        EndSession();
                        CompleteOpen(false);
                        return;
                    }
                    window = candidate;
                    closing = false;
                    StartOpen();
                }
                catch (...) { FailOpen(std::current_exception()); }
            });
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
    options.contextMenu = [&controller](const auto &request, bool refresh) {
        if (const auto snapshot = controller.Snapshot()) snowdesktop::shell_extensions::SharedMenuService().Configure(snapshot->values.general.shellExtensions);
        return snowdesktop::shell_extensions::SharedMenuService().Inspect(request, refresh);
    };
    impl_->options = std::move(options);
    impl_->lastError.clear();
    return true;
}
void SettingsWindow::Shutdown() noexcept
{
    impl_->opening = false;
    impl_->openCompleted = {};
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
bool SettingsWindow::Open(const snowdesktop::SettingsRoute& route, OpenCompleted completed)
{
    auto canonical = snowdesktop::CanonicalizeSettingsRoute(route);
    if (!impl_->instance || !impl_->controller || impl_->opening) return false;
    impl_->opening = true;
    impl_->openingRoute = std::move(canonical);
    impl_->openCompleted = std::move(completed);
    impl_->lastError.clear();
    try
    {
        impl_->EnsureInitialized();
        return true;
    }
    catch (...) { impl_->FailOpen(std::current_exception()); return true; }
}
bool SettingsWindow::Show()
{ return Open(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::General)); }
void SettingsWindow::CloseIfMinimized()
{ impl_->Notify("ui.closeIfMinimized"); }
bool SettingsWindow::ShowDockSettings()
{ return Open(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Dock, "dock.enable")); }
bool SettingsWindow::ShowAppearanceSettings()
{ return Open(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Personalization)); }
void SettingsWindow::ShowWidgetEditor(std::size_t, const wchar_t* id, const wchar_t*, const wchar_t*)
{ if (id && *id) (void)Open(snowdesktop::SettingsRoute::ForWidget(id)); }
bool SettingsWindow::ShowExitConfirm()
{
    if (!impl_->controller) return false;
    if (!IsVisible())
        return Open(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::General),
            [this](bool opened) { if (opened) impl_->Notify("ui.exitConfirmation"); });
    try { impl_->channel->Notify("ui.exitConfirmation"); return true; } catch (...) { return false; }
}
bool SettingsWindow::FlushPendingChanges()
{
    if (!impl_->channel || !impl_->process.Running() || impl_->closing || impl_->opening)
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
    if (!impl_->channel || !impl_->process.Running() || impl_->closing || impl_->opening) return true;
    try { return impl_->channel->Call<bool>("ui.prepareLanguage"); } catch (...) { return false; }
}
void SettingsWindow::ApplyLanguageChange(bool reloaded) { impl_->Notify("ui.applyLanguage", reloaded); }
bool SettingsWindow::PublishHomeAboutStatus(snowdesktop::winui::HomeAboutStatusPatch patch)
{
    if (!impl_->channel || !impl_->channel->Connected() || impl_->closing || impl_->opening) return false;
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
    if (!IsVisible() || !impl_->channel || !impl_->channel->Connected() || impl_->opening) return false;
    try { return impl_->channel->Call<bool>("ui.hotkeyCapture"); } catch (...) { return false; }
}
void SettingsWindow::CaptureRegisteredHotkey(UINT modifiers, UINT key) { impl_->Notify("ui.captureHotkey", modifiers, key); }
const std::wstring& SettingsWindow::LastError() const noexcept { return impl_->lastError; }
