/**
 * @file utils.h
 * @brief 工具函数集合
 * @details 提供桌面图标管理、窗口查找、字符串处理、位图操作、矩形计算、JSON 解析等
 *          通用工具函数的声明。所有函数均为独立函数，不依赖于全局状态。
 */

#pragma once
#include "types.h"

#include <wrl/client.h>
#include <shlobj.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <cstdint>
#include <string>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

/**
 * @brief 查找 DefView 窗口的回调函数
 * @details 配合 EnumChildWindows 使用，枚举子窗口以定位 WorkerW 或 Progman
 *          下的 SHELLDLL_DefView 窗口。将匹配到的窗口句柄写入 lParam 指向的 DefViewSearch 结构。
 * @param hwnd 当前正在枚举的子窗口句柄
 * @param lParam 指向 DefViewSearch 结构的指针，用于返回找到的 DefView 句柄
 * @return FALSE 继续枚举，TRUE 停止枚举
 */
BOOL CALLBACK FindDefViewProc(HWND hwnd, LPARAM lParam);

/**
 * @brief 枚举显示器以初始化网格页面的回调函数
 * @details 配合 EnumDisplayMonitors 使用，遍历所有显示器并将每台显示器的信息
 *          转换为 GridPage 结构并存入 lParam 指向的向量中。
 * @param monitor 当前枚举到的显示器句柄
 * @param hdc 显示器 DC（未使用）
 * @param lprcMonitor 显示器在虚拟桌面上的边界矩形
 * @param lParam 指向 MonitorEnumContext 结构的指针，用于收集 GridPage 列表
 * @return TRUE 继续枚举，FALSE 停止枚举
 */
BOOL CALLBACK EnumGridPageMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam);

/**
 * @brief 加载应用程序图标
 * @details 从应用程序资源中加载默认图标，通常用于窗口和任务栏表示。
 * @return 成功返回图标句柄，失败返回 nullptr
 */
HICON LoadAppIcon();

/**
 * @brief 加载 Font Awesome 字体资源
 * @details 从嵌入资源或系统字体中加载 Font Awesome 图标字体，用于在 UI 中
 *          呈现矢量图标。
 * @return 成功返回字体资源句柄，失败返回 nullptr
 */
HANDLE LoadFontAwesome();

/**
 * @brief 创建 Font Awesome 文本格式对象
 * @details 创建 DirectWrite 文本格式对象，配置 Font Awesome 字体族和字号，
 *          用于在 Direct2D 中渲染图标字符。
 * @param factory DirectWrite 工厂接口指针
 * @param fontSize 字号大小，默认 14.0f
 * @return 成功返回 IDWriteTextFormat 指针，失败返回 nullptr
 */
IDWriteTextFormat* CreateFaTextFormat(IDWriteFactory* factory, float fontSize = 14.0f);

/**
 * @brief 查找桌面相关窗口
 * @details 定位 Progman、WorkerW、DefView、ListView 以及桌面宿主窗口的句柄，
 *          同时记录 ListView 的初始可见状态。是桌面集成功能的核心入口。
 * @return DesktopWindows 结构，包含所有找到的窗口句柄
 */
DesktopWindows FindDesktopWindows();

/**
 * @brief 恢复桌面图标层
 * @details 立即恢复被隐藏的桌面图标层（DefView 中的 ListView），
 *          确保原生桌面图标在组件模式下能正确显示。
 */
void RestoreExplorerIconLayerNow();

/**
 * @brief 将 STRRET 结构转换为宽字符串
 * @details 处理 STRRET 在不同存储方式（STRRET_WSTR、STRRET_CSTR、STRRET_OFFSET）
 *          下的字符串提取，统一返回 std::wstring。
 * @param folder IShellFolder 接口指针，用于 STRRET_OFFSET 模式的解析
 * @param child 子项的 PIDL
 * @param flags 显示名称的获取标志（SHGDNF 枚举组合）
 * @return 提取到的宽字符串，失败时返回空字符串
 */
std::wstring StrRetToString(IShellFolder* folder, PCUITEMID_CHILD child, SHGDNF flags);

/**
 * @brief 将字符串转换为大写（不随区域改变）
 * @details 使用 LCMAP_UPPPERCASE 进行不随区域变化的大写转换，
 *          用于不区分大小写的比较操作。
 * @param value 待转换的源字符串
 * @return 转换后的大写字符串
 */
std::wstring ToUpperInvariant(std::wstring value);

