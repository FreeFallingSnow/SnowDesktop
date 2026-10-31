#include "test_source_boundary.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "expected the repository root argument\n";
        return 1;
    }

    // These lexical bans are limited to sensitive negative boundaries. They
    // do not execute settings actions or prove the corresponding behavior.
    const bool passed = snowdesktop::test::CheckSourceBoundaries(
        std::filesystem::path(argv[1]), {
            {"src/app/lifecycle/app_settings_apply.cpp",
             "snowdesktop::SettingsActionResult OnSettingsPreview(",
             "snowdesktop::SettingsActionResult OnSettingsCommitted(",
             {"RequestSystemTaskbar"}},
            {"src/app/lifecycle/app_settings_apply.cpp",
             "snowdesktop::SettingsActionResult OnSettingsRouteChanged(",
             "snowdesktop::SettingsActionResult Invoke(",
             {"RequestSystemTaskbar"}},
            {"src/app/lifecycle/app_settings_apply.cpp",
             "if (generalCommitted)",
             "if (HasSettingsDomain(domains, SettingsDomain::Category))",
             {"ApplyAutoStartEnabled"}},
            {"src/app/lifecycle/app_settings_apply.cpp",
             "DesktopApp::ApplyAutoStartEnabled(",
             "DesktopApp::OpenStoreUpdates()",
             {"QueryAutoStartState()", "ReconcileAutoStart("}},
            {"src/app/lifecycle/app_run.cpp", "", "",
             {"widgetsPage.agentSkillTargetMask", "widgetsPage.setAgentSkillTargetMask"}},
        });
    if (passed)
        std::cout << "Settings host lexical negative boundaries passed; runtime behavior is not exercised.\n";
    return passed ? 0 : 1;
}
