#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
int failures = 0;

std::string ReadSource(const std::filesystem::path& root, const std::string& name)
{
    std::ifstream input(root / name, std::ios::binary);
    std::string value{std::istreambuf_iterator<char>(input), {}};
    if (!input.is_open() || value.empty())
    {
        std::cerr << "FAIL: source unavailable: " << name << '\n';
        ++failures;
    }
    std::erase_if(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
    return value;
}

// Conservative source bans, including comments. No runtime/UI claim is made.
void Forbid(std::string_view source, const char* token, const char* boundary)
{
    if (!source.empty() && source.find(token) == std::string_view::npos) return;
    std::cerr << "FAIL: " << boundary << ": forbidden token " << token << '\n';
    ++failures;
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: SnowDesktopWinUiPresenterBoundaryTests <source-root> <profile>\n";
        return 2;
    }
    const std::string profile(argv[2]);
    constexpr std::string_view profiles[]{"personalization", "desktop", "dock",
        "home_about", "widget_settings", "widgets", "backup_data"};
    if (std::find(std::begin(profiles), std::end(profiles), profile) == std::end(profiles))
    {
        std::cerr << "unknown presenter boundary profile\n";
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    const std::string stem = "src/winui/" + profile +
        (profile == "widget_settings" ? "_presenter" : "_page_presenter");
    const auto source = ReadSource(root, stem + ".cpp");
    for (const auto token : {"ShellExecute", "WinHttp", "CreateThread", "std::thread"})
        Forbid(source, token, "presenters delegate external operations to host actions");

    if (profile == "dock")
    {
        for (const auto token : {"RestartWindowsExplorer(", "RequestSystemTaskbar",
                 "IsSystemTaskbarAutoHideEnabled(", "IsSystemTaskbarAlignmentCentered(",
                 "SyncSystemTaskbarSettingsFromWindows"})
            Forbid(source, token, "Dock presenter must not mutate or reread Windows state directly");
    }
    else if (profile == "widgets")
    {
        for (const auto token : {"WidgetPackageManager", "InstallArchive(",
                 "SetPermissionDecision(", "std::filesystem"})
            Forbid(source, token, "package and permission operations belong to the backend");
    }
    else if (profile == "backup_data")
    {
        for (const auto token : {"FullDataBackupManager", "CopyDataTree", "std::filesystem::remove",
                 "MessageBox", "ContentDialog{}", "IFileOpenDialog", "IFileSaveDialog"})
            Forbid(source, token, "backup presenter delegates data operations, confirmations and pickers");
    }
    else if (profile == "widget_settings")
    {
        const auto header = ReadSource(root, stem + ".h");
        Forbid(header, "constSettingsSnapshot&", "widget settings use their scoped service snapshot");
        Forbid(header, "settings_controller.h", "widget settings do not depend on the global controller");
        Forbid(source, "MakeWidgetSettingString(state.opaque", "opaque values must not enter ordinary string storage");
        Forbid(source, "SetOrdinary(guard,key,plaintext", "plaintext secrets must not enter ordinary storage");
        for (const auto token : {"ImGui", "imgui", "lua_ImGui"})
            Forbid(source, token, "widget settings rendering belongs to WinUI");
    }

    if (failures == 0)
        std::cout << profile << ": source boundaries passed; UI behavior was not exercised.\n";
    return failures == 0 ? 0 : 1;
}
