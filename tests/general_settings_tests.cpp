#include "general_settings.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

std::wstring GetDataFilePath(const wchar_t* filename)
{
    return filename ? std::wstring(filename) : std::wstring{};
}

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}

int main()
{
    std::error_code error;
    const auto path = std::filesystem::temp_directory_path(error) /
        (L"SnowDesktopGeneralSettingsTests-" +
            std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(path, error);

    GeneralSettings saved;
    saved.widgetDeveloperToolsEnabled = true;
    saved.agentSkillTargetMask = 0x15;
    strcpy_s(saved.language, "zh-CN");
    Check(SaveGeneralSettings(path.c_str(), saved),
        "general settings save succeeds");

    GeneralSettings loaded;
    Check(LoadGeneralSettings(path.c_str(), loaded),
        "general settings load succeeds");
    Check(loaded.widgetDeveloperToolsEnabled &&
        loaded.agentSkillTargetMask == 0x15 &&
        std::strcmp(loaded.language, "zh-CN") == 0,
        "developer tools visibility and Agent Skill targets persist");

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "{\n  \"language\": \"system\"\n}\n";
    }
    GeneralSettings migrated;
    Check(LoadGeneralSettings(path.c_str(), migrated),
        "legacy general settings still load");
    Check(!migrated.widgetDeveloperToolsEnabled &&
        migrated.agentSkillTargetMask ==
            GeneralSettings::kAllAgentSkillTargetsMask,
        "legacy settings default to hidden developer tools and all Skill targets selected");

    std::filesystem::remove(path, error);
    if (failures == 0)
        std::cout << "All general settings tests passed.\n";
    return failures == 0 ? 0 : 1;
}
