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
 * @brief 获取可供 Explorer 加载的任务栏 Hook DLL 路径。
 * @details 便携版直接返回 exe 同目录 DLL；MSIX 版先将 DLL 部署到
 * LocalState\TempState\<version>，避免 Explorer 直接读取 WindowsApps。
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
