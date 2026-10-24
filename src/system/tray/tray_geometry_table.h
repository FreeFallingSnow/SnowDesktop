#pragma once
#include "tray_service.h"
#include <algorithm>
#include <map>
#include <span>

namespace snowdesktop::tray
{
// Host-private storage. Callers serialize model changes and publication with
// the Service mutex; Explorer keeps its existing nonblocking seqlock reader.
class GeometryTable
{
public:
    void Set(std::span<const Icon> icons, std::span<const GeometryUpdate> updates, SharedState* shared)
    {
        // Remember only actually changed keys. This allocates no bookkeeping
        // on an unchanged frame and suppresses a change followed by a restore
        // of the same key within one batch as well.
        struct Previous { const std::string* key; std::optional<Geometry> geometry; };
        std::vector<Previous> originals;
        const auto remember = [&](const auto& update, const auto& old) {
            if (std::none_of(originals.begin(), originals.end(), [&](const auto& entry) { return *entry.key == update.key; }))
                originals.push_back({&update.key, old == values_.end() ? std::optional<Geometry>{} : std::optional<Geometry>{old->second}});
        };
        for (const auto& update : updates)
        {
            const auto icon = std::find_if(icons.begin(), icons.end(), [&](const auto& candidate) {
                return candidate.key == update.key;
            });
            if (icon == icons.end()) continue;
            const auto old = values_.find(update.key);
            if (IsRectEmpty(&update.rect))
            {
                if (old != values_.end()) { remember(update, old); values_.erase(old); }
            }
            else if (old == values_.end() || !SameFocusIdentity(old->second.identity, icon->identity) ||
                !EqualRect(&old->second.rect, &update.rect))
            {
                remember(update, old);
                values_[update.key] = {icon->identity, update.rect};
            }
        }
        if (std::any_of(originals.begin(), originals.end(), [&](const auto& entry) {
            const auto found = values_.find(*entry.key);
            if (!entry.geometry) return found != values_.end();
            return found == values_.end() || !SameFocusIdentity(found->second.identity, entry.geometry->identity) ||
                !EqualRect(&found->second.rect, &entry.geometry->rect);
        })) dirty_ = true;
        PublishPending(shared);
    }
    void Clear(SharedState* shared)
    {
        if (!values_.empty()) { values_.clear(); dirty_ = true; }
        PublishPending(shared);
    }
    void Prune(std::span<const Icon> icons, SharedState* shared)
    {
        if (std::erase_if(values_, [&](const auto& pair) {
            return std::none_of(icons.begin(), icons.end(), [&](const auto& icon) {
                return icon.key == pair.first && !(icon.state & NIS_HIDDEN) &&
                    SameFocusIdentity(icon.identity, pair.second.identity);
            });
        })) dirty_ = true;
        PublishPending(shared);
    }
    const Geometry* Find(const std::string& key) const
    {
        const auto found = values_.find(key);
        return found == values_.end() ? nullptr : &found->second;
    }
    // A new Connection owns an initially empty SharedState and icon model.
    // No geometry from a retired collector generation can carry into it.
    void Reset() { values_.clear(); dirty_ = false; }
private:
    void PublishPending(SharedState* shared)
    {
        if (!dirty_ || !shared) return;
        InterlockedIncrement(&shared->geometrySequence);
        shared->geometryCount = 0;
        for (const auto& [key, geometry] : values_)
        {
            (void)key;
            if (shared->geometryCount == kGeometries) break;
            shared->geometries[shared->geometryCount++] = geometry;
        }
        MemoryBarrier(); InterlockedIncrement(&shared->geometrySequence);
        dirty_ = false;
    }
    std::map<std::string, Geometry> values_;
    bool dirty_ = false;
};
}
