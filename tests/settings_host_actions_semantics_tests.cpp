#include <algorithm>
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
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string source = contents.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
}

std::string_view Between(std::string_view source,
    std::string_view beginMarker, std::string_view endMarker)
{
    const std::size_t begin = source.find(beginMarker);
    if (begin == std::string_view::npos)
        return {};
    const std::size_t end = source.find(
        endMarker, begin + beginMarker.size());
    if (end == std::string_view::npos)
        return source.substr(begin);
    return source.substr(begin, end - begin);
}

bool AppearsBefore(std::string_view source,
    std::string_view first, std::string_view second)
{
    const std::size_t firstPosition = source.find(first);
    const std::size_t secondPosition = source.find(second);
    return firstPosition != std::string_view::npos &&
        secondPosition != std::string_view::npos &&
        firstPosition < secondPosition;
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2, "source root argument is provided");
    if (argc != 2)
        return 1;

    const std::filesystem::path root(argv[1]);
    const std::string source = ReadFile(
        root / "src" / "app" / "app_settings_apply.cpp");
    Check(!source.empty(), "settings host actions source is readable");

    const std::string_view preview = Between(source,
        "snowdesktop::SettingsActionResult OnSettingsPreview(",
        "snowdesktop::SettingsActionResult OnSettingsCommitted(");
    const std::string_view commit = Between(source,
        "snowdesktop::SettingsActionResult OnSettingsCommitted(",
        "snowdesktop::SettingsActionResult OnSettingsRouteChanged(");
    Check(!preview.empty() && !commit.empty(),
        "settings host preview and commit handlers are discoverable");

    Check(AppearsBefore(preview,
            "const bool committedTaskbarAutoHide",
            "app_.dockSettings_ = snapshot.values.dock;") &&
            AppearsBefore(preview,
                "app_.dockSettings_ = snapshot.values.dock;",
                "app_.dockSettings_.systemTaskbarAutoHide =") &&
            AppearsBefore(preview,
                "app_.dockSettings_ = snapshot.values.dock;",
                "app_.dockSettings_.systemTaskbarAlignment ="),
        "Dock previews retain the committed system taskbar mirrors");
    Check(preview.find("RequestSystemTaskbar") == std::string_view::npos,
        "Dock previews never request Windows-owned taskbar changes");

    const std::size_t dockAssignment = commit.find(
        "app_.dockSettings_ = requestedDockSettings;");
    const std::size_t autoHideRequest = commit.find(
        "RequestSystemTaskbarAutoHideEnabled(");
    const std::size_t alignmentRequest = commit.find(
        "RequestSystemTaskbarAlignmentCentered(");
    Check(autoHideRequest != std::string_view::npos &&
            alignmentRequest != std::string_view::npos &&
            dockAssignment != std::string_view::npos &&
            autoHideRequest < dockAssignment &&
            alignmentRequest < dockAssignment,
        "system taskbar requests precede mutation of the application mirror");
    Check(commit.find("if (autoHideChanged &&") !=
                std::string_view::npos &&
            commit.find("if (alignmentChanged &&") !=
                std::string_view::npos,
        "system taskbar requests are issued only for changed values");
    Check(commit.find(
            "L\"The system taskbar auto-hide change could not be queued.\"") !=
                std::string_view::npos &&
            commit.find(
                "L\"The system taskbar alignment change could not be queued.\"") !=
                std::string_view::npos &&
            commit.find("SettingsDomain::Dock);") !=
                std::string_view::npos,
        "rejected system taskbar requests return explicit Dock-domain failures");
    Check(commit.find("SyncSystemTaskbarSettingsFromWindows();") ==
            std::string_view::npos,
        "a successful Dock commit keeps requested values instead of rereading Windows");

    const std::string_view general = Between(commit,
        "if (HasSettingsDomain(domains, SettingsDomain::General))",
        "if (HasSettingsDomain(domains, SettingsDomain::Category))");
    Check(!general.empty(), "General commit block is discoverable");
    Check(AppearsBefore(general,
            "const bool dockEnabledChanged",
            "app_.generalSettings_ = snapshot.values.general;") &&
            general.find("if (dockEnabledChanged)") !=
                std::string_view::npos,
        "General commits detect Dock enablement changes before replacing state");
    Check(general.find(
            "desktop.dockEnabled =\n                        app_.generalSettings_.dockEnabled;") !=
                std::string_view::npos &&
            general.find("SynchronizeDesktop(") != std::string_view::npos,
        "General Dock enablement keeps the controller desktop mirror aligned");
    Check(AppearsBefore(general,
            "app_.UpdateLayoutWorkArea();",
            "app_.RestoreDockEntriesToDesktop();") &&
            AppearsBefore(general,
                "app_.RestoreDockEntriesToDesktop();",
                "app_.LayoutItems();") &&
            AppearsBefore(general,
                "app_.LayoutItems();",
                "app_.SaveLayoutSlots();") &&
            AppearsBefore(general,
                "app_.SaveLayoutSlots();",
                "app_.InvalidateDragStaticScene();"),
        "General Dock enablement changes relayout and persist in runtime order");
    Check(general.find("if (!app_.generalSettings_.dockEnabled)\n") !=
                std::string_view::npos,
        "disabling the Dock restores its entries to the desktop before saving");

    if (failures == 0)
        std::cout << "Settings host action semantics tests passed\n";
    return failures == 0 ? 0 : 1;
}
