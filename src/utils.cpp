/**
 * @file utils.cpp
 * @brief 杂项工具函数实现
 *
 * 本文件汇集了桌面窗口枚举、资源加载、字符串/路径处理、
 * 注册表操作、位图/图标创建、RECT 几何计算以及 UTF-8/Wide
 * 字符串转换与 JSON 解析等通用工具函数。
 */

#include "utils.h"
#include "resource.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <d2d1_1.h>
#include <dwrite_3.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_set>
#include <vector>

// ============================================================================
// 窗口枚举回调函数
// ============================================================================

/**
 * @brief EnumWindows 回调函数，查找 SHELLDLL_DefView 窗口。
 *
 * 遍历顶层窗口，查找包含 "SHELLDLL_DefView" 子窗口的父窗口，
 * 并将结果写入 search 结构体。找到后返回 FALSE 以停止枚举。
 *
 * @param hwnd 当前枚举到的窗口句柄。
 * @param lParam 指向 DefViewSearch 结构的指针。
 * @return FALSE 表示找到目标并停止枚举；TRUE 表示继续枚举。
 */
BOOL CALLBACK FindDefViewProc(HWND hwnd, LPARAM lParam)
{
    auto* search = reinterpret_cast<DefViewSearch*>(lParam);
    HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView != nullptr)
    {
        search->defView = defView;
        search->parent = hwnd;
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief EnumDisplayMonitors 回调函数，枚举所有显示器并构建 GridPage 列表。
 *
 * 为每台显示器创建 GridPage 记录，包含显示器 ID、是否为
 * 主显示器、相对于虚拟桌面左上角的边界矩形和工作区矩形。
 *
 * @param monitor 当前枚举到的显示器句柄。
 * @param lParam 指向 MonitorEnumContext 结构的指针。
 * @return TRUE 表示继续枚举。
 */
BOOL CALLBACK EnumGridPageMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam)
{
    auto* context = reinterpret_cast<MonitorEnumContext*>(lParam);
    if (context == nullptr || context->pages == nullptr)
    {
        return TRUE;
    }

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return TRUE;
    }

    GridPage page;
    page.monitorId = monitorInfo.szDevice[0] != L'\0'
        ? monitorInfo.szDevice
        : (L"Monitor" + std::to_wstring(context->pages->size()));
    page.id = page.monitorId;
    page.isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &page.dpiX, &page.dpiY);
    page.bounds = {
        monitorInfo.rcMonitor.left - context->virtualLeft,
        monitorInfo.rcMonitor.top - context->virtualTop,
        monitorInfo.rcMonitor.right - context->virtualLeft,
        monitorInfo.rcMonitor.bottom - context->virtualTop,
    };
    page.workArea = {
        monitorInfo.rcWork.left - context->virtualLeft,
        monitorInfo.rcWork.top - context->virtualTop,
        monitorInfo.rcWork.right - context->virtualLeft,
        monitorInfo.rcWork.bottom - context->virtualTop,
    };

    context->pages->push_back(page);
    return TRUE;
}

// ============================================================================
// 资源加载函数
// ============================================================================

/**
 * @brief 从应用程序资源中加载应用程序图标。
 *
 * 从当前模块资源中加载 IDI_APPICON 图标，尺寸为 32x32。
 *
 * @return 成功时返回图标句柄，失败时返回 nullptr。
 */
HICON LoadAppIcon()
{
    HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON,
        32, 32,
        LR_DEFAULTSIZE));
    return icon;
}

/**
 * @brief 从资源中加载 Font Awesome 字体到内存中。
 *
 * 从 RT_RCDATA 资源中读取 Font Awesome 字体数据，
 * 并通过 AddFontMemResourceEx 注册为内存字体。
 *
 * @return 成功时返回字体句柄，失败时返回 nullptr。
 */
HANDLE LoadFontAwesome()
{
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_FA_FONT), RT_RCDATA);
    if (res == nullptr) return nullptr;
    HGLOBAL handle = LoadResource(module, res);
    if (handle == nullptr) return nullptr;
    void* data = LockResource(handle);
    DWORD size = SizeofResource(module, res);
    if (data == nullptr || size == 0) return nullptr;

    DWORD count = 0;
    HANDLE fontHandle = AddFontMemResourceEx(data, size, nullptr, &count);
    return fontHandle;
}

namespace
{
    const void* g_faFontData = nullptr;
    DWORD g_faFontSize = 0;

