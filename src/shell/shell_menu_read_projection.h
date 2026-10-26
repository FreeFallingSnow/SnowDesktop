#pragma once
#include "shell_extension_catalogue.h"
#include <set>

namespace snowdesktop::shell_extensions
{
// Build only from persisted command IDs and registration evidence. Unknown
// registration ownership retains the complete read rather than guessing by app.
inline std::vector<std::string> MenuReadProjection(const Preferences &prefs,
                                                  const Catalogue &catalogue, Category category)
{
    std::set<std::string> ids;
    for (int i = 0; i < 4; ++i)
        if (CategoryOf(static_cast<Context>(i)) == category)
            for (const auto &id : EffectiveShownIds(prefs, static_cast<Context>(i)))
            {
                const auto *pair = StatePairForId(id);
                if (pair)
                {
                    ids.insert("verb:" + std::string(pair->forward));
                    ids.insert("verb:" + std::string(pair->reverse));
                }
                else if (id.starts_with("verb:") || id.starts_with("menu:")) ids.insert(id);
                else
                {
                    const auto row = std::find_if(catalogue.rows.begin(), catalogue.rows.end(),
                        [&](const auto &r) { return r.id == id; });
                    if (row == catalogue.rows.end() || row->kind != RegistrationKind::Verb || row->verbs.empty())
                        ids.insert("*");
                    else
                        for (const auto &verb : row->verbs) ids.insert("verb:" + verb);
                    for (const auto &a : catalogue.associations)
                        if (a.registration == id) ids.insert(a.provider);
                }
            }
    return {ids.begin(), ids.end()};
}

inline bool ReadMenuRootDetails(const Request &request, const std::string &canonicalProvider,
                                const std::string &legacyPrefix)
{
    if (!request.visibleMenuProviders || request.startPinOnly || !request.sourceClsid.empty()) return true;
    for (const auto &id : *request.visibleMenuProviders)
        if (id == "*" || (!canonicalProvider.empty() && id == canonicalProvider) ||
            (!legacyPrefix.empty() && id.starts_with(legacyPrefix))) return true;
    return false;
}
}
