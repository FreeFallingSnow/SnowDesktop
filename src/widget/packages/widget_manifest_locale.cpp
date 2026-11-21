#include "widget_manifest_locale.h"

#include "common/l10n.h"

#include <vector>

namespace snowdesktop::widget_l10n
{
const LocaleCatalog* SelectManifestLocale(const LocaleCatalogs& catalogs)
{
    const std::string language = Locale::Instance().GetEffectiveLanguage();
    std::vector<std::string> available;
    available.reserve(catalogs.size());
    for (const auto& [code, catalog] : catalogs)
    {
        (void)catalog;
        available.push_back(code);
    }
    const std::string selected =
        snowdesktop::localization::ResolveBestLanguage(available, language);
    auto catalog = catalogs.find(selected);
    if (catalog != catalogs.end()) return &catalog->second;
    catalog = catalogs.find("en-US");
    if (catalog != catalogs.end())
        return &catalog->second;
    return catalogs.empty() ? nullptr : &catalogs.begin()->second;
}

std::string TranslateManifest(const LocaleCatalogs& catalogs,
    const std::string& key, const std::string& fallback)
{
    if (const auto* catalog = SelectManifestLocale(catalogs))
    {
        auto value = catalog->find(key);
        if (value != catalog->end())
            return value->second;
    }
    auto english = catalogs.find("en-US");
    if (english != catalogs.end())
    {
        auto value = english->second.find(key);
        if (value != english->second.end())
            return value->second;
    }
    return fallback.empty() ? key : fallback;
}
}