    /**
     * @brief 构建 Font Awesome 内存字体的 DirectWrite 字体集合。
     *
     * 使用 IDWriteInMemoryFontFileLoader 从内存字体数据创建
     * IDWriteFontCollection1 集合，供 CreateTextFormat 使用。
     *
     * @param factory IDWriteFactory 接口指针。
     * @return 成功时返回字体集合，失败时返回 nullptr。
     */
    ComPtr<IDWriteFontCollection1> BuildFaFontCollection(IDWriteFactory* factory)
    {
        ComPtr<IDWriteFactory5> factory5;
        if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
        {
            return nullptr;
        }

        ComPtr<IDWriteInMemoryFontFileLoader> loader;
        if (FAILED(factory5->CreateInMemoryFontFileLoader(&loader)))
        {
            return nullptr;
        }
        if (FAILED(factory5->RegisterFontFileLoader(loader.Get())))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontFile> fontFile;
        if (FAILED(loader->CreateInMemoryFontFileReference(
                factory5.Get(),
                g_faFontData,
                g_faFontSize,
                nullptr,
                &fontFile)))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontSetBuilder1> builder;
        if (FAILED(factory5->CreateFontSetBuilder(&builder)))
        {
            return nullptr;
        }
        if (FAILED(builder->AddFontFile(fontFile.Get())))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontSet> fontSet;
        if (FAILED(builder->CreateFontSet(&fontSet)))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontCollection1> collection;
        factory5->CreateFontCollectionFromFontSet(fontSet.Get(), collection.GetAddressOf());
        return collection;
    }
}

/**
 * @brief 创建 Font Awesome 文本格式对象。
 *
 * 首次调用时缓存字体数据，构建字体集合，然后创建居中对齐的
 * IDWriteTextFormat 用于渲染 Font Awesome 图标字符。
 *
 * @param factory IDWriteFactory 接口指针，不能为 nullptr。
 * @param fontSize 字体大小（DIP）。
 * @return 成功时返回 IDWriteTextFormat 指针，失败时返回 nullptr。
 */
IDWriteTextFormat* CreateFaTextFormat(IDWriteFactory* factory, float fontSize)
{
    if (factory == nullptr) return nullptr;

    // Cache font data on first call
    if (g_faFontData == nullptr)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_FA_FONT), RT_RCDATA);
        if (res != nullptr)
        {
            HGLOBAL handle = LoadResource(module, res);
            if (handle != nullptr)
            {
                g_faFontData = LockResource(handle);
                g_faFontSize = SizeofResource(module, res);
            }
        }
    }

    static ComPtr<IDWriteFontCollection1> s_faFontCollection;
    ComPtr<IDWriteFontCollection1> fontCollection;
    if (g_faFontData != nullptr)
    {
        if (s_faFontCollection)
            fontCollection = s_faFontCollection;
        else
        {
            fontCollection = BuildFaFontCollection(factory);
            s_faFontCollection = fontCollection;
        }
    }

    IDWriteTextFormat* format = nullptr;
    HRESULT hr = factory->CreateTextFormat(
        L"Font Awesome 6 Free Solid",
        fontCollection.Get(),
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"",
        &format);
    if (FAILED(hr)) return nullptr;

    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return format;
}

// ============================================================================
// 桌面窗口管理
// ============================================================================

/**
 * @brief 查找桌面窗口及其子窗口（Progman、DefView、ListView）。
 *
 * 通过查找 "Progman" 窗口并发送 0x052C 消息触发工作区创建，
 * 然后递归查找 SHELLDLL_DefView 和 SysListView32 子窗口。
 * 如果在 Progman 下未找到 DefView，则通过 EnumWindows 全局查找。
 *
 * @return DesktopWindows 结构体，包含 progman、defView、host、listView 句柄。
 */
