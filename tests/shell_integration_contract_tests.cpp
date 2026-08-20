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
    return contents.str();
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
    const std::string settingsWindow = ReadFile(
        root / "src" / "settings_window.cpp");
    const std::string messageDispatch = ReadFile(
        root / "src" / "app" / "app_message_dispatch.cpp");
    const std::string controlDispatch = ReadFile(
        root / "src" / "app" / "app_desktop_reload.cpp");
    Check(!utils.empty() && !lifecycle.empty() && !settingsApply.empty() &&
            !dockSettings.empty() && !settingsWindow.empty() &&
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
            std::string_view::npos,
        "taskbar state is not mirrored while Explorer is unavailable");

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

    const std::string_view systemTaskbarPage = FunctionBody(settingsWindow,
        "void SettingsWindow::DrawSystemTaskbarPage()",
        "void SettingsWindow::DrawDisplayPage()");
    Check(systemTaskbarPage.find("RequestWindowsSystemLightThemeEnabled") !=
            std::string_view::npos &&
            systemTaskbarPage.find("RequestSystemTaskbarAlignmentCentered") !=
                std::string_view::npos &&
            systemTaskbarPage.find("RequestSystemTaskbarAutoHideEnabled") !=
                std::string_view::npos,
        "taskbar and Shell panel controls enqueue system changes from the UI");
    Check(systemTaskbarPage.find("SendMessageTimeoutW") ==
            std::string_view::npos,
        "the settings UI does not perform a blocking Shell broadcast");

    const std::size_t taskbarThemeBegin = settingsWindow.find(
        "int taskbarThemeMode;");
    const std::size_t systemTaskbarPageCall = settingsWindow.find(
        "DrawSystemTaskbarPage();", taskbarThemeBegin);
    const std::string_view taskbarAppearance =
        taskbarThemeBegin == std::string::npos ||
            systemTaskbarPageCall == std::string::npos
        ? std::string_view{}
        : std::string_view(settingsWindow).substr(
            taskbarThemeBegin,
            systemTaskbarPageCall - taskbarThemeBegin);
    const std::size_t nativeForegroundGuard = taskbarAppearance.find(
        "if (taskbarThemeMode != 0)");
    const std::size_t mainForegroundLabel = taskbarAppearance.find(
        "app.settings.taskbar_foreground_color");
    Check(nativeForegroundGuard != std::string_view::npos &&
            mainForegroundLabel != std::string_view::npos &&
            nativeForegroundGuard < mainForegroundLabel,
        "Windows-native taskbar mode hides the inactive foreground control");
    Check(taskbarAppearance.find("app.settings.widget_content_theme") ==
            std::string_view::npos,
        "taskbar controls do not reuse the ambiguous widget theme label");

    const std::size_t dynamicRuleBegin = taskbarAppearance.find(
        "auto drawDynamicTaskbarRule");
    const std::string_view dynamicRule = dynamicRuleBegin ==
            std::string_view::npos
        ? std::string_view{}
        : taskbarAppearance.substr(dynamicRuleBegin);
    const std::size_t dynamicNativeGuard = dynamicRule.find(
        "if (rule.themeMode != SystemTaskbarThemeMode::Native)");
    const std::size_t dynamicForegroundLabel = dynamicRule.find(
        "app.settings.taskbar_foreground_color");
    Check(dynamicNativeGuard != std::string_view::npos &&
            dynamicForegroundLabel != std::string_view::npos &&
            dynamicNativeGuard < dynamicForegroundLabel,
        "Windows-native dynamic rules hide their inactive foreground control");

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
            controlHandler.find("SyncSystemTaskbarSettingsFromWindows();") !=
                std::string_view::npos,
        "the top-level control window synchronizes external taskbar changes");
    Check(controlHandler.find("case WM_THEMECHANGED:") !=
            std::string_view::npos &&
            controlHandler.find("RefreshSystemTaskbarAppearance(false);") !=
                std::string_view::npos,
        "the top-level control window refreshes taskbar visuals for system themes");

    if (failures == 0)
        std::cout << "Shell integration contract tests passed\n";
    return failures == 0 ? 0 : 1;
}
