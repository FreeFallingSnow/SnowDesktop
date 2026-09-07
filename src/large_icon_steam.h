#pragma once
#include "json_value.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::large_icon_steam
{
inline std::optional<std::uint32_t> AppId(std::wstring_view url)
{
    std::wstring lower(url);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) -> wchar_t { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; });
    std::wstring_view value = lower;
    if (value.starts_with(L"steam://rungameid/")) value.remove_prefix(18);
    else if (value.starts_with(L"steam://run/")) value.remove_prefix(12);
    else return {};
    const auto end = value.find_first_of(L"/?# ");
    value = value.substr(0, end);
    if (value.empty() || value.size() > 10) return {};
    std::uint64_t id = 0;
    for (const auto c : value)
    {
        if (c < L'0' || c > L'9') return {};
        id = id * 10 + c - L'0';
    }
    if (id == 0 || id > UINT32_MAX) return {};
    return static_cast<std::uint32_t>(id);
}

inline std::string Language(std::string_view locale)
{
    if (locale == "zh-CN") return "schinese";
    if (locale == "zh-TW") return "tchinese";
    if (locale == "ja-JP") return "japanese";
    if (locale == "ko-KR") return "koreana";
    if (locale == "de-DE") return "german";
    if (locale == "fr-FR") return "french";
    if (locale == "pt-BR") return "brazilian";
    if (locale == "es-419") return "latam";
    if (locale == "es-ES") return "spanish";
    return "english";
}

inline std::string UrlEncode(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

inline std::string MetadataUrl(std::uint32_t id, std::string_view language)
{
    return "https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?input_json=" +
        UrlEncode("{\"ids\":[{\"appid\":" + std::to_string(id) + "}],\"context\":{\"country_code\":\"US\",\"language\":\"" +
            std::string(language) + "\"},\"data_request\":{\"include_assets\":true}}");
}

inline bool SafeAssetPath(std::string_view path)
{
    return !path.empty() && path.size() <= 1024 && path.find("..") == path.npos &&
        std::all_of(path.begin(), path.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '/' || c == '_' || c == '-' || c == '.' || c == '?' || c == '=' || c == '&' || c == '%';
        }) && path.front() != '/';
}

// Resolve filenames relative to Valve's format, including newer hash directories.
// The metadata never gets to choose a host or a local destination path.
inline std::string AssetUrl(const JsonValue& response, std::uint32_t id, bool portrait)
{
    const auto* envelope = response.Find("response");
    const auto* items = envelope ? envelope->Find("store_items") : nullptr;
    if (!items || !items->IsArray()) return {};
    for (const auto& item : items->array)
    {
        const auto* app = item.Find("appid");
        const auto* assets = item.Find("assets");
        if (!app || !app->IsNumber() || app->number != id || !assets) continue;
        const auto* format = assets->Find("asset_url_format");
        if (!format || !format->IsString()) continue;
        const std::string prefix = "steam/apps/" + std::to_string(id) + "/";
        if (!format->string.starts_with(prefix)) continue;
        for (const char* key : portrait ? std::initializer_list<const char*>{"library_capsule_2x", "library_capsule"}
                                       : std::initializer_list<const char*>{"library_header_2x", "library_header", "header_2x", "header", "main_capsule"})
        {
            const auto* name = assets->Find(key);
            if (!name || !name->IsString() || !SafeAssetPath(name->string)) continue;
            std::string path = format->string;
            const auto marker = path.find("${FILENAME}");
            if (marker == path.npos) continue;
            path.replace(marker, 11, name->string);
            if (SafeAssetPath(path)) return "https://shared.akamai.steamstatic.com/store_item_assets/" + path;
        }
    }
    return {};
}
}