DesktopWindows FindDesktopWindows()
{
    DesktopWindows result{};
    result.progman = FindWindowW(L"Progman", nullptr);

    if (result.progman != nullptr)
    {
        DWORD_PTR unused = 0;
        SendMessageTimeoutW(result.progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &unused);
    }

    DefViewSearch search{};
    if (result.progman != nullptr)
    {
        search.defView = FindWindowExW(result.progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (search.defView != nullptr)
        {
            search.parent = result.progman;
        }
    }

    if (search.defView == nullptr)
    {
        EnumWindows(FindDefViewProc, reinterpret_cast<LPARAM>(&search));
    }

    result.defView = search.defView;
    result.host = search.parent != nullptr ? search.parent : result.progman;

    if (result.defView != nullptr)
    {
        result.listView = FindWindowExW(result.defView, nullptr, L"SysListView32", nullptr);
    }

    return result;
}

/**
 * @brief 立即恢复资源管理器桌面图标层。
 *
 * 显示 ListView 窗口并移除隐藏属性，使被 SnowDesktop 隐藏的
 * 桌面图标重新可见。
 */
void RestoreExplorerIconLayerNow()
{
    DesktopWindows windows = FindDesktopWindows();
    if (windows.listView != nullptr && IsWindow(windows.listView))
    {
        ShowWindow(windows.listView, SW_SHOW);
        RemovePropW(windows.listView, kHiddenBySnowDesktopProp);
    }
}

// ============================================================================
// 字符串与路径工具函数
// ============================================================================

/**
 * @brief 将 IShellFolder 的显示名称转换为 std::wstring。
 *
 * 调用 GetDisplayNameOf 获取 STRRET，再通过 StrRetToBufW
 * 转换为宽字符串。
 *
 * @param folder IShellFolder 接口指针。
 * @param child 子项的 PIDL。
 * @param flags 显示名称的获取标志（SHGDNF）。
 * @return 成功时返回显示名称字符串，失败时返回空字符串。
 */
std::wstring StrRetToString(IShellFolder* folder, PCUITEMID_CHILD child, SHGDNF flags)
{
    STRRET str{};
    if (FAILED(folder->GetDisplayNameOf(child, flags, &str)))
    {
        return {};
    }

    wchar_t buffer[MAX_PATH * 4]{};
    if (FAILED(StrRetToBufW(&str, child, buffer, static_cast<UINT>(std::size(buffer)))))
    {
        return {};
    }

    return buffer;
}

/**
 * @brief 将字符串转换为大写（原地转换）。
 *
 * 使用 CharUpperBuffW API 将字符串中的小写字母转为大写。
 *
 * @param value 要转换的字符串。
 * @return 转换后的大写字符串。
 */
std::wstring ToUpperInvariant(std::wstring value)
{
    CharUpperBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

/**
 * @brief 从解析名中提取 CLSID（花括号内的 GUID 文本）。
 *
 * 在字符串中查找 '{' 和 '}' 之间的内容并转为大写返回。
 *
 * @param parsingName 包含 CLSID 的解析名字符串。
 * @return 找到时返回大写的 CLSID 字符串（含花括号），未找到时返回空字符串。
 */
std::wstring ExtractClsidText(const std::wstring& parsingName)
{
    size_t open = parsingName.find(L'{');
    size_t close = parsingName.find(L'}', open == std::wstring::npos ? 0 : open);
    if (open == std::wstring::npos || close == std::wstring::npos || close <= open)
    {
        return {};
    }

    return ToUpperInvariant(parsingName.substr(open, close - open + 1));
}

/**
 * @brief 去除路径末尾的分隔符（'\\' 和 '/'），但保留驱动器根路径（如 "C:\\"）。
 *
 * @param path 原始路径字符串。
 * @return 去除末尾分隔符后的路径字符串。
 */
std::wstring TrimTrailingPathSeparators(std::wstring path)
{
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.pop_back();
    }
    return path;
}

/**
 * @brief 不区分大小写比较两个路径是否相等（去除末尾分隔符后）。
 *
 * 使用 CompareStringOrdinal 进行不区分大小写的序数比较。
 *
 * @param left 左侧路径。
 * @param right 右侧路径。
 * @return 如果路径相等则返回 true，否则返回 false。
 */
bool PathsEqualInsensitive(std::wstring left, std::wstring right)
{
    left = TrimTrailingPathSeparators(std::move(left));
    right = TrimTrailingPathSeparators(std::move(right));
    if (left.empty() || right.empty())
    {
        return false;
    }
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

/**
 * @brief 解析桌面图标的 CLSID。
 *
 * 优先从 parsingName 中提取 CLSID；如果 parsingName 中没有
 * GUID，且 itemPath 与用户配置文件夹路径匹配，则返回
 * 用户文件桌面图标 CLSID。
 *
 * @param parsingName 项解析名字符串。
 * @param itemPath 项的文件系统路径。
 * @param userProfilePath 用户配置文件夹路径。
 * @return 解析得到的 CLSID 字符串，或空字符串。
 */
std::wstring ResolveDesktopIconClsid(
    const std::wstring& parsingName,
    const std::wstring& itemPath,
    const std::wstring& userProfilePath)
{
    std::wstring clsid = ExtractClsidText(parsingName);
    if (!clsid.empty())
    {
        return clsid;
    }

    if (PathsEqualInsensitive(itemPath, userProfilePath))
    {
        return kDesktopIconClsidUserFiles;
    }

    return {};
}

// ============================================================================
// 桌面图标注册表操作
// ============================================================================

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
void WriteDesktopIconRegistryValue(const std::wstring& clsid, bool visible)
{
    DWORD value = visible ? 0 : 1;
    TryWriteDesktopIconRegistryValue(HKEY_CURRENT_USER,
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
 * 默认为隐藏，回收站默认为显示）；4) 其他图标默认为隐藏。
 *
 * @param desktopIconClsid 桌面图标的 CLSID。
 * @param settingsIconVisibility 来自应用设置的图标可见性映射表。
 * @return 如果图标应可见则返回 true，否则返回 false。
 */
bool IsVisibleByDesktopIconSettings(const std::wstring& desktopIconClsid, const std::unordered_map<std::wstring, bool>& settingsIconVisibility)
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

    return false;
}

// ============================================================================
// 位图与图标创建/操作函数
// ============================================================================

/**
 * @brief 创建 32 位自顶向下的 DIB 节位图。
 *
 * biHeight 设为负数以创建自顶向下（非翻转）的 DIB 位图。
 *
 * @param referenceDc 参考 DC 句柄。
 * @param width 位图宽度（像素）。
 * @param height 位图高度（像素）。
 * @param bits [out] 指向位图像素数据的指针。
 * @return 成功时返回 HBITMAP 句柄，失败时返回 nullptr。
 */
HBITMAP CreateTopDown32BppDib(HDC referenceDc, int width, int height, void** bits)
{
    if (width <= 0 || height <= 0)
    {
        return nullptr;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    return CreateDIBSection(referenceDc, &bitmapInfo, DIB_RGB_COLORS, bits, nullptr, 0);
}

/**
 * @brief 对 BGRA 像素缓冲区执行预乘 Alpha 操作。
 *
 * 遍历像素检查是否有需要预乘的像素（Alpha 分量不为 0 或 255
 * 且颜色分量超过 Alpha），如有则对每个非纯透明像素执行
 * 预乘 Alpha 计算，使颜色值乘以 Alpha/255。
 *
 * @param pixels BGRA 像素数据指针。
 * @param width 图像宽度（像素）。
 * @param height 图像高度（像素）。
 */
void PremultiplyBgraPixels(std::uint32_t* pixels, int width, int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return;
    }

    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    bool needsPremultiply = false;
    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
        if (a == 0 || a == 255)
        {
            continue;
        }

        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        if (r > a || g > a || b > a)
        {
            needsPremultiply = true;
            break;
        }
    }

    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);

        if (a == 0)
        {
            pixels[i] = 0;
            continue;
        }

        if (needsPremultiply && a < 255)
        {
            r = static_cast<std::uint8_t>((static_cast<int>(r) * a + 127) / 255);
            g = static_cast<std::uint8_t>((static_cast<int>(g) * a + 127) / 255);
            b = static_cast<std::uint8_t>((static_cast<int>(b) * a + 127) / 255);
            pixels[i] = (static_cast<std::uint32_t>(a) << 24) |
                (static_cast<std::uint32_t>(r) << 16) |
                (static_cast<std::uint32_t>(g) << 8) |
                static_cast<std::uint32_t>(b);
        }
    }
}

