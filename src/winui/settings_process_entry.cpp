#include "../settings_process.h"
#include "../settings_ipc_services.h"
#include "settings_ipc_backends.h"
#include "settings_ipc_values.h"
#include "../l10n.h"

#include <commctrl.h>
#include <ole2.h>

namespace snowdesktop::settings_ipc
{
int RunSettingsProcess(HINSTANCE instance)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return static_cast<int>(ole);
    struct OleGuard { ~OleGuard() { OleUninitialize(); } } oleGuard;
    try
    {
        Channel channel;
        OpenInheritedSettingsChannel(channel);
        std::unique_ptr<ISettingsController> controller;
        std::unique_ptr<widget_runtime::IWidgetSettingsService> widgets;
        std::unique_ptr<winui::SettingsWindowHost> host;
        bool closing = false;
        auto loadLanguage = [&] {
            const auto language = channel.Call<std::string>("options.locale");
            Locale::Instance().SetLanguage(language.c_str());
        };
        channel.SetDisconnected([&] { closing = true; PostQuitMessage(ERROR_BROKEN_PIPE); });
        using Initialized = std::tuple<bool, std::uint64_t, std::wstring>;
        channel.Bind<Initialized, std::string>("ui.initialize", [&](const std::string& identity) {
            if (identity != ExecutableIdentity() || host)
                throw ProtocolError("settings executable/protocol identity mismatch");
            wchar_t executable[32768]{};
            if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))))
                throw ProtocolError("cannot locate settings language resources");
            const auto directory = std::filesystem::path(executable).parent_path() / L"lang";
            Locale::Instance().Init(directory.c_str());
            loadLanguage();
            controller = CreateControllerProxy(channel, [](std::string_view key) {
                return std::wstring(Locale::Instance().TrW(std::string(key).c_str()));
            });
            widgets = CreateWidgetServiceProxy(channel);
            auto options = CreateRemoteHostOptions(channel);
            options.sessionClosed = [&] {
                closing = true;
                channel.Notify("ui.closed");
                PostQuitMessage(0);
            };
            host = std::make_unique<winui::SettingsWindowHost>();
            const bool initialized = host->Initialize(instance, *controller, widgets.get(), std::move(options));
            return Initialized{initialized, reinterpret_cast<std::uint64_t>(host->Window()), host->LastError()};
        });
        channel.Bind<std::pair<bool, std::wstring>, SettingsRoute>("ui.open", [&](SettingsRoute route) {
            const bool opened = host && !closing && host->Open(route);
            return std::pair{opened, host ? host->LastError() : std::wstring{}};
        });
        channel.Bind<bool>("ui.flush", [&] { return host && host->FlushPendingChanges(); });
        channel.Bind<bool>("ui.prepareLanguage", [&] { return !host || host->PrepareLanguageChange(); });
        channel.Bind<void, bool>("ui.applyLanguage", [&](bool reloaded) {
            loadLanguage();
            if (host) host->ApplyLanguageChange(reloaded);
        });
        channel.Bind<void>("ui.refreshWidgets", [&] { if (host && !closing) host->RefreshWidgetsPage(); });
        channel.Bind<void>("ui.refreshGeneral", [&] { if (host && !closing) host->RefreshGeneralRuntimeState(); });
        channel.Bind<bool, winui::HomeAboutStatusPatch>("ui.homeStatus", [&](auto patch) {
            return host && !closing && host->PublishHomeAboutStatus(std::move(patch));
        });
        channel.Bind<bool>("ui.hotkeyCapture", [&] { return host && !closing && host->IsHotkeyCaptureActive(); });
        channel.Bind<void, UINT, UINT>("ui.captureHotkey", [&](UINT modifiers, UINT key) {
            if (host && !closing) host->CaptureRegisteredHotkey(modifiers, key);
        });
        channel.Bind<void>("ui.exitConfirmation", [&] {
            if (host && !closing) host->ShowExitConfirmation([&](bool confirmed) {
                if (!confirmed || !host->FlushPendingChanges()) return;
                SettingsHostActions::Request request;
                request.action = SettingsHostActions::Action::ExitApplication;
                (void)controller->InvokeHostAction(request);
            });
        });
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (host && (host->PreTranslateMessage(&message) || host->ProcessTabNavigation(&message))) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (host) host->Shutdown();
        host.reset();
        widgets.reset();
        controller.reset();
        channel.SetDisconnected({});
        channel.Close();
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("SnowDesktop settings process: ") + error.what() + "\n").c_str());
        return ERROR_INVALID_DATA;
    }
    catch (...) { return ERROR_INVALID_DATA; }
}
}
