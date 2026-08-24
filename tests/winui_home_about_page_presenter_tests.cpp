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

    Check(!header.empty() && !source.empty() && !model.empty() &&
            !application.empty(),
        "Home/About presenter, native status model and host are readable");
    Check(header.find("HomeContent()") != std::string::npos &&
            header.find("AboutContent()") != std::string::npos &&
            header.find("DebugContent()") != std::string::npos &&
            source.find("muxc::Button") != std::string::npos &&
            source.find("muxc::HyperlinkButton") != std::string::npos &&
            source.find("muxc::ToggleSwitch") != std::string::npos &&
            source.find("muxc::InfoBar") != std::string::npos &&
            source.find("muxc::ProgressRing") != std::string::npos,
        "Home, About and Debug use cached native WinUI controls");

    Check(model.find("HomeAboutStatusPatch") != std::string::npos &&
            model.find("std::optional<std::size_t> installedWidgetCount") !=
                std::string::npos &&
            model.find("SettingsUpdateState") != std::string::npos &&
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
    Check(model.find("HomeAboutLinkUri") != std::string::npos &&
            model.find("space.bilibili.com/32837853") !=
                std::string::npos &&
            model.find("SnowDesktop_Release") != std::string::npos &&
            model.find("qm.qq.com/q/HyazkCIRig") != std::string::npos &&
            model.find("322e2b7395a51975150126276308b415970e080b") !=
                std::string::npos,
        "typed About links preserve the exact legacy destinations");
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
             "SettingsPage::DockAndTaskbar",
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
    Check(source.find("updateProgress.IsActive(updateRunning)") !=
                std::string::npos &&
            source.find("updateState == SettingsUpdateState::Checking") !=
                std::string::npos &&
            source.find("checkUpdateButton.IsEnabled(!updateRunning)") !=
                std::string::npos &&
            source.find("HomeAboutCommand::CancelUpdateCheck") ==
                std::string::npos &&
            source.find("updateInfoBar.IsOpen(showUpdateInfo)") !=
                std::string::npos &&
            source.find("backupCard.progress.IsActive(backupRunning)") !=
                std::string::npos,
        "host-published states drive feedback and checking disables update");

    for (const char* aboutItem : {
             "app.settings.about_description",
             "逍遥飘雪（郭云哲）", // l10n-allow: fixed author name contract
             "app.settings.copyright_notice",
             "app.settings.license_notice",
             "HomeAboutLink::Bilibili",
             "HomeAboutLink::AuthorGitHub",
             "HomeAboutLink::Douyin",
             "HomeAboutLink::Xiaohongshu",
             "HomeAboutLink::ReleaseRepository",
             "HomeAboutLink::SourceRepository",
             "HomeAboutLink::QqGroup",
             "HomeAboutLink::EverythingSdk",
             "HomeAboutLink::DearImGui",
             "HomeAboutLink::Lua",
             "HomeAboutLink::Spdlog",
             "HomeAboutLink::PinyinData",
             "HomeAboutLink::TranslucentTb",
             "322e2b7"})
    {
        Check(source.find(aboutItem) != std::string::npos,
            "legacy About content and attribution remain present");
    }
    const auto introduction = source.find(
        "InitializeSection(introductionSection");
    const auto author = source.find("InitializeSection(authorSection");
    const auto copyright = source.find(
        "InitializeSection(copyrightSection");
    const auto profiles = source.find("InitializeSection(profileSection");
    const auto project = source.find("InitializeSection(projectSection");
    const auto community = source.find(
        "InitializeSection(communitySection");
    const auto version = source.find("InitializeSection(versionSection");
    const auto thirdParty = source.find(
        "InitializeSection(thirdPartySection");
    const auto reference = source.find(
        "InitializeSection(referenceSection");
    Check(introduction < author && author < copyright &&
            copyright < profiles && profiles < project &&
            project < community && community < version &&
            version < thirdParty && thirdParty < reference,
        "About sections retain the legacy visible order");
    const auto buildControls = source.find("void BuildControls()");
    const auto inBuild = [&source, buildControls](const char* text) {
        return source.find(text, buildControls);
    };
    Check(inBuild("HomeAboutLink::Bilibili") <
                inBuild("HomeAboutLink::AuthorGitHub") &&
            inBuild("HomeAboutLink::AuthorGitHub") <
                inBuild("HomeAboutLink::Douyin") &&
            inBuild("HomeAboutLink::Douyin") <
                inBuild("HomeAboutLink::Xiaohongshu") &&
            inBuild("HomeAboutLink::ReleaseRepository") <
                inBuild("HomeAboutLink::SourceRepository") &&
            inBuild("HomeAboutLink::EverythingSdk") <
                inBuild("HomeAboutLink::DearImGui") &&
            inBuild("HomeAboutLink::DearImGui") <
                inBuild("HomeAboutLink::Lua") &&
            inBuild("HomeAboutLink::Lua") <
                inBuild("HomeAboutLink::Spdlog") &&
            inBuild("HomeAboutLink::Spdlog") <
                inBuild("HomeAboutLink::PinyinData") &&
            inBuild("HomeAboutLink::PinyinData") <
                inBuild("HomeAboutLink::TranslucentTb"),
        "profiles, project links and attributions retain legacy ordering");
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
    Check(application.find(
                "app.settings.animation_diagnostics_status") !=
                std::string::npos &&
            application.find("L\"Target %.1f Hz") == std::string::npos,
        "animation diagnostics status is dynamically localized");
    const auto debugTitle = source.find(
        "InitializeSection(debugTitleSection");
    const auto demo = source.find("InitializeSection(demoModeSection");
    const auto animation = source.find(
        "InitializeSection(animationSection");
    const auto crash = source.find("InitializeSection(crashSection");
    Check(debugTitle < demo && demo < animation && animation < crash,
        "Debug retains demo, animation and crash-test ordering");
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