/**
 * @brief 从解析名中提取 CLSID 文本
 * @details 解析形如 "::{CLSID}" 格式的桌面项解析名，提取其中的 CLSID 字符串部分。
 * @param parsingName 桌面项的解析名称
 * @return 提取到的 CLSID 字符串（含两侧花括号），不包含 CLSID 时返回空字符串
 */
std::wstring ExtractClsidText(const std::wstring& parsingName);

/**
 * @brief 去除路径末尾的路径分隔符
 * @details 移除路径字符串末尾的 '\\' 和 '/' 字符，保留根路径（如 "C:\\"）不受影响。
 * @param path 待处理的路径字符串
 * @return 处理后的路径字符串
 */
std::wstring TrimTrailingPathSeparators(std::wstring path);

/**
 * @brief 不区分大小写比较两个路径是否相等
 * @details 在比较前统一规范化路径字符串，去除尾部分隔符后再进行不区分大小写的比较。
 * @param left 左侧路径
 * @param right 右侧路径
 * @return 两个路径相同时返回 true，否则返回 false
 */
bool PathsEqualInsensitive(std::wstring left, std::wstring right);

/**
 * @brief 解析桌面图标的 CLSID
 * @details 根据桌面项的解析名、完整路径和用户配置文件路径，确定该图标对应的
 *          注册表 CLSID。用于后续读取或修改该图标的注册表设置。
 * @param parsingName 桌面项的解析名称
 * @param itemPath 桌面项的完整文件系统路径
 * @param userProfilePath 当前用户的配置文件路径
 * @return 解析到的 CLSID 字符串，失败时返回空字符串
 */
std::wstring ResolveDesktopIconClsid(
    const std::wstring& parsingName,
    const std::wstring& itemPath,
    const std::wstring& userProfilePath);

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
 *          会同时更新 HKCU 和 HKLM 下的对应注册表项。
 * @param clsid 图标的 CLSID 字符串
 * @param visible true 表示显示该图标，false 表示隐藏该图标
 */
void WriteDesktopIconRegistryValue(const std::wstring& clsid, bool visible);

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
bool IsVisibleByDesktopIconSettings(const std::wstring& desktopIconClsid, const std::unordered_map<std::wstring, bool>& settingsIconVisibility);

/**
 * @brief 创建自顶向下的 32 位 DIB 位图
 * @details 创建 32 位深、自顶向下（非倒置）的 DIB（设备无关位图），
 *          适合用于 Alpha 混合处理。位图像素数据可通过 bits 指针直接访问。
 * @param referenceDc 参考 DC，用于创建兼容的 DIB
 * @param width 位图宽度（像素）
 * @param height 位图高度（像素）
 * @param bits [out] 返回位图像素数据起始地址
 * @return 成功返回 DIB 位图句柄，失败返回 nullptr
 */
HBITMAP CreateTopDown32BppDib(HDC referenceDc, int width, int height, void** bits);

/**
 * @brief 预乘 BGRA 像素的 Alpha 通道
 * @details 将 BGRA 格式的像素数据进行 Alpha 预乘处理，
 *          即将各颜色通道值乘以 Alpha 值并除以 255，满足 Direct2D 等 API 的输入要求。
 * @param pixels BGRA 像素数据数组
 * @param width 图像宽度（像素）
 * @param height 图像高度（像素）
 */
void PremultiplyBgraPixels(std::uint32_t* pixels, int width, int height);

/**
 * @brief 将位图复制到支持 Alpha 通道的 DIB 中
 * @details 从源位图创建包含 Alpha 通道的 DIB 副本，确保位图具备 Alpha 混合能力。
 * @param source 源位图句柄
 * @param size [out] 返回位图的实际尺寸
 * @return 成功返回新的 DIB 位图句柄，失败返回 nullptr
 */
HBITMAP CopyBitmapToAlphaDib(HBITMAP source, SIZE& size);

/**
 * @brief 从图标创建带 Alpha 通道的位图
 * @details 从指定的 HICON 图标中提取图像数据，创建指定尺寸的 32 位带 Alpha 通道的 DIB 位图。
 * @param icon 源图标句柄
 * @param width 目标位图宽度（像素）
 * @param height 目标位图高度（像素）
 * @param size [out] 返回位图的实际尺寸
 * @return 成功返回 DIB 位图句柄，失败返回 nullptr
 */
HBITMAP CreateAlphaBitmapFromIcon(HICON icon, int width, int height, SIZE& size);

