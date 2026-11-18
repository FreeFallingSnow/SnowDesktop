#pragma once

#include "large_icon_config_types.h"

#include <string_view>

namespace snowdesktop
{
bool IsManagedLargeIconImage(std::string_view name);
bool ValidateLargeIconConfig(const LargeIconConfig& c);

inline bool IsLargeIconFill(const LargeIconConfig& config) { return config.backgroundStyle == -2; }
inline int LargeIconActiveContent(const LargeIconConfig& config)
{ return IsLargeIconFill(config) ? config.content : config.foregroundContent; }
inline const std::string& LargeIconActiveImage(const LargeIconConfig& config)
{
    if (IsLargeIconFill(config)) return config.imageAssetId.empty() ? config.image : config.imageAssetId;
    return config.foregroundImageAssetId.empty() ? config.foregroundImage : config.foregroundImageAssetId;
}
}
