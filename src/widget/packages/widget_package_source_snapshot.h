#pragma once

#include "widget_package.h"
#include "steam/steam_workshop_sync.h"

namespace snowdesktop::widget
{
// One save transaction owns these source strings. Construct from the current
// catalogue once; no package references or cross-save cache survive the save.
class PackageSourceSnapshot
{
public:
    explicit PackageSourceSnapshot(const std::vector<InstalledPackage>& packages)
    {
        for (const auto& package : packages)
        {
            if (package.manifest.id.empty() || package.source.providerId.empty() ||
                package.source.externalItemId.empty())
                continue;
            auto found = sources_.find(package.manifest.id);
            if (found != sources_.end() && found->second.workshop) continue;
            const bool workshop = package.source.providerId == "steam-workshop" &&
                !SteamPublishedFileId(package.source.externalItemId).empty();
            if (found == sources_.end())
                sources_.emplace(package.manifest.id, Entry{package.source, workshop});
            else if (workshop || (package.active && package.enabled))
                found->second = {package.source, workshop};
        }
    }

    std::optional<PackageSourceRef> Find(const std::string& packageId) const
    {
        const auto found = sources_.find(packageId);
        return found == sources_.end()
            ? std::nullopt : std::optional(found->second.source);
    }

private:
    struct Entry
    {
        PackageSourceRef source;
        bool workshop = false;
    };
    std::unordered_map<std::string, Entry> sources_;
};
}
