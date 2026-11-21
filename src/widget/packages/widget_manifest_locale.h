#pragma once

#include <string>
#include <unordered_map>

namespace snowdesktop::widget_l10n
{
using LocaleCatalog = std::unordered_map<std::string, std::string>;
using LocaleCatalogs = std::unordered_map<std::string, LocaleCatalog>;

std::string TranslateManifest(const LocaleCatalogs& catalogs,
    const std::string& key, const std::string& fallback = {});
const LocaleCatalog* SelectManifestLocale(const LocaleCatalogs& catalogs);
}
