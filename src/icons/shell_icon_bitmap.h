#pragma once

#include "common/constants.h"

#include <shlobj.h>
#include <string_view>

/**
 * @brief 直接从文件资源提取图标位图
 * @details 绕过 Shell 图标画布，按资源索引提取原始 HICON，并转换为
 *          32 位带 Alpha 通道的 DIB 位图。适用于 Shell 为低分辨率图标
 *          强制添加兼容外框的经典应用程序。
 * @param resourcePath EXE、DLL 或 ICO 资源路径
 * @param iconIndex 图标资源索引
 * @param bitmapSize [out] 返回位图的实际尺寸
 * @param requestedSize 目标源位图长边，内部会归一化到 64..256 像素档位
 * @return 成功返回 DIB 位图句柄，失败返回 nullptr
 */
HBITMAP GetDirectIconResourceBitmap(std::wstring_view resourcePath,
    int iconIndex, SIZE& bitmapSize,
    int requestedSize = kIconBitmapSize);

// Local first-image hints only; nullptr requests a separately scheduled Shell fallback.
HBITMAP GetLocalIconResourceBitmap(std::wstring_view path, SIZE& bitmapSize, int requestedSize);

/**
 * @brief 获取高分辨率 Shell 图标位图
 * @details 通过 IShellItemImageFactory 或 Shell 图像列表获取桌面项
 *          的高分辨率图标，并转换为 32 位带 Alpha 通道的 DIB 位图。
 * @param pidl 桌面项的绝对 PIDL
 * @param fallbackIndex 获取低分辨率图标时的回退系统图标索引
 * @param bitmapSize [out] 返回位图的实际尺寸
 * @param allowThumbnail 是否允许 Shell 返回缩略图；为 false 时强制原生图标表示
 * @param requestedSize 目标源位图长边，内部会归一化到 64..256 像素档位
 * @param preferDirectIconExtraction 是否优先绕过 ImageFactory 直接提取 HICON
 * @param forShortcut 是否按快捷方式语义请求未叠加箭头的源图标
 * @param sourcePath Shell 项的文件系统路径，用于从快捷方式源文件直接提取图标
 * @param returnedThumbnail [out] 实际返回缩略图时写入 true，普通图标或失败时写入 false
 * @return 成功返回 DIB 位图句柄，失败返回 nullptr
 */
HBITMAP GetHighResolutionShellIconBitmap(PCIDLIST_ABSOLUTE pidl,
    int fallbackIndex, SIZE& bitmapSize, bool allowThumbnail = false,
    int requestedSize = kIconBitmapSize,
    bool preferDirectIconExtraction = false,
    bool forShortcut = false,
    std::wstring_view sourcePath = {},
    bool* returnedThumbnail = nullptr);
