#pragma once

#include "widget_logical_slot.h"
#include "widget/view/widget_interaction_types.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Host-facing requests are values, independent of the Lua runtime, task
// executors and per-widget VM ownership held by WidgetEngine.
/**
 * @struct LuaDesktopItemInfo
 * @brief 桌面项目基本信息，由宿主提供给 Lua 沙箱的快照条目
 *
 * 当 Lua 脚本调用桌面查询 API 时，宿主通过回调返回该项目列表，
 * 用于支持小部件内的文件浏览、快速启动等功能。
 */
struct LuaDesktopItemInfo
{
    std::string id;         ///< 项目唯一标识符
    std::string title;      ///< 项目显示标题
    std::string path;       ///< 项目完整路径
    std::string source;     ///< 来源标识（如 "desktop", "startmenu", "custom"）
    std::string type;       ///< 项目类型（如 "file", "folder", "shortcut"）
    bool selected = false;  ///< 当前是否处于选中状态
};

/**
 * @struct LuaInlineTextEditRequest
 * @brief 内联文本编辑请求，由 Lua 脚本发起，宿主编排文本输入
 *
 * 当 Lua 小部件需要就地编辑文本时（如重命名文件），引擎通过回调将此结构
 * 传递给宿主，由宿主显示内联输入框并将结果写回存储。
 */
struct LuaInlineTextEditRequest
{
    std::wstring widgetId;   ///< 发起请求的小部件 ID
    std::string storageKey;  ///< 存储键名，宿主完成编辑后通过该键写回
    std::string text;        ///< 初始文本内容
    RECT localRect{};        ///< 输入框在小部件本地坐标系中的位置和尺寸
    bool multiline = false;  ///< 是否支持多行输入
    bool selectAll = true;   ///< 是否自动全选文本
    bool liveUpdate = false; ///< 输入过程中是否实时写回存储
    int textColor = 0x000000; ///< 文本颜色（ARGB 格式，默认黑色）
    int backgroundColor = 0xFFFFFF; ///< 编辑框背景颜色（RGB，默认白色）
    float fontSize = 15.0f;  ///< 编辑框字号（像素，默认 15）
};

struct LuaWidgetPanelRequest
{
    std::wstring widgetId;
    std::wstring title;
    std::string surface = "panel";
    std::string placement = "auto";
    std::string anchorKey;
    RECT anchorRect{};
    bool hasAnchor = false;
    bool showHeader = true;
    int width = 520;
    int height = 620;
    bool dismissOnOutside = true;
    bool dismissOnEscape = true;
    bool modal = false;
    bool desktopGesture = false;
};

struct LogicalSlotPickerRequest
{
    std::wstring widgetId;
    std::uint64_t ownerToken = 0;
    std::string slotId;
    snowdesktop::widget_runtime::LogicalSlotKind kind =
        snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    std::vector<std::string> accepts;
    std::string referenceType;
    std::size_t targetIndex = 0;
};

namespace snowdesktop::widget_runtime
{
struct WidgetFileDropTarget
{
    std::wstring widgetId;
    std::string packageId;
    std::string targetKey;
    std::uint64_t runtimeToken = 0;
    snowdesktop::widget_runtime::InteractionAction action;
};
}