/**
 * @brief 获取高分辨率 Shell 图标位图
 * @details 通过 IShellIconImageSize 或 Shell_GetImageLists 接口获取桌面项
 *          的高分辨率图标，并转换为 32 位带 Alpha 通道的 DIB 位图。
 * @param pidl 桌面项的绝对 PIDL
 * @param fallbackIndex 获取低分辨率图标时的回退系统图标索引
 * @param bitmapSize [out] 返回位图的实际尺寸
 * @return 成功返回 DIB 位图句柄，失败返回 nullptr
 */
HBITMAP GetHighResolutionShellIconBitmap(PCIDLIST_ABSOLUTE pidl, int fallbackIndex, SIZE& bitmapSize, bool fullQuality = false);

/**
 * @brief 从坐标值创建 RECT 结构
 * @details 构造一个 RECT，left/top 为左上角坐标，right/bottom 为右下角坐标。
 * @param left 左侧坐标
 * @param top 顶部坐标
 * @param right 右侧坐标
 * @param bottom 底部坐标
 * @return 构造完成的 RECT 结构
 */
RECT MakeRect(int left, int top, int right, int bottom);

/**
 * @brief 判断两个矩形是否相交
 * @details 使用 IntersectRect 检测两个 RECT 是否存在重叠区域。
 * @param a 第一个矩形
 * @param b 第二个矩形
 * @return 相交返回 true，不相交返回 false
 */
bool RectsIntersect(const RECT& a, const RECT& b);

/**
 * @brief 从两个点创建规范化矩形
 * @details 根据两个对角点创建 RECT，自动调整 left/right 和 top/bottom，
 *          保证左上角坐标不大于右下角坐标。
 * @param a 第一个点
 * @param b 第二个点
 * @return 规范化后的 RECT 结构
 */
RECT NormalizeRect(POINT a, POINT b);

/**
 * @brief 判断矩形是否为空
 * @details 检查矩形的宽度和高度是否为 0 或负数。
 * @param rect 待检查的矩形
 * @return 矩形为空返回 true，非空返回 false
 */
bool IsRectEmptyRect(const RECT& rect);

/**
 * @brief 创建矩形的膨胀副本
 * @details 在矩形四边各增加指定的像素数，生成一个新的矩形。
 *          正值向外扩张，负值向内收缩。
 * @param rect 源矩形
 * @param amount 各边膨胀的像素数
 * @return 膨胀后的新矩形
 */
RECT InflateCopy(RECT rect, int amount);

/**
 * @brief 计算两个矩形的并集
 * @details 返回能同时包含两个矩形的最小矩形。
 * @param a 第一个矩形
 * @param b 第二个矩形
 * @return 两个矩形的并集矩形
 */
RECT UnionCopy(const RECT& a, const RECT& b);

/**
 * @brief 将宽字符串转换为 UTF-8 编码字符串
 * @details 使用 WideCharToMultiByte 将 UTF-16 宽字符串转换为 UTF-8 多字节字符串。
 * @param value 待转换的宽字符串
 * @return UTF-8 编码的字符串
 */
std::string WideToUtf8(const std::wstring& value);

/**
 * @brief 将 UTF-8 编码字符串转换为宽字符串
 * @details 使用 MultiByteToWideChar 将 UTF-8 多字节字符串转换为 UTF-16 宽字符串。
 * @param value 待转换的 UTF-8 字符串
 * @return UTF-16 编码的宽字符串
 */
std::wstring Utf8ToWide(const std::string& value);

/**
 * @brief 对宽字符串进行 JSON 转义并输出为 UTF-8
 * @details 将宽字符串中的特殊字符（引号、反斜杠、控制字符等）进行 JSON 转义处理，
 *          同时将结果编码为 UTF-8 字符串。
 * @param value 待转义的宽字符串
 * @return JSON 转义后的 UTF-8 字符串
 */
std::string JsonEscapeUtf8(const std::wstring& value);

/**
 * @brief 在指定位置解析 JSON 字符串值
 * @details 从文本中指定引号位置开始，解析完整的 JSON 字符串值（支持转义字符），
 *          并将解析进度更新到 end 参数中。
 * @param text 包含 JSON 字符串的完整文本
 * @param quote 起始引号在 text 中的位置索引
 * @param value [out] 解析到的字符串值
 * @param end [out] 解析结束位置（字符串 closing quote 之后的下一个索引）
 * @return 成功解析返回 true，格式错误返回 false
 */
bool ParseJsonStringAt(const std::string& text, size_t quote, std::string& value, size_t& end);
