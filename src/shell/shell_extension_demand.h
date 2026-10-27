#pragma once
#include "shell_extension_attribution.h"
#include <algorithm>

namespace snowdesktop::shell_extensions
{
struct DemandPlan
{
    std::vector<Request> sources;
    bool full = true;
};

// Catalogue evidence names the COM source; captions and application names are
// never routing keys. Ambiguous or unobserved commands keep the aggregate path.
inline const Registration *DemandSource(const Catalogue &catalogue, const Catalogue &observed,
                                         const std::string &id, unsigned contexts)
{
    std::set<std::string> ids;
    for (const auto &item : observed.rows)
        if (item.id == id && (item.contexts & contexts) == contexts &&
            ((!item.display.key.empty() && item.display.provider == "verb:" + AttributionVerb(item.display.key)) ||
             (item.display.key.empty() && item.display.provider == id && id.starts_with("menu:") &&
              !item.display.pixels.empty())))
            for (const auto &path : item.sources)
                if (path.starts_with(L"routing:"))
                {
                    // Application attribution may have been learned in another
                    // context. Only a routing proof from this selection scope
                    // can authorize an independent metadata query.
                    const auto colon = path.find(L':', 8);
                    if (colon == std::wstring::npos || colon == 8 || colon > 10) continue;
                    unsigned scope = 0;
                    for (const auto c : std::wstring_view(path).substr(8, colon - 8))
                    {
                        if (c < L'0' || c > L'9') { scope = 0; break; }
                        scope = scope * 10 + static_cast<unsigned>(c - L'0');
                    }
                    if (!scope || (scope & ~15u) || (scope & contexts) != contexts) continue;
                    // Registration IDs used here contain ASCII CLSIDs. Reject
                    // malformed evidence instead of narrowing arbitrary Unicode.
                    std::string registration;
                    for (const auto c : std::wstring_view(path).substr(colon + 1))
                    {
                        if (c > 0x7f) { registration.clear(); break; }
                        registration.push_back(static_cast<char>(c));
                    }
                    if (!registration.empty()) ids.insert(std::move(registration));
                }
    for (const auto &a : catalogue.associations)
        if (a.provider == id && (ContextBit(a.context) & contexts)) ids.insert(a.registration);
    if (ids.size() != 1) return nullptr;
    const auto found = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) {
        return r.id == *ids.begin() && r.kind == RegistrationKind::Handler && r.systemEnabled &&
            (r.contexts & contexts) == contexts && r.verbs.size() == 1;
    });
    return found == catalogue.rows.end() ? nullptr : &*found;
}

inline std::wstring DemandSourceKey(const Registration &source, const Request &request, unsigned contexts)
{
    // Only select an initialization key whose scope is proven for the entire
    // selection. Merged ProgID/type registrations need the aggregate's class
    // resolution; using the first registration would silently select a wrong key.
    for (const auto &path : source.sources)
    {
        const auto marker = path.find(L"\\shellex\\ContextMenuHandlers\\");
        if (marker == std::wstring::npos) continue;
        const auto key = path.substr(0, marker);
        auto lower = key;
        for (auto &c : lower) c = towlower(c);
        if (request.background)
        {
            if (lower == L"directory\\background" ||
                (contexts == ContextBit(Context::Desktop) && lower == L"desktopbackground")) return key;
        }
        else if (lower == L"allfilesystemobjects" ||
            (contexts == ContextBit(Context::File) && lower == L"*") ||
            (contexts == ContextBit(Context::Folder) && (lower == L"directory" || lower == L"folder"))) return key;
    }
    return {};
}

inline DemandPlan PlanDemandQueries(const Catalogue &catalogue, const Catalogue &observed,
                                    const Preferences &prefs, const Request &request, unsigned contexts)
{
    DemandPlan plan;
    if (!catalogue.revision || !contexts || request.paths.empty() || !request.sourceClsid.empty() ||
        request.startPinOnly || request.originalShortcutOnly || request.extended) return plan;
    if (!request.background && std::any_of(request.paths.begin(), request.paths.end(), [](const auto &path) {
        const auto target = std::filesystem::path(path).lexically_normal();
        return !target.is_absolute() || !target.has_relative_path();
    })) return plan; // Drives and virtual namespace objects retain Shell class selection.
    plan.full = false;
    std::set<std::string> wanted;
    for (int i = 0; i < 4; ++i) if (contexts & (1u << i))
        for (const auto &id : EffectiveShownIds(prefs, static_cast<Context>(i))) wanted.insert(id);
    for (const auto &id : wanted)
    {
        bool visible = true;
        for (int i = 0; i < 4; ++i) if (contexts & (1u << i))
            visible &= !IsHidden(prefs, id, static_cast<Context>(i));
        if (!visible) continue;
        if (StateVisibilityId(id) == "state:start-pin" && !request.background && request.paths.size() == 1) continue;
        const auto *source = StatePairForId(id) ? nullptr : DemandSource(catalogue, observed, id, contexts);
        const auto key = source ? DemandSourceKey(*source, request, contexts) : std::wstring{};
        if (key.empty()) { plan.full = true; continue; }
        auto query = request;
        query.catalogueOnly = false;
        query.omitNvidiaCompatibility = false;
        query.sourceClsid.assign(source->verbs.front().begin(), source->verbs.front().end());
        query.sourceKey = key;
        query.visibleMenuProviders.reset();
        if (std::find(plan.sources.begin(), plan.sources.end(), query) == plan.sources.end())
            plan.sources.push_back(std::move(query));
    }
    return plan;
}
}
