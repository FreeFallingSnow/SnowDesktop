#pragma once
#include "icon_beautify.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace snowdesktop
{
// No record follows both theme and processing. A record may follow only the theme; explicit None remains a record.
struct IconAppearanceRecord
{
    std::wstring target, name;
    IconBeautifySettings settings;
    std::string themeId;
    bool available = true;
    bool followGlobalTheme = false;
};
struct IconAppearanceChoice
{
    std::string id;
    std::wstring name;
    IconBeautifySettings settings;
};
struct IconAppearanceRequest
{
    std::string action = "read"; // read, begin, apply, inherit, cancel, themes
    std::wstring target;
    std::uint64_t session = 0, revision = 0;
    IconBeautifySettings settings;
    std::string themeId;
    bool available = true;
    bool followGlobalTheme = false;
};
struct IconAppearanceSnapshot
{
    std::vector<IconAppearanceRecord> entries;
    std::vector<IconAppearanceChoice> themes;
    IconAppearanceRecord current;
    IconBeautifySettings globalSettings;
    std::uint64_t session = 0, revision = 0;
    bool succeeded = false, available = false, independent = false;
    bool nativeControlSource = false;
    std::string error, previewPng;
};
using IconAppearanceAction = std::function<IconAppearanceSnapshot(IconAppearanceRequest)>;
}
