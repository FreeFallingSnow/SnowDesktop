#include "settings_window_open_rules.h"
#include "onboarding_state.h"
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message);

void CheckOnboarding()
{
    using namespace snowdesktop::onboarding;
    // These are the production state transitions used by initialization, IPC,
    // successful menu creation, committed drops and committed widget geometry.
    State progress;
    Practice practice;
    Check(!progress.Visible() && !progress.welcomePending &&
        !progress.CollectionCreated("old-user-widget", false, true) &&
        !progress.FilesCreated(true) && !practice.Begin(progress, Task::Collection, false),
        "missing eligibility never enrolls an existing user through ordinary actions");
    Check(progress.Initialized() && progress.Visible() && progress.welcomePending,
        "actual initialization enables a pending guide even if native extraction later fails");
    progress.welcomePending = false; // settings opened, not dismissed
    Check(progress.Visible(), "closing the settings window does not dismiss the guide");
    const auto beforePractice = progress;
    Check(practice.Begin(progress, Task::Collection, false) && progress == beforePractice,
        "practice selection changes no completion, target, or persisted state");
    Check(!progress.CollectionCreated("widget-1", false, false) &&
        !progress.CollectionCreated("", false, true) && progress == beforePractice,
        "programmatic creation and a cancelled/failed menu without a created ID cannot finish a task");
    practice.Advance(progress);
    Check(practice.active == Task::Collection, "menu cancellation stays on the add instruction");
    Check(!progress.ApplicationDropped("widget-1", true, true),
        "an unrelated drop before collection creation cannot finish onboarding");
    Check(progress.CollectionCreated("widget-1", false, true) && Completed(progress.steps, Task::Collection),
        "successful menu creation completes only the collection task");
    practice.Advance(progress);
    Check(practice.active == Task::Application && !Completed(progress.steps, Task::Application) &&
        !Completed(progress.steps, Task::Layout), "creation advances the instruction without completing later tasks");
    Check(!progress.ApplicationDropped("widget-2", true, true) &&
        !progress.ApplicationDropped("widget-1", false, true) &&
        !progress.ApplicationDropped("widget-1", true, false),
        "wrong target, ordinary file, and duplicate/reorder cannot complete application ingress");
    Check(progress.ApplicationDropped("widget-1", true, true) && Completed(progress.steps, Task::Application),
        "actual application ingress into the practice collection completes the task");
    practice.Advance(progress);
    Check(practice.active == Task::Layout, "committed application drop advances to geometry practice");
    Check(!progress.GeometryCommitted("widget-2", true, true) &&
        !progress.GeometryCommitted("widget-1", false, false),
        "other collections and cancelled/no-op gestures cannot advance layout practice");
    Check(progress.GeometryCommitted("widget-1", true, false) && !Completed(progress.steps, Task::Layout),
        "moving alone does not finish the combined move/resize task");
    practice.Advance(progress);
    Check(practice.active == Task::Layout, "resize instruction remains after moving alone");
    Check(progress.GeometryCommitted("widget-1", false, true) && Completed(progress.steps, Task::Layout),
        "a separate resize finishes layout practice");
    practice.Advance(progress);
    Check(practice.active == Task::Files && !progress.FilesCreated(false) &&
        !Completed(progress.steps, Task::Files), "desktop files must also come from the real menu");
    Check(progress.FilesCreated(true) && Completed(progress.steps, Task::Files),
        "menu creation completes Files without requiring automatic collection");
    practice.Advance(progress);
    Check(practice.active == Task::Files, "final optional auto-collect instruction remains visible");
    practice.Pause();
    Check(!practice.active && progress.Visible(), "pausing the prompt does not dismiss the guide");
    Check(practice.Begin(progress, Task::Application, false) && practice.active == Task::Collection,
        "missing/grouped/docked practice collection redirects to independent collection creation");
    progress.CollectionCreated("widget-2", true, true);
    Check(progress.collectionId == "widget-1", "unrelated collection creation preserves the practice target");
    progress.CollectionCreated("widget-3", false, true);
    Check(progress.collectionId == "widget-3" && progress.steps == kAllSteps,
        "replacing a missing collection retains learned history");
    Check(progress.Initialized() && progress.steps == kAllSteps && progress.welcomePending,
        "reinitialization requests one new focus while retaining learned steps");
    Check(progress.Dismiss() && !progress.Visible() && !progress.welcomePending,
        "explicit dismissal hides the guide and clears any pending first focus");
    const auto dismissed = progress;
    Check(!progress.Initialized() && progress == dismissed && !practice.Begin(progress, Task::Files, true),
        "permanent dismissal wins over reinitialization and practice requests");
    practice.Advance(progress);
    Check(!practice.active && !progress.CollectionCreated("widget-4", false, true),
        "dismissal ends the live prompt and prevents later events from replacing its target");

    Session session;
    session.regular = progress;
    const auto saved = session.regular;
    session.BeginExperiment();
    Check(session.Current().Visible() && session.Current().welcomePending && session.Current().steps == 0,
        "every debug initialization starts a fresh visible guide even if normal user dismissed it");
    session.Current().CollectionCreated("widget-test", false, true);
    session.Current().Dismiss();
    Check(session.regular == saved, "debug activity and dismissal cannot mutate normal onboarding state");
    session.EndExperiment();
    Check(session.Current() == saved, "ending initialization restores original dismissal and history");
    session.BeginExperiment();
    Check(session.Current().Visible() && session.Current().welcomePending && session.Current().collectionId.empty(),
        "a second experiment replays the complete first-run experience");

    State decoded;
    Check(Decode(Encode(progress), decoded) && decoded == progress, "guide dismissal and progress round-trip");
    Check(!decoded.Initialized() && decoded == dismissed, "restart cannot defeat persisted dismissal");
    const auto beforeBadRead = decoded;
    Check(!Decode("{\"version\":1,\"steps\":999}", decoded) && decoded == beforeBadRead,
        "invalid stored progress cannot replace valid in-memory history");
    Check(Decode(R"({"version":1,"welcomePending":false,"steps":7,"deferred":0,"collectionId":"widget-old"})", decoded) &&
        !decoded.Visible() && !decoded.dismissed && decoded.steps == 7,
        "old shown flag is not dismissal and does not enroll old users from historical operations");
    Check(decoded.Initialized() && decoded.Visible() && decoded.steps == 7,
        "actual reinitialization enables migrated state without erasing progress");
    Check(Decode(R"({"version":1,"welcomePending":true,"steps":1,"deferred":0,"collectionId":"widget-old"})", decoded) &&
        decoded.Visible() && decoded.welcomePending && decoded.steps == 1,
        "a pending old guide migrates as eligible, preserving learned tasks");

    const auto directory = std::filesystem::temp_directory_path() /
        ("SnowDesktop-onboarding-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!std::filesystem::create_directory(directory))
    { Check(false, "isolated onboarding test directory is created"); return; }
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{directory};
    State first;
    Check(Load(directory / "new.json", first) && !first.Visible() && !first.welcomePending,
        "a missing progress file alone never opens or shows onboarding");
    first.Initialized();
    Check(Save(directory / "new.json", first) && Load(directory / "new.json", decoded) && decoded.welcomePending,
        "an initialization offer survives a failed first display and restart");
    first.welcomePending = false;
    first.CollectionCreated("widget-42", false, true);
    Check(Save(directory / "new.json", first) && Load(directory / "new.json", decoded) && decoded == first && decoded.Visible(),
        "ordinary restart retains the panel and progress without reopening settings");
    first.Dismiss();
    Check(Save(directory / "new.json", first) && Load(directory / "new.json", decoded) &&
        !decoded.Initialized() && !decoded.Visible(), "dismissal persists across restart and later initialization");

}

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
    auto result = contents.str();
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
    return result;
}
}

