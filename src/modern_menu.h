#pragma once

#include "menu_quick_icon.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace snowdesktop::modern_menu
{

// Shared by real submenus and companion previews so their dwell, grace
// period and edge overlap feel identical.
inline constexpr UINT kSubmenuOpenDelayMs = 480;
inline constexpr UINT kSubmenuCloseDelayMs = 420;
inline constexpr int kSubmenuOverlapDip = 3;
inline constexpr int kSubmenuPanelPaddingDip = 5;

enum class RootPlacement
{
    Default,
    AboveAnchorRect,
    BelowAnchorRect,
    LeftOfAnchorRect,
    RightOfAnchorRect,
};

/** 右键菜单的独立模糊外观，不跟随组件主题。 */
enum class Appearance
{
    /** 根据 Windows 应用/菜单主题自动选择浅色或深色模糊。 */
    FollowSystem = 0,
    SystemLightBlur = 1,
    SystemDarkBlur = 2,
};

enum class IconFont
{
    FluentRegular,
    FontAwesomeSolid,
};

struct Item
{
    UINT command = 0;
    std::wstring label;
    std::wstring glyph;
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    std::vector<Item> children;
    IconFont iconFont = IconFont::FluentRegular;
    /** 根菜单中以 Windows 11 风格的顶部快捷按钮显示。 */
    bool quickAction = false;
    MenuQuickIcon quickIcon = MenuQuickIcon::FontGlyph;
    /** 与相邻的同类菜单项共用一行，用于分页等紧凑操作组。 */
    bool inlineAction = false;
    /** 非零值将连续的行内操作拆分为独立行组。 */
    UINT inlineGroup = 0;
    /** 行内操作使用适合短文本的固定宽度，其余空间留给主操作。 */
    bool compactInlineAction = false;
};

struct HoverInfo
{
    UINT command = 0;
    RECT itemScreenRect{};
    RECT popupScreenRect{};
    int depth = 0;
    bool keyboard = false;
};

struct Options
{
    HWND owner = nullptr;
    POINT anchor{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool lightTheme = true;
    /** 托盘菜单使用；确保根菜单和子菜单位于任务栏之上。 */
    bool topmost = false;
    Appearance appearance = Appearance::FollowSystem;
    RootPlacement rootPlacement = RootPlacement::Default;
    RECT anchorRect{};
    /** 返回 true 时应用命令并保持根菜单打开。 */
    std::function<bool(UINT, std::vector<Item>&)> onCommand;
    /** 鼠标或键盘高亮项变化；command=0 表示当前没有可预览项。 */
    std::function<void(const HoverInfo&)> onHover;
};

struct Result
{
    UINT command = 0;
    RECT itemScreenRect{};
};

/**
 * @brief 使用完全自绘的分层弹窗显示现代菜单。
 *
 * 不创建或显示 HMENU 窗口；定位、绘制、命中测试、滚动、键盘导航和
 * 级联子菜单均由该组件管理。调用会像 TrackPopupMenuEx 一样同步返回。
 */
Result Show(const std::vector<Item>& items, const Options& options);

/** Close the currently active menu without activating a command. */
void DismissActive();

} // namespace snowdesktop::modern_menu
