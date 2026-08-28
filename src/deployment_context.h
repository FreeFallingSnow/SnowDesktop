/**
 * @file deployment_context.h
 * @brief 便携版与 MSIX 版的运行时部署环境适配。
 */

#pragma once

#include <array>
#include <cstdint>
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
 * @brief 获取可供其他进程加载的运行时 DLL 路径。
 * @details 将随软件分发的 DLL 复制到用户临时目录的进程专属目录后再返回，
 * 避免目标进程无法加载受保护的 MSIX 安装目录文件，也避免已注入模块继续锁住
 * 构建、便携或安装目录。
 * @param filename 不包含目录的 DLL 文件名。
 */
std::wstring GetInjectableRuntimeFilePath(const wchar_t* filename);

/**
 * @brief 获取可供 Explorer 加载的任务栏 Hook DLL 路径。
 * @details 兼容现有调用；内部复用通用的可注入运行时副本。
 */
std::wstring GetTaskbarHookPath();

struct UnvirtualizedRegistryBinaryValue
{
    std::uint32_t win32Result = 1;
    std::uint32_t type = 0;
    std::uint32_t size = 0;
    std::array<std::uint8_t, 32> data{};
};

/**
 * @brief 从当前用户的真实注册表视图读取二进制值。
 * @details MSIX 进程访问 HKCU 时可能看到包私有的写时复制视图。安装版通过
 * 已有 Explorer Hook DLL 的短生命周期查询入口读取 Explorer 所见的真实视图；
 * 便携版直接读取 HKCU。该函数不写入注册表，也不保留常驻桥接进程。
 */
UnvirtualizedRegistryBinaryValue QueryUnvirtualizedCurrentUserBinaryValue(
    const wchar_t* subKey, const wchar_t* valueName) noexcept;

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
