#pragma once
#include "shell_extension_catalogue.h"
#include <set>

namespace snowdesktop::shell_extensions
{
inline std::string AttributionVerb(std::string value)
{
    for (auto &c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return value;
}
inline bool SameSourceCommand(const Entry &actual, const Entry &probe)
{
    if (actual.separator || probe.separator || actual.label.empty() || probe.label.empty()) return false;
    // A canonical command emitted by this exact COM provider is stronger than
    // its caption. For legacy commands without verbs require the same bitmap
    // and complete materialized subtree as well; a caption alone proves nothing.
    if (!actual.key.empty() || !probe.key.empty())
        return !actual.key.empty() && AttributionVerb(actual.key) == AttributionVerb(probe.key);
    if (actual.label != probe.label || actual.pixels.empty() || actual.width != probe.width || actual.height != probe.height || actual.pixels != probe.pixels ||
        actual.native != probe.native || actual.children.size() != probe.children.size()) return false;
    for (size_t i = 0; i < actual.children.size(); ++i)
        if (!(actual.children[i].separator && probe.children[i].separator) && !SameSourceCommand(actual.children[i], probe.children[i])) return false;
    return true;
}
inline std::vector<std::string> MatchSourceCommands(const Reply &actual, const Reply &probe)
{
    std::vector<std::string> matches;
    if (!actual.ok || !probe.ok) return matches;
    for (const auto &entry : actual.entries)
        if (!entry.provider.empty() &&
            std::count_if(actual.entries.begin(), actual.entries.end(), [&](const auto &e) { return SameSourceCommand(entry, e); }) == 1 &&
            std::count_if(probe.entries.begin(), probe.entries.end(), [&](const auto &e) { return SameSourceCommand(entry, e); }) == 1)
            matches.push_back(entry.provider);
    return matches;
}
inline Application RegisteredApplication(const Catalogue &catalogue, const Request &request, const Entry &entry)
{
    const auto verb = AttributionVerb(entry.key);
    if (verb.empty()) return {};
    Application result;
    for (const auto &candidate : catalogue.rows)
        if (candidate.systemEnabled && Applies(candidate, request) &&
            std::find(candidate.verbs.begin(), candidate.verbs.end(), verb) != candidate.verbs.end())
        {
            if (candidate.application.id.empty()) return {};
            if (!result.id.empty() && result.id != candidate.application.id) return {};
            result = candidate.application;
        }
    // This is display metadata only; ambiguous registration IDs remain separate.
    return result;
}
inline void RecordSourceApplication(Registration &item, const Registration &source, const Catalogue &catalogue)
{
    const auto evidence = L"attribution:" + std::wstring(source.id.begin(), source.id.end());
    if (std::find(item.sources.begin(), item.sources.end(), evidence) == item.sources.end()) item.sources.push_back(evidence);
    Application application;
    for (const auto &candidate : catalogue.rows)
        if (std::find(item.sources.begin(), item.sources.end(), L"attribution:" + std::wstring(candidate.id.begin(), candidate.id.end())) != item.sources.end())
        {
            if (!candidate.systemEnabled || candidate.application.id.empty() || (!application.id.empty() && application.id != candidate.application.id))
            { item.application = {}; return; }
            application = candidate.application;
        }
    item.application = std::move(application);
}
}
