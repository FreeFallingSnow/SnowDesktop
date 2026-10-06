#pragma once
#include "large_icon_config.h"
#include <functional>
#include <map>

namespace snowdesktop
{
struct LargeIconSettingsRequest
{
    std::wstring key;
    std::uint64_t session = 0, revision = 0;
    std::string action; // read, preview, commit, cancel, import, refresh
    std::string config;
    std::wstring path;
    std::vector<std::string> fields; // Internal editor patch: explicitly changed settings.
};

struct LargeIconSettingsSnapshot
{
    std::wstring key, name, imagePath;
    std::wstring landscapePath, portraitPath;
    std::string source, landscapeSource, portraitSource;
    std::uint32_t accent = 0;
    std::uint32_t edgeColor = 0;
    bool hasEdgeColor = false;
    bool steam = false;
    std::uint64_t session = 0, revision = 0;
    bool available = false, editable = false, succeeded = false;
    int maxColumns = 1, maxRows = 1;
    int frameWidth = 192, frameHeight = 192, frameColumns = 2, frameRows = 2, frameLimit = 60;
    double unitScale = 1, durationScale = 1;
    std::vector<int> frameWidths, frameHeights;
    bool animations = true;
    std::uint32_t neutral = 0x414751;
    std::string config, error, defaultConfig;
    int imageWidth = 0, imageHeight = 0;
    bool loading = false;
    int itemCount = 1;
    bool anyFill = false;
    std::vector<std::string> mixedFields;
};

using LargeIconSettingsAction = std::function<LargeIconSettingsSnapshot(LargeIconSettingsRequest)>;
struct LargeIconEditSession
{
    std::wstring key;
    std::uint64_t token = 0, revision = 1;
    std::optional<LargeIconConfig> preview;
    std::vector<std::wstring> keys;
    std::map<std::wstring, LargeIconConfig> previews;
};
}
