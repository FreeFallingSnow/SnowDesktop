/**
 * @file navigation_settings.h
 * @brief 快捷导航面板设置
 * @details 定义快捷导航面板的数据结构（启用状态和热键组合），
 *          以及设置文件的读写与热键格式化函数。
 */

#pragma once

#include <windows.h>

#include <string>

/**
 * @brief 快捷导航设置
 * @details 存储快捷导航面板的启用状态和热键组合（修饰键+虚拟键码）
 */
struct NavigationSettings
{
    bool enabled = false;
    UINT modifiers = MOD_CONTROL | MOD_ALT;
    UINT virtualKey = VK_SPACE;
};

/**
 * @brief 获取导航设置文件路径
 * @details 返回存储快捷导航设置的 JSON 文件的完整路径
 * @return std::wstring 设置文件的绝对路径
 */
std::wstring GetNavigationSettingsPath();
/**
 * @brief 从文件加载导航设置
 * @details 读取指定路径的 JSON 文件，反序列化到 NavigationSettings 结构体
 * @param path   设置文件路径
 * @param settings 输出参数，接收加载的设置数据
 * @return true  加载成功
 * @return false 加载失败（文件不存在或格式错误）
 */
bool LoadNavigationSettings(const wchar_t* path, NavigationSettings& settings);
/**
 * @brief 保存导航设置到文件
 * @details 将 NavigationSettings 序列化为 JSON 并写入指定路径
 * @param path     设置文件路径
 * @param settings 待保存的设置数据
 * @return true  保存成功
 * @return false 保存失败
 */
bool SaveNavigationSettings(const wchar_t* path, const NavigationSettings& settings);
/**
 * @brief 格式化导航热键文本
 * @details 将 NavigationSettings 中的修饰键与虚拟键码转换为可读的字符串（如 "Ctrl+Alt+Space"）
 * @param settings 导航设置数据
 * @return std::wstring 格式化后的热键文本
 */
std::wstring FormatNavigationHotkey(const NavigationSettings& settings);
