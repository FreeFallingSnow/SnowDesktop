#include "settings_window_open_rules.h"
#include "usage_guide.h"
#include "usage_guide_panel.h"
#include "usage_guide_settings.h"
#include "json_value.h"
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

void CheckUsageGuide()
{
    using namespace snowdesktop::usage_guide;
    Check(!ParseTopic("unknown") && ParseTopic("layout") == Topic::Move &&
        ParseTopic("resize") == Topic::Move && ParseTopic("application") == Topic::Collection &&
        ParseTopic("dockMapping") == Topic::DockPin,
        "merged tutorials preserve old routes without duplicate entries");
    Check(!Find(Topic::DockSummon)->practice && !Find(Topic::Navigation)->practice,
        "preference choices never start desktop exercises");
    Check(!CanShowDesktop(*Find(Topic::DockFiles), false) && CanShowDesktop(*Find(Topic::DockFiles), true),
        "Dock guidance requires Dock but never a newly created file component");
    Check(!CanShowDesktop(*Find(Topic::DockSummon), true), "preference articles cannot become desktop exercises");
    const auto destination = snowdesktop::CanonicalizeSettingsRoute(SettingsDestination(*Find(Topic::DockPosition)));
    Check(destination.page == snowdesktop::SettingsPage::Dock && destination.focusId == "dock.position" &&
        ReturnDestination(destination) == snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::General, "start.dockPosition"),
        "settings links return to the originating guide without dropping the setting focus");
    const auto secondary = SettingsDestination(*Find(Topic::DockAppearance), true);
    Check(secondary.page == snowdesktop::SettingsPage::AnimationPerformance && secondary.focusId == "animation.hover" &&
        ReturnDestination(secondary) == snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::General, "start.dockAppearance"),
        "secondary settings links retain the same guide origin");
    Check(!ReturnDestination(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Dock)),
        "ordinary settings navigation has no guide return banner");
    auto invalidReturn = destination; invalidReturn.guideTopic = "removed-topic";
    Check(!ReturnDestination(invalidReturn), "unknown guide origins cannot create dead return links");
    const auto returnRoute = ReturnDestination(destination);
    Check(returnRoute && !ReturnDestination(*returnRoute), "returning consumes the guide context");
    Check(Find(Topic::PagesDrag)->section == Section::Pages && CanShowDesktop(*Find(Topic::PagesDrag), false) &&
        !CanShowDesktop(*Find(Topic::PagesKeys), true),
        "page dragging supports desktop reference while keyboard preferences only open settings");

    // The renderer and pointer handlers use this exact placement model.
    // Repaints have no menu or cursor input, so they cannot chase the pointer.
    PanelPlacement panel;
    RECT work{0, 0, 1920, 1040};
    auto frame = panel.Arrange(work, 440, 300, 24);
    Check(frame.left == 1456 && frame.top == 716, "first reference starts inside the work area");
    panel.BeginDrag({1500, 730}); panel.DragTo({544, 214}, true, true); panel.EndDrag();
    frame = panel.Arrange(work, 440, 300, 24);
    Check(frame.left == 500 && frame.top == 200, "drag keeps the grabbed header offset");
    Check(!panel.DragTo({1700, 900}, false, false), "pointer motion after release cannot reposition the panel");
    frame = panel.Arrange(work, 440, 300, 24);
    Check(frame.left == 500 && frame.top == 200, "ordinary repaint preserves the manual anchor");
    frame = panel.Arrange(work, 440, 500, 24);
    Check(frame.left == 500 && frame.top == 200, "opening details keeps the top-left position when space permits");
    frame = panel.Arrange({0, 0, 800, 600}, 440, 500, 24);
    Check(frame.left == 336 && frame.top == 76, "smaller work area keeps the entire panel reachable");
    panel.BeginDrag({380, 90}); panel.DragTo({-1456, 214}, true, true); panel.EndDrag();
    frame = panel.Arrange({-1920, 0, 0, 1040}, 440, 300, 24);
    Check(frame.left == -1500 && frame.top == 200, "drag supports a monitor with negative screen coordinates");

    // User regression: queued/resampled moves precede WM_LBUTTONUP. Physical
    // release must freeze the last accepted anchor even before that message.
    const POINT releasedAnchor = *panel.anchor;
    panel.BeginDrag({-1450, 220});
    Check(!panel.DragTo({-1000, 500}, false, true) && !panel.dragOffset &&
        panel.anchor->x == releasedAnchor.x && panel.anchor->y == releasedAnchor.y,
        "physical release rejects a queued move before button-up dispatch");
    Check(!panel.DragTo({-900, 550}, true, true),
        "later button state cannot revive the ended guide drag");
    panel.BeginDrag({-1450, 220});
    Check(!panel.DragTo({-1000, 500}, true, false) && !panel.dragOffset &&
        panel.anchor->x == releasedAnchor.x && panel.anchor->y == releasedAnchor.y,
        "capture loss cancels movement even while the button remains down");

    // Real renderer/input use this model: every line must remain reachable,
    // with no blank overscroll after text scaling or a monitor change.
    PanelScroll scroll;
    scroll.Arrange(900, 300); scroll.By(48);
    Check(scroll.offset == 48 && scroll.maximum == 600, "wheel exposes the next part of a long document");
    scroll.ToFraction(1.0);
    Check(scroll.offset == 600, "dragging the scrollbar can reach the final instruction");
    scroll.By(1000);
    Check(scroll.offset == 600, "scrolling cannot pass the final line");
    scroll.Arrange(250, 300);
    Check(scroll.offset == 0 && scroll.maximum == 0, "a resized document that fits shows all text from the start");
    scroll.By(-48);
    Check(scroll.offset == 0, "scrolling above the first line is clamped");

    snowdesktop::StaticSettingSearchDescriptor entry;
    entry.focusId = "widgets.workshop"; entry.visible = false;
    Check(!IsLessonVisible(*Find(Topic::Workshop), {entry}), "unavailable Workshop cannot appear in the guide");
    entry.visible = true;
    Check(IsLessonVisible(*Find(Topic::Workshop), {entry}), "available Workshop retains its entry");
    Check(!IsLessonVisible(*Find(Topic::CategoryRules), {entry}), "an unrelated visible setting cannot unlock an article");
    entry.focusId = "desktop.categoryRules";
    Check(IsLessonVisible(*Find(Topic::CategoryRules), {entry}) &&
        Find(Topic::CategoryRules)->section == Section::Files && !Find(Topic::CategoryRules)->practice,
        "file classification stays with organizers and only opens settings");
    entry.focusId = "widgets.developer";
    Check(IsLessonVisible(*Find(Topic::Develop), {entry}), "development help is discoverable before opting into developer tools");
    Check(IsLessonVisible(*Find(Topic::Move), {}), "basic component guidance does not depend on settings search availability");
    for (const auto& lesson : kLessons)
    {
        Check(ParseTopic(lesson.key) == lesson.topic, "every visible lesson can be requested through the host");
        Check(std::count_if(kLessons.begin(), kLessons.end(), [&](const auto& other) {
            return std::string_view(other.key) == lesson.key; }) == 1, "lesson routes have unique identities");
    }
    const auto directory = std::filesystem::temp_directory_path() /
        ("SnowDesktop-usage-guide-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!std::filesystem::create_directory(directory)) { Check(false, "create isolated preference directory"); return; }
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{directory};
    // An old opt-out file is retained, but cannot hide the new permanent help.
    { std::ofstream old(directory / "SnowDesktop.onboarding.json"); old << R"({"version":2,"dismissed":true,"steps":31})"; }
    bool expanded = false;
    const auto path = directory / "SnowDesktop.guide.json";
    Check(LoadExpanded(path, expanded) && expanded, "first-use help defaults expanded regardless of old tutorial dismissal");
    Check(SaveExpanded(path, false) && LoadExpanded(path, expanded) && !expanded,
        "the user's collapsed panel preference survives a new load");
    Check(SaveExpanded(path, true) && LoadExpanded(path, expanded) && expanded, "expanded preference also round-trips");
    { std::ifstream input(path); std::string text((std::istreambuf_iterator<char>(input)), {}); JsonValue value;
      Check(ParseJson(text, value) && value.Find("expanded") && !value.Find("steps") && !value.Find("collectionId") && !value.Find("active"),
          "preference storage contains neither progress nor practice identities"); }
    { std::ofstream output(path); output << R"({"version":1,"expanded":"false"})"; }
    Check(!LoadExpanded(path, expanded) && expanded, "malformed preferences cannot overwrite the last valid in-memory choice");
    { std::ofstream blocker(directory / "blocked"); blocker << "not a directory"; }
    Check(!SaveExpanded(directory / "blocked" / "value.json", false), "preference write failure is observable");
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
    CheckUsageGuide();
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