int main(int argc, char** argv)
{
    CheckOnboarding();
    using snowdesktop::settings_window_open_rules::PostOpenAction;
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
    Check(state.MarkShown() == PostOpenAction::None,
        "ordinary completion has no post-open action");
    Check(!state.Pending() && state.RetryCount() == 0,
        "successful display clears pending state and retries");
    Check(!state.RecordFailure(3),
        "completed requests cannot schedule retries");

    state.Request({}, PostOpenAction::ShowExitConfirmation);
    Check(state.RecordFailure(3),
        "post-open actions survive an automatic open retry");
    Check(state.MarkShown() == PostOpenAction::ShowExitConfirmation,
        "successful display returns its deferred exit confirmation action");
    state.Request();
    Check(state.MarkShown() == PostOpenAction::None,
        "a later ordinary request cannot inherit a consumed action");

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
                source.find("canonical.page = snowdesktop::SettingsPage::General") == std::string::npos &&
                source.find("SettingsPage::General") !=
                    std::string::npos &&
                source.find("SettingsPage::Dock, \"dock.enable\"") !=
                    std::string::npos &&
                source.find("SettingsPage::DockAndTaskbar") ==
                    std::string::npos &&
                source.find("SettingsPage::Personalization") !=
                    std::string::npos &&
                source.find("SettingsRoute::ForWidget(") !=
                    std::string::npos,
            "compatibility entry points canonicalize legacy routes and use current typed destinations");
        Check(header.find("HWND Window() const noexcept") !=
                    std::string::npos &&
                source.find("HWND SettingsWindow::Window() const noexcept") !=
                    std::string::npos &&
                source.find("GetWindowThreadProcessId(impl_->window, &owner)") !=
                    std::string::npos &&
                source.find("owner == impl_->process.ProcessId()") != std::string::npos,
            "the settings facade only exposes an HWND owned by its current child process");
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
        const std::size_t openFacadeBegin = source.find(
            "bool SettingsWindow::Open(");
        const std::size_t openFacadeEnd = source.find(
            "bool SettingsWindow::Show()", openFacadeBegin);
        const std::string_view openFacade =
            openFacadeBegin != std::string::npos &&
                    openFacadeEnd != std::string::npos
                ? std::string_view(source).substr(
                      openFacadeBegin, openFacadeEnd - openFacadeBegin)
                : std::string_view{};
        const std::string entry = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "main.cpp");
        const std::string child = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "winui" /
                "settings_process_entry.cpp");
        Check(source.find("std::unique_ptr<winui::SettingsWindowHost>") == std::string::npos &&
                source.find("SettingsWindowHost>();") == std::string::npos &&
                source.find("process.Start(*channel)") != std::string::npos &&
                openFacade.find("ui.open") != std::string_view::npos &&
                child.find("SettingsWindowHost>();") != std::string::npos,
            "only the child entry creates the XAML host; the application facade opens routes over IPC");
        const auto settingsEntry = entry.find("return snowdesktop::settings_ipc::RunSettingsProcess(instance)");
        Check(settingsEntry != std::string::npos &&
                settingsEntry < entry.find("snowdesktop::single_instance::Guard singleInstance") &&
                settingsEntry < entry.find("StartForCurrentProcess(GetCurrentExecutablePath())") &&
                settingsEntry < entry.find("DesktopApp app;"),
            "settings mode returns before owning the desktop, single-instance guard, or crash watchdog");
        Check(child.find("options.sessionClosed =") != std::string::npos &&
                child.find("PostQuitMessage(0)") != std::string::npos &&
                source.find("SetDisconnected([this] { EndSession(); })") != std::string::npos,
            "a settings session close exits its message loop and disconnect tears down application-side session resources");
        Check(appSettings.find("foregroundMatch=%d") !=
                    std::string::npos &&
                appSettings.find("settingsWindow_->LastError()") !=
                    std::string::npos,
            "settings open diagnostics distinguish visibility and foreground activation and retain the host failure stage");
        Check(host.find("shell->ReleaseSessionResources();") !=
                    std::string::npos &&
                host.find("QueueViewRelease();") != std::string::npos &&
                host.find("owner->ReleaseSessionView();") !=
                    std::string::npos &&
                host.find("runtime.Detach();") != std::string::npos &&
                host.find("releaseAfterClose") == std::string::npos &&
                source.find("ReleaseClosedHost") == std::string::npos,
            "the child releases route resources safely beyond the input callback before final process teardown");
        Check(appRun.find("ensureWidgetSettingsInstance") !=
                    std::string::npos &&
                appRun.find("widgetEngine_->EnsureWidgetLoaded(") !=
                    std::string::npos,
            "the application supplies persisted instance loading before widget settings navigation");
        const std::size_t exitCase = tray.find(
            "case kTrayExitCommand:");
        const std::size_t exitCaseEnd = tray.find("break;", exitCase);
        const std::string exitCommand = exitCase == std::string::npos ||
                exitCaseEnd == std::string::npos
            ? std::string{}
            : tray.substr(exitCase, exitCaseEnd - exitCase);
        Check(!tray.empty() &&
                exitCommand.find("ShowSettingsExitConfirmation();") !=
                    std::string::npos &&
                exitCommand.find("RequestExit();") == std::string::npos &&
                appSettings.find(
                    "PostOpenAction::\n            ShowExitConfirmation") !=
                    std::string::npos &&
                appSettings.find(
                    "settingsWindowOpenRequest_.MarkShown()") !=
                    std::string::npos &&
                appSettings.find(
                    "!settingsWindow_->ShowExitConfirm()") !=
                    std::string::npos,
            "tray exit uses the recoverable settings-open request and shows confirmation only after the window opens");
        Check(host.find("controller->CloseSession()") !=
                    std::string::npos &&
                appRun.find("settingsWindow_->PreTranslateMessage(&msg)") !=
                    std::string::npos &&
                appRun.find("settingsWindow_->ProcessTabNavigation(&msg)") !=
                    std::string::npos &&
                appRun.find("settingsWindow_->Render()") ==
                    std::string::npos,
            "the WinUI host retains durable close flushing and the application has no frame-render loop for settings");

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
