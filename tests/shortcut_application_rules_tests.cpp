#include "shortcut_application_rules.h"

#include <iostream>

namespace rules =
    snowdesktop::shortcut_application_rules;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}
} // namespace

int main()
{
    constexpr std::wstring_view appUserModelId =
        L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";

    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", appUserModelId, L"", false),
        "an AppUserModelID must identify a Microsoft Store shortcut");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", L"", appUserModelId, false),
        "an AUMID target parsing path must identify an Applications shortcut");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", L"",
            L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
            false),
        "shell:AppsFolder targets must identify Applications shortcuts");
    Check(
        rules::LooksLikeApplicationsParsingName(
            L"::{4234D49B-0245-4DF3-B780-3893943456E1}\\"
            L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"),
        "the Applications known-folder CLSID must be recognized");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", L"", L"", true),
        "an Applications PIDL must identify an Applications shortcut");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"C:\\Windows\\explorer.exe",
            L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
            L"", L"", false),
        "Explorer AppsFolder launch arguments must identify an application");
    Check(
        !rules::IsApplicationsShellLinkTarget(
            L"C:\\Windows\\explorer.exe", L"C:\\Temp", L"", L"", false),
        "ordinary Explorer shortcuts must not be classified as applications");
    Check(
        !rules::IsApplicationsShellLinkTarget(
            L"", L"", L"", L"C:\\Docs\\notes.txt", false),
        "document shortcuts must not be classified as Applications targets");

    Check(
        rules::IsSteamApplicationUrl(L"steam://rungameid/730"),
        "Steam rungameid URLs must identify application shortcuts");
    Check(
        rules::IsSteamApplicationUrl(L"  StEaM://run/570/  "),
        "Steam run URLs must be matched case-insensitively");
    Check(
        !rules::IsSteamApplicationUrl(L"steam://rungameid/not-a-number"),
        "invalid Steam application IDs must be rejected");
    Check(
        !rules::IsSteamApplicationUrl(L"https://store.steampowered.com/app/730"),
        "Steam web links must remain ordinary Internet shortcuts");
    Check(
        !rules::IsSteamApplicationUrl(L"steam://open/store/730"),
        "non-launch Steam URLs must remain ordinary Internet shortcuts");

    if (failures != 0)
    {
        std::cerr << failures << " shortcut application rule test(s) failed\n";
        return 1;
    }
    std::cout << "Shortcut application rule tests passed\n";
    return 0;
}
