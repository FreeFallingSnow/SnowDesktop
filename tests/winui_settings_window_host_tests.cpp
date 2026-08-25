#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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
            header.find("native non-client title bar") !=
                std::string::npos &&
            source.find("Microsoft.UI.Windowing") == std::string::npos &&
            source.find("AppWindow") == std::string::npos &&
            source.find("AppWindowTitleBar") == std::string::npos &&
            source.find("ExtendsContentIntoTitleBar") ==
                std::string::npos &&
            source.find("WM_NCHITTEST") == std::string::npos &&
            source.find("WM_NCCALCSIZE") == std::string::npos &&
            source.find("SetTitleBar(") == std::string::npos &&
            runtime.find("SetTitleBar(") == std::string::npos &&
            shellMarkup.find("x:Name=\"MinimizeButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"MaximizeButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"CloseButton\"") ==
                std::string::npos,
        "WS_OVERLAPPEDWINDOW leaves the native caption, three system buttons, Snap, and accessibility entirely owned by Windows");
    Check(shellMarkup.find("x:Name=\"IntegratedTitleBarHost\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"IntegratedTitleBarText\"") ==
                std::string::npos &&
            shellMarkup.find("TitleBarLeftInsetColumn") ==
                std::string::npos &&
            shellMarkup.find("TitleBarRightInsetColumn") ==
                std::string::npos &&
            shellMarkup.find("IsTitleBarAutoPaddingEnabled") ==
                std::string::npos &&
            shellHeader.find("SetIntegratedTitleBar") ==
                std::string::npos &&
            shell.find("SetIntegratedTitleBar") == std::string::npos &&
            shell.find("UpdateIntegratedTitleBar") == std::string::npos &&
            shell.find("NavigationRoot().PaneTitle(shellTitle)") !=
                std::string::npos,
        "the Island starts below the native caption and contains no integrated XAML title-bar row or inset bookkeeping");
    Check(source.find("constexpr int kMinimumClientWidth = 840;") !=
                std::string::npos &&
            source.find("constexpr int kMinimumClientHeight = 520;") !=
                std::string::npos &&
            shellMarkup.find("PaneDisplayMode=\"Left\"") !=
                std::string::npos &&
            shellMarkup.find("IsPaneOpen=\"True\"") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"NarrowState\"") ==
                std::string::npos &&
            shellMarkup.find("AdaptiveTrigger") == std::string::npos &&
            shellMarkup.find("CompactModeThresholdWidth") ==
                std::string::npos &&
            shellMarkup.find("ExpandedModeThresholdWidth") ==
                std::string::npos,
        "the host enforces the supported fixed-width settings surface and expanded left navigation without a narrow adaptive state");
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
    Check(source.find("case WM_ACTIVATE:") == std::string::npos &&
            source.find("LOWORD(wParam) != WA_INACTIVE") ==
                std::string::npos &&
            source.find("UpdateIntegratedTitleBarActivationVisual") ==
                std::string::npos &&
            shellHeader.find("SetIntegratedTitleBarWindowActive") ==
                std::string::npos &&
            shell.find("IntegratedTitleBarText") == std::string::npos,
        "caption activation visuals remain native instead of being mirrored into XAML");
    Check(source.find("ApplySettingsWindowChrome(window, darkTheme)") !=
                std::string::npos &&
            source.find("DWMWA_USE_IMMERSIVE_DARK_MODE") !=
                std::string::npos &&
            source.find("DWMWA_SYSTEMBACKDROP_TYPE") !=
                std::string::npos &&
            source.find("DWMSBT_MAINWINDOW") != std::string::npos &&
            source.find("DWMSBT_NONE") != std::string::npos &&
            source.find("SupportsDwmSystemBackdrop()") !=
                std::string::npos &&
            source.find("version.dwBuildNumber") != std::string::npos &&
            source.find(">= 22621") != std::string::npos &&
            source.find("DWMWA_CAPTION_COLOR") != std::string::npos &&
            source.find("DWMWA_COLOR_DEFAULT") != std::string::npos &&
            source.find("QueryHighContrastEnabled(highContrast)") !=
                std::string::npos &&
            source.find("AppWindow") == std::string::npos &&
            source.find("TitleBarTheme") == std::string::npos,
        "theme and contrast changes coordinate native DWM caption material while Windows retains title-bar layout and ownership");

    Check(source.find("\"desktop.tabFontSize\"") !=
                std::string::npos &&
            source.find("\"desktop.categoryRules\"") !=
                std::string::npos &&
            source.find("\"app.settings.category_font_size\"") !=
                std::string::npos &&
            source.find("\"app.settings.category_rules\"") !=
                std::string::npos &&
            shell.find("\"desktop.fontWeight\", "
                       "\"desktop.tabFontSize\"") !=
                std::string::npos &&
            shell.find("\"desktop.categoryRules\"") !=
                std::string::npos,
        "visible category font-size and rule controls are both searchable and registered as focus targets");
    Check(source.find("ImGui") == std::string::npos &&
            source.find("ID3D11") == std::string::npos &&
            source.find("IDXGISwapChain") == std::string::npos &&
            source.find("Present(") == std::string::npos,
        "the new settings host has no ImGui, D3D, swap-chain, or manual-present path");

    const std::size_t pendingFlushBegin = source.find(
        "void QueuePendingFlush()");
    const std::size_t pendingFlushEnd = source.find(
        "void FlushPendingNow()", pendingFlushBegin);
    const std::string_view pendingFlushFunction =
        pendingFlushBegin != std::string::npos &&
            pendingFlushEnd != std::string::npos
        ? std::string_view(source).substr(
            pendingFlushBegin, pendingFlushEnd - pendingFlushBegin)
        : std::string_view{};
    Check(pendingFlushFunction.find("flushQueued.exchange(true)") !=
                std::string_view::npos &&
            pendingFlushFunction.find("flushQueued.store(false)") !=
                std::string_view::npos &&
            pendingFlushFunction.find("FlushPendingNow()") !=
                std::string_view::npos &&
            pendingFlushFunction.find("expectedEpoch") ==
                std::string_view::npos &&
            pendingFlushFunction.find("viewEpoch") ==
                std::string_view::npos &&
            source.find("++impl_->viewEpoch;") != std::string::npos,
        "controller pending work survives a visible-window Open that advances only the rendered-view epoch");

    const std::size_t flushNowBegin = source.find(
        "void FlushPendingNow()", pendingFlushEnd);
    const std::size_t flushNowEnd = source.find(
        "std::wstring BackupConfirmationMessage", flushNowBegin);
    const std::string_view flushNowFunction =
        flushNowBegin != std::string::npos && flushNowEnd != std::string::npos
        ? std::string_view(source).substr(
            flushNowBegin, flushNowEnd - flushNowBegin)
        : std::string_view{};
    Check(flushNowFunction.find("controller->FlushPending()") !=
                std::string_view::npos &&
            flushNowFunction.find("ShowActionError(result)") !=
                std::string_view::npos &&
            flushNowFunction.find("RefreshLocalizedPresentation()") ==
                std::string_view::npos,
        "coalesced preview and commit work does not rewrite localized XAML while a continuous control owns pointer or flyout interaction");

    const std::size_t snapshotQueueBegin = source.find(
        "void QueueSnapshot(SettingsController::SnapshotPtr snapshot)");
    const std::size_t snapshotQueueEnd = source.find(
        "void ApplySnapshotNow", snapshotQueueBegin);
    const std::string_view snapshotQueueFunction =
        snapshotQueueBegin != std::string::npos &&
            snapshotQueueEnd != std::string::npos
        ? std::string_view(source).substr(
            snapshotQueueBegin, snapshotQueueEnd - snapshotQueueBegin)
        : std::string_view{};
    Check(snapshotQueueFunction.find("snapshotQueued.exchange(true)") !=
                std::string_view::npos &&
            snapshotQueueFunction.find("latestSnapshot") !=
                std::string_view::npos &&
            snapshotQueueFunction.find("ApplySnapshotNow") !=
                std::string_view::npos &&
            snapshotQueueFunction.find("expectedEpoch") ==
                std::string_view::npos &&
            snapshotQueueFunction.find("viewEpoch") ==
                std::string_view::npos &&
            source.find("impl_->ApplySnapshotNow(snapshot);") !=
                std::string::npos,
        "immutable revisioned snapshots coalesce independently of view epochs and Open applies the authoritative route immediately");
    const std::size_t applySnapshotBegin = source.find(
        "void ApplySnapshotNow", snapshotQueueBegin);
    const std::size_t pendingWorkBegin = source.find(
        "void QueuePendingFlush()", applySnapshotBegin);
    const std::string_view applySnapshotFunction =
        applySnapshotBegin != std::string::npos &&
                pendingWorkBegin != std::string::npos
            ? std::string_view(source).substr(
                  applySnapshotBegin,
                  pendingWorkBegin - applySnapshotBegin)
            : std::string_view{};
    Check(applySnapshotFunction.find("QueueSystemBackdropUpdate()") ==
                std::string_view::npos &&
            source.find("case WM_THEMECHANGED:") != std::string::npos &&
            source.find("self->QueueSystemBackdropUpdate();") !=
                std::string::npos,
        "ordinary snapshots leave the Island backdrop untouched while system theme and contrast messages may refresh it");

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
        "snapshot and view-scoped async work are coalesced on the DispatcherQueue with their respective stale-result gates");

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
            source.find("DWMWA_SYSTEMBACKDROP_TYPE") !=
                std::string::npos &&
            source.find("DWMSBT_MAINWINDOW") != std::string::npos &&
            source.find("DWMSBT_NONE") != std::string::npos &&
            source.find("DwmExtendFrameIntoClientArea") ==
                std::string::npos,
        "Detach clears the Island material while the untouched native frame uses the matching DWM system backdrop with a contrast fallback");
    const std::size_t shutdownBegin = source.find(
        "void SettingsWindowHost::Shutdown() noexcept");
    const std::size_t openBegin = source.find(
        "bool SettingsWindowHost::Open(", shutdownBegin);
    const std::string_view shutdownFunction =
        shutdownBegin != std::string::npos && openBegin != std::string::npos
        ? std::string_view(source).substr(
            shutdownBegin, openBegin - shutdownBegin)
        : std::string_view{};
    Check(shutdownFunction.find("ResetIntegratedTitleBar()") ==
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
            shutdownFunction.find("shell->Close()") <
                shutdownFunction.find("runtime.Detach()") &&
            shutdownFunction.find("runtime.Detach()") <
                shutdownFunction.find("DestroyWindow("),
        "the Shell callbacks close before the XAML Island and native overlapped HWND are destroyed, with no AppWindow title-bar state to unwind");
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
    const std::size_t languagePrepare = source.find(
        "[[nodiscard]] bool PrepareLanguageChange()");
    const std::size_t languageApply = source.find(
        "void SettingsWindowHost::ApplyLanguageChange(");
    const std::string_view languageFunctions =
        languagePrepare != std::string::npos &&
            languageApply != std::string::npos
        ? std::string_view(source).substr(languagePrepare)
        : std::string_view{};
    Check(header.find("bool PrepareLanguageChange()") !=
                std::string::npos &&
            header.find("ApplyLanguageChange(bool widgetRuntimeReloaded)") !=
                std::string::npos &&
            languageFunctions.find("shell->FlushPendingWidgetSettings()") !=
                std::string_view::npos &&
            languageFunctions.find("widgetSettingsService->Reload(instanceId)") !=
                std::string_view::npos &&
            languageFunctions.find("current->generation != loaded.snapshot->generation") !=
                std::string_view::npos &&
            languageFunctions.find("ApplyWidgetSettingsSnapshot(") !=
                std::string_view::npos &&
            languageFunctions.find("widgetSettingsService->Close(instanceId)") !=
                std::string_view::npos &&
            languageFunctions.find("shell->SuspendInteraction()") !=
                std::string_view::npos &&
            languageFunctions.find("ReloadActiveWidgetSettingsForLanguageChange()") <
                languageFunctions.find("RefreshLocalizedPresentation()"),
        "language changes flush the active editor before runtime replacement and bind the exact localized generation before rebuilding presentation");
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

    Check(source.find("std::wstring primaryButtonText") !=
                std::string::npos &&
            source.find("primaryButtonText.empty()") !=
                std::string::npos &&
            source.find("std::move(primaryButtonText)") !=
                std::string::npos &&
            source.find("settings.dialog.confirm") != std::string::npos,
        "destructive widget confirmations may retain their specific legacy action label while other dialogs keep the generic fallback");

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

    const auto developerToggleStart = source.find(
        "actions.setDeveloperToolsEnabled = [weak]");
    const auto developerToggleEnd = source.find(
        "actions.reloadWidgetInstance = [weak]", developerToggleStart);
    const std::string developerToggle =
        developerToggleStart != std::string::npos &&
            developerToggleEnd != std::string::npos
        ? source.substr(developerToggleStart,
              developerToggleEnd - developerToggleStart)
        : std::string{};
    const auto appliedCheck = developerToggle.find("if (applied)");
    const auto refreshVisibility = developerToggle.find(
        "state->owner->RebuildSearchIndex()", appliedCheck);
    const auto returnApplied = developerToggle.find(
        "return applied", refreshVisibility);
    Check(appliedCheck != std::string::npos &&
            refreshVisibility != std::string::npos &&
            returnApplied != std::string::npos,
        "enabling Developer Tools refreshes conditional navigation and search before the presenter navigates");
}
} // namespace

int main(int argc, char** argv)
{
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