/**
 * @brief 将源位图复制为带有 Alpha 通道的 DIB 位图。
 *
 * 通过 GetDIBits 获取源位图像素数据，检查是否需要为无 Alpha
 * 但有可见像素的位图添加不透明 Alpha 通道，然后执行预乘 Alpha
 * 并创建新的自顶向下 DIB 位图。
 *
 * @param source 源位图句柄。
 * @param size [out] 输出位图的尺寸。
 * @return 成功时返回新的 HBITMAP 句柄，失败时返回 nullptr。
 */
HBITMAP CopyBitmapToAlphaDib(HBITMAP source, SIZE& size)
{
    size = {};
    if (source == nullptr)
    {
        return nullptr;
    }

    BITMAP sourceInfo{};
    if (GetObjectW(source, sizeof(sourceInfo), &sourceInfo) == 0 ||
        sourceInfo.bmWidth <= 0 ||
        sourceInfo.bmHeight <= 0)
    {
        return nullptr;
    }

    const int width = sourceInfo.bmWidth;
    const int height = sourceInfo.bmHeight;
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr)
    {
        return nullptr;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
    if (GetDIBits(screenDc, source, 0, static_cast<UINT>(height), pixels.data(), &bitmapInfo, DIB_RGB_COLORS) == 0)
    {
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    bool hasAlpha = false;
    bool hasVisiblePixels = false;
    for (std::uint32_t pixel : pixels)
    {
        std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
        if (a != 0)
        {
            hasAlpha = true;
        }
        if ((pixel & 0x00ffffff) != 0)
        {
            hasVisiblePixels = true;
        }
    }

    if (!hasAlpha && hasVisiblePixels)
    {
        for (std::uint32_t& pixel : pixels)
        {
            pixel |= 0xff000000;
        }
    }

    PremultiplyBgraPixels(pixels.data(), width, height);

    void* bits = nullptr;
    HBITMAP copy = CreateTopDown32BppDib(screenDc, width, height, &bits);
    if (copy != nullptr && bits != nullptr)
    {
        std::copy(pixels.begin(), pixels.end(), static_cast<std::uint32_t*>(bits));
        size = { width, height };
    }
    ReleaseDC(nullptr, screenDc);
    return copy;
}

/**
 * @brief 从图标创建带有 Alpha 通道的位图。
 *
 * 创建自顶向下 DIB 位图，使用 DrawIconEx 将图标绘制到位图上，
 * 然后执行 Alpha 检查与预乘操作。
 *
 * @param icon 源图标句柄。
 * @param width 目标位图宽度。
 * @param height 目标位图高度。
 * @param size [out] 输出位图的尺寸。
 * @return 成功时返回 HBITMAP 句柄，失败时返回 nullptr。
 */
HBITMAP CreateAlphaBitmapFromIcon(HICON icon, int width, int height, SIZE& size)
{
    size = {};
    if (icon == nullptr || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr)
    {
        return nullptr;
    }

    void* bits = nullptr;
    HBITMAP bitmap = CreateTopDown32BppDib(screenDc, width, height, &bits);
    if (bitmap == nullptr || bits == nullptr)
    {
        if (bitmap != nullptr)
        {
            DeleteObject(bitmap);
        }
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::fill(pixels, pixels + (static_cast<size_t>(width) * static_cast<size_t>(height)), 0);

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    DrawIconEx(memoryDc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL);
    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);

    bool hasAlpha = false;
    bool hasVisiblePixels = false;
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t pixel = pixels[i];
        if (((pixel >> 24) & 0xff) != 0)
        {
            hasAlpha = true;
        }
        if ((pixel & 0x00ffffff) != 0)
        {
            hasVisiblePixels = true;
        }
    }

    if (!hasAlpha && hasVisiblePixels)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if ((pixels[i] & 0x00ffffff) != 0)
            {
                pixels[i] |= 0xff000000;
            }
        }
    }
    PremultiplyBgraPixels(pixels, width, height);

    size = { width, height };
    ReleaseDC(nullptr, screenDc);
    return bitmap;
}

