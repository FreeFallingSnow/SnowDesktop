#include "folder_self_drop_rules.h"

#include <iostream>
#include <string>
#include <vector>

namespace rules = snowdesktop::folder_self_drop_rules;

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

bool SelfDrop(std::vector<std::wstring> sources, const std::wstring& target)
{
    return rules::IsSelfContainedFolderDrop(sources, target);
}

} // namespace

int main()
{
    // 目标 == 源文件夹
    Check(SelfDrop({ L"C:\\data\\foo" }, L"C:\\data\\foo"),
        "target equal to source folder");
    // 目标为源的子目录（一层）
    Check(SelfDrop({ L"C:\\data\\foo" }, L"C:\\data\\foo\\bar"),
        "target inside source (one level)");
    // 目标为源的深层子目录
    Check(SelfDrop({ L"C:\\data\\foo" }, L"C:\\data\\foo\\bar\\baz\\qux"),
        "target inside source (deep)");
    // 大小写不敏感
    Check(SelfDrop({ L"c:\\DATA\\FOO" }, L"C:\\data\\foo\\bar"),
        "case-insensitive match");
    // 尾部分隔符被忽略
    Check(SelfDrop({ L"C:\\data\\foo\\" }, L"C:\\data\\foo"),
        "trailing separators ignored on source");
    Check(SelfDrop({ L"C:\\data\\foo" }, L"C:\\data\\foo\\"),
        "trailing separators ignored on target");
    // 正斜杠与反斜杠混用
    Check(SelfDrop({ L"C:/data/foo" }, L"C:\\data\\foo\\bar"),
        "forward slash normalized");
    // 多源：任意一个命中即拒绝
    Check(SelfDrop({ L"C:\\other\\a", L"C:\\data\\foo" },
            L"C:\\data\\foo\\bar"),
        "any source matching blocks");
    // 目标 == 源且源带 .lnk 之外的普通目录
    Check(SelfDrop({ L"D:\\work" }, L"D:\\work\\"),
        "equal with trailing separator");

    // 合法场景：不得误拦
    Check(!SelfDrop({ L"C:\\data\\foo" }, L"C:\\data"),
        "target is parent of source (allowed)");
    Check(!SelfDrop({ L"C:\\data\\foo" }, L"C:\\data\\other"),
        "unrelated sibling folder (allowed)");
    Check(!SelfDrop({ L"C:\\data\\foo" }, L"C:\\data\\foobar"),
        "name prefix without separator (allowed)");
    Check(!SelfDrop({ L"C:\\data\\file.txt" }, L"C:\\data\\folder"),
        "file source into unrelated folder (allowed)");
    Check(!SelfDrop({ L"C:\\data\\file.txt" }, L"C:\\data"),
        "file source into its parent folder (allowed)");
    Check(!SelfDrop({}, L"C:\\data\\foo"),
        "empty source list (allowed)");
    Check(!SelfDrop({ L"C:\\data\\foo" }, L""),
        "empty target (allowed)");
    Check(!SelfDrop({ L"" }, L"C:\\data\\foo"),
        "empty source path (allowed)");

    // 目标在源内部但未命中分隔符边界
    Check(!SelfDrop({ L"C:\\foo" }, L"C:\\foo2"),
        "prefix overlap without separator (allowed)");

    if (failures == 0)
    {
        std::cout << "folder_self_drop_rules: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
