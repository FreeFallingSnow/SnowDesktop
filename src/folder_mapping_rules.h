#pragma once

#include <string>

namespace snowdesktop::folder_mapping_rules
{

/**
 * @brief 拼接映射目录下的子项路径。
 *
 * 先剥离源目录路径尾部的分隔符，再用单一反斜杠连接子项名。
 * 磁盘根目录（如 "C:\"）自身以反斜杠结尾，若直接拼接会得到
 * "C:\\名称" 这种双反斜杠路径；SHParseDisplayName 会以
 * E_INVALIDARG 拒绝它，导致图标加载任务永远无法入队、
 * 条目只能停留在占位图标。UNC 根目录同理。
 *
 * @param basePath 源目录路径（可带尾部分隔符）
 * @param name 子项名称或搜索通配符
 * @return 规范化后的子项路径
 */
inline std::wstring ChildPath(
    const std::wstring& basePath,
    const std::wstring& name)
{
    std::wstring base = basePath;
    while (base.size() > 1 &&
        (base.back() == L'\\' || base.back() == L'/'))
        base.pop_back();
    return base + L'\\' + name;
}

} // namespace snowdesktop::folder_mapping_rules
