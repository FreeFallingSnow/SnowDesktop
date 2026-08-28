#include "widget_l10n_format.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void CheckEqual(const std::string& actual, std::string_view expected,
    const char* message)
{
    if (actual != expected)
    {
        std::cerr << "FAIL: " << message << "\nexpected: " << expected
                  << "\nactual:   " << actual << '\n';
        std::exit(1);
    }
}

void TestNumberFormatting()
{
    using snowdesktop::widget_l10n::FormatNumber;
    using snowdesktop::widget_l10n::NumberOptions;

    CheckEqual(FormatNumber(1234.5, "en-US"), "1,234.5",
        "English numbers must use host grouping and decimal separators");
    CheckEqual(FormatNumber(1234.5, "de-DE"), "1.234,5",
        "German numbers must use host grouping and decimal separators");
    CheckEqual(FormatNumber(1234.5, "en-US", { 0, 2, false }),
        "1234.5", "number grouping must be configurable");
    CheckEqual(FormatNumber(12.5, "en-US", { 2, 2, true }), "12.50",
        "minimum fraction digits must be preserved");
    Check(FormatNumber(HUGE_VAL, "en-US").empty(),
        "non-finite numbers must be rejected");
}

void TestByteFormatting()
{
    using snowdesktop::widget_l10n::FormatBytes;
    CheckEqual(FormatBytes(1536, "en-US"), "1.5\xE2\x80\xAFKiB",
        "binary byte values must use IEC units");
    CheckEqual(FormatBytes(1500, "de-DE", 1000),
        "1,5\xE2\x80\xAFKB",
        "decimal byte values must localize their number");
    CheckEqual(FormatBytes(0, "en-US"), "0\xE2\x80\xAF" "B",
        "zero bytes must remain visible");
    Check(FormatBytes(-1, "en-US").empty(),
        "negative byte values must be rejected");
    Check(FormatBytes(1, "en-US", 512).empty(),
        "unsupported byte bases must be rejected");
}

void TestDurationFormatting()
{
    using snowdesktop::widget_l10n::FormatDuration;
    CheckEqual(FormatDuration(3661000, "en-US"), "1 h 1 min 1 s",
        "short durations must contain localized unit parts");
    CheckEqual(FormatDuration(3661000, "zh-CN"),
        "1 小时 1 分钟 1 秒",
        "Chinese durations must use Chinese unit names");
    CheckEqual(FormatDuration(61000, "en-US", "clock"), "1:01",
        "clock durations below one hour must use minute-second form");
    CheckEqual(FormatDuration(3661000, "en-US", "clock"), "1:01:01",
        "clock durations with hours must use hour-minute-second form");
    CheckEqual(FormatDuration(0, "en-US"), "0 s",
        "zero durations must remain visible");
    Check(FormatDuration(-1, "en-US").empty(),
        "negative durations must be rejected");
    Check(FormatDuration(0, "en-US", "long").empty(),
        "unsupported duration styles must be rejected");
}

void TestRelativeTimeFormatting()
{
    using snowdesktop::widget_l10n::FormatRelativeTime;
    constexpr std::int64_t minute = 60 * 1000;
    constexpr std::int64_t day = 24 * 60 * minute;
    CheckEqual(FormatRelativeTime(-90 * minute, "en-US"),
        "2 hours ago", "relative time must select and round an automatic unit");
    CheckEqual(FormatRelativeTime(day, "zh-CN"), "明天",
        "Chinese automatic relative time must use calendar terms");
    CheckEqual(FormatRelativeTime(-day, "de-DE"), "gestern",
        "German automatic relative time must use calendar terms");
    CheckEqual(FormatRelativeTime(2 * day, "pt-BR", "day", "always"),
        "em 2 dias", "numeric relative time must localize grammar and units");
    CheckEqual(FormatRelativeTime(2 * day, "zh-TW", "day", "always"),
        "2天後", "Traditional Chinese relative time must use traditional suffixes");
    CheckEqual(FormatRelativeTime(0, "en-US", "auto", "always"),
        "in 0 seconds", "numeric always must not replace zero with now");
    Check(FormatRelativeTime(0, "en-US", "quarter", "auto").empty(),
        "unsupported relative units must be rejected");
    Check(FormatRelativeTime(0, "en-US", "auto", "sometimes").empty(),
        "unsupported numeric modes must be rejected");
}

void TestListFormatting()
{
    using snowdesktop::widget_l10n::FormatList;
    CheckEqual(FormatList({ "A", "B", "C" }, "en-US"),
        "A, B, and C", "English lists must use an Oxford comma");
    CheckEqual(FormatList({ "甲", "乙", "丙" }, "zh-CN"),
        "甲、乙和丙", "Chinese lists must use ideographic separators");
    CheckEqual(FormatList({ "甲", "乙", "丙" }, "ja-JP"),
        "甲、乙、丙", "Japanese lists must use ideographic separators");
    CheckEqual(FormatList({ "A", "B" }, "de-DE"),
        "A und B", "German lists must use a localized conjunction");
    CheckEqual(FormatList({ "only" }, "en-US"), "only",
        "single-value lists must not add separators");
    Check(FormatList({}, "en-US").empty(),
        "empty lists must produce an empty string");
}
}

int main()
{
    TestNumberFormatting();
    TestByteFormatting();
    TestDurationFormatting();
    TestRelativeTimeFormatting();
    TestListFormatting();
    std::cout << "widget l10n format tests passed\n";
    return 0;
}
