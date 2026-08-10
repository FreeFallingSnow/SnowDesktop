/**
 * @file steam_workshop_cache.cpp
 * @brief Safe, read-only parsing of Steam's local Workshop KeyValues cache.
 */

#include "steam_workshop_cache.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_set>

namespace snowdesktop::widget
{
namespace
{
constexpr std::size_t kMaximumVdfBytes = 16u * 1024u * 1024u;

bool DigitsOnly(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char character)
        {
            return character >= '0' && character <= '9';
        });
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::optional<std::wstring> Utf8ToWide(std::string_view value)
{
    if (value.empty()) return std::wstring{};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), result.data(),
        length) != length)
        return std::nullopt;
    return result;
}

bool ReadTextFile(const std::filesystem::path& path, std::string& text)
{
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(path, filesystemError);
    if (filesystemError || size > kMaximumVdfBytes) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    text.assign(std::istreambuf_iterator<char>(input), {});
    return input.good() || input.eof();
}

struct VdfObject;
struct VdfEntry
{
    std::string key;
    std::string value;
    std::unique_ptr<VdfObject> object;
};

struct VdfObject
{
    std::vector<VdfEntry> entries;
};

enum class TokenKind
{
    String,
    Open,
    Close,
    End,
    Invalid,
};

struct Token
{
    TokenKind kind = TokenKind::Invalid;
    std::string value;
};

class VdfParser
{
public:
    explicit VdfParser(std::string_view input) : input_(input) {}

    bool Parse(VdfObject& output, std::string& error)
    {
        if (!ParseObject(output, false, error)) return false;
        return true;
    }

private:
    void SkipTrivia()
    {
        while (position_ < input_.size())
        {
            const unsigned char character = input_[position_];
            if (std::isspace(character))
            {
                ++position_;
                continue;
            }
            if (character == '/' && position_ + 1 < input_.size() &&
                input_[position_ + 1] == '/')
            {
                position_ += 2;
                while (position_ < input_.size() &&
                    input_[position_] != '\n')
                    ++position_;
                continue;
            }
            break;
        }
    }

    Token Next()
    {
        SkipTrivia();
        if (position_ >= input_.size()) return { TokenKind::End, {} };
        const char first = input_[position_++];
        if (first == '{') return { TokenKind::Open, {} };
        if (first == '}') return { TokenKind::Close, {} };
        if (first == '"')
        {
            std::string value;
            while (position_ < input_.size())
            {
                const char character = input_[position_++];
                if (character == '"')
                    return { TokenKind::String, std::move(value) };
                if (character == '\\' && position_ < input_.size())
                {
                    const char escaped = input_[position_++];
                    if (escaped == '\\' || escaped == '"')
                        value.push_back(escaped);
                    else
                    {
                        value.push_back('\\');
                        value.push_back(escaped);
                    }
                    continue;
                }
                value.push_back(character);
            }
            return { TokenKind::Invalid, {} };
        }

        std::string value(1, first);
        while (position_ < input_.size())
        {
            const unsigned char character = input_[position_];
            if (std::isspace(character) || character == '{' ||
                character == '}')
                break;
            value.push_back(input_[position_++]);
        }
        return { TokenKind::String, std::move(value) };
    }

