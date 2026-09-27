#pragma once
#include "widget_gpu_sampler.h"
#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace snowdesktop::widget_runtime
{
// Internal evidence from D3DKMT. physicalKey is populated only for a successful
// hardware PnP-key query on an adapter with exactly one physical GPU. It is never
// exported: display names, PCI model IDs and missing counters are not identity.
struct WidgetGpuAdapterIdentity
{
    std::wstring physicalKey;
    bool typeKnown = false;
    bool renderSupported = false;
    bool indirectDisplay = false;
};
struct WidgetGpuTopologyEntry
{
    WidgetGpuAdapterDataSnapshot adapter;
    WidgetGpuAdapterIdentity identity;
};

// Owned by the sampler, alongside its topology cache. Remember only proven
// aliases across topology refreshes; Reset/destruction releases the history.
class WidgetGpuIdentityInventory
{
public:
    std::vector<WidgetGpuAdapterDataSnapshot> Resolve(std::vector<WidgetGpuTopologyEntry> entries)
    {
        for (const auto& entry : entries)
            if (!entry.identity.physicalKey.empty() && !entry.adapter.id.empty())
                history_[entry.identity.physicalKey].insert(entry.adapter.id);

        std::vector<WidgetGpuAdapterDataSnapshot> result;
        std::set<std::wstring> emitted;
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto& key = entries[index].identity.physicalKey;
            if (!key.empty() && !emitted.insert(key).second) continue;
            auto selected = index;
            if (!key.empty())
                for (std::size_t candidate = index + 1; candidate < entries.size(); ++candidate)
                    if (entries[candidate].identity.physicalKey == key &&
                        Preferred(entries[candidate], entries[selected])) selected = candidate;

            // Keep capacities and PDH ownership from the representative LUID;
            // adding logical aliases would count the same physical GPU twice.
            auto adapter = entries[selected].adapter;
            adapter.aliasIds.clear();
            const auto history = history_.find(key);
            if (history != history_.end())
                for (const auto& id : history->second)
                {
                    if (id == adapter.id) continue;
                    // A currently independent/unknown record wins over cached
                    // evidence. Do not publish an ambiguous migration target.
                    const bool other = std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                        return entry.adapter.id == id && entry.identity.physicalKey != key;
                    });
                    if (!other) adapter.aliasIds.push_back(id);
                }
            result.push_back(std::move(adapter));
        }
        return result;
    }

private:
    static bool Preferred(const WidgetGpuTopologyEntry& candidate, const WidgetGpuTopologyEntry& current)
    {
        const auto rank = [](const WidgetGpuAdapterIdentity& identity) {
            if (!identity.typeKnown) return 2;
            if (identity.renderSupported) return identity.indirectDisplay ? 1 : 0;
            return identity.indirectDisplay ? 4 : 3;
        };
        const int candidateRank = rank(candidate.identity), currentRank = rank(current.identity);
        if (candidateRank != currentRank) return candidateRank < currentRank;
        // Enumeration order and transient load/validity never select the card.
        return candidate.adapter.luid < current.adapter.luid;
    }
    std::map<std::wstring, std::set<std::string>> history_;
};
}
