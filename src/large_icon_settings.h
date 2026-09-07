#pragma once
#include "large_icon_config.h"
#include <functional>

namespace snowdesktop
{
struct LargeIconSettingsRequest
{
    std::wstring key;
    std::uint64_t session = 0, revision = 0;
    std::string action; // read, preview, commit, cancel, import, refresh
    std::string config;
    std::wstring path;
};

struct LargeIconSettingsSnapshot
{
    std::wstring key, name, imagePath;
    std::wstring landscapePath, portraitPath;
    std::string source, landscapeSource, portraitSource;
    std::uint32_t accent = 0x505866;
    bool steam = false;
    std::uint64_t session = 0, revision = 0;
    bool available = false, editable = false, succeeded = false;
    int maxColumns = 1, maxRows = 1;
    std::string config, error;
};

using LargeIconSettingsAction = std::function<LargeIconSettingsSnapshot(LargeIconSettingsRequest)>;
struct LargeIconEditSession
{
    std::wstring key;
    std::uint64_t token = 0, revision = 1;
    std::optional<LargeIconConfig> preview;
};
}