/**
 * @brief 获取 Shell 项的高分辨率图标位图。
 *
 * 优先通过 IShellItemImageFactory 获取高分辨率缩略图或图标，
 * 失败时依次尝试 JUMBO/EXTRALARGE/LARGE 图像列表，
 * 最后回退到 SHGetFileInfo 获取图标。
 *
 * @param pidl Shell 项的绝对 PIDL。
 * @param fallbackIndex 图像列表中的回退索引。
 * @param bitmapSize [out] 输出位图的尺寸。
 * @return 成功时返回带有 Alpha 通道的 HBITMAP，失败时返回 nullptr。
 */
HBITMAP GetHighResolutionShellIconBitmap(PCIDLIST_ABSOLUTE pidl, int fallbackIndex, SIZE& bitmapSize, bool fullQuality)
{
    bitmapSize = {};
    ComPtr<IShellItemImageFactory> imageFactory;
    if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&imageFactory))) && imageFactory)
    {
        SIZE size{ kIconBitmapSize, kIconBitmapSize };
        HBITMAP bitmap = nullptr;
        UINT flags = fullQuality ? SIIGBF_RESIZETOFIT : SIIGBF_ICONONLY;
        if (SUCCEEDED(imageFactory->GetImage(size, flags, &bitmap)) && bitmap != nullptr)
        {
            HBITMAP alphaBitmap = CopyBitmapToAlphaDib(bitmap, bitmapSize);
            DeleteObject(bitmap);
            if (alphaBitmap != nullptr)
            {
                return alphaBitmap;
            }
        }
    }

    ComPtr<IImageList> imageList;
    HRESULT hr = SHGetImageList(SHIL_JUMBO, __uuidof(IImageList), reinterpret_cast<void**>(imageList.GetAddressOf()));
    if (FAILED(hr) || !imageList)
    {
        imageList.Reset();
        hr = SHGetImageList(SHIL_EXTRALARGE, __uuidof(IImageList), reinterpret_cast<void**>(imageList.GetAddressOf()));
    }
    if (FAILED(hr) || !imageList)
    {
        imageList.Reset();
        hr = SHGetImageList(SHIL_LARGE, __uuidof(IImageList), reinterpret_cast<void**>(imageList.GetAddressOf()));
    }

    HICON icon = nullptr;
    if (SUCCEEDED(hr) && imageList && fallbackIndex >= 0)
    {
        imageList->GetIcon(fallbackIndex, ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon);
        if (icon != nullptr)
        {
            HBITMAP bitmap = CreateAlphaBitmapFromIcon(icon, kIconBitmapSize, kIconBitmapSize, bitmapSize);
            DestroyIcon(icon);
            if (bitmap != nullptr)
            {
                return bitmap;
            }
        }
    }

    SHFILEINFOW iconInfo{};
    SHGetFileInfoW(
        reinterpret_cast<LPCWSTR>(pidl),
        0,
        &iconInfo,
        sizeof(iconInfo),
        SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS);
    icon = iconInfo.hIcon;
    if (icon != nullptr)
    {
        HBITMAP bitmap = CreateAlphaBitmapFromIcon(icon, kIconBitmapSize, kIconBitmapSize, bitmapSize);
        DestroyIcon(icon);
        return bitmap;
    }

    return nullptr;
}

