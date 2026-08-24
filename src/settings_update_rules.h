#pragma once

#include "json_value.h"

#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace snowdesktop::settings_update_rules
{

struct ReleaseStatus
{
    bool parsed = false;
    bool updateAvailable = false;
    std::string version;
    std::string downloadUrl;
};

inline bool ParseVersion(
    std::string_view text, std::array<unsigned, 4>& version) noexcept
{
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
        text.remove_prefix(1);
    for (std::size_t part = 0; part < version.size(); ++part)
    {
        const std::size_t separator = text.find('.');
        const std::string_view token = separator == std::string_view::npos
            ? text : text.substr(0, separator);
        if (token.empty()) return false;
        unsigned value = 0;
        const auto parsed = std::from_chars(
            token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size())
        {
            return false;
        }
        if ((part == 0 && (value == 0 || value > 65535)) ||
            ((part == 1 || part == 2) && value > 65535) ||
            (part == 3 && value != 0))
        {
            return false;
        }
        version[part] = value;
        if (part + 1 == version.size())
            return separator == std::string_view::npos;
        if (separator == std::string_view::npos) return false;
        text.remove_prefix(separator + 1);
    }
    return false;
}

inline ReleaseStatus ParseGitHubRelease(
    std::string_view body, std::string_view currentVersion)
{
    JsonValue document;
    if (!ParseJson(body, document) || !document.IsObject()) return {};
    const JsonValue* tag = document.Find("tag_name");
    const JsonValue* url = document.Find("html_url");
    if (!tag || !tag->IsString() || !url || !url->IsString()) return {};

    constexpr std::string_view kReleasePrefix =
        "https://github.com/FreeFallingSnow/SnowDesktop_Release/";
    if (!std::string_view(url->string).starts_with(kReleasePrefix)) return {};

    std::array<unsigned, 4> current{};
    std::array<unsigned, 4> available{};
    if (!ParseVersion(currentVersion, current) ||
        !ParseVersion(tag->string, available))
    {
        return {};
    }

    ReleaseStatus result;
    result.parsed = true;
    result.updateAvailable = available > current;
    result.version = tag->string;
    if (!result.version.empty() &&
        (result.version.front() == 'v' || result.version.front() == 'V'))
    {
        result.version.erase(result.version.begin());
    }
    result.downloadUrl = url->string;
    return result;
}

} // namespace snowdesktop::settings_update_rules
