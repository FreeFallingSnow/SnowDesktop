#include "widget_setting_rules.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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
    using namespace snowdesktop::widget_runtime;
    Check(IsValidUrlSettingValue("https://example.com/feed?q=1") &&
            IsValidUrlSettingValue("") &&
            !IsValidUrlSettingValue("file:///C:/secret") &&
            !IsValidUrlSettingValue("https://"),
        "url settings accept only bounded HTTP(S) URLs with an authority");
    Check(IsValidDateSettingValue("2024-02-29") &&
            !IsValidDateSettingValue("2023-02-29") &&
            !IsValidDateSettingValue("2024-2-9"),
        "date settings use strict Gregorian YYYY-MM-DD values");
    Check(IsValidTimeSettingValue("00:00") &&
            IsValidTimeSettingValue("23:59") &&
            !IsValidTimeSettingValue("24:00"),
        "time settings use strict 24-hour HH:MM values");

    double number = 0.0;
    Check(ParseFiniteSettingNumber("3.25", number) && number == 3.25 &&
            !ParseFiniteSettingNumber("3.25x", number),
        "range numbers require a complete finite representation");
    Check(std::abs(SnapRangeSettingValue(4.6, 0.0, 10.0, 0.5) - 4.5) <
            0.000001 &&
            SnapRangeSettingValue(15.0, 0.0, 10.0, 0.5) == 10.0,
        "range settings snap to step and remain within bounds");

    const std::vector<std::string> options = { "news", "weather", "media" };
    Check(IsValidMultiSelectSettingValue(options, { "news", "media" }) &&
            IsValidMultiSelectSettingValue(options, {}) &&
            !IsValidMultiSelectSettingValue(options, { "missing" }) &&
            !IsValidMultiSelectSettingValue(options, { "news", "news" }),
        "multi-select settings accept unique values from declared options");

    std::vector<std::string> normalizedExtensions;
    Check(IsValidFilesystemSettingAccess("read") &&
            IsValidFilesystemSettingAccess("readWrite") &&
            !IsValidFilesystemSettingAccess("all") &&
            NormalizeFilesystemSettingExtensions(
                { ".PNG", "tar.gz", "png" }, normalizedExtensions) &&
            normalizedExtensions ==
                std::vector<std::string>({ "png", "tar.gz" }) &&
            !NormalizeFilesystemSettingExtensions(
                { "../exe" }, normalizedExtensions),
        "filesystem-handle settings validate access and normalize safe extensions");
    Check(IsValidSettingGroupId("content.behavior-2") &&
            !IsValidSettingGroupId("") &&
            !IsValidSettingGroupId("content/advanced"),
        "setting group IDs use a bounded stable ASCII identifier");

    if (failures != 0)
    {
        std::cerr << failures << " widget setting rule checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget setting rule checks passed\n";
    return EXIT_SUCCESS;
}
