#include "widget/settings/widget_setting_rules.h"

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
    Check(DurationSettingUnitSeconds("minutes") == 60 &&
        IsValidDurationSettingValue(90061, "seconds", 0, 359999, 1) &&
        IsValidDurationSettingValue(25, "minutes", 1, 120, 1) &&
        !IsValidDurationSettingValue(25.5, "minutes", 1, 120, 1) &&
        !IsValidDurationSettingValue(-1, "seconds", 0, 359999, 1) &&
        !IsValidDurationSettingValue(359999 + 1, "seconds", 0, 359999, 1) &&
        !IsValidDurationSettingValue(26, "minutes", 5, 120, 5) &&
        !IsValidDurationSettingSchema("milliseconds", 0, 100, 1) &&
        !IsValidDurationSettingSchema("minutes", 0, 6000, 1),
        "duration values preserve units, reject fractional or unaligned input, and exceed one day safely");
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
    Check(IsValidSettingCondition("equals", 1) &&
            IsValidSettingCondition("truthy", 0) &&
            IsValidSettingCondition("oneOf", 3) &&
            !IsValidSettingCondition("contains", 0) &&
            EvaluateSettingCondition("truthy", { "1" }, {}) &&
            !EvaluateSettingCondition("truthy", { "false" }, {}) &&
            EvaluateSettingCondition("contains",
                { "news", "media" }, { "media" }) &&
            EvaluateSettingCondition("notContains",
                { "news" }, { "media" }) &&
            EvaluateSettingCondition("equals", { "compact" },
                { "compact" }) &&
            EvaluateSettingCondition("notEquals", {}, { "compact" }) &&
            EvaluateSettingCondition("oneOf", { "wide" },
                { "compact", "wide" }) &&
            EvaluateSettingCondition("notOneOf",
                { "compact" }, { "wide", "dense" }) &&
            EvaluateSettingCondition("set", { "0" }, {}) &&
            EvaluateSettingCondition("unset", {}, {}) &&
            EvaluateSettingCondition("falsy", { "off" }, {}),
        "setting conditions validate arity and evaluate scalar and array values");
    Check(ValidateSettingTextValue("雪桌面", true, 3, 3) &&
            !ValidateSettingTextValue("雪桌面", true, 4, 8) &&
            !ValidateSettingTextValue("12345", false, 0, 4) &&
            !ValidateSettingTextValue("", true, 0, 8) &&
            ValidateSettingTextValue("", false, 0, 8) &&
            !ValidateSettingTextValue("\xF0\x80\x80\x80", false, 0, 8),
        "setting text validation counts Unicode code points and required values");

    if (failures != 0)
    {
        std::cerr << failures << " widget setting rule checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget setting rule checks passed\n";
    return EXIT_SUCCESS;
}
