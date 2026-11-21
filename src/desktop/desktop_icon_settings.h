#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>

/**
 * @brief 尝试读取桌面图标注册表值
 * @details 从指定根键下读取桌面图标的 DWORD 类型注册表值（如 Flags、ShowState 等）。
 * @param root 注册表根键（HKEY_CURRENT_USER 或 HKEY_LOCAL_MACHINE）
 * @param subKey 子键路径
 * @param clsid 图标的 CLSID 字符串
 * @param value [out] 读取到的 DWORD 值
 * @return 成功读取到值返回 true，键不存在或读取失败返回 false
 */
bool TryReadDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD& value);

/**
 * @brief 尝试写入桌面图标注册表值
 * @details 将指定的 DWORD 值写入指定根键下桌面图标对应的注册表项。
 * @param root 注册表根键（HKEY_CURRENT_USER 或 HKEY_LOCAL_MACHINE）
 * @param subKey 子键路径
 * @param clsid 图标的 CLSID 字符串
 * @param value 待写入的 DWORD 值
 * @return 写入成功返回 true，写入失败返回 false
 */
bool TryWriteDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD value);

/**
 * @brief 写入桌面图标可见性设置
 * @details 根据 visible 参数设置桌面图标在注册表中的可见性状态（隐藏或显示）。
 *          写入当前用户的 Explorer 桌面图标设置。
 * @param clsid 图标的 CLSID 字符串
 * @param visible true 表示显示该图标，false 表示隐藏该图标
 */
bool WriteDesktopIconRegistryValue(const std::wstring& clsid, bool visible);

/**
 * @brief 尝试从任意根键读取桌面图标注册表值
 * @details 依次尝试 HKEY_CURRENT_USER 和 HKEY_LOCAL_MACHINE，
 *          从任一成功的位置读取桌面图标的 DWORD 值。
 * @param clsid 图标的 CLSID 字符串
 * @param value [out] 读取到的 DWORD 值
 * @return 任一位置成功读取返回 true，均失败返回 false
 */
bool TryReadDesktopIconRegistryValueAnyRoot(const std::wstring& clsid, DWORD& value);

/**
 * @brief 根据注册表设置判断桌面图标是否可见
 * @details 通过桌面图标的 CLSID 查找其在注册表中的设置状态，
 *          综合 iconVisibility 映射表中的覆盖策略，判断该图标是否应当显示。
 * @param desktopIconClsid 桌面图标的 CLSID 字符串
 * @param settingsIconVisibility CLSID 到可见性布尔值的映射表
 * @return 图标可见返回 true，不可见返回 false
 */
bool IsVisibleByDesktopIconSettings(
    const std::wstring& desktopIconClsid,
    const std::unordered_map<std::wstring, bool>& settingsIconVisibility,
    bool registeredNamespaceVisibleByDefault = false);
