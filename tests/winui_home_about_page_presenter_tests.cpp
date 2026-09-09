#include <algorithm>
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
    std::string text = content.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

void TestPresenterContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/home_about_page_presenter.h");
    const std::string source = ReadText(
        repository / "src/winui/home_about_page_presenter.cpp");
    const std::string model = ReadText(
        repository / "src/winui/home_about_page_model.h");
    const std::string application = ReadText(
        repository / "src/app/app_settings_apply.cpp");
    const std::string applicationRun = ReadText(
        repository / "src/app/app_run.cpp");

    Check(!header.empty() && !source.empty() && !model.empty() &&
            !application.empty() && !applicationRun.empty(),
        "Home/About presenter, native status model and host are readable");
    Check(header.find("HomeContent()") != std::string::npos &&
            header.find("AboutContent()") != std::string::npos &&
            header.find("DebugContent()") != std::string::npos &&
            source.find("muxc::Button") != std::string::npos &&
            source.find("muxc::HyperlinkButton") != std::string::npos &&
            source.find("muxc::ToggleSwitch") != std::string::npos &&
            source.find("muxc::ProgressRing") != std::string::npos,
        "Home, About and Debug use cached native WinUI controls");

    Check(model.find("HomeAboutStatusPatch") != std::string::npos &&
            model.find("std::optional<std::size_t> installedWidgetCount") !=
                std::string::npos &&
            model.find("std::optional<bool> packaged") !=
                std::string::npos &&
            model.find("SettingsBackupState") != std::string::npos &&
            model.find("animationDiagnosticsEnabled") !=
                std::string::npos &&
            model.find("animationDiagnosticsStatus") !=
                std::string::npos &&
            source.find("patch.generation != generation") !=
                std::string::npos &&
            source.find("patch.revision <= statusRevision") !=
                std::string::npos,
        "host status patches are typed and reject stale async results");
    const auto updateStart = application.find(
        "DesktopApp::OpenStoreUpdates()");
    const auto updateCancel = application.find(
        "DesktopApp::PublishHomeAboutStatus()", updateStart);
    const std::string_view updateAction =
        updateStart != std::string::npos &&
            updateCancel != std::string::npos
        ? std::string_view(application).substr(
            updateStart, updateCancel - updateStart)
        : std::string_view{};
    Check(updateAction.find(
              "if (!snowdesktop::deployment::IsPackaged())") !=
                std::string_view::npos &&
            updateAction.find("GetStoreProductPageUri()") !=
                std::string_view::npos &&
            updateAction.find("api.github.com") == std::string_view::npos &&
            updateAction.find("AsyncHttpService") == std::string_view::npos &&
            applicationRun.find(
                "patch.packaged = snowdesktop::deployment::IsPackaged()") !=
                std::string::npos,
        "packaged About updates open Microsoft Store while portable actions never start GitHub HTTP");
    // Keep the retired updater absent across the whole host, not just its button.
    Check(application.find("ParseGitHubRelease") == std::string::npos &&
            application.find("settingsUpdateHttpService_") == std::string::npos &&
            application.find("PollSettingsUpdateCheck") == std::string::npos &&
            model.find("SettingsUpdateState") == std::string::npos &&
            source.find("updateInfoBar") == std::string::npos &&
            source.find("app.settings.store_managed_updates") == std::string::npos,
        "retired network update parsing, polling, and status UI remain absent");
    Check(source.find("if (packaged)") != std::string::npos &&
            source.find("checkUpdateButton.Visibility(packaged") != std::string::npos,
        "the Store update action stays hidden and inert for portable builds");
    Check(model.find("HomeAboutLinkUri") != std::string::npos &&
            model.find("space.bilibili.com/32837853") !=
                std::string::npos &&
            model.find("SnowDesktop_Release") == std::string::npos &&
            source.find("GitHub (Source)") == std::string::npos &&
            source.find("HomeAboutLink::ReleaseRepository") == std::string::npos &&
            model.find("qm.qq.com/q/HyazkCIRig") != std::string::npos &&
            model.find("322e2b7395a51975150126276308b415970e080b") !=
                std::string::npos,
        "About retains source and attribution destinations without the retired release link");
    Check(source.find("snapshot.domainRevisions.personalization") !=
                std::string::npos &&
            source.find("snapshot.domainRevisions.general") !=
                std::string::npos &&
            source.find("snapshot.values.general.dockEnabled") !=
                std::string::npos &&
            source.find("SettingsSnapshot snapshot_") == std::string::npos,
        "theme and Dock values patch by domain revision without retaining snapshots");

    for (const char* route : {
             "SettingsPage::Personalization",
             "SettingsPage::Dock",
             "SettingsPage::Widgets",
             "SettingsPage::BackupAndData",
             "SettingsPage::About"})
    {
        Check(source.find(route) != std::string::npos,
            "Home shortcut uses a strongly typed SettingsRoute destination");
    }
    for (const char* command : {
             "HomeAboutCommand::CheckForUpdates",
             "actions.openLink",
             "actions.updateGeneral",
             "actions.setAnimationDiagnostics",
             "actions.unlockDebug",
             "actions.requestCrashTestConfirmation"})
    {
        Check(source.find(command) != std::string::npos,
            "About action is emitted through the injected command callback");
    }
    Check(source.find("ShellExecute") == std::string::npos &&
            source.find("WinHttp") == std::string::npos &&
            source.find("std::thread") == std::string::npos &&
            source.find("CreateThread") == std::string::npos,
        "presenter performs no shell, network, or private background work");

    Check(source.find("AutomationProperties::SetName") !=
                std::string::npos &&
            source.find("AutomationProperties::SetHelpText") !=
                std::string::npos &&
            source.find("UseSystemFocusVisuals(true)") !=
                std::string::npos &&
            source.find("FocusTarget(") != std::string::npos,
        "cards expose automation text and keyboard focus targets");
    Check(source.find("RefreshLocalizedText()") != std::string::npos &&
            source.find("settings.home.update") != std::string::npos &&
            source.find("app.settings.third_party_libs") != std::string::npos &&
            source.find("RenderStatus();") != std::string::npos,
        "static and enum-derived status text refresh dynamically");
    for (const char* aboutItem : {
             "app.settings.about_description",
             "逍遥飘雪（郭云哲）", // l10n-allow: fixed author name contract
             "app.settings.copyright_notice",
             "app.settings.license_notice",
             "HomeAboutLink::Bilibili",
             "HomeAboutLink::AuthorGitHub",
             "HomeAboutLink::Douyin",
             "HomeAboutLink::Xiaohongshu",
             "HomeAboutLink::SourceRepository",
             "HomeAboutLink::QqGroup",
             "HomeAboutLink::EverythingSdk",
             "HomeAboutLink::DearImGui",
             "HomeAboutLink::Lua",
             "HomeAboutLink::PinyinData",
             "HomeAboutLink::TranslucentTb",
             "322e2b7"})
    {
        Check(source.find(aboutItem) != std::string::npos,
            "legacy About content and attribution remain present");
    }
    Check(source.find("versionClickCount < 5") != std::string::npos &&
            source.find("SettingsPage::Debug") != std::string::npos,
        "five version clicks unlock and navigate to Debug");
    Check(source.find("controls::SettingRow") != std::string::npos &&
            source.find("snapshot.values.general.demoModeEnabled") !=
                std::string::npos &&
            source.find("SettingsUpdateMode::PreviewAndCommit") !=
                std::string::npos &&
            source.find("app.settings.animation_diagnostics_desc") !=
                std::string::npos &&
            source.find("muxc::Expander") != std::string::npos &&
            source.find("requestCrashTestConfirmation(generation)") !=
                std::string::npos,
        "Debug keeps responsive rows, demo persistence, metrics and the legacy collapsible crash action");
    Check(source.find(
                "if (focusId == \"debug.animation\") return animationToggle;") !=
                std::string::npos,
        "Debug search results focus the animation diagnostics toggle");
    const auto ipcValues = ReadText(repository / "src/winui/settings_ipc_values.h");
    const auto generalStore = ReadText(repository / "src/general_settings.cpp");
    Check(ipcValues.find("v.temporaryInitializationEnabled") != std::string::npos &&
            source.find("patch.temporaryInitializationEnabled") != std::string::npos &&
            generalStore.find("temporaryInitialization") == std::string::npos,
        "the isolated settings process receives the real experiment state without persisting the switch");
    Check(application.find(
                "app.settings.animation_diagnostics_status") !=
                std::string::npos &&
            application.find("L\"Target %.1f Hz") == std::string::npos,
        "animation diagnostics status is dynamically localized");
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the Home/About presenter contract");
    if (argc == 2)
        TestPresenterContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures
                  << " WinUI Home/About presenter check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI Home/About presenter checks passed\n";
    return EXIT_SUCCESS;
}
