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
    const std::size_t staticSearchBegin = source.find(
        "constexpr StaticSearchDefinition kStaticSearchDefinitions[]");
    const std::size_t staticSearchEnd = source.find(
        "\n};", staticSearchBegin);
    const std::string_view staticSearchDefinitions =
        staticSearchBegin != std::string::npos &&
                staticSearchEnd != std::string::npos
            ? std::string_view(source).substr(
                staticSearchBegin, staticSearchEnd - staticSearchBegin)
            : std::string_view{};
    const auto staticSearchMapsTo = [&staticSearchDefinitions](
        std::string_view focusId, std::string_view pageName) {
        const std::string focusToken = "\"" + std::string(focusId) + "\"";
        const std::size_t focus = staticSearchDefinitions.find(focusToken);
        if (focus == std::string_view::npos)
            return false;
        const std::size_t entry = staticSearchDefinitions.rfind(
            "{SettingsPage::", focus);
        const std::size_t pageEnd = staticSearchDefinitions.find(',', entry);
        if (entry == std::string_view::npos ||
            pageEnd == std::string_view::npos || pageEnd > focus)
        {
            return false;
        }
        return staticSearchDefinitions.substr(
                   entry + std::string_view("{SettingsPage::").size(),
                   pageEnd - entry -
                       std::string_view("{SettingsPage::").size()) ==
            pageName;
    };
    const auto pageContextMapsTo = [&source](
        std::string_view pageName, std::string_view localizationKey) {
        const std::string caseToken =
            "case SettingsPage::" + std::string(pageName) + ":";
        const std::size_t begin = source.find(caseToken);
        if (begin == std::string::npos)
            return false;
        const std::size_t end = source.find(
            "case SettingsPage::", begin + caseToken.size());
        const std::string expectedReturn =
            "return L(\"" + std::string(localizationKey) + "\")";
        return std::string_view(source).substr(
                   begin, end == std::string::npos ? end : end - begin)
                .find(expectedReturn) != std::string_view::npos;
    };
    const std::size_t integratedTitleBarBegin =
        shellMarkup.find("x:Name=\"IntegratedTitleBarHost\"");
    const std::size_t integratedTitleBarEnd = shellMarkup.find(
        "<NavigationView", integratedTitleBarBegin);
    const std::string_view integratedTitleBarMarkup =
        integratedTitleBarBegin != std::string::npos &&
                integratedTitleBarEnd != std::string::npos
        ? std::string_view(shellMarkup).substr(
            integratedTitleBarBegin,
            integratedTitleBarEnd - integratedTitleBarBegin)
        : std::string_view{};
    const std::size_t titleBarIdentityBegin =
        integratedTitleBarMarkup.find(
            "x:Name=\"IntegratedTitleBarIdentity\"");
    const std::size_t paneHeaderBegin =
        shellMarkup.find("<NavigationView.PaneHeader>");
    const std::size_t paneHeaderEnd = shellMarkup.find(
        "</NavigationView.PaneHeader>", paneHeaderBegin);
    const std::string_view paneHeaderMarkup =
        paneHeaderBegin != std::string::npos &&
                paneHeaderEnd != std::string::npos
        ? std::string_view(shellMarkup).substr(
            paneHeaderBegin, paneHeaderEnd - paneHeaderBegin)
        : std::string_view{};
    const std::size_t paneCustomContentBegin =
        shellMarkup.find("<NavigationView.PaneCustomContent>");
    const std::size_t paneCustomContentEnd = shellMarkup.find(
        "</NavigationView.PaneCustomContent>", paneCustomContentBegin);
    const std::string_view paneCustomContentMarkup =
        paneCustomContentBegin != std::string::npos &&
                paneCustomContentEnd != std::string::npos
        ? std::string_view(shellMarkup).substr(
            paneCustomContentBegin,
            paneCustomContentEnd - paneCustomContentBegin)
        : std::string_view{};
    const std::size_t navigationRootBegin =
        shellMarkup.find("x:Name=\"NavigationRoot\"");
    const std::size_t navigationRootEnd = shellMarkup.find(
        "<NavigationView.PaneHeader>", navigationRootBegin);
    const std::string_view navigationRootMarkup =
        navigationRootBegin != std::string::npos &&
                navigationRootEnd != std::string::npos
        ? std::string_view(shellMarkup).substr(
            navigationRootBegin,
            navigationRootEnd - navigationRootBegin)
        : std::string_view{};
    const std::size_t pageScrollerBegin =
        shellMarkup.find("x:Name=\"PageScrollViewer\"");
    const std::size_t pageScrollerEnd = shellMarkup.find(
        "</ScrollViewer>", pageScrollerBegin);
    const std::string_view pageScrollerMarkup =
        pageScrollerBegin != std::string::npos &&
                pageScrollerEnd != std::string::npos
        ? std::string_view(shellMarkup).substr(
            pageScrollerBegin, pageScrollerEnd - pageScrollerBegin)
        : std::string_view{};

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
        "the reusable Win32 top-level HWND delegates its full measured client area, including the integrated title-bar row, to DesktopWindowXamlSource");
    Check(source.find("WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN") !=
                std::string::npos &&
            header.find("platform default") !=
                std::string::npos &&
            source.find("muw::AppWindow::GetFromWindowId") !=
                std::string::npos &&
            source.find("ExtendsContentIntoTitleBar(true)") !=
                std::string::npos &&
            source.find("muw::TitleBarHeightOption::Standard") !=
                std::string::npos &&
            source.find("muw::TitleBarHeightOption::Tall") ==
                std::string::npos &&
            source.find("muw::IconShowOptions::HideIconAndSystemMenu") !=
                std::string::npos &&
            source.find("SetDragRectangles(") ==
                std::string::npos &&
            source.find("InputNonClientPointerSource") ==
                std::string::npos &&
            source.find("SetRegionRects(") == std::string::npos &&
            source.find("platform-owned default drag region") !=
                std::string::npos &&
            source.find("GetDpiForWindow(window)") !=
                std::string::npos &&
            source.find("ButtonBackgroundColor(transparent)") !=
                std::string::npos &&
            source.find("ButtonInactiveBackgroundColor(transparent)") !=
                std::string::npos &&
            source.find("ButtonHoverBackgroundColor(hover)") !=
                std::string::npos &&
            source.find("ButtonPressedBackgroundColor(pressed)") !=
                std::string::npos &&
            source.find("WM_NCHITTEST") == std::string::npos &&
            source.find("WM_NCCALCSIZE") == std::string::npos &&
            shellMarkup.find("x:Name=\"MinimizeButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"MaximizeButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"CloseButton\"") ==
                std::string::npos,
        "AppWindow keeps its platform default drag region and system caption buttons over a non-interactive XAML identity surface without custom non-client rectangles");
    Check(shellMarkup.find("x:Name=\"IntegratedTitleBarHost\"") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"IntegratedTitleBarText\"") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"IntegratedTitleBarIcon\"") !=
                std::string::npos &&
            shellMarkup.find("x:Name=\"TitleBarBackButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"TitleBarPaneToggleButton\"") ==
                std::string::npos &&
            shellMarkup.find("x:Name=\"TitleBarDragRegion\"") ==
                std::string::npos &&
            shellMarkup.find("TitleBarLeftInsetColumn") !=
                std::string::npos &&
            shellMarkup.find("TitleBarRightInsetColumn") !=
                std::string::npos &&
            shellMarkup.find("<NavigationView.PaneHeader>") !=
                std::string::npos &&
            shellMarkup.find("<NavigationView.AutoSuggestBox>") ==
                std::string::npos &&
            Count(shellMarkup, "x:Name=\"SettingsSearchBox\"") == 1 &&
            shellMarkup.find("<NavigationView.Template>") ==
                std::string::npos &&
            shellMarkup.find("IsBackButtonVisible=\"Collapsed\"") !=
                std::string::npos &&
            shellMarkup.find("IsPaneToggleButtonVisible=\"True\"") !=
                std::string::npos &&
            shellHeader.find("SetIntegratedTitleBarInsets") !=
                std::string::npos &&
            shellHeader.find("IntegratedTitleBarDragRectangles") ==
                std::string::npos &&
            shellHeader.find("IntegratedTitleBarLayoutChangedCallback") ==
                std::string::npos &&
            shellHeader.find("titleBarBackToken_") ==
                std::string::npos &&
            shellHeader.find("titleBarPaneToggleToken_") ==
                std::string::npos &&
            shell.find("TitleBarBackButton().Click") == std::string::npos &&
            shell.find("TitleBarPaneToggleButton().Click") ==
                std::string::npos &&
            shell.find(
              "IntegratedTitleBarText().Text(Localize(\"app.settings.title\"))") !=
                std::string::npos &&
            shell.find("NavigationRoot().PaneTitle(shellTitle)") ==
                std::string::npos,
        "the standard XAML caption shows the localized app identity while the built-in NavigationView toggle and one PaneHeader search box share the navigation row without a custom template");
    Check(integratedTitleBarMarkup.find("Height=\"32\"") !=
                std::string_view::npos &&
            integratedTitleBarMarkup.find("Height=\"48\"") ==
                std::string_view::npos &&
            Count(integratedTitleBarMarkup, "<Button x:Name=") == 0 &&
            integratedTitleBarMarkup.find("<TextBlock") !=
                std::string_view::npos &&
            integratedTitleBarMarkup.find("<Image") !=
                std::string_view::npos &&
            integratedTitleBarMarkup.find(
              "Source=\"ms-appx:///Assets/App/SnowDesktop.png\"") !=
                std::string_view::npos &&
            integratedTitleBarMarkup.find("<AutoSuggestBox") ==
                std::string_view::npos &&
            integratedTitleBarMarkup.find("x:Name=\"TitleBarDragRegion\"") ==
                std::string_view::npos &&
            integratedTitleBarMarkup.find(
              "x:Name=\"IntegratedTitleBarIdentity\"") !=
                std::string_view::npos &&
            integratedTitleBarMarkup.find(
              "IsHitTestVisible=\"False\"", titleBarIdentityBegin) !=
                std::string_view::npos &&
            paneHeaderMarkup.find(
              "HorizontalAlignment=\"Stretch\"") !=
                std::string_view::npos &&
            paneHeaderMarkup.find("x:Name=\"SettingsSearchBox\"") !=
                std::string_view::npos &&
            paneHeaderMarkup.find("x:Name=\"ClearSearchButton\"") ==
                std::string_view::npos &&
            paneCustomContentMarkup.find(
              "x:Name=\"CompactSearchButton\"") !=
                std::string_view::npos &&
            paneCustomContentMarkup.find("Width=\"48\"") !=
                std::string_view::npos &&
            paneCustomContentMarkup.find(
              "Style=\"{ThemeResource NavigationViewPaneSearchButtonStyle}\"") !=
                std::string_view::npos,
        "the expanded pane aligns its unique search box beside the built-in toggle while a platform-styled compact search button preserves the closed-rail affordance");
    Check(shellHeader.find("OpenPaneAndFocusSearch()") !=
                std::string::npos &&
            shellHeader.find("focusSearchWhenPaneOpens_") !=
                std::string::npos &&
            shellHeader.find("compactSearchButtonClickToken_") !=
                std::string::npos &&
            shellHeader.find("paneOpenedToken_") !=
                std::string::npos &&
            shell.find("CompactSearchButton().Click(") !=
                std::string::npos &&
            shell.find("NavigationRoot().PaneOpening(") !=
                std::string::npos &&
            shell.find("NavigationRoot().PaneOpened(") !=
                std::string::npos &&
            shell.find("NavigationRoot().PaneClosed(") !=
                std::string::npos &&
            shell.find("NavigationRoot().DisplayModeChanged(") !=
                std::string::npos &&
            shell.find("NavigationRoot().IsPaneOpen(true);") !=
                std::string::npos &&
            shell.find(
              "SettingsSearchBox().Focus(mux::FocusState::Keyboard)") !=
                std::string::npos &&
            shell.find(
              "CompactSearchButton().Click(compactSearchButtonClickToken_);") !=
                std::string::npos &&
            shell.find(
              "NavigationRoot().PaneOpening(paneOpeningToken_);") !=
                std::string::npos &&
            shell.find("NavigationRoot().PaneOpened(paneOpenedToken_);") !=
                std::string::npos &&
            shell.find("NavigationRoot().PaneClosed(paneClosedToken_);") !=
                std::string::npos &&
            shell.find("compactSearchButtonClickToken_ = {};") !=
                std::string::npos &&
            shell.find("paneOpeningToken_ = {};") !=
                std::string::npos &&
            shell.find("paneOpenedToken_ = {};") !=
                std::string::npos &&
            shell.find("paneClosedToken_ = {};") !=
                std::string::npos &&
            shell.find("navigationDisplayModeChangedToken_ = {};") !=
                std::string::npos,
        "compact search and Ctrl+F focus the unique search box after pane opening, and every added lifetime handler is unhooked during shutdown");
    Check(shellMarkup.find(
              "Background=\"{ThemeResource ApplicationPageBackgroundThemeBrush}\"") !=
                std::string::npos &&
            integratedTitleBarMarkup.find(
              "Background=\"Transparent\"") !=
                std::string_view::npos &&
            navigationRootMarkup.find(
              "Background=\"Transparent\"") !=
                std::string_view::npos &&
            navigationRootMarkup.find(
              "x:Key=\"NavigationViewDefaultPaneBackground\"") ==
                std::string_view::npos &&
            navigationRootMarkup.find(
              "x:Key=\"NavigationViewExpandedPaneBackground\"") ==
                std::string_view::npos,
        "the shared shell remains backdrop-aware while NavigationView retains its theme-provided opaque overlay pane fills");
    Check(source.find("constexpr int kMinimumClientWidth = 840;") !=
                std::string::npos &&
            source.find("constexpr int kMinimumClientHeight = 520;") !=
                std::string::npos &&
            shellMarkup.find("PaneDisplayMode=\"Auto\"") !=
                std::string::npos &&
            shellMarkup.find("CompactModeThresholdWidth=\"720\"") !=
                std::string::npos &&
            shellMarkup.find("CompactPaneLength=\"48\"") !=
                std::string::npos &&
            shellMarkup.find("ExpandedModeThresholdWidth=\"1008\"") !=
                std::string::npos &&
            shellMarkup.find("AdaptiveTrigger MinWindowWidth=\"0\"") !=
                std::string::npos &&
            shellMarkup.find("AdaptiveTrigger MinWindowWidth=\"1008\"") !=
                std::string::npos &&
            shellMarkup.find(
              "Target=\"PageSurface.Margin\" Value=\"20,12,20,32\"") !=
                std::string::npos &&
            shellMarkup.find(
              "Target=\"PageSurface.Margin\" Value=\"40,16,40,48\"") !=
                std::string::npos &&
            shellMarkup.find("Target=\"TitleBarBackButton.Width\"") ==
                std::string::npos &&
            shellMarkup.find("PaneDisplayMode=\"Left\"") ==
                std::string::npos &&
            shellMarkup.find("IsPaneOpen=\"True\"") ==
                std::string::npos,
        "the minimum host size is retained while NavigationView and page margins adapt between compact and wide window states");
    Check(pageScrollerMarkup.find(
              "MaxWidth=\"1200\"") !=
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "HorizontalAlignment=\"Stretch\"") !=
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "HorizontalContentAlignment=\"Stretch\"") !=
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "HorizontalScrollBarVisibility=\"Disabled\"") !=
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "x:Name=\"PageSurface\"") !=
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "Margin=\"40,16,40,48\"") !=
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "HorizontalContentAlignment=\"Center\"") ==
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "HorizontalScrollMode=\"Disabled\"") ==
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "MaxWidth=\"1120\"") !=
                std::string_view::npos &&
            pageScrollerMarkup.find(
              "HorizontalAlignment=\"Center\"") ==
                std::string_view::npos &&
            pageScrollerMarkup.find("<Grid>") ==
                std::string_view::npos,
        "the viewport owns the centered width cap while dynamic page content remains stretched and free of desired-width feedback");
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
            shell.find(
              "IntegratedTitleBarText().Text(Localize(\"app.settings.title\"))") !=
                std::string::npos,
        "Windows owns caption activation visuals while the drag-only XAML caption presents the localized app identity without custom activation state");
    Check(source.find("case WM_ACTIVATEAPP:") != std::string::npos &&
            source.find("QueueExternalStateRefresh();") !=
                std::string::npos &&
            source.find("kRefreshExternalStateMessage") !=
                std::string::npos &&
            source.find("RefreshExternalStateNow();") !=
                std::string::npos &&
            shellHeader.find("RefreshRuntimeState() noexcept") !=
                std::string::npos,
        "reactivating Settings refreshes Windows-owned state without taking over non-client activation visuals");
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
            source.find("appWindowTitleBar.PreferredTheme") !=
                std::string::npos &&
            source.find("muw::TitleBarTheme::Dark") !=
                std::string::npos &&
            source.find("muw::TitleBarTheme::Light") !=
                std::string::npos,
        "theme and contrast changes coordinate DWM material and the AppWindow-owned caption-button theme");

    Check(source.find("\"desktop.tabFontSize\"") ==
                std::string::npos &&
            source.find("\"desktop.categoryRules\"") !=
                std::string::npos &&
            source.find("\"app.settings.category_font_size\"") ==
                std::string::npos &&
            source.find("\"app.settings.category_rules\"") !=
                std::string::npos &&
            shell.find("\"desktop.tabFontSize\"") ==
                std::string::npos &&
            shell.find("\"desktop.categoryLayout\"") !=
                std::string::npos &&
            shell.find("\"desktop.categoryRules\"") !=
                std::string::npos,
        "retired category font size is absent from search and focus registration while visible category layout and rules remain addressable");
    Check(!staticSearchDefinitions.empty() &&
            staticSearchMapsTo(
                "personalization.theme", "AppearanceTheme") &&
            staticSearchMapsTo(
                "personalization.contextMenu", "AppearanceTheme") &&
            staticSearchMapsTo(
                "personalization.backgroundColor", "AppearanceTheme") &&
            staticSearchMapsTo(
                "personalization.cornerRadius", "AppearanceWidgets") &&
            staticSearchMapsTo(
                "desktop.categoryLayout", "AppearanceWidgets") &&
            staticSearchMapsTo(
                "desktop.spacing", "AppearanceDesktopIcons") &&
            staticSearchMapsTo(
                "desktop.shortcutArrow", "AppearanceDesktopIcons") &&
            staticSearchMapsTo("desktop.iconBeautify",
                "AppearanceIconBeautification") &&
            staticSearchMapsTo("desktop.iconBeautify.outlineColor",
                "AppearanceIconBeautification") &&
            staticSearchMapsTo(
                "desktop.categoryCounts", "DesktopCategories") &&
            staticSearchMapsTo(
                "desktop.categoryRules", "DesktopCategories"),
        "production search definitions route each Appearance leaf and keep category behavior with Categories");
    Check(staticSearchMapsTo(
              "dock.allowDesktopContentOverlap", "Dock") &&
            staticSearchMapsTo("dock.showOnlyWhenSummoned", "Dock") &&
            !staticSearchMapsTo("dock.autoHide", "Dock") &&
            shell.find("\"dock.allowDesktopContentOverlap\"") !=
                std::string::npos &&
            shell.find("\"dock.showOnlyWhenSummoned\"") !=
                std::string::npos &&
            shell.find("\"dock.autoHide\"") == std::string::npos,
        "Dock overlap and summon-only display are searchable and register stable focus targets");
    Check(pageContextMapsTo(
              "AppearanceTheme", "settings.personalization.theme") &&
            pageContextMapsTo(
              "AppearanceWidgets", "settings.personalization.widgets") &&
            pageContextMapsTo(
              "AppearanceDesktopIcons", "app.settings.desktop_icons") &&
            pageContextMapsTo("AppearanceIconBeautification",
              "app.settings.icon_beautify"),
        "Appearance search results expose their localized leaf-page context");
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
    const std::size_t nonClientLeave =
        source.find("case WM_NCMOUSELEAVE:");
    const std::size_t dwmNonClientLeave = source.find(
        "DwmDefWindowProc(", nonClientLeave);
    const std::size_t closeMessage = source.find(
        "case WM_CLOSE:", nonClientLeave);
    Check(nonClientLeave != std::string::npos &&
            dwmNonClientLeave != std::string::npos &&
            closeMessage != std::string::npos &&
            nonClientLeave < dwmNonClientLeave &&
            dwmNonClientLeave < closeMessage,
        "system-generated non-client leave reaches DWM for caption-button hover cleanup");
    const std::size_t hideWindowBegin = source.find(
        "[[nodiscard]] bool HideWindow()");
    const std::size_t hideWindowEnd = source.find(
        "SettingsWindowHost::SettingsWindowHost()", hideWindowBegin);
    const std::string_view hideWindow =
        hideWindowBegin != std::string::npos &&
                hideWindowEnd != std::string::npos
            ? std::string_view(source).substr(
                hideWindowBegin, hideWindowEnd - hideWindowBegin)
            : std::string_view{};
    const std::size_t hideReusableWindow = hideWindow.find(
        "ShowWindow(window, SW_HIDE)");
    const std::size_t resetHiddenTitleBar = hideWindow.find(
        "ResetIntegratedTitleBar()");
    const std::size_t queueViewRelease = hideWindow.find(
        "QueueViewRelease()");
    Check(hideReusableWindow != std::string_view::npos &&
            resetHiddenTitleBar != std::string_view::npos &&
            queueViewRelease != std::string_view::npos &&
            hideWindow.find("shell->Close()") == std::string_view::npos &&
            hideWindow.find("runtime.Detach()") == std::string_view::npos &&
            hideReusableWindow < resetHiddenTitleBar &&
            resetHiddenTitleBar < queueViewRelease,
        "WM_CLOSE hides the reusable HWND and defers XAML view teardown beyond its input stack");

    const std::size_t releaseViewBegin = source.find(
        "void ReleaseView() noexcept");
    const std::size_t releaseViewEnd = source.find(
        "void QueueViewRelease() noexcept", releaseViewBegin);
    const std::string_view releaseView =
        releaseViewBegin != std::string::npos &&
                releaseViewEnd != std::string::npos
            ? std::string_view(source).substr(
                releaseViewBegin, releaseViewEnd - releaseViewBegin)
            : std::string_view{};
    Check(releaseView.find("shell->Close()") != std::string_view::npos &&
            releaseView.find("runtime.Detach()") !=
                std::string_view::npos &&
            releaseView.find("searchIndex = {}") !=
                std::string_view::npos &&
            releaseView.find("runtime.Shutdown()") ==
                std::string_view::npos &&
            releaseView.find("DestroyWindow(") ==
                std::string_view::npos &&
            releaseView.find("callbacks->alive.store(false)") ==
                std::string_view::npos &&
            releaseView.find("shell->Close()") <
                releaseView.find("runtime.Detach()"),
        "deferred view teardown releases the Shell, Island, and search index while preserving the HWND, callbacks, and process runtime");

    const std::size_t queueReleaseBegin = source.find(
        "void QueueViewRelease() noexcept");
    const std::size_t queueReleaseEnd = source.find(
        "[[nodiscard]] bool CreateView()", queueReleaseBegin);
    const std::string_view queueRelease =
        queueReleaseBegin != std::string::npos &&
                queueReleaseEnd != std::string::npos
            ? std::string_view(source).substr(
                queueReleaseBegin, queueReleaseEnd - queueReleaseBegin)
            : std::string_view{};
    Check(queueRelease.find("dispatcher.TryEnqueue") !=
                std::string_view::npos &&
            queueRelease.find("expectedEpoch") !=
                std::string_view::npos &&
            queueRelease.find("owner->Visible()") !=
                std::string_view::npos &&
            queueRelease.find("owner->ReleaseView()") !=
                std::string_view::npos,
        "view teardown runs on a later owner DispatcherQueue turn and rejects stale or reopened sessions");
    const std::size_t reopenWindowBegin = source.find(
        "bool SettingsWindowHost::Open(");
    const std::size_t reopenWindowEnd = source.find(
        "bool SettingsWindowHost::Hide()", reopenWindowBegin);
    const std::string_view reopenWindow =
        reopenWindowBegin != std::string::npos &&
                reopenWindowEnd != std::string::npos
            ? std::string_view(source).substr(
                reopenWindowBegin, reopenWindowEnd - reopenWindowBegin)
            : std::string_view{};
    const std::size_t missingTitleBarGuard = reopenWindow.find(
        "!impl_->appWindowTitleBar");
    const std::size_t recreateView = reopenWindow.find(
        "impl_->CreateView()");
    const std::size_t reconfigureTitleBar = reopenWindow.find(
        "impl_->ConfigureIntegratedTitleBar()");
    const std::size_t refreshTitleBarInsets = reopenWindow.find(
        "impl_->QueueIntegratedTitleBarInsetsUpdate()");
    const std::size_t showReopenedWindow = reopenWindow.find(
        "ShowWindow(impl_->window");
    Check(recreateView != std::string_view::npos &&
            missingTitleBarGuard != std::string_view::npos &&
            reconfigureTitleBar != std::string_view::npos &&
            refreshTitleBarInsets != std::string_view::npos &&
            showReopenedWindow != std::string_view::npos &&
            recreateView < missingTitleBarGuard &&
            missingTitleBarGuard < reconfigureTitleBar &&
            reconfigureTitleBar < refreshTitleBarInsets &&
            refreshTitleBarInsets < showReopenedWindow,
        "reopening rebuilds the XAML view and caption customization on the retained settings HWND");
    Check(source.find("QueueIntegratedTitleBarUpdate(true)") ==
                std::string::npos &&
            source.find("bool force = false") == std::string::npos &&
            source.find("QueueIntegratedTitleBarInsetsUpdate()") !=
                std::string::npos &&
            source.find("SetDragRectangles(") == std::string::npos &&
            shell.find("TitleBarDragRegion().Loaded(") ==
                std::string::npos &&
            shell.find("TitleBarDragRegion().SizeChanged(") ==
                std::string::npos,
        "caption drag ownership no longer depends on hidden-window XAML layout or a post-show timing override; only caption-button insets are refreshed");
    Check(source.find("case WM_SIZE:") == std::string::npos &&
            source.find("CS_HREDRAW | CS_VREDRAW") == std::string::npos &&
            shellHeader.find("integratedTitleBarLeftInset_") !=
                std::string::npos &&
            shellHeader.find("integratedTitleBarRightInset_") !=
                std::string::npos &&
            shell.find("leftInset != integratedTitleBarLeftInset_") !=
                std::string::npos &&
            shell.find("rightInset != integratedTitleBarRightInset_") !=
                std::string::npos,
        "live resize avoids redundant caption-inset layout writes and full-window class redraws");
    const std::size_t shutdownStart = source.find(
        "void SettingsWindowHost::Shutdown() noexcept");
    const std::size_t shutdownEnd = source.find(
        "bool SettingsWindowHost::Open(", shutdownStart);
    const std::string shutdown = shutdownStart == std::string::npos ||
            shutdownEnd == std::string::npos
        ? std::string{}
        : source.substr(shutdownStart, shutdownEnd - shutdownStart);
    Check(shutdown.find("impl_->controller") != std::string::npos &&
            shutdown.find("impl_->FlushPendingChanges()") !=
                std::string::npos &&
            source.find("shell->ReleaseSessionResources();") !=
                std::string::npos &&
            shutdown.find("impl_->ReleaseView();") !=
                std::string::npos,
        "ordinary shutdown flushes pending settings and reuses the same view-release boundary before process runtime shutdown");
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
        "Detach clears the Island material while the integrated top-level frame uses the matching DWM system backdrop with a contrast fallback");
    const std::size_t shutdownBegin = source.find(
        "void SettingsWindowHost::Shutdown() noexcept");
    const std::size_t openBegin = source.find(
        "bool SettingsWindowHost::Open(", shutdownBegin);
    const std::string_view shutdownFunction =
        shutdownBegin != std::string::npos && openBegin != std::string::npos
        ? std::string_view(source).substr(
            shutdownBegin, openBegin - shutdownBegin)
        : std::string_view{};
    Check(shutdownFunction.find("SetActualThemeChangedCallback({})") !=
                std::string_view::npos &&
            shutdownFunction.find("callbacks->alive.store(false)") !=
                std::string_view::npos &&
            shutdownFunction.find("impl_->ReleaseView();") !=
                std::string_view::npos &&
            shutdownFunction.find("DestroyWindow(") !=
                std::string_view::npos &&
            shutdownFunction.find("impl_->runtime.Shutdown()") !=
                std::string_view::npos &&
            shutdownFunction.find("SetActualThemeChangedCallback({})") <
                shutdownFunction.find("callbacks->alive.store(false)") &&
            shutdownFunction.find("callbacks->alive.store(false)") <
                shutdownFunction.find("impl_->ReleaseView();") &&
            shutdownFunction.find("impl_->ReleaseView();") <
                shutdownFunction.find("DestroyWindow(") &&
            shutdownFunction.find("DestroyWindow(") <
                shutdownFunction.find("impl_->runtime.Shutdown()") &&
            releaseView.find("ResetIntegratedTitleBar()") !=
                std::string_view::npos,
        "final shutdown invalidates callbacks, releases the view, destroys the HWND, and only then stops the process WinUI runtime");
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
    const std::size_t applyWidgetSnapshotBegin = shell.find(
        "bool SettingsShell::ApplyWidgetSettingsSnapshot(");
    const std::size_t setWidgetsActionsBegin = shell.find(
        "void SettingsShell::SetWidgetsPageActions(",
        applyWidgetSnapshotBegin);
    const std::string_view applyWidgetSnapshotFunction =
        applyWidgetSnapshotBegin != std::string::npos &&
                setWidgetsActionsBegin != std::string::npos
            ? std::string_view(shell).substr(
                  applyWidgetSnapshotBegin,
                  setWidgetsActionsBegin - applyWidgetSnapshotBegin)
            : std::string_view{};
    Check(applyWidgetSnapshotFunction.find("const bool alreadyApplied") !=
                std::string_view::npos &&
            applyWidgetSnapshotFunction.find(
                "widgetSettingsPage_->WidgetId() == snapshot.widgetId") !=
                std::string_view::npos &&
            applyWidgetSnapshotFunction.find(
                "widgetSettingsPage_->Generation() == snapshot.generation") !=
                std::string_view::npos &&
            applyWidgetSnapshotFunction.find(
                "widgetSettingsPage_->Revision() == snapshot.revision") !=
                std::string_view::npos &&
            applyWidgetSnapshotFunction.find(
                "!alreadyApplied && !widgetSettingsPage_->ApplySnapshot(snapshot)") !=
                std::string_view::npos,
        "reopening a component route accepts the exact snapshot already bound while constructing its presenter");
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
    const auto refreshPresenter = developerToggle.find(
        "state->owner->widgetsPageBackend->Refresh()", appliedCheck);
    const auto refreshVisibility = developerToggle.find(
        "state->owner->RebuildSearchIndex()", refreshPresenter);
    const auto returnApplied = developerToggle.find(
        "return applied", refreshVisibility);
    Check(appliedCheck != std::string::npos &&
            refreshPresenter != std::string::npos &&
            refreshVisibility != std::string::npos &&
            returnApplied != std::string::npos,
        "Developer Tools toggles refresh the cached presenter and conditional navigation before navigation or another click");
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
