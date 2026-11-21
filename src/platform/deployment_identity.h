#pragma once

#include "steam/steam_runtime_context.h"

#include <string>

namespace snowdesktop::deployment
{
/**
 * @brief 当前进程是否具有 MSIX 包身份。
 */
bool IsPackaged() noexcept;

/** Resolve the current explicit Steam/MSIX/portable runtime identity. */
const RuntimeDeploymentContext& GetRuntimeDeploymentContext() noexcept;

/** True when a Steam sidecar was present but failed validation. */
bool HasInvalidRuntimeDeploymentContext() noexcept;

/**
 * @brief 获取 MSIX 的 LocalState 目录。
 * @return 具有包身份时返回可写目录；便携版或失败时返回空字符串。
 */
std::wstring GetPackageLocalStatePath();
}
