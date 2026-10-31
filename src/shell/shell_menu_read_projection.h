#pragma once
#include "shell_extension_catalogue.h"
#include <set>

namespace snowdesktop::shell_extensions
{
// Static registrations/package roots already name an individual command.
// Preserve their established authority; dynamic CLSID groups allow exceptions
// for each generated root without changing unrelated legacy alias scopes.
inline std::string MenuEntryVisibilityId(const Preferences& prefs, const Entry& entry, Context context)
{
    if (const auto* pair = StatePairForVerb(entry.key)) return std::string(pair->id);
    if (!entry.registration.empty() && !entry.registration.starts_with("clsid:") &&
        HasVisibilityRule(prefs, entry.registration, context)) return entry.registration;
    return EntryVisibilityId(prefs, entry, context);
}

// Popup projection is independent of selected object type. Metadata and
// management queries never publish provisional user-menu headers.
inline bool CanPreviewPopupRoots(const Request& request, int depth)
{
    return depth == 0 && request.visibleMenuProviders && !request.visibleMenuProviders->empty() &&
        !request.catalogueOnly && !request.rootMetadataOnly && !request.startPinOnly && request.sourceClsid.empty() && request.sourceKey.empty();
}

// Build only from persisted command IDs and registration evidence. Unknown
// registration ownership retains the complete read rather than guessing by app.
inline std::vector<std::string> MenuReadPolicy(const Preferences &prefs, Category category)
{
    std::vector<std::string> result;
    for (int i = 0; i < 4; ++i)
        if (CategoryOf(static_cast<Context>(i)) == category)
            for (const auto &id : EffectiveShownIds(prefs, static_cast<Context>(i)))
                result.push_back(std::to_string(i) + "|" + id);
    return result;
}

// Cache identity contains only the configured policy above. Resolve aliases
// against the live inventory on dispatch; learning an association must not
// change the key of a query that is already waiting for that inventory.
inline std::vector<std::string> ResolveMenuReadProjection(const std::vector<std::string> &policy,
                                                        const Catalogue &catalogue, unsigned contexts)
{
    std::set<std::string> ids;
    for (auto id : policy)
    {
        if (id.size() > 2 && id[0] >= '0' && id[0] <= '3' && id[1] == '|')
        {
            if (contexts && !(contexts & (1u << (id[0] - '0')))) continue;
            id.erase(0, 2);
        }
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
    }
    return {ids.begin(), ids.end()};
}

inline std::vector<std::string> MenuReadProjection(const Preferences &prefs,
                                                  const Catalogue &catalogue, Category category)
{
    return ResolveMenuReadProjection(MenuReadPolicy(prefs, category), catalogue, 0);
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

inline bool NeedsSynchronousCascades(const Request& request)
{
    // Send To can expose a readable but incomplete asynchronous subtree.
    // Preserve Shell's completion guarantee whenever that subtree is read;
    // selective popups can initialize their other cascades after the headers.
    return !CanPreviewPopupRoots(request, 0) ||
        ReadMenuRootDetails(request, "verb:sendto", "menu:send to:") ||
        std::any_of(request.visibleMenuProviders->begin(), request.visibleMenuProviders->end(), [](const auto& id) {
            // Unknown caption identities can include a localized Send To.
            // WinRAR's observed cascade has the explicit initialization path.
            return id.starts_with("menu:") && !id.starts_with("menu:winrar:");
        });
}
}
