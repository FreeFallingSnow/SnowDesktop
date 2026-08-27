#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string source = contents.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
}

std::string_view FunctionBody(std::string_view source,
    std::string_view signature, std::string_view nextSignature)
{
    const std::size_t begin = source.find(signature);
    if (begin == std::string_view::npos)
        return {};
    const std::size_t end = source.find(nextSignature, begin + signature.size());
    if (end == std::string_view::npos)
        return source.substr(begin);
    return source.substr(begin, end - begin);
}
}

int main(int argc, char** argv)
{
    Check(argc == 2, "source root argument is provided");
    if (argc != 2)
        return 1;

    const std::filesystem::path root(argv[1]);
    const std::string utils = ReadFile(root / "src" / "utils.cpp");
    const std::string lifecycle = ReadFile(
        root / "src" / "app" / "app_lifecycle.cpp");
    const std::string settingsApply = ReadFile(
        root / "src" / "app" / "app_settings_apply.cpp");
    const std::string dockSettings = ReadFile(
        root / "src" / "dock_settings.cpp");
    const std::string taskbarHook = ReadFile(
        root / "src" / "taskbar_hook" / "taskbar_hook.cpp");
    const std::string settingsWindow = ReadFile(
        root / "src" / "settings_window.cpp");
    const std::string settingsHost = ReadFile(
        root / "src" / "winui" / "settings_window_host.cpp");
    const std::string dockPresenter = ReadFile(
        root / "src" / "winui" / "dock_page_presenter.cpp");
    const std::string presenterControls = ReadFile(
        root / "src" / "winui" / "settings_presenter_controls.h");
    const std::string messageDispatch = ReadFile(
        root / "src" / "app" / "app_message_dispatch.cpp");
    const std::string controlDispatch = ReadFile(
        root / "src" / "app" / "app_desktop_reload.cpp");
    Check(!utils.empty() && !lifecycle.empty() && !settingsApply.empty() &&
            !dockSettings.empty() && !taskbarHook.empty() &&
            !settingsWindow.empty() &&
            !settingsHost.empty() && !dockPresenter.empty() &&
            !presenterControls.empty() &&
            !messageDispatch.empty() && !controlDispatch.empty(),
        "shell integration sources are readable");

    const std::string_view ensureDesktop = FunctionBody(utils,
        "bool EnsureDesktopWorkerWindow()",
        "DesktopWindows FindDesktopWindows()");
    const std::string_view findDesktop = FunctionBody(utils,
        "DesktopWindows FindDesktopWindows()",
        "void RestoreExplorerIconLayerNow()");
    Check(ensureDesktop.find("0x052C") != std::string_view::npos &&
            ensureDesktop.find("SendMessageTimeoutW") != std::string_view::npos,
        "WorkerW creation is isolated behind an explicit mutating operation");
    Check(findDesktop.find("0x052C") == std::string_view::npos &&
            findDesktop.find("SendMessageTimeoutW") == std::string_view::npos,
        "desktop window discovery remains read-only for periodic polling");

    const std::string_view recovery = FunctionBody(lifecycle,
        "void DesktopApp::RecoverDesktopHostAfterExplorerRestart()",
        "void DesktopApp::WatchDesktopHost()");
    const std::string_view watcher = FunctionBody(lifecycle,
        "void DesktopApp::WatchDesktopHost()",
        "void DesktopApp::InvalidateAllWidgetSlots()");
    Check(recovery.find("SyncSystemTaskbarSettingsFromWindows();") !=
            std::string_view::npos &&
            recovery.find("RequestSystemTaskbar") == std::string_view::npos,
        "desktop-host recovery reads system taskbar state instead of restoring cached values");
    Check(recovery.find("if (explorerDesktopRecreatePending_)\n        EnsureDesktopWorkerWindow();") !=
            std::string_view::npos,
        "desktop-host recovery requests WorkerW only for a confirmed Explorer replacement");
    Check(watcher.find("FindDesktopWindows();") != std::string_view::npos &&
            watcher.find("EnsureDesktopWorkerWindow();") == std::string_view::npos,
        "the periodic desktop-host watcher never mutates the Explorer window tree");

    const std::string_view loadDock = FunctionBody(settingsApply,
        "void DesktopApp::LoadDockSettingsAndApply()",
        "void DesktopApp::SyncSystemTaskbarSettingsFromWindows()");
    Check(loadDock.find("SyncSystemTaskbarSettingsFromWindows();") !=
            std::string_view::npos &&
            loadDock.find("RequestSystemTaskbar") == std::string_view::npos,
        "loading software settings synchronizes from Windows without overwriting it");
    const std::string_view syncTaskbar = FunctionBody(settingsApply,
        "void DesktopApp::SyncSystemTaskbarSettingsFromWindows()",
        "void DesktopApp::LoadCategorySettingsAndApply()");
    Check(syncTaskbar.find("FindWindowW(L\"Shell_TrayWnd\", nullptr)") !=
                std::string_view::npos &&
            syncTaskbar.find("snapshot->externalReplacementPending") !=
                std::string_view::npos &&
            syncTaskbar.find("snapshot->dirtyDomains") !=
                std::string_view::npos &&
            syncTaskbar.find("SynchronizeSystemTaskbarState(") !=
                std::string_view::npos,
        "taskbar state is field-synchronized only while Explorer is available");

    const std::string_view controller = FunctionBody(dockSettings,
        "class WindowsShellSettingsController",
        "class TaskbarBackdropController");
    Check(controller.find("std::thread worker_") != std::string_view::npos &&
            controller.find("std::condition_variable wake_") !=
                std::string_view::npos,
        "Windows Shell settings use a dedicated background worker");
    Check(controller.find("ApplyWindowsSystemLightThemeEnabled(systemTheme.value)") !=
            std::string_view::npos &&
            controller.find("ApplySystemTaskbarAlignmentCentered(alignment.value)") !=
                std::string_view::npos,
        "blocking registry and Shell notification work executes in the worker");

    const std::string_view backdropController = FunctionBody(dockSettings,
        "class TaskbarBackdropController",
        "TaskbarBackdropController& GetTaskbarBackdropController()");
    Check(backdropController.find("if (hookEnabled)") !=
                std::string_view::npos &&
            backdropController.find("SendMessageTimeoutW(taskbar, applyMessage") !=
                std::string_view::npos,
        "graceful shutdown synchronously asks Explorer to restore the taskbar");

    Check(taskbarHook.find(
            "info.nativeRequestedTheme = info.rootElement.RequestedTheme();") !=
                std::string::npos &&
            taskbarHook.find(
            "info.rootElement.RequestedTheme(info.nativeRequestedTheme);") !=
                std::string::npos &&
            taskbarHook.find(
            "PostMessageW(info.taskbar, WM_DWMCOMPOSITIONCHANGED, 1, 0);") !=
                std::string::npos,
        "taskbar hook restores Explorer's theme value and requests native recomposition");

    Check(settingsWindow.find("ImGui") == std::string::npos &&
            settingsWindow.find("ID3D11") == std::string::npos &&
            settingsWindow.find("IDXGISwapChain") == std::string::npos,
        "the application settings facade no longer owns a rendering backend");
    Check(dockPresenter.find("Action::RestartExplorer") !=
                std::string::npos &&
            dockPresenter.find("actions.confirm") !=
                std::string::npos &&
            dockPresenter.find("RequestSystemTaskbar") ==
                std::string::npos,
        "dangerous taskbar work is confirmed and routed through typed host actions");
    Check(dockPresenter.find("muxc::ToggleSwitch") !=
                std::string::npos &&
            dockPresenter.find("muxc::ComboBox") !=
                std::string::npos &&
            dockPresenter.find("ColorFlyoutEditor editor") !=
                std::string::npos &&
            presenterControls.find("muxc::ColorPicker picker") !=
                std::string::npos &&
            dockPresenter.find("muxc::Slider") !=
                std::string::npos,
        "taskbar appearance and dynamic rules use native WinUI controls");
    Check(settingsHost.find("controller->InvokeHostAction(request)") !=
                std::string::npos &&
            settingsHost.find("ShowConfirmation(") !=
                std::string::npos &&
            settingsHost.find("SendMessageTimeoutW") ==
                std::string::npos,
        "the WinUI host forwards typed actions without blocking Shell broadcasts");

    const std::size_t settingChange = messageDispatch.find(
        "case WM_SETTINGCHANGE:");
    const std::size_t themeChange = messageDispatch.find(
        "case WM_THEMECHANGED:", settingChange);
    const std::string_view settingHandler = settingChange == std::string::npos ||
            themeChange == std::string::npos
        ? std::string_view{}
        : std::string_view(messageDispatch).substr(
            settingChange, themeChange - settingChange);
    Check(settingHandler.find("if (!traySettings && !immersiveColor)\n            ReloadItems(false);") !=
            std::string_view::npos,
        "theme notifications avoid a synchronous desktop item reload");

    const std::string_view controlHandler = FunctionBody(controlDispatch,
        "LRESULT DesktopApp::HandleControlMessage(",
        "void DesktopApp::ReloadItems(");
    Check(controlHandler.find("case WM_SETTINGCHANGE:") !=
                std::string_view::npos &&
            controlHandler.find("if (traySettings || immersiveColor)") !=
                std::string_view::npos &&
            controlHandler.find("SyncSystemTaskbarSettingsFromWindows();") !=
                std::string_view::npos,
        "the top-level control window synchronizes taskbar and system-panel theme changes");
    Check(controlHandler.find("case WM_THEMECHANGED:") !=
            std::string_view::npos &&
            controlHandler.find("RefreshSystemTaskbarAppearance(false);") !=
                std::string_view::npos,
        "the top-level control window refreshes taskbar visuals for system themes");

    if (failures == 0)
        std::cout << "Shell integration contract tests passed\n";
    return failures == 0 ? 0 : 1;
}
