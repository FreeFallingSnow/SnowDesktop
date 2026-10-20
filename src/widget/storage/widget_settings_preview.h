#pragma once

#include "widget/preview/widget_preview_context.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
// Settings previews are a read projection. The supplied storage map always
// contains committed values, including ordinary writes made during a preview.
class WidgetSettingsPreviewLayers
{
public:
    void InvalidateReads() noexcept { dirty_ = true; }

    const StorageMap& Read(const StorageMap& committed) const
    {
        if (layers_.empty()) return committed;
        if (dirty_)
        {
            StorageMap candidate = committed;
            for (const auto& [owner, layer] : layers_)
            {
                (void)owner;
                Apply(layer, candidate);
            }
            effective_.swap(candidate);
            dirty_ = false;
        }
        return effective_;
    }

    bool Update(const void* owner, const std::string& prefix,
        const StorageMap& previousView, const StorageMap& candidate)
    {
        if (!owner || prefix.empty()) return false;
        for (const auto& [other, layer] : layers_)
            if (other != owner && layer.prefix == prefix)
                return false;
        const auto existing = layers_.find(owner);
        if (existing != layers_.end() && existing->second.prefix != prefix)
            return false;
        Layer next = existing == layers_.end() ? Layer{} : existing->second;
        next.prefix = prefix;
        for (const auto& [key, value] : previousView)
        {
            if (!key.starts_with(prefix)) continue;
            const auto after = candidate.find(key);
            if (after == candidate.end()) next.values[key] = std::nullopt;
            else if (after->second != value) next.values[key] = after->second;
        }
        for (const auto& [key, value] : candidate)
            if (key.starts_with(prefix) && !previousView.contains(key))
                next.values[key] = value;
        layers_.insert_or_assign(owner, std::move(next));
        InvalidateReads();
        return true;
    }

    template<class Save, class Validate>
    bool Commit(const void* owner, StorageMap& committed,
        Save save, Validate validate)
    {
        const auto found = layers_.find(owner);
        if (found == layers_.end()) return true;
        StorageMap candidate = committed;
        Apply(found->second, candidate);
        if (!validate(candidate, found->second.prefix)) return false;
        committed.swap(candidate);
        InvalidateReads();
        if (!save())
        {
            committed.swap(candidate);
            InvalidateReads();
            return false;
        }
        Remove(owner);
        return true;
    }

    void Remove(const void* owner) noexcept
    {
        layers_.erase(owner);
        InvalidateReads();
    }

    void Clear() noexcept
    {
        layers_.clear();
        effective_.clear();
        InvalidateReads();
    }

private:
    struct Layer
    {
        std::string prefix;
        std::unordered_map<std::string, std::optional<std::string>> values;
    };
    static void Apply(const Layer& layer, StorageMap& storage)
    {
        for (const auto& [key, value] : layer.values)
            if (value) storage.insert_or_assign(key, *value);
            else storage.erase(key);
    }
    std::unordered_map<const void*, Layer> layers_;
    mutable StorageMap effective_;
    mutable bool dirty_ = true;
};
}
