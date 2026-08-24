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

    Check(!header.empty() && !source.empty(),
        "Home/About presenter sources are readable");
    Check(header.find("HomeContent()") != std::string::npos &&
            header.find("AboutContent()") != std::string::npos &&
            source.find("muxc::Button") != std::string::npos &&
            source.find("muxc::InfoBar") != std::string::npos &&
            source.find("muxc::ProgressRing") != std::string::npos,
        "both routes use cached native WinUI controls");

    Check(header.find("HomeAboutStatusPatch") != std::string::npos &&
            header.find("std::optional<std::size_t> installedWidgetCount") !=
                std::string::npos &&
            header.find("SettingsUpdateState") != std::string::npos &&
            header.find("SettingsBackupState") != std::string::npos &&
            source.find("patch.generation != generation") !=
                std::string::npos &&
            source.find("patch.revision <= statusRevision") !=
                std::string::npos,
        "host status patches are typed and reject stale async results");
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
             "HomeAboutCommand::OpenProject",
             "HomeAboutCommand::OpenLicense",
             "HomeAboutCommand::OpenThirdPartyNotices"})
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
            source.find("settings.about.thirdparty") != std::string::npos &&
            source.find("RenderStatus();") != std::string::npos,
        "static and enum-derived status text refresh dynamically");
    Check(source.find("updateProgress.IsActive(updateRunning)") !=
                std::string::npos &&
            source.find("updateInfoBar.IsOpen(showUpdateInfo)") !=
                std::string::npos &&
            source.find("backupCard.progress.IsActive(backupRunning)") !=
                std::string::npos,
        "host-published states alone drive progress and update feedback");
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
