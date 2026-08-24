#include "settings_update_rules.h"

#include <cstdlib>
#include <iostream>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
}

int main()
{
    using snowdesktop::settings_update_rules::ParseGitHubRelease;

    const auto newer = ParseGitHubRelease(
        R"({"tag_name":"v1.0.5.0","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/tag/v1.0.5.0"})",
        "1.0.4.0");
    Check(newer.parsed && newer.updateAvailable &&
            newer.version == "1.0.5.0",
        "a newer official GitHub release is detected");

    const auto current = ParseGitHubRelease(
        R"({"tag_name":"1.0.4.0","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/tag/v1.0.4.0"})",
        "1.0.4.0");
    Check(current.parsed && !current.updateAvailable,
        "the installed release is reported as current");

    const auto older = ParseGitHubRelease(
        R"({"tag_name":"v1.0.3.0","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/tag/v1.0.3.0"})",
        "1.0.4.0");
    Check(older.parsed && !older.updateAvailable,
        "an older official release never becomes an update");

    Check(!ParseGitHubRelease(
            R"({"tag_name":"v1.0.5.0","html_url":"https://example.com/download"})",
            "1.0.4.0").parsed,
        "an untrusted download URL is rejected");
    Check(!ParseGitHubRelease("not json", "1.0.4.0").parsed,
        "an invalid response is rejected");
    for (const char* invalidVersion : {
             "v0.1.0.0", "v65536.0.0.0", "v1.65536.0.0",
             "v1.0.65536.0", "v1.0.4.1", "v1.0.4",
             "v1.0.4.0.0", "v42949672960.0.0.0"})
    {
        const std::string body = std::string(
            R"({"tag_name":")") + invalidVersion +
            R"(","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/latest"})";
        Check(!ParseGitHubRelease(body, "1.0.4.0").parsed,
            "versions outside the Store four-part contract are rejected");
    }
    Check(!ParseGitHubRelease(
            R"({"tag_name":"v1.0.5.0"})", "1.0.4.0").parsed,
        "a release without an official download URL is rejected");
    Check(!ParseGitHubRelease(
            R"({"html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/latest"})",
            "1.0.4.0").parsed,
        "a release without a version tag is rejected");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "Settings update rule checks passed\n";
    return EXIT_SUCCESS;
}
