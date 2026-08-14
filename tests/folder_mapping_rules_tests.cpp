#include "folder_mapping_rules.h"

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