// ============================================================================
// RECT 几何工具函数
// ============================================================================

/**
 * @brief 根据四个边创建 RECT 结构。
 *
 * @param left 左边界。
 * @param top 上边界。
 * @param right 右边界。
 * @param bottom 下边界。
 * @return 构造的 RECT 结构。
 */
RECT MakeRect(int left, int top, int right, int bottom)
{
    RECT r{ left, top, right, bottom };
    return r;
}

/**
 * @brief 判断两个 RECT 是否相交。
 *
 * 封装 IntersectRect API。
 *
 * @param a 第一个矩形。
 * @param b 第二个矩形。
 * @return 如果相交则返回 true，否则返回 false。
 */
bool RectsIntersect(const RECT& a, const RECT& b)
{
    RECT tmp{};
    return IntersectRect(&tmp, &a, &b) != FALSE;
}

/**
 * @brief 根据两个对角点创建归一化的 RECT（确保 left < right, top < bottom）。
 *
 * 取最小 x/y 作为 left/top，最大 x/y 作为 right/bottom。
 *
 * @param a 第一个点。
 * @param b 第二个点。
 * @return 归一化后的 RECT。
 */
RECT NormalizeRect(POINT a, POINT b)
{
    return MakeRect(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

/**
 * @brief 判断 RECT 是否为空（宽度或高度小于等于 0）。
 *
 * @param rect 要判断的矩形。
 * @return 如果矩形为空则返回 true，否则返回 false。
 */
bool IsRectEmptyRect(const RECT& rect)
{
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

/**
 * @brief 对 RECT 进行膨胀（向内为负值）并返回副本。
 *
 * 对矩形的四个边同时增加指定的像素量。
 *
 * @param rect 原始矩形。
 * @param amount 每个边膨胀的像素数。
 * @return 膨胀后的 RECT。
 */
RECT InflateCopy(RECT rect, int amount)
{
    InflateRect(&rect, amount, amount);
    return rect;
}

/**
 * @brief 计算两个 RECT 的并集矩形。
 *
 * 如果任一矩形为空，则直接返回另一个矩形。
 *
 * @param a 第一个矩形。
 * @param b 第二个矩形。
 * @return 包含两个矩形的最小矩形。
 */
RECT UnionCopy(const RECT& a, const RECT& b)
{
    if (IsRectEmptyRect(a))
    {
        return b;
    }
    if (IsRectEmptyRect(b))
    {
        return a;
    }

    RECT result{};
    UnionRect(&result, &a, &b);
    return result;
}

// ============================================================================
// UTF-8/Wide 字符串转换与 JSON 处理
// ============================================================================

/**
 * @brief 将宽字符串（UTF-16）转换为 UTF-8 编码字符串。
 *
 * 使用 WideCharToMultiByte API 进行转换。
 *
 * @param value 要转换的宽字符串。
 * @return UTF-8 编码的 std::string。
 */
std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

/**
 * @brief 将 UTF-8 字符串转换为宽字符串（UTF-16）。
 *
 * 使用 MultiByteToWideChar API 进行转换。
 *
 * @param value 要转换的 UTF-8 字符串。
 * @return UTF-16 编码的 std::wstring。
 */
std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

/**
 * @brief 对宽字符串进行 JSON 转义，输出 UTF-8 格式字符串。
 *
 * 转义特殊字符：反斜杠、双引号、换行符、回车符、制表符。
 *
 * @param value 要转义的原始宽字符串。
 * @return JSON 安全转义后的 UTF-8 字符串。
 */
std::string JsonEscapeUtf8(const std::wstring& value)
{
    std::string input = WideToUtf8(value);
    std::string output;
    output.reserve(input.size() + 16);
    for (unsigned char ch : input)
    {
        switch (ch)
        {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output.push_back(static_cast<char>(ch));
            break;
        }
    }
    return output;
}

/**
 * @brief 从 JSON 字符串的指定位置解析一个字符串值（含转义处理）。
 *
 * 从 quote 位置的引号开始读取，处理 \\n、\\r、\\t 等转义序列，
 * 直到找到闭合引号为止。
 *
 * @param text JSON 源文本。
 * @param quote 起始引号的位置索引。
 * @param value [out] 解析出的字符串值（不含外层引号）。
 * @param end [out] 闭合引号后的下一个位置。
 * @return 成功解析到闭合引号时返回 true，否则返回 false。
 */
bool ParseJsonStringAt(const std::string& text, size_t quote, std::string& value, size_t& end)
{
    if (quote >= text.size() || text[quote] != '"')
    {
        return false;
    }

    value.clear();
    for (size_t i = quote + 1; i < text.size(); ++i)
    {
        char ch = text[i];
        if (ch == '"')
        {
            end = i + 1;
            return true;
        }

        if (ch == '\\' && i + 1 < text.size())
        {
            char escaped = text[++i];
            switch (escaped)
            {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(escaped);
                break;
            }
        }
        else
        {
            value.push_back(ch);
        }
    }

    return false;
}

// ============================================================================
// 日志写入（自动裁剪至最新 500 条）
// ============================================================================

void WriteCrashLogEntry(const wchar_t* message)
{
    const wchar_t* filename = L"SnowDesktop_crash.log";

    HANDLE f = CreateFileW(filename, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD w;
        WriteFile(f, message, static_cast<DWORD>(wcslen(message) * sizeof(wchar_t)), &w, nullptr);
        WriteFile(f, L"\r\n", 2 * sizeof(wchar_t), &w, nullptr);
        CloseHandle(f);
    }

    f = CreateFileW(filename, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size < sizeof(wchar_t)) { CloseHandle(f); return; }

    std::vector<wchar_t> content(size / sizeof(wchar_t) + 1);
    DWORD read;
    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(f, content.data(), size, &read, nullptr)) { CloseHandle(f); return; }
    DWORD numChars = read / sizeof(wchar_t);

    int totalLines = 0;
    for (DWORD i = 0; i < numChars; i++)
        if (content[i] == L'\n') totalLines++;

    if (totalLines <= 500) { CloseHandle(f); return; }

    int linesToSkip = totalLines - 500;
    DWORD startIdx = 0;
    while (startIdx < numChars && linesToSkip > 0)
    {
        if (content[startIdx] == L'\n') linesToSkip--;
        startIdx++;
    }

    DWORD tailBytes = (numChars - startIdx) * sizeof(wchar_t);
    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    WriteFile(f, content.data() + startIdx, tailBytes, &read, nullptr);
    SetEndOfFile(f);
    CloseHandle(f);
}
