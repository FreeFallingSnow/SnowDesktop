// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::steam_bridge
{
class ManagerLocalization
{
public:
    bool Load(const std::filesystem::path& languageDirectory,
        std::string_view requestedLanguage, std::string& error);

    const char* Translate(const char* english,
        const char* builtInChinese) const;
    void SelectLanguage(std::string_view requestedLanguage);

private:
    struct Catalog
    {
        std::string language;
        std::map<std::string, std::string, std::less<>> entries;
        std::map<std::string, std::string, std::less<>> byEnglishValue;
    };

    std::size_t Resolve(std::string_view requestedLanguage) const;
    bool SelectedLanguageIsChinese() const;

    std::vector<Catalog> catalogs_;
    std::atomic_size_t selected_ = 0;
};
}
