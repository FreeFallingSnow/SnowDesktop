#include "widget_system_settings.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    using snowdesktop::widget_runtime::SystemSettingsUri;
    Check(SystemSettingsUri("notifications") ==
            L"ms-settings:notifications" &&
            SystemSettingsUri("audio") == L"ms-settings:sound" &&
            SystemSettingsUri("display") == L"ms-settings:display" &&
            SystemSettingsUri("network") ==
                L"ms-settings:network-status" &&
            SystemSettingsUri("bluetooth") ==
                L"ms-settings:bluetooth" &&
            SystemSettingsUri("power") == L"ms-settings:powersleep" &&
            SystemSettingsUri("storage") ==
                L"ms-settings:storagesense" &&
            SystemSettingsUri("apps") == L"ms-settings:appsfeatures" &&
            SystemSettingsUri("personalization") ==
                L"ms-settings:personalization",
        "documented setting page names must map to fixed Windows URIs");
    Check(!SystemSettingsUri("") &&
            !SystemSettingsUri("privacy-location") &&
            !SystemSettingsUri("ms-settings:display") &&
            !SystemSettingsUri("DISPLAY"),
        "raw, unknown, and differently-cased settings targets must fail");
    std::cout << "widget system settings tests passed\n";
    return 0;
}
