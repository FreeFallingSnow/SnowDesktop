#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../src/winui/settings_titlebar_policy.h"

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::size_t Count(std::string_view text, std::string_view token)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(token, position)) != std::string_view::npos)
    {
        ++count;
        position += token.size();
    }
    return count;
}

void TestTitleBarPolicy()
{
    using snowdesktop::winui::SettingsTitleBarPolicyInput;
    using snowdesktop::winui::ShouldUseIntegratedSettingsTitleBar;

    Check(ShouldUseIntegratedSettingsTitleBar(
              SettingsTitleBarPolicyInput{true, true, false, true}),
        "Windows 11 uses the integrated title bar only when all safety probes pass");
    Check(!ShouldUseIntegratedSettingsTitleBar(
              SettingsTitleBarPolicyInput{false, true, false, true}),
        "Windows 10 retains the complete native title bar");
    Check(!ShouldUseIntegratedSettingsTitleBar(
              SettingsTitleBarPolicyInput{true, false, false, true}),
        "a failed high-contrast query retains the native title bar");
    Check(!ShouldUseIntegratedSettingsTitleBar(
              SettingsTitleBarPolicyInput{true, true, true, true}),
        "high contrast retains the complete native title bar");
    Check(!ShouldUseIntegratedSettingsTitleBar(
              SettingsTitleBarPolicyInput{true, true, false, false}),
        "unsupported AppWindow customization retains the native title bar");
}

void TestHostContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/settings_window_host.h");
    const std::string source = ReadText(
        repository / "src/winui/settings_window_host.cpp");
    const std::string runtimeHeader = ReadText(
        repository / "src/winui/winui_runtime.h");
    const std::string runtime = ReadText(
        repository / "src/winui/winui_runtime.cpp");
    const std::string shellMarkup = ReadText(
        repository / "src/winui/SettingsShell.xaml");
    const std::string shellHeader = ReadText(
        repository / "src/winui/SettingsShell.xaml.h");
    const std::string shell = ReadText(
        repository / "src/winui/SettingsShell.xaml.cpp");

    Check(!header.empty() && !source.empty() && !runtimeHeader.empty() &&
            !runtime.empty() && !shellMarkup.empty() &&
            !shellHeader.empty() && !shell.empty(),
        "WinUI settings host contract sources are readable");
    Check(source.find("DesktopWindowXamlSource") == std::string::npos &&
            source.find("runtime.Attach(impl_->window") !=
                std::string::npos &&
            runtime.find("muxh::DesktopWindowXamlSource xamlSource") !=
                std::string::npos &&
            runtime.find("GetClientRect(impl_->parentWindow, &client)") !=
                std::string::npos &&
            source.find("WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN") !=
                std::string::npos,
        "the reusable Win32 top-level HWND delegates only its measured client area to DesktopWindowXamlSource");
    Check(source.find("WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN") !=
                std::string::npos &&
            header.find("AppWindowTitleBar extends the client") !=
                std::string::npos &&
            source.find("Microsoft.UI.Windowing") != std::string::npos &&
            source.find("GetWindowIdFromWindow(window)") !=
                std::string::npos &&
            source.find("AppWindow::GetFromWindowId(windowId)") !=
                std::string::npos &&
            source.find("appWindow.AssociateWithDispatcherQueue(dispatcher)") !=
                std::string::npos &&
            source.find("AppWindowTitleBar::IsCustomizationSupported()") !=
                std::string::npos &&
            source.find("ExtendsContentIntoTitleBar(true)") !=
                std::string::npos &&
            source.find("ExtendsContentIntoTitleBar())") !=
                std::string::npos &&
            source.find("WM_NCHITTEST") == std::string::npos &&
            source.find("WM_NCCALCSIZE") == std::string::npos &&
            source.find("SetTitleBar(") == std::string::npos &&
            source.find("SetDragRectangles") == std::string::npos &&
            runtime.find("SetTitleBar(") == std::string::npos &&
            shellMarkup.find("x:Name=\"MinimizeButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"MaximizeButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"CloseButton\"") ==
                std::string::npos,
        "AppWindow overlays the system caption buttons and Snap on the client without custom caption hit testing");
    Check(source.find("const HWND titleBarWindow = window;") !=
                std::string::npos &&
            source.find("[titleBarWindow](const muw::AppWindow&") !=
                std::string::npos &&
            source.find("[this](const muw::AppWindow&") ==
                std::string::npos &&
            source.find("PostMessageW(titleBarWindow,") !=
                std::string::npos,
        "AppWindow changes post back to the owner HWND without capturing host lifetime state");
    Check(shellMarkup.find("x:Name=\"IntegratedTitleBarHost\"") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"IntegratedTitleBarText\"") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"TitleBarLeftInsetColumn\"") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"TitleBarRightInsetColumn\"") !=
                std::string::npos &&
            shellMarkup.find("IsHitTestVisible=\"False\"") !=
                std::string::npos &&
            shellMarkup.find("IsTitleBarAutoPaddingEnabled=\"False\"") !=
                std::string::npos &&
            shellHeader.find("SetIntegratedTitleBarLayout(") !=
                std::string::npos &&
            shell.find("static_cast<double>(heightPixels) / scale") !=
                std::string::npos &&
            shell.find("static_cast<double>(leftInsetPixels) / scale") !=
                std::string::npos &&
            shell.find("static_cast<double>(rightInsetPixels) / scale") !=
                std::string::npos &&
            shell.find("if (foreground)") != std::string::npos &&
            shell.find("IntegratedTitleBarText().Foreground(foreground)") !=
                std::string::npos &&
            shell.find("IntegratedTitleBarText().Foreground(muxm::Brush{nullptr})") ==
                std::string::npos,
        "the non-interactive XAML title row mirrors AppWindow pixel metrics at the XamlRoot scale");
    Check(source.find("constexpr int kMinimumClientWidth = 840;") !=
                std::string::npos &&
            source.find("constexpr int kMinimumClientHeight = 520;") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"NarrowState\"") ==
                std::string::npos &&
            shellMarkup.find("AdaptiveTrigger") == std::string::npos,
        "the host enforces the supported fixed single-line settings layout instead of stacking text and controls");
    Check(source.find("case WM_GETMINMAXINFO:") != std::string::npos &&
            source.find("AdjustWindowRectExForDpi(&minimumBounds") !=
                std::string::npos &&
            source.find("GetWindowLongPtrW(hwnd, GWL_STYLE)") !=
                std::string::npos &&
            source.find("GetWindowLongPtrW(hwnd, GWL_EXSTYLE)") !=
                std::string::npos &&
            source.find("minimumBounds.right - minimumBounds.left") !=
                std::string::npos &&
            source.find("minimumBounds.bottom - minimumBounds.top") !=
                std::string::npos,
        "minimum client DIPs are converted to complete DPI-aware tracking dimensions");
    Check(shellHeader.find("ActualThemeChangedCallback") !=
                std::string::npos &&
            shellHeader.find("SetActualThemeChangedCallback(") !=
                std::string::npos &&
            shell.find("ShellRoot().ActualThemeChanged(") !=
                std::string::npos &&
            shell.find("ShellRoot().ActualTheme() == mux::ElementTheme::Dark") !=
                std::string::npos &&
            source.find("shell->SetActualThemeChangedCallback(") !=
                std::string::npos &&
            source.find("state->owner->ApplyActualTheme(darkTheme)") !=
                std::string::npos &&
            source.find("darkTheme = isDark;") != std::string::npos &&
            source.find(
                "darkTheme = snapshot->values.personalization.contentTheme") ==
                std::string::npos,
        "the native HWND chrome follows ShellRoot ActualTheme instead of content appearance settings");
    Check(source.find("case WM_ACTIVATE:") != std::string::npos &&
            source.find("LOWORD(wParam) != WA_INACTIVE") !=
                std::string::npos &&
            source.find("UpdateIntegratedTitleBarActivationVisual") !=
                std::string::npos &&
            shellHeader.find("SetIntegratedTitleBarWindowActive") !=
                std::string::npos &&
            shell.find("TextFillColorSecondaryBrush") != std::string::npos,
        "the XAML title text mirrors native active and inactive caption state");
    Check(source.find("ApplySettingsWindowChrome(window, darkTheme)") !=
                std::string::npos &&
            source.find("DWMWA_USE_IMMERSIVE_DARK_MODE") !=
                std::string::npos &&
            source.find("QueryHighContrastEnabled(highContrast)") !=
                std::string::npos &&
            source.find("IsWindows11OrGreater()") != std::string::npos &&
            source.find("ShouldUseIntegratedSettingsTitleBar({") !=
                std::string::npos &&
            source.find("appWindowTitleBar.PreferredTheme(") !=
                std::string::npos &&
            source.find("appWindowTitleBar.ButtonBackgroundColor(transparent)") !=
                std::string::npos &&
            source.find("appWindowTitleBar.ButtonInactiveBackgroundColor(transparent)") !=
                std::string::npos &&
            source.find("appWindowTitleBar.BackgroundColor(transparent)") ==
                std::string::npos &&
            source.find("appWindowTitleBar.InactiveBackgroundColor(transparent)") ==
                std::string::npos,
        "theme changes integrate only supported caption-button backgrounds while preserving native hover, pressed, and Close colors");
    Check(source.find("appWindowTitleBar.ResetToDefault()") !=
                std::string::npos &&
            source.find("ReconcileIntegratedTitleBar();") !=
                std::string::npos &&
            source.find("case WM_SETTINGCHANGE:") != std::string::npos &&
            source.find("case WM_THEMECHANGED:") != std::string::npos &&
            source.find("case WM_DPICHANGED:") != std::string::npos &&
            source.find("case kRefreshIntegratedTitleBarMessage:") !=
                std::string::npos &&
            source.find("ConfigureIntegratedTitleBar();") !=
                std::string::npos &&
            source.find("SupportsMicaBackdrop()") != std::string::npos,
        "title-bar policy, metrics, and safe native fallback reconcile independently from the optional Mica backdrop");
    Check(source.find("ImGui") == std::string::npos &&
            source.find("ID3D11") == std::string::npos &&
            source.find("IDXGISwapChain") == std::string::npos &&
            source.find("Present(") == std::string::npos,
        "the new settings host has no ImGui, D3D, swap-chain, or manual-present path");

    const std::size_t commitBegin = source.find(
        "bool CommitRoute(const SettingsRoute& route");
    const std::size_t commitEnd = source.find(
        "void RequestRoute(const SettingsRoute& route)", commitBegin);
    const std::string_view commitFunction =
        commitBegin != std::string::npos && commitEnd != std::string::npos
        ? std::string_view(source).substr(
            commitBegin, commitEnd - commitBegin)
        : std::string_view{};
    Check(Count(commitFunction, "controller->Open(route)") == 1 &&
            source.find("impl_->CommitRoute(route, &openResult)") !=
                std::string::npos &&
            source.find("CommitRoute(route, &result)") !=
                std::string::npos,
        "external and in-window routes share one authoritative controller commit");
    Check(commitFunction.find("ensureWidgetSettingsInstance") !=
                std::string::npos &&
            commitFunction.find("widgetSettingsService->Load(") !=
                std::string::npos &&
            commitFunction.find(
                "current->generation != loaded.snapshot->generation") !=
                std::string::npos &&
            commitFunction.find("ApplyWidgetSettingsSnapshot(") !=
                std::string::npos,
        "widget routes load the instance and validate the exact presenter snapshot before activation");
    Check(source.find("controller->CloseSession()") != std::string::npos &&
            source.find("FlushPendingChanges()") != std::string::npos &&
            source.find("shell->FlushPendingWidgetSettings()") !=
                std::string::npos &&
            source.find("widgetSettingsService->CloseAll()") !=
                std::string::npos &&
            source.find("ShowWindow(window, SW_HIDE)") !=
                std::string::npos,
        "closing flushes the controller and widget sessions before hiding");
    Check(source.find(
              "if (impl_->initialized && impl_->OnOwnerThread() && impl_->controller)") !=
                std::string::npos,
        "host shutdown attempts a final component and controller flush");
    Check(source.find("viewEpoch") != std::string::npos &&
            source.find("expectedEpoch") != std::string::npos &&
            source.find("DispatcherQueue") != std::string::npos &&
            source.find("latestSnapshot") != std::string::npos,
        "snapshot and async work are coalesced on the DispatcherQueue and gated by view epoch");

    Check(runtime.find("GetAncestor(target, GA_ROOTOWNER)") !=
                std::string::npos &&
            runtime.find("!IsChild(impl_->parentWindow, target)") !=
                std::string::npos &&
            runtime.find("return ::ContentPreTranslateMessage(message)") !=
                std::string::npos,
        "WinUI message preprocessing is restricted to the settings HWND tree");

    const std::size_t attachBegin = runtime.find(
        "bool WinUiRuntime::Attach(");
    const std::size_t detachBegin = runtime.find(
        "void WinUiRuntime::Detach()", attachBegin);
    const std::size_t backdropSetterBegin = runtime.find(
        "bool WinUiRuntime::SetSystemBackdropEnabled(", detachBegin);
    const std::size_t resizeBegin = runtime.find(
        "void WinUiRuntime::ResizeToClient()", backdropSetterBegin);
    const std::string_view attachFunction =
        attachBegin != std::string::npos && detachBegin != std::string::npos
        ? std::string_view(runtime).substr(
            attachBegin, detachBegin - attachBegin)
        : std::string_view{};
    const std::string_view detachFunction =
        detachBegin != std::string::npos &&
            backdropSetterBegin != std::string::npos
        ? std::string_view(runtime).substr(
            detachBegin, backdropSetterBegin - detachBegin)
        : std::string_view{};
    const std::string_view backdropSetter =
        backdropSetterBegin != std::string::npos &&
            resizeBegin != std::string::npos
        ? std::string_view(runtime).substr(
            backdropSetterBegin, resizeBegin - backdropSetterBegin)
        : std::string_view{};
    Check(runtimeHeader.find("SetSystemBackdropEnabled(bool enabled)") !=
                std::string::npos &&
            attachFunction.find("SystemBackdrop(") ==
                std::string_view::npos &&
            source.find(
                "PostMessageW(window, kApplyXamlBackdropMessage") !=
                std::string::npos &&
            source.find("case kApplyXamlBackdropMessage:") !=
                std::string::npos &&
            backdropSetter.find(
                "xamlSource.SystemBackdrop(muxm::MicaBackdrop{})") !=
                std::string_view::npos,
        "the Island creates Mica only from a posted host message after Attach returns");
    Check(detachFunction.find("xamlSource.SystemBackdrop(") !=
                std::string_view::npos &&
            detachFunction.find("xamlSource.Content(nullptr)") !=
                std::string_view::npos &&
            detachFunction.find("xamlSource.SystemBackdrop(") <
                detachFunction.find("xamlSource.Content(nullptr)") &&
            source.find("DWMWA_SYSTEMBACKDROP_TYPE") ==
                std::string::npos &&
            source.find("DwmExtendFrameIntoClientArea") ==
                std::string::npos,
        "Detach clears the Island backdrop and the HWND does not install a competing client backdrop");
    const std::size_t shutdownBegin = source.find(
        "void SettingsWindowHost::Shutdown() noexcept");
    const std::size_t openBegin = source.find(
        "bool SettingsWindowHost::Open(", shutdownBegin);
    const std::string_view shutdownFunction =
        shutdownBegin != std::string::npos && openBegin != std::string::npos
        ? std::string_view(source).substr(
            shutdownBegin, openBegin - shutdownBegin)
        : std::string_view{};
    Check(shutdownFunction.find("ResetIntegratedTitleBar()") !=
                std::string_view::npos &&
            shutdownFunction.find("SetActualThemeChangedCallback({})") !=
                std::string_view::npos &&
            shutdownFunction.find("callbacks->alive.store(false)") !=
                std::string_view::npos &&
            shutdownFunction.find("shell->Close()") !=
                std::string_view::npos &&
            shutdownFunction.find("runtime.Detach()") !=
                std::string_view::npos &&
            shutdownFunction.find("DestroyWindow(") !=
                std::string_view::npos &&
            shutdownFunction.find("SetActualThemeChangedCallback({})") <
                shutdownFunction.find("callbacks->alive.store(false)") &&
            shutdownFunction.find("ResetIntegratedTitleBar()") <
                shutdownFunction.find("shell->Close()") &&
            shutdownFunction.find("shell->Close()") <
                shutdownFunction.find("runtime.Detach()") &&
            shutdownFunction.find("runtime.Detach()") <
                shutdownFunction.find("DestroyWindow("),
        "the AppWindow title bar and Shell callbacks reset before the XAML Island and native overlapped HWND are destroyed");
    Check(source.find("QueryHighContrastEnabled(highContrast)") !=
                std::string::npos &&
            source.find("SupportsMicaBackdrop()") != std::string::npos &&
            shellHeader.find("SetSystemBackdropActive(bool active)") !=
                std::string::npos &&
            shell.find("ShellRoot().Background(muxm::Brush{nullptr})") !=
                std::string::npos &&
            shell.find("ApplicationPageBackgroundThemeBrush") !=
                std::string::npos &&
            shell.find("GetSysColor(COLOR_WINDOW)") !=
                std::string::npos &&
            shellMarkup.find(
                "Background=\"{ThemeResource ApplicationPageBackgroundThemeBrush}\"") !=
                std::string::npos,
        "Mica exposes a transparent ShellRoot while Windows 10, high contrast, and failures retain a solid theme brush");
    Check(shellHeader.find("SuspendInteraction()") != std::string::npos &&
            shellHeader.find("ResumeInteraction()") != std::string::npos &&
            shell.find("generalPage_->Deactivate()") !=
                std::string::npos &&
            shell.find("RenderPageCards(true)") != std::string::npos,
        "hidden settings sessions suspend controls and rebind them when reopened");
    Check(shellHeader.find("SetWidgetSettingsService(") !=
                std::string::npos &&
            shellHeader.find("ApplyWidgetSettingsSnapshot(") !=
                std::string::npos &&
            shell.find("widgetSettingsPage_->EventDispatchers()") !=
                std::string::npos &&
            shell.find("widgetSettingsPage_->Content()") !=
                std::string::npos &&
            shell.find("widgetSettingsPage_->Deactivate()") !=
                std::string::npos &&
            source.find("ApplyWidgetSettingsSnapshot(") !=
                std::string::npos,
        "widget settings snapshots, service events, native content, and close flush are wired through the WinUI shell");
    Check(shellHeader.find("ApplyWidgetsPageSnapshot(") !=
                std::string::npos &&
            shellHeader.find("ApplyBackupDataPageSnapshot(") !=
                std::string::npos &&
            shell.find("widgetsPage_->Content()") !=
                std::string::npos &&
            shell.find("backupDataPage_->Content()") !=
                std::string::npos &&
            shell.find("widgetsPage_->Activate(") !=
                std::string::npos &&
            shell.find("backupDataPage_->Activate()") !=
                std::string::npos,
        "widget management and backup routes render cached native WinUI presenters driven by immutable snapshots");

    Check(shellHeader.find("ShowWidgetPermissionEditor(") !=
                std::string::npos &&
            shell.find("ShowWidgetPermissionEditorAsync(") !=
                std::string::npos &&
            shell.find("dialog.SecondaryButtonText") !=
                std::string::npos &&
            shell.find("WidgetPermissionEditorAction::Apply") !=
                std::string::npos &&
            shell.find("WidgetPermissionEditorAction::Revoke") !=
                std::string::npos &&
            shell.find("navigation_.Route() != route") !=
                std::string::npos &&
            source.find("actions.editPermissions") !=
                std::string::npos,
        "the Shell owns the batch permission ContentDialog and drops stale route results");

    Check(shellHeader.find("ShowWidgetInstallConfirmation(") !=
                std::string::npos &&
            shell.find("ShowWidgetInstallConfirmationAsync(") !=
                std::string::npos &&
            shell.find("WidgetInstallConfirmationReasonKind::NewPermission") !=
                std::string::npos &&
            shell.find("WidgetInstallConfirmationReasonKind::NewWebsite") !=
                std::string::npos &&
            shell.find("WidgetInstallConfirmationReasonKind::SourceChange") !=
                std::string::npos &&
            shell.find("app.settings.widgets_new_permission") !=
                std::string::npos &&
            shell.find("app.settings.widgets_new_website") !=
                std::string::npos &&
            shell.find("app.settings.widgets_source_change") !=
                std::string::npos &&
            shell.find("app.settings.widgets_technical_details") !=
                std::string::npos &&
            shell.find("muxc::Expander technicalDetails") !=
                std::string::npos &&
            shell.find("technicalDetails.HorizontalAlignment(") !=
                std::string::npos &&
            shell.find("technicalDetails.HorizontalContentAlignment(") !=
                std::string::npos &&
            source.find("shell->ShowWidgetInstallConfirmation(") !=
                std::string::npos,
        "the Shell renders structured install changes and collapsible technical details in its ContentDialog");

    Check(source.find(
              "route.page == SettingsPage::DeveloperTools") !=
                std::string::npos &&
            source.find("route.page == SettingsPage::Debug") !=
                std::string::npos &&
            source.find("options.developerToolsVisible()") !=
                std::string::npos &&
            source.find("options.debugVisible()") !=
                std::string::npos &&
            source.find("configured.diagnosticsVisible") !=
                std::string::npos &&
            source.find("widgetsBackendPage != snapshot.route.page") !=
                std::string::npos &&
            source.find(
                "snapshot.route.page == SettingsPage::Widgets") !=
                std::string::npos &&
            source.find(
                "SettingsHostActions::Action::ReloadWidgetInstance") !=
                std::string::npos &&
            shell.find("widgetsPage_->DeveloperToolsContent()") !=
                std::string::npos &&
            shell.find("homeAboutPage_->DebugContent()") !=
                std::string::npos,
        "conditional pages validate independent gates while Debug restores its legacy presenter");
}
} // namespace

int main(int argc, char** argv)
{
    TestTitleBarPolicy();
    Check(argc == 2,
        "source root is supplied for the WinUI settings host contract");
    if (argc == 2)
        TestHostContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures << " WinUI settings host check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI settings host checks passed\n";
    return EXIT_SUCCESS;
}
