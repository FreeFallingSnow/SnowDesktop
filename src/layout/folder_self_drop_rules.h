#pragma once

#include <cwctype>
#include <string>
#include <vector>

namespace snowdesktop::folder_self_drop_rules
{

/**
 * @brief 规范化文件夹路径用于自包含比较。
 *
 * 去除尾部分隔符，将内部正斜杠统一为反斜杠，并按
 * 不区分大小写的方式比较。
 */
inline std::wstring NormalizeFolderPath(std::wstring path)
{
    while (!path.empty() &&
           (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    for (auto& ch : path)
    {
        if (ch == L'/')
            ch = L'\\';
        else
            ch = static_cast<wchar_t>(
                std::towupper(static_cast<wchar_t>(ch)));
    }
    return path;
}

/**
 * @brief 目标文件夹是否等于某个源文件夹，或位于某个源文件夹内部。
 *
 * 与 Explorer 的规则一致：把文件夹拖进它自身（或拖进它的任意
 * 子目录）必须被拒绝，否则 Shell 文件操作会递归地把文件夹复制
 * 进自己。仅比较源文件夹；普通文件不可能作为目录祖先，因此
 * 文件路径不会误命中。
 *
 * @param sourcePaths 拖拽源的路径列表
 * @param targetFolder 目标文件夹路径
 * @return 目标包含于源内部时返回 true
 */
inline bool IsSelfContainedFolderDrop(
    const std::vector<std::wstring>& sourcePaths,
    const std::wstring& targetFolder)
{
    const std::wstring target =
        NormalizeFolderPath(targetFolder);
    if (target.empty())
        return false;

    for (const std::wstring& source : sourcePaths)
    {
        const std::wstring normalized =
            NormalizeFolderPath(source);
        if (normalized.empty())
            continue;
        if (normalized == target)
            return true;
        if (target.size() > normalized.size() &&
            target.compare(0, normalized.size(), normalized) == 0 &&
            (target[normalized.size()] == L'\\' ||
             target[normalized.size()] == L'/'))
            return true;
    }
    return false;
}

} // namespace snowdesktop::folder_self_drop_rules
