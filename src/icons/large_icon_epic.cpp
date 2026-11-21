#include "large_icon_epic.h"

#include "common/json_value.h"
#include "shell/shortcut_application_rules.h"

#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

namespace snowdesktop::large_icon_epic
{
namespace
{
constexpr std::size_t MaxCatalogSize = 24 * 1024 * 1024;

template<class Char>
Char LowerAscii(Char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<Char>(c + ('a' - 'A')) : c;
}

bool Matches(std::string_view actual, std::string_view lower)
{
    return std::ranges::equal(actual, lower, [](char left, char right) { return LowerAscii(left) == right; });
}

std::filesystem::path ProgramData()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &path))) return {};
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::string ReadText(const std::filesystem::path& path, std::size_t limit)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(limit)) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) return {};
    return text;
}

std::string_view Field(const JsonValue& object, const char* name)
{
    const auto* value = object.Find(name);
    return value && value->IsString() ? std::string_view(value->string) : std::string_view{};
}

std::string LegacyCatalogKey(const std::filesystem::path& manifests, std::string_view appName, std::stop_token stop)
{
    std::error_code ec;
    for (auto entry = std::filesystem::directory_iterator(manifests, ec);
        !ec && entry != std::filesystem::directory_iterator{} && !stop.stop_requested(); entry.increment(ec))
    {
        if (entry->path().extension() != L".item") continue;
        auto text = ReadText(entry->path(), 512 * 1024);
        std::string_view input(text);
        if (input.starts_with("\xef\xbb\xbf")) input.remove_prefix(3);
        JsonValue manifest;
        if (!ParseJson(input, manifest) || !Matches(Field(manifest, "AppName"), appName)) continue;
        const auto ns = Field(manifest, "CatalogNamespace"), id = Field(manifest, "CatalogItemId");
        if (ns.empty() || id.empty()) continue;
        std::string key = std::string(ns) + '.' + std::string(id);
        std::ranges::transform(key, key.begin(), LowerAscii<char>);
        if (shortcut_application_rules::ValidEpicGameKey(key)) return key;
    }
    return {};
}

bool HasArtifact(const JsonValue& item, std::string_view appName)
{
    const auto* releases = item.Find("releaseInfo");
    return releases && releases->IsArray() && std::ranges::any_of(releases->array,
        [&](const JsonValue& release) { return Matches(Field(release, "appId"), appName); });
}
}

bool IsCoverUrl(std::string_view url)
{
    constexpr std::array hosts{
        "https://cdn1.epicgames.com/", "https://cdn2.epicgames.com/",
        "https://cdn.epicgames.com/"};
    return url.size() <= 2048 && url.find_first_of("\\#") == url.npos &&
        std::ranges::none_of(url, [](unsigned char c) { return c <= 0x20 || c >= 0x7f; }) &&
        std::ranges::any_of(hosts, [&](std::string_view prefix) { return url.starts_with(prefix) && url.size() > prefix.size(); });
}

std::string CoverUrl(std::string_view gameKey, bool portrait, std::stop_token stop)
{
    if (!shortcut_application_rules::ValidEpicGameKey(gameKey) || stop.stop_requested()) return {};
    const auto programData = ProgramData();
    if (programData.empty()) return {};
    const auto data = programData / L"Epic" / L"EpicGamesLauncher" / L"Data";
    auto encoded = ReadText(data / L"Catalog" / L"catcache.bin", MaxCatalogSize);
    if (encoded.empty() || stop.stop_requested()) return {};
    const auto length = static_cast<DWORD>(encoded.size());
    DWORD decodedSize = 0;
    if (!CryptStringToBinaryA(encoded.data(), length, CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
            nullptr, &decodedSize, nullptr, nullptr) || !decodedSize) return {};
    std::string decoded(decodedSize, '\0');
    if (!CryptStringToBinaryA(encoded.data(), length, CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
            reinterpret_cast<BYTE*>(decoded.data()), &decodedSize, nullptr, nullptr)) return {};
    JsonValue catalog;
    if (!ParseJson(std::string_view(decoded).substr(0, decodedSize), catalog) || !catalog.IsArray()) return {};

    const auto installedKey = gameKey.find('.') == gameKey.npos ? LegacyCatalogKey(data / L"Manifests", gameKey, stop) : std::string{};
    const auto catalogKey = installedKey.empty() ? gameKey : std::string_view(installedKey);
    const auto dot = catalogKey.find('.');
    for (const auto& item : catalog.array)
    {
        if (stop.stop_requested()) return {};
        const bool match = dot != catalogKey.npos ?
            Matches(Field(item, "namespace"), catalogKey.substr(0, dot)) &&
                Matches(Field(item, "id"), catalogKey.substr(dot + 1)) :
            HasArtifact(item, gameKey);
        if (!match) continue;
        const auto* images = item.Find("keyImages");
        if (!images || !images->IsArray()) continue;
        const auto types = portrait ? std::array{"DieselGameBoxTall", "OfferImageTall", "DieselStoreFrontTall"} :
            std::array{"DieselGameBox", "OfferImageWide", "DieselGameBoxWide"};
        for (const auto type : types)
            for (const auto& image : images->array)
                if (Field(image, "type") == type && IsCoverUrl(Field(image, "url")))
                    return std::string(Field(image, "url"));
    }
    return {};
}
}
