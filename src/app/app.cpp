/**
 * @file app.cpp
 * @brief SnowDesktop 主应用程序模块。
 *
 * 本文件是桌面应用程序的主编译单元，包含 DesktopApp 类的实例化入口。
 * DesktopApp 封装了桌面窗口管理、Direct2D/Direct3D 图形渲染管线、
 * OLE 拖放协议（IDropTarget / IDropSource）、托盘图标、右键菜单、
 * 快速导航面板、集合弹窗、Lua 脚本引擎集成等核心功能。
 *
 * 该类的大多数成员函数实现在以下子头文件中，通过 #include 引入：
 * - app_run.h    : 主循环、窗口过程、初始化与清理
 * - app_gfx.h    : 图形渲染（D2D/D3D/DComp）、绘画与缓存
 * - app_interact.h : 鼠标/键盘交互、拖放、选择与上下文菜单
 * - app_menu.h   : 托盘菜单与右键菜单构建
 * - app_grid.h   : 网格布局、分页、自动排列与对齐
 *
 * @note 全局 SettingsWindow 指针由 DesktopApp 通过 std::unique_ptr<SettingsWindow>
 *       成员变量 settingsWindow_ 管理，在 ShowSettingsWindow() 中创建。
 *       应用级静态数据（如 D2D 工厂、DWrite 工厂等）随 DesktopApp 实例
 *       的生命周期初始化与销毁。
 *
 * @see app.h  DesktopApp 类的完整声明
 */

#include "app.h"
