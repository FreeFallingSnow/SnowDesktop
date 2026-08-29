#include "settings_window_open_rules.h"

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
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
}

int main(int argc, char** argv)
{
    using snowdesktop::settings_window_open_rules::RequestState;

    RequestState state;
    Check(!state.Pending(), "new state has no pending request");

    state.Request();
    Check(state.Pending() && state.RetryCount() == 0,
        "request becomes pending and resets retry count");
    Check(state.Route().page == snowdesktop::SettingsPage::General,
        "default request targets the legacy General settings page");
    Check(state.RecordFailure(3) && state.RetryCount() == 1,
        "first failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 2,
        "second failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 3,
        "third failure schedules the final retry");
    Check(!state.RecordFailure(3) && state.Pending(),
        "retry exhaustion preserves the pending request");

    snowdesktop::SettingsRoute widgetRoute;
    widgetRoute.page = snowdesktop::SettingsPage::WidgetSettings;
    widgetRoute.widgetInstanceId = L"widget-1";
    state.Request(widgetRoute);
    Check(state.Pending() && state.RetryCount() == 0,
        "a new user request restores the retry budget");
    Check(state.Route() == widgetRoute,
        "a replacement request retains its typed route across retries");
    state.MarkShown();
    Check(!state.Pending() && state.RetryCount() == 0,
        "successful display clears pending state and retries");
    Check(!state.RecordFailure(3),
        "completed requests cannot schedule retries");

    Check(argc == 2, "source root argument is provided");
    if (argc == 2)
    {
        const std::string source = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "settings_window.cpp");
        const std::string header = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "settings_window.h");
        const std::string host = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "winui" /
                "settings_window_host.cpp");
        const std::string appRun = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_run.cpp");
        const std::string appSettings = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_settings_apply.cpp");
        const std::string dockTracking = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_dock_window_tracking.cpp");
        const std::string dockControl = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_dock_window_control.cpp");
        const std::string floatingPopup = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_floating_popup_window.cpp");
        const std::string tray = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_tray.cpp");
        Check(!source.empty(), "settings window source is readable");
        Check(!header.empty() && !host.empty() && !appRun.empty() &&
                !appSettings.empty() && !dockTracking.empty() &&
                !dockControl.empty() && !floatingPopup.empty(),
            "WinUI settings facade, host, and message pump are readable");
        const std::size_t oleInitialize = appRun.find(
            "const HRESULT oleInitializeResult = OleInitialize(nullptr);");
        const std::size_t oleFailure = appRun.find(
            "if (FAILED(oleInitializeResult))", oleInitialize);
        const std::size_t oleFailureReturn = appRun.find(
            "return __LINE__;", oleFailure);
        const std::size_t oleGuard = appRun.find(
            "struct OleUninitializeOnExit final", oleFailureReturn);
        const std::size_t schedulerInitialize = appRun.find(
            "uiAnimationScheduler_.Initialize()", oleGuard);
        Check(oleInitialize != std::string::npos &&
                oleFailure != std::string::npos &&
                oleFailureReturn != std::string::npos &&
                oleGuard != std::string::npos &&
                schedulerInitialize != std::string::npos &&
                oleInitialize < oleFailure &&
                oleFailure < oleFailureReturn &&
                oleFailureReturn < oleGuard &&
                oleGuard < schedulerInitialize,
            "STA initialization failure returns before later application or WinUI startup");
        const std::size_t oleUninitialize =
            appRun.find("OleUninitialize();");
        Check(oleUninitialize != std::string::npos &&
                oleUninitialize == appRun.rfind("OleUninitialize();") &&
                oleGuard < oleUninitialize &&
                appRun.find("OleUninitializeOnExit oleUninitializeOnExit") !=
                    std::string::npos,
            "only a successfully initialized STA installs one scoped OleUninitialize");
        Check(source.find("ImGui") == std::string::npos &&
                source.find("ID3D11") == std::string::npos &&
                source.find("IDXGISwapChain") == std::string::npos &&
                header.find("Render()") == std::string::npos &&
                header.find("NeedsRender()") == std::string::npos,
            "the settings facade owns no ImGui, D3D, swap chain, or frame renderer");
        Check(source.find("SettingsWindow::Open(") != std::string::npos &&
                source.find("CanonicalizeSettingsRoute(route)") !=
                    std::string::npos &&
                source.find("SettingsPage::Home") != std::string::npos &&
                source.find("SettingsPage::General") !=
                    std::string::npos &&
                source.find("SettingsPage::Dock, \"dock.enable\"") !=
                    std::string::npos &&
                source.find("SettingsPage::DockAndTaskbar") ==
                    std::string::npos &&
                source.find("SettingsPage::Personalization") !=
                    std::string::npos &&
                source.find("SettingsRoute::ForWidget(widgetId)") !=
                    std::string::npos,
            "compatibility entry points canonicalize legacy routes and use current typed destinations");
        Check(header.find("HWND Window() const noexcept") !=
                    std::string::npos &&
                source.find("HWND SettingsWindow::Window() const noexcept") !=
                    std::string::npos &&
                source.find("impl_->host->Window()") !=
                    std::string::npos,
            "the settings facade exposes its lazy application-level HWND for window classification");
        Check(appSettings.find(
                  "bool DesktopApp::IsSettingsApplicationWindow(") !=
                    std::string::npos &&
                appSettings.find("settingsWindow_->Window()") !=
                    std::string::npos &&
                appSettings.find("GetAncestor(window, GA_ROOTOWNER)") !=
                    std::string::npos &&
                dockTracking.find("IsSettingsApplicationWindow(window)") !=
                    std::string::npos &&
                dockTracking.find("IsTaskWindowProcessEligible(") !=
                    std::string::npos &&
                dockControl.find("IsSettingsApplicationWindow(window)") !=
                    std::string::npos &&
                dockControl.find("IsTaskWindowProcessEligible(") !=
                    std::string::npos &&
                floatingPopup.find("IsSettingsApplicationWindow(window)") !=
                    std::string::npos &&
                floatingPopup.find("IsInternalPointerTarget(") !=
                    std::string::npos,
            "Dock discovery, previews, and outside-click dismissal share the settings application-window identity");
        const std::size_t markSettingsShown =
            appSettings.find("settingsWindowOpenRequest_.MarkShown();");
        const std::size_t refreshDockAfterShow =
            appSettings.find("RefreshDockRunningWindows();",
                markSettingsShown);
        const std::size_t logSettingsShown =
            appSettings.find("SettingsWindow shown",
                refreshDockAfterShow);
        Check(markSettingsShown != std::string::npos &&
                refreshDockAfterShow != std::string::npos &&
                logSettingsShown != std::string::npos &&
                markSettingsShown < refreshDockAfterShow &&
                refreshDockAfterShow < logSettingsShown,
            "showing settings refreshes the Dock immediately after the window becomes visible");
        Check(source.find("bool EnsureInitialized()") !=
                    std::string::npos &&
                source.find("auto candidate =") != std::string::npos &&
                source.find("host = std::move(candidate)") !=
                    std::string::npos &&
                source.find("return impl_->EnsureInitialized() &&") !=
                    std::string::npos,
            "each failed lazy initialization is retried with a newly constructed WinUI host");
        Check(host.find("shell->ReleaseSessionResources();") !=
                    std::string::npos &&
                host.find("QueueViewRelease();") != std::string::npos &&
                host.find("owner->ReleaseSessionView();") !=
                    std::string::npos &&
                host.find("runtime.Detach();") != std::string::npos &&
                host.find("releaseAfterClose") == std::string::npos &&
                source.find("ReleaseClosedHost") == std::string::npos,
            "a successful close asynchronously releases route resources while retaining the process WinUI runtime, root view, and host");
        Check(appRun.find("ensureWidgetSettingsInstance") !=
                    std::string::npos &&
                appRun.find("widgetEngine_->EnsureWidgetLoaded(") !=
                    std::string::npos,
            "the application supplies persisted instance loading before widget settings navigation");
        Check(!tray.empty() &&
                tray.find("!settingsWindow_->ShowExitConfirm()") !=
                    std::string::npos &&
                tray.find("RequestExit();") != std::string::npos,
            "tray exit falls back safely when the WinUI confirmation cannot be shown");
        Check(host.find("controller->CloseSession()") !=
                    std::string::npos &&
                appRun.find("settingsWindow_->PreTranslateMessage(&msg)") !=
                    std::string::npos &&
                appRun.find("settingsWindow_->ProcessTabNavigation(&msg)") !=
                    std::string::npos &&
                appRun.find("settingsWindow_->Render()") ==
                    std::string::npos,
            "the reusable WinUI host flushes on close and participates in the native message pump");

        const std::string pageGridSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_page_grid.cpp");
        const std::size_t previewBegin = pageGridSource.find(
            "void DesktopApp::PreviewIconSpacing(float value)");
        const std::size_t previewEnd = pageGridSource.find(
            "void DesktopApp::SetIconSpacing(float value)", previewBegin);
        const std::string preview = previewBegin == std::string::npos ||
                previewEnd == std::string::npos
            ? std::string{}
            : pageGridSource.substr(previewBegin, previewEnd - previewBegin);
        const std::size_t widgetBounds = preview.find(
            "widget.bounds = GetGridRect(");
        const std::size_t invalidateSlots = preview.find(
            "widgetContainer->InvalidateSlots();");
        const std::size_t synchronousPaint = preview.find(
            "PresentDesktopPointerUpdate();");
        Check(!pageGridSource.empty(), "page-grid source is readable");
        Check(widgetBounds != std::string::npos &&
                invalidateSlots != std::string::npos &&
                synchronousPaint != std::string::npos &&
                widgetBounds < invalidateSlots &&
                invalidateSlots < synchronousPaint,
            "layout-spacing preview refreshes widget item geometry before synchronous painting");

        const std::size_t iconSizePreviewBegin = pageGridSource.find(
            "void DesktopApp::PreviewItemIconSize(float value)");
        const std::size_t iconSizePreviewEnd = pageGridSource.find(
            "void DesktopApp::SetItemIconSize(float value)",
            iconSizePreviewBegin);
        const std::string iconSizePreview =
            iconSizePreviewBegin == std::string::npos ||
                iconSizePreviewEnd == std::string::npos
            ? std::string{}
            : pageGridSource.substr(iconSizePreviewBegin,
                iconSizePreviewEnd - iconSizePreviewBegin);
        const std::size_t refreshSlots = iconSizePreview.find(
            "container->InvalidateSlots();");
        const std::size_t reserveDock = iconSizePreview.find(
            "ApplyDockWorkAreaReservation();");
        const std::size_t synchronizeDock = iconSizePreview.find(
            "SynchronizeDockContainerAreas()");
        const std::size_t paintIconSize = iconSizePreview.find(
            "PresentDesktopPointerUpdate();");
        Check(reserveDock != std::string::npos &&
                synchronizeDock != std::string::npos &&
                reserveDock < synchronizeDock &&
                synchronizeDock < refreshSlots &&
                refreshSlots != std::string::npos &&
                paintIconSize != std::string::npos &&
                refreshSlots < paintIconSize,
            "icon-size preview synchronizes the Dock reservation and shared "
            "item geometry before synchronous painting");
    }

    if (failures == 0)
        std::cout << "All settings window open rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
