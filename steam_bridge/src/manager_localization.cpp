// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include "manager_localization.h"
#include "language_fallback.h"

#include "bridge_json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace snowdesktop::steam_bridge
{
namespace
{
std::string NormalizeLanguage(std::string_view language)
{
    std::string result(language);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char value)
        {
            return value == '_' ? '-' :
                static_cast<char>(std::tolower(value));
        });
    return result;
}

std::string BaseLanguage(std::string_view language)
{
    const std::string normalized = NormalizeLanguage(language);
    const std::size_t separator = normalized.find('-');
    return normalized.substr(0, separator);
}

bool IsManagerKey(std::string_view key)
{
    return key.starts_with("workshop_manager.");
}
}

bool ManagerLocalization::Load(
    const std::filesystem::path& languageDirectory,
    std::string_view requestedLanguage, std::string& error)
{
    catalogs_.clear();
    selected_.store(0);
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(languageDirectory, filesystemError))
    {
        error = "language directory not found: " +
            languageDirectory.string();
        return false;
    }

    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator iterator(languageDirectory,
             std::filesystem::directory_options::skip_permission_denied,
             filesystemError), end;
         !filesystemError && iterator != end; iterator.increment(filesystemError))
    {
        if (iterator->is_regular_file(filesystemError) &&
            iterator->path().extension() == L".json")
            files.push_back(iterator->path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files)
    {
        std::ifstream input(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xef &&
            static_cast<unsigned char>(text[1]) == 0xbb &&
            static_cast<unsigned char>(text[2]) == 0xbf)
            text.erase(0, 3);
        JsonValue root;
        std::string parseError;
        if (!input || !ParseJson(text, root, parseError) || !root.IsObject())
        {
            error = "failed to load language catalog " + file.string() +
                ": " + (parseError.empty() ? "invalid JSON object" : parseError);
            return false;
        }
        Catalog catalog;
        catalog.language = file.stem().string();
        for (const auto& [key, value] : root.object)
        {
            if (value.IsString()) catalog.entries.emplace(key, value.string);
        }
        catalogs_.push_back(std::move(catalog));
    }
    if (catalogs_.empty())
    {
        error = "no language catalogs found in " + languageDirectory.string();
        return false;
    }

    const std::size_t englishIndex = Resolve("en-US");
    const auto& english = catalogs_[englishIndex].entries;
    for (auto& catalog : catalogs_)
    {
        for (const auto& [key, englishValue] : english)
        {
            if (!IsManagerKey(key)) continue;
            const auto translated = catalog.entries.find(key);
            if (translated != catalog.entries.end())
                catalog.byEnglishValue.emplace(englishValue,
                    translated->second);
        }
    }
    selected_.store(Resolve(requestedLanguage));
    error.clear();
    return true;
}

std::size_t ManagerLocalization::Resolve(
    std::string_view requestedLanguage) const
{
    if (catalogs_.empty()) return 0;
    std::vector<std::string> available;
    available.reserve(catalogs_.size());
    for (const Catalog& catalog : catalogs_)
        available.push_back(catalog.language);
    const std::string selected =
        snowdesktop::localization::ResolveBestLanguage(
            available, requestedLanguage);
    if (!selected.empty())
    {
        const std::string normalized = NormalizeLanguage(selected);
        for (std::size_t index = 0; index < catalogs_.size(); ++index)
            if (NormalizeLanguage(catalogs_[index].language) == normalized)
                return index;
    }
    for (std::size_t index = 0; index < catalogs_.size(); ++index)
    {
        if (NormalizeLanguage(catalogs_[index].language) == "en-us")
            return index;
    }
    return 0;
}

bool ManagerLocalization::SelectedLanguageIsChinese() const
{
    if (catalogs_.empty()) return false;
    return BaseLanguage(catalogs_[selected_.load()].language) == "zh";
}

const char* ManagerLocalization::Translate(const char* english,
    const char* builtInChinese) const
{
    if (catalogs_.empty())
        return SelectedLanguageIsChinese() ? builtInChinese : english;
    const Catalog& catalog = catalogs_[selected_.load()];
    const auto translated = catalog.byEnglishValue.find(english);
    if (translated != catalog.byEnglishValue.end())
        return translated->second.c_str();
    return SelectedLanguageIsChinese() ? builtInChinese : english;
}

void ManagerLocalization::SelectLanguage(
    std::string_view requestedLanguage)
{
    if (!catalogs_.empty()) selected_.store(Resolve(requestedLanguage));
}
}
