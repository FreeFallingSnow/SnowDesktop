// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include "workshop_localization.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace snowdesktop::steam_bridge
{
namespace
{
std::string NormalizeLocale(std::string_view locale)
{
    std::string normalized(locale);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char value)
        {
            return value == '_' ? '-' :
                static_cast<char>(std::tolower(value));
        });
    return normalized;
}

std::string_view BaseLanguage(std::string_view locale)
{
    const std::size_t separator = locale.find('-');
    return locale.substr(0, separator);
}
}

std::optional<std::string> SteamApiLanguageForLocale(
    std::string_view locale)
{
    const std::string normalized = NormalizeLocale(locale);
    if (normalized.empty()) return std::nullopt;

    static const std::map<std::string, std::string, std::less<>> direct = {
        { "arabic", "arabic" }, { "bulgarian", "bulgarian" },
        { "schinese", "schinese" }, { "tchinese", "tchinese" },
        { "czech", "czech" }, { "danish", "danish" },
        { "dutch", "dutch" }, { "english", "english" },
        { "finnish", "finnish" }, { "french", "french" },
        { "german", "german" }, { "greek", "greek" },
        { "hungarian", "hungarian" }, { "indonesian", "indonesian" },
        { "italian", "italian" }, { "japanese", "japanese" },
        { "koreana", "koreana" }, { "malay", "malay" },
        { "norwegian", "norwegian" }, { "polish", "polish" },
        { "portuguese", "portuguese" }, { "brazilian", "brazilian" },
        { "romanian", "romanian" }, { "russian", "russian" },
        { "spanish", "spanish" }, { "latam", "latam" },
        { "swedish", "swedish" }, { "thai", "thai" },
        { "turkish", "turkish" }, { "ukrainian", "ukrainian" },
        { "vietnamese", "vietnamese" },
    };
    if (const auto found = direct.find(normalized); found != direct.end())
        return found->second;

    const std::string_view language = BaseLanguage(normalized);
    if (language == "zh")
    {
        if (normalized.find("-hant") != std::string::npos ||
            normalized.starts_with("zh-tw") ||
            normalized.starts_with("zh-hk") ||
            normalized.starts_with("zh-mo"))
            return "tchinese";
        return "schinese";
    }
    if (language == "pt")
        return normalized.starts_with("pt-br") ? "brazilian" :
            "portuguese";
    if (language == "es")
        return normalized == "es-419" ? "latam" : "spanish";
    if (language == "no" || language == "nb" || language == "nn")
        return "norwegian";

    static const std::map<std::string_view, std::string_view> byLanguage = {
        { "ar", "arabic" }, { "bg", "bulgarian" }, { "cs", "czech" },
        { "da", "danish" }, { "nl", "dutch" }, { "en", "english" },
        { "fi", "finnish" }, { "fr", "french" }, { "de", "german" },
        { "el", "greek" }, { "hu", "hungarian" },
        { "id", "indonesian" }, { "it", "italian" },
        { "ja", "japanese" }, { "ko", "koreana" }, { "ms", "malay" },
        { "pl", "polish" }, { "ro", "romanian" }, { "ru", "russian" },
        { "sv", "swedish" }, { "th", "thai" }, { "tr", "turkish" },
        { "uk", "ukrainian" }, { "vi", "vietnamese" },
    };
    if (const auto found = byLanguage.find(language);
        found != byLanguage.end())
        return std::string(found->second);
    return std::nullopt;
}

std::vector<SteamWorkshopLocalization> BuildSteamWorkshopLocalizations(
    std::string_view fallbackTitle, std::string_view fallbackDescription,
    const std::vector<WidgetLocalization>& localizations)
{
    std::map<std::string, SteamWorkshopLocalization, std::less<>> mapped;
    for (const auto& localized : localizations)
    {
        const auto language = SteamApiLanguageForLocale(localized.locale);
        if (!language || (localized.title.empty() &&
                localized.description.empty()))
            continue;
        auto& destination = mapped[*language];
        destination.language = *language;
        if (destination.title.empty() && !localized.title.empty())
            destination.title = localized.title;
        if (destination.description.empty() && !localized.description.empty())
            destination.description = localized.description;
    }

    auto& english = mapped["english"];
    english.language = "english";
    if (english.title.empty()) english.title = fallbackTitle;
    if (english.description.empty()) english.description = fallbackDescription;

    std::vector<SteamWorkshopLocalization> result;
    result.reserve(mapped.size());
    if (!english.title.empty() || !english.description.empty())
        result.push_back(english);
    for (auto& [language, localized] : mapped)
    {
        if (language == "english" ||
            (localized.title.empty() && localized.description.empty()))
            continue;
        result.push_back(std::move(localized));
    }
    return result;
}
}
