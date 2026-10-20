#include "layout/folder_mapping_rules.h"
#include "desktop/folder_mapping_visibility_cases.h"
#include "layout/file_date_category.h"

#include <iostream>
#include <string>

namespace rules = snowdesktop::folder_mapping_rules;

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void CheckEqual(const std::wstring& actual,
    const std::wstring& expected, const char* message)
{
    if (actual == expected) return;
    ++failures;
    std::cerr << "FAILED: " << message
              << " (actual size=" << actual.size()
              << " expected size=" << expected.size() << ")\n";
}

} // namespace

int main()
{
    const auto date = [](int year, int month, int day) {
        SYSTEMTIME value{};
        value.wYear = static_cast<WORD>(year);
        value.wMonth = static_cast<WORD>(month);
        value.wDay = static_cast<WORD>(day);
        FILETIME ticks{};
        SystemTimeToFileTime(&value, &ticks);
        FileTimeToSystemTime(&ticks, &value);
        return value;
    };
    for (WORD month = 1; month <= 12; ++month)
    {
        const auto now = date(2027, month, 20);
        const auto previous = date(month == 1 ? 2026 : 2027, month == 1 ? 12 : month - 1, 15);
        for (bool folderFuture : {false, true})
            Check(snowdesktop::file_date_category::ForLocalDate(previous, now, folderFuture) == L"last_month",
                "file categories and mapped-folder groups recognize every previous month including December across New Year");
    }
    using snowdesktop::file_date_category::ForLocalDate;
    // File groups retain calendar-month grouping for future timestamps, while
    // mapped folders deliberately place every future date in the today group.
    for (WORD month = 1; month <= 12; ++month)
    {
        const auto now = date(2027, month, 20);
        const auto future = date(2027, month, 21);
        CheckEqual(ForLocalDate(future, now), L"this_month",
            "file categories retain same-month future dates in the current month");
        CheckEqual(ForLocalDate(future, now, true), L"today",
            "mapped folders retain same-month future dates in today");
    }
    CheckEqual(ForLocalDate(date(2027, 2, 1), date(2027, 1, 20)), L"this_year",
        "file categories group a future month in the same year as this year");
    CheckEqual(ForLocalDate(date(2027, 2, 1), date(2027, 1, 20), true), L"today",
        "mapped folders group a future month in the same year as today");
    CheckEqual(ForLocalDate(date(2028, 1, 1), date(2027, 12, 20)), L"older",
        "file categories retain their fallback group for a future year");
    CheckEqual(ForLocalDate(date(2028, 1, 1), date(2027, 12, 20), true), L"today",
        "mapped folders group a future year as today across New Year");
    CheckEqual(ForLocalDate(date(2028, 1, 21), date(2027, 1, 20)), L"older",
        "a matching month in a future year is not the current month");
    CheckEqual(ForLocalDate(date(2028, 1, 21), date(2027, 1, 20), true), L"today",
        "mapped folders group a matching month in a future year as today");
    Check(ForLocalDate(date(2026, 12, 31), date(2027, 1, 3)) == L"this_week",
        "a recent December file retains the more specific week bucket in January");
    Check(ForLocalDate(date(2027, 1, 19), date(2027, 1, 20)) == L"yesterday" &&
        ForLocalDate(date(2027, 1, 20), date(2027, 1, 20)) == L"today" &&
        ForLocalDate(date(2027, 1, 1), date(2027, 1, 20)) == L"this_month",
        "shared date grouping retains day and current-month precedence");
    Check(ForLocalDate(date(2026, 11, 15), date(2027, 1, 20)) == L"older" &&
        ForLocalDate(date(2027, 1, 15), date(2027, 3, 20)) == L"this_year" &&
        ForLocalDate(date(2028, 2, 29), date(2028, 3, 20)) == L"last_month",
        "previous-month grouping excludes unrelated years and handles leap days");
    CheckFolderMappingVisibility(Check);
    std::vector<std::wstring> source{L"C:\\a.txt", L"C:\\b.txt"};
    const auto snapshot = source;
    unsigned reads = 0;
    auto path = [&](const auto& value) -> const std::wstring& { ++reads; return value; };
    Check(rules::PathsMatch(source, snapshot, path) && reads == 2,
        "unchanged snapshot compares borrowed paths");
    source[0] = L"C:\\A.txt";
    Check(!rules::PathsMatch(source, snapshot, path), "case-only mutation permits a safe rebuild");
    source = snapshot; std::swap(source[0], source[1]);
    Check(!rules::PathsMatch(source, snapshot, path), "reordered entries invalidate their indices");
    source = snapshot; source.push_back(L"C:\\c.txt");
    Check(!rules::PathsMatch(source, snapshot, path), "added entries invalidate their indices");
    source = snapshot; source.pop_back();
    Check(!rules::PathsMatch(source, snapshot, path), "removed entries invalidate their indices");
    source.clear();
    Check(rules::PathsMatch(source, std::vector<std::wstring>{}, path), "empty snapshots match");
    // 普通目录：路径原样拼接
    CheckEqual(rules::ChildPath(L"D:\\Code\\工作区", L"a.txt"),
        L"D:\\Code\\工作区\\a.txt",
        "plain folder keeps single backslash");

    // 磁盘根目录：不得拼出双反斜杠
    CheckEqual(rules::ChildPath(L"C:\\", L"Windows"),
        L"C:\\Windows",
        "drive root must not produce double backslash");
    CheckEqual(rules::ChildPath(L"D:\\", L"Data"),
        L"D:\\Data",
        "drive root D must not produce double backslash");

    // 根目录带正斜杠结尾
    CheckEqual(rules::ChildPath(L"C:/", L"Users"),
        L"C:\\Users",
        "forward-slash drive root normalized");

    // 裸盘符（无尾部分隔符）
    CheckEqual(rules::ChildPath(L"C:", L"Temp"),
        L"C:\\Temp",
        "bare drive letter joined with backslash");

    // UNC 根目录
    CheckEqual(rules::ChildPath(L"\\\\server\\share\\", L"dir"),
        L"\\\\server\\share\\dir",
        "UNC root must not produce double backslash");
    CheckEqual(rules::ChildPath(L"\\\\server\\share", L"dir"),
        L"\\\\server\\share\\dir",
        "UNC root without trailing separator");

    // 多层尾部反斜杠只剥一层以上冗余
    CheckEqual(rules::ChildPath(L"C:\\data\\foo\\\\", L"bar"),
        L"C:\\data\\foo\\bar",
        "redundant trailing separators stripped");

    // 搜索通配符与子项名同样适用
    CheckEqual(rules::ChildPath(L"C:\\", L"*"),
        L"C:\\*",
        "search wildcard under drive root");

    if (failures == 0)
    {
        std::cout << "folder_mapping_rules tests passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