    bool ParseObject(VdfObject& output, bool expectsClose,
        std::string& error)
    {
        while (true)
        {
            Token key = Next();
            if (key.kind == TokenKind::End)
            {
                if (expectsClose)
                {
                    error = "unterminated Steam KeyValues object";
                    return false;
                }
                return true;
            }
            if (key.kind == TokenKind::Close)
            {
                if (!expectsClose)
                {
                    error = "unexpected closing brace in Steam KeyValues";
                    return false;
                }
                return true;
            }
            if (key.kind != TokenKind::String || key.value.empty())
            {
                error = "invalid key in Steam KeyValues";
                return false;
            }

            Token value = Next();
            VdfEntry entry;
            entry.key = LowerAscii(std::move(key.value));
            if (value.kind == TokenKind::String)
            {
                entry.value = std::move(value.value);
            }
            else if (value.kind == TokenKind::Open)
            {
                entry.object = std::make_unique<VdfObject>();
                if (!ParseObject(*entry.object, true, error)) return false;
            }
            else
            {
                error = "missing value in Steam KeyValues";
                return false;
            }
            output.entries.push_back(std::move(entry));
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

const VdfObject* FindObject(const VdfObject& object, std::string_view key)
{
    const std::string lowerKey = LowerAscii(std::string(key));
    for (const auto& entry : object.entries)
        if (entry.key == lowerKey && entry.object)
            return entry.object.get();
    return nullptr;
}

std::optional<std::string> FindValue(
    const VdfObject& object, std::string_view key)
{
    const std::string lowerKey = LowerAscii(std::string(key));
    for (const auto& entry : object.entries)
        if (entry.key == lowerKey && !entry.object)
            return entry.value;
    return std::nullopt;
}

bool ParseVdfFile(const std::filesystem::path& path, VdfObject& root,
    std::string& error)
{
    std::string text;
    if (!ReadTextFile(path, text))
    {
        error = "cannot read Steam KeyValues file";
        return false;
    }
    return VdfParser(text).Parse(root, error);
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subKey,
    const wchar_t* valueName)
{
    DWORD bytes = 0;
    if (RegGetValueW(root, subKey, valueName, RRF_RT_REG_SZ, nullptr,
        nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subKey, valueName, RRF_RT_REG_SZ, nullptr,
        value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::optional<DWORD> ReadRegistryDword(HKEY root, const wchar_t* subKey,
    const wchar_t* valueName)
{
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(root, subKey, valueName, RRF_RT_REG_DWORD, nullptr,
        &value, &bytes) != ERROR_SUCCESS || bytes != sizeof(value))
        return std::nullopt;
    return value;
}

std::filesystem::path NormalizePath(const std::filesystem::path& input)
{
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(input, error);
    if (error)
    {
        error.clear();
        result = std::filesystem::absolute(input, error);
    }
    return error ? input.lexically_normal() : result.lexically_normal();
}

std::wstring PathKey(const std::filesystem::path& path)
{
    std::wstring key = NormalizePath(path).wstring();
    std::transform(key.begin(), key.end(), key.begin(),
        [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return key;
}

void AppendError(std::string& destination, std::string_view message)
{
    if (!destination.empty()) destination += " | ";
    destination += message;
}
}

std::string ReadSteamActiveUserAccountId()
{
    const auto activeUser = ReadRegistryDword(HKEY_CURRENT_USER,
        L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser");
    return activeUser && *activeUser != 0
        ? std::to_string(*activeUser) : std::string{};
}

std::vector<std::filesystem::path> DiscoverSteamLibraryRoots(
    std::uint32_t appId, std::string& error)
{
    error.clear();
    std::wstring steamPath = ReadRegistryString(HKEY_CURRENT_USER,
        L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty())
        steamPath = ReadRegistryString(HKEY_LOCAL_MACHINE,
            L"Software\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty())
    {
        error = "Steam installation path is unavailable";
        return {};
    }

    std::vector<std::filesystem::path> libraries;
    std::unordered_set<std::wstring> seen;
    auto append = [&](const std::filesystem::path& path)
    {
        if (path.empty()) return;
        const auto normalized = NormalizePath(path);
        if (seen.insert(PathKey(normalized)).second)
            libraries.push_back(normalized);
    };
    const auto libraryFile = std::filesystem::path(steamPath) /
        L"steamapps" / L"libraryfolders.vdf";
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(libraryFile, filesystemError))
    {
        append(steamPath);
        return libraries;
    }

    VdfObject root;
    std::string parseError;
    if (!ParseVdfFile(libraryFile, root, parseError))
    {
        error = "cannot parse Steam libraryfolders.vdf: " + parseError;
        append(steamPath);
        return libraries;
    }
    const VdfObject* folders = FindObject(root, "libraryfolders");
    if (!folders) folders = &root;
    const std::string expectedAppId = std::to_string(appId);
    for (const auto& entry : folders->entries)
    {
        if (!DigitsOnly(entry.key)) continue;
        std::optional<std::string> path;
        if (entry.object)
        {
            path = FindValue(*entry.object, "path");
            if (appId != 0)
            {
                const VdfObject* apps =
                    FindObject(*entry.object, "apps");
                if (!apps || !FindValue(*apps, expectedAppId))
                    continue;
            }
        }
        else if (appId == 0)
            path = entry.value;
        else
            continue;
        if (!path) continue;
        const auto wide = Utf8ToWide(*path);
        if (wide && !wide->empty()) append(*wide);
    }
    // Old libraryfolders formats do not carry the per-library app map. The
    // Steam install root remains the safest fallback and avoids probing every
    // unrelated/offline library merely to discover one Workshop manifest.
    if (libraries.empty()) append(steamPath);
    return libraries;
}

SteamWorkshopLocalCache ReadSteamWorkshopLocalCache(
    const std::vector<std::filesystem::path>& libraryRoots,
    std::uint32_t appId)
{
    SteamWorkshopLocalCache result;
    const std::string expectedAppId = std::to_string(appId);
    std::set<std::string> subscribed;
    std::map<std::string, std::filesystem::path> ready;
    bool foundManifest = false;
    bool invalidManifest = false;

    for (const auto& library : libraryRoots)
    {
        const auto workshopRoot = library / L"steamapps" / L"workshop";
        const auto manifest = workshopRoot /
            (L"appworkshop_" + std::to_wstring(appId) + L".acf");
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(manifest, filesystemError))
            continue;
        foundManifest = true;

        VdfObject root;
        std::string parseError;
        if (!ParseVdfFile(manifest, root, parseError))
        {
            invalidManifest = true;
            AppendError(result.error,
                "cannot parse Steam Workshop cache: " + parseError);
            continue;
        }
        const VdfObject* app = FindObject(root, "appworkshop");
        if (!app || FindValue(*app, "appid").value_or("") != expectedAppId)
        {
            invalidManifest = true;
            AppendError(result.error,
                "Steam Workshop cache has an unexpected App ID");
            continue;
        }

        const VdfObject* installed =
            FindObject(*app, "workshopitemsinstalled");
        const VdfObject* details =
            FindObject(*app, "workshopitemdetails");

        std::map<std::string, std::string> installedManifests;
        if (installed)
        {
            for (const auto& entry : installed->entries)
            {
                if (!DigitsOnly(entry.key) || !entry.object)
                {
                    invalidManifest = true;
                    AppendError(result.error,
                        "Steam Workshop cache contains an invalid installed item");
                    continue;
                }
                const auto itemManifest = FindValue(*entry.object, "manifest");
                if (itemManifest && DigitsOnly(*itemManifest))
                    installedManifests[entry.key] = *itemManifest;
            }
        }

        std::map<std::string, std::string> latestManifests;
        if (details)
        {
            for (const auto& entry : details->entries)
            {
                if (!DigitsOnly(entry.key) || !entry.object)
                {
                    invalidManifest = true;
                    AppendError(result.error,
                        "Steam Workshop cache contains an invalid item detail");
                    continue;
                }
                const auto subscribedBy =
                    FindValue(*entry.object, "subscribedby");
                if (subscribedBy && !DigitsOnly(*subscribedBy))
                {
                    invalidManifest = true;
                    AppendError(result.error,
                        "Steam Workshop cache contains an invalid subscription marker");
                    continue;
                }
                // Current Steam clients persist item details after an
                // unsubscribe. A zero subscribedby marker is therefore not a
                // subscription, even if the old content is still installed.
                // Older cache formats omit the marker, so keep accepting those
                // detail entries for compatibility.
                if (subscribedBy && std::all_of(subscribedBy->begin(),
                    subscribedBy->end(),
                    [](char value) { return value == '0'; }))
                    continue;
                subscribed.insert(entry.key);
                const auto latest =
                    FindValue(*entry.object, "latest_manifest");
                if (latest && DigitsOnly(*latest))
                    latestManifests[entry.key] = *latest;
            }
        }

        for (const auto& [publishedFileId, installedManifest] :
            installedManifests)
        {
            const auto latest = latestManifests.find(publishedFileId);
            // NeedsDownload/NeedsUpdate are application-wide flags. One new
            // download must not demote every other current item to pending.
            if (!subscribed.contains(publishedFileId) ||
                latest == latestManifests.end() ||
                latest->second != installedManifest)
                continue;
            const auto content = workshopRoot / L"content" /
                std::to_wstring(appId) / Utf8ToWide(publishedFileId).value();
            if (std::filesystem::is_directory(content, filesystemError) &&
                !filesystemError)
                ready[publishedFileId] = content;
        }
    }

    result.authoritative = foundManifest && !invalidManifest;
    if (!foundManifest)
        result.error = "Steam Workshop cache is unavailable";
    result.subscribedPublishedFileIds.assign(
        subscribed.begin(), subscribed.end());
    for (auto& [publishedFileId, contentDirectory] : ready)
        result.readyItems.push_back(
            { std::move(publishedFileId), std::move(contentDirectory) });
    return result;
}
}
