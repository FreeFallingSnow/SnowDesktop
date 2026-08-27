/**
 * @file deployment_context.h
 * @brief 便携版与 MSIX 版的运行时部署环境适配。
 */

#pragma once

#include <string>

namespace snowdesktop::deployment
{
/**
 * @brief 当前进程是否具有 MSIX 包身份。
 */
bool IsPackaged() noexcept;

/**
 * @brief 获取 MSIX 的 LocalState 目录。
 * @return 具有包身份时返回可写目录；便携版或失败时返回空字符串。
 */
std::wstring GetPackageLocalStatePath();

/**
 * @brief 获取随软件分发的运行文件路径。
 * @details 优先使用 exe 同目录的 SnowDesktop.Runtime 子目录；开发构建仍兼容
 * 旧的 exe 同目录布局。
 * @param filename 不包含目录的文件名。
 */
std::wstring GetRuntimeFilePath(const wchar_t* filename);

/**
 * @brief 获取可供 Explorer 加载的任务栏 Hook DLL 路径。
 * @details 始终把随软件分发的 DLL 复制到用户临时目录的进程专属目录后再返回，
 * 避免 XAML Diagnostics TAP 在软件退出后继续锁住构建、便携或安装目录。
 */
std::wstring GetTaskbarHookPath();

/**
 * @brief 获取当前产品的 Microsoft Store 详情页 URI。
 */
std::wstring GetStoreProductPageUri();

/**
 * @brief MSIX StartupTask 的完整状态。
 */
enum class PackagedAutoStartState
{
    Unavailable,
    Disabled,
    DisabledByUser,
    Enabled,
    DisabledByPolicy,
    EnabledByPolicy,
};

/**
 * @brief 查询 MSIX StartupTask 状态。
 * @return 当前任务状态；便携版或查询失败返回 Unavailable。
 */
PackagedAutoStartState GetPackagedAutoStartState() noexcept;

/**
 * @brief 查询本机当前用户安装的 Microsoft Store 版 StartupTask 状态。
 * @details 供无包身份的携带版检测同一产品安装版是否已负责开机自启。
 * @return 安装版未注册、状态不存在或读取失败时返回 Unavailable。
 */
PackagedAutoStartState GetInstalledPackagedAutoStartState() noexcept;

/**
 * @brief 启用或禁用 MSIX StartupTask。
 * @param enable 期望状态。
 * @return 操作后的实际状态；便携版或操作失败返回 Unavailable。
 */
PackagedAutoStartState SetPackagedAutoStartEnabled(bool enable) noexcept;

/**
 * @brief 判断 StartupTask 状态是否表示已启用。
 */
bool IsPackagedAutoStartStateEnabled(
    PackagedAutoStartState state) noexcept;
}
