#include "widget_runtime_diagnostics.h"

#include <cstdlib>
#include <iostream>
#include <string>

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

void TestLevelNormalizationAndFiltering()
{
    snowdesktop::widget_runtime::DiagnosticsLog diagnostics;
    diagnostics.Add("first", "", "default level");
    diagnostics.Add("second", "warn", "other widget");
    diagnostics.Add("first", "error", "latest");

    const auto first = diagnostics.EntriesFor("first");
    Check(first.size() == 2,
        "filtering must return only the requested widget entries");
    Check(first[0].level == "info" &&
            first[0].message == "default level",
        "empty levels must normalize to info");
    Check(first[1].level == "error" &&
            first[1].message == "latest",
        "filtered entries must preserve insertion order and content");
    Check(diagnostics.EntriesFor("missing").empty(),
        "unknown widgets must have no diagnostic entries");
}

void TestBoundedHistory()
{
    snowdesktop::widget_runtime::DiagnosticsLog diagnostics;
    const std::size_t inserted =
        snowdesktop::widget_runtime::DiagnosticsLog::MaxEntries + 3;
    for (std::size_t index = 0; index < inserted; ++index)
    {
        diagnostics.Add(
            "widget", "debug", std::to_string(index));
    }

    Check(
        diagnostics.Size() ==
            snowdesktop::widget_runtime::DiagnosticsLog::MaxEntries,
        "diagnostic history must remain bounded");
    const auto entries = diagnostics.EntriesFor("widget");
    Check(entries.size() == diagnostics.Size(),
        "bounded history must remain queryable");
    Check(entries.front().message == "3",
        "bounded history must evict the oldest entries first");
    Check(entries.back().message == std::to_string(inserted - 1),
        "bounded history must retain the newest entry");
}
}

int main()
{
    TestLevelNormalizationAndFiltering();
    TestBoundedHistory();
    std::cout << "widget runtime diagnostics tests passed\n";
    return 0;
}
