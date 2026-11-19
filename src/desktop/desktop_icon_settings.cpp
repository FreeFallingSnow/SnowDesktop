#include "desktop_icon_settings.h"

#include "common/constants.h"
#include "common/utils.h"

/**
 * @brief 从注册表中读取桌面图标的可见性设置值。
 *
 * 尝试以 KEY_READ | KEY_WOW64_64KEY 权限打开注册表键，
 * 读取指定 CLSID 的 REG_DWORD 值。
 *
 * @param root 注册表根键（如 HKEY_CURRENT_USER）。
 * @param subKey 注册表子键路径。
 * @param clsid 桌面图标的 CLSID 字符串。
 * @param value [out] 读取到的 DWORD 值。
 * @return 读取成功时返回 true，否则返回 false。
 */
bool TryReadDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD& value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS &&
        RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD type = 0;
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    LONG status = RegQueryValueExW(key, clsid.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize);
    RegCloseKey(key);

    if (status == ERROR_SUCCESS && type == REG_DWORD && dataSize == sizeof(data))
    {
        value = data;
        return true;
    }

    return false;
}

/**
 * @brief 将桌面图标的可见性设置写入注册表。
 *
 * 创建或打开注册表键，写入指定 CLSID 的 REG_DWORD 值。
 * 优先尝试 KEY_WOW64_64KEY 访问。
 *
 * @param root 注册表根键。
 * @param subKey 注册表子键路径。
 * @param clsid 桌面图标的 CLSID 字符串。
 * @param value 要写入的 DWORD 值。
 * @return 写入成功时返回 true，否则返回 false。
 */
bool TryWriteDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS &&
        RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return false;
    }
    LONG status = RegSetValueExW(key, clsid.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

/**
 * @brief 写入桌面图标的可见性设置（NewStartPanel 路径）。
 *
 * visible 为 true 时写入 0（显示），false 时写入 1（隐藏）。
 *
 * @param clsid 桌面图标的 CLSID 字符串。
 * @param visible 是否可见。
 */
bool WriteDesktopIconRegistryValue(const std::wstring& clsid, bool visible)
{
    DWORD value = visible ? 0 : 1;
    return TryWriteDesktopIconRegistryValue(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel",
        clsid, value);
}

/**
 * @brief 在多个注册表位置中查找桌面图标的可见性设置。
 *
 * 依次查找 HKCU 和 HKLM 的 NewStartPanel 和 ClassicStartMenu 路径。
 *
 * @param clsid 桌面图标的 CLSID 字符串。
 * @param value [out] 找到的注册表值。
 * @return 在任意位置找到有效值时返回 true，否则返回 false。
 */
bool TryReadDesktopIconRegistryValueAnyRoot(const std::wstring& clsid, DWORD& value)
{
    static const wchar_t* keys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\ClassicStartMenu",
    };

    for (const wchar_t* key : keys)
    {
        if (TryReadDesktopIconRegistryValue(HKEY_CURRENT_USER, key, clsid, value))
        {
            return true;
        }
    }

    for (const wchar_t* key : keys)
    {
        if (TryReadDesktopIconRegistryValue(HKEY_LOCAL_MACHINE, key, clsid, value))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断桌面图标在系统设置中是否可见。
 *
 * 优先级：1) settingsIconVisibility 缓存；2) 注册表设置；
 * 3) 系统桌面图标默认可见性（此电脑、用户文件、网络、控制面板
 * 默认为隐藏，回收站默认为显示）；4) 其他已由 Shell 枚举的
 * 用户范围注册的命名空间图标可由调用方指定为默认显示。
 *
 * @param desktopIconClsid 桌面图标的 CLSID。
 * @param settingsIconVisibility 来自应用设置的图标可见性映射表。
 * @return 如果图标应可见则返回 true，否则返回 false。
 */
bool IsVisibleByDesktopIconSettings(
    const std::wstring& desktopIconClsid,
    const std::unordered_map<std::wstring, bool>& settingsIconVisibility,
    bool registeredNamespaceVisibleByDefault)
{
    std::wstring clsid = ToUpperInvariant(desktopIconClsid);
    if (clsid.empty())
    {
        return true;
    }

    auto settingsIt = settingsIconVisibility.find(clsid);
    if (settingsIt != settingsIconVisibility.end())
    {
        return settingsIt->second;
    }

    DWORD value = 0;
    if (TryReadDesktopIconRegistryValueAnyRoot(clsid, value))
    {
        return value == 0;
    }

    static const std::unordered_map<std::wstring, bool> defaultVisibility = {
        { kDesktopIconClsidThisPC, false },
        { kDesktopIconClsidUserFiles, false },
        { kDesktopIconClsidNetwork, false },
        { kDesktopIconClsidControlPanel, false },
        { kDesktopIconClsidRecycleBin, true },
    };

    auto found = defaultVisibility.find(clsid);
    if (found != defaultVisibility.end())
    {
        return found->second;
    }

    return registeredNamespaceVisibleByDefault;
}
