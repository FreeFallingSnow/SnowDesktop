#include <cstdlib>
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
        repository / "src/winui/dock_page_presenter.h");
    const std::string source = ReadText(
        repository / "src/winui/dock_page_presenter.cpp");

    Check(!header.empty() && !source.empty(),
        "Dock presenter sources are readable");
    Check(header.find("std::uint64_t generation") != std::string::npos &&
            header.find("SettingsUpdateMode mode") != std::string::npos &&
            header.find("GeneralEdit edit") != std::string::npos &&
            header.find("DockEdit edit") != std::string::npos,
        "edits carry generation and update mode without a stale snapshot");
    Check(source.find("snapshot.domainRevisions.general") !=
                std::string::npos &&
            source.find("snapshot.domainRevisions.dock") !=
                std::string::npos &&
            source.find("SettingsSnapshot snapshot_") == std::string::npos &&
            source.find("DockSettings settings_") == std::string::npos &&
            source.find("updatingControls") != std::string::npos,
        "only changed General and Dock domains patch cached controls");

    for (const char* field : {
             "dockEnabled", "position", "edgeAttached",
             "floatingShortcutMode", "floatingEdgeSwipeEnabled",
             "monitorScope", "showWindowsButton", "showFrequentItems",
             "frequentItemCount", "keepWhenDesktopHidden",
             "thicknessScale", "systemTaskbarAutoHide",
             "systemTaskbarAlignment", "systemTaskbarBackdropEnabled",
             "systemTaskbarFollowPersonalization",
             "systemTaskbarContentTheme", "systemTaskbarAppearance",
             "systemTaskbarVisibleWindow",
             "systemTaskbarMaximizedWindow", "systemTaskbarShellUi"})
    {
        Check(source.find(field) != std::string::npos,
            "the requested Dock/taskbar field has a real binding");
    }
    for (const char* control : {
             "muxc::ToggleSwitch", "muxc::ComboBox", "muxc::Slider",
             "muxc::NumberBox", "muxc::ColorPicker", "muxc::Button"})
    {
        Check(source.find(control) != std::string::npos,
            "the Dock page uses native WinUI controls");
    }
    Check(source.find("SettingsUpdateMode::Preview,") !=
                std::string::npos &&
            source.find("SettingsUpdateMode::PreviewAndCommit,") !=
                std::string::npos &&
            source.find("PointerReleased") != std::string::npos &&
            source.find("LostFocus") != std::string::npos &&
            source.find("IsEnter(args)") != std::string::npos,
        "continuous controls preview and commit at interaction boundaries");

    Check(source.find("actions.confirm") != std::string::npos &&
            source.find("Action::RestartExplorer") != std::string::npos &&
            source.find("invokeHost(expectedGeneration") !=
                std::string::npos &&
            source.find("RestartWindowsExplorer()") == std::string::npos,
        "Explorer restart requires confirmation and a typed host request");
    Check(source.find("IsSystemTaskbarAutoHideEnabled()") ==
                std::string::npos &&
            source.find("IsSystemTaskbarAlignmentCentered()") ==
                std::string::npos &&
            source.find("SyncSystemTaskbarSettingsFromWindows") ==
                std::string::npos,
        "the presenter never overwrites requested taskbar values by rereading Windows");
    Check(source.find("SystemTaskbarDynamicRule DockSettings::* member") !=
                std::string::npos &&
            source.find("PrepareDynamicRuleTheme") != std::string::npos &&
            source.find("contentTheme =") != std::string::npos,
        "all three dynamic rules share enabled, theme, and content-theme bindings");
    Check(source.find("ColorChanged(control.changed)") !=
                std::string::npos &&
            source.find("ValueChanged(control.sliderChanged)") !=
                std::string::npos &&
            source.find("SelectionChanged(control.themeToken)") !=
                std::string::npos,
        "Close revokes custom appearance, continuous, and dynamic rule events");
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the Dock presenter contract");
    if (argc == 2)
        TestPresenterContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures
                  << " WinUI Dock presenter check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI Dock presenter checks passed\n";
    return EXIT_SUCCESS;
}
