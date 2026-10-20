#pragma once
#include "icon_library.h"
#include "common/background_work.h"
#include <unordered_set>

namespace snowdesktop
{
// UI-thread source cache. Asset IDs address immutable originals; a metadata
// refresh must not discard their decoded pixels or cancel unrelated requests.
class IconLibraryBitmapCache
{
public:
    explicit IconLibraryBitmapCache(size_t capacity = 256) : capacity_(capacity) {}
    bool Contains(const std::string& key) const { return entries_.contains(key); }

    template<class Release>
    std::uint64_t Begin(const std::string& key, const std::string& asset,
        const std::wstring& identity, const std::unordered_set<std::string>& pinned, Release release)
    {
        if (Contains(key)) return 0;
        if (!identity.empty()) displayed_[identity].requested = key;
        while (entries_.size() >= capacity_ && !entries_.empty())
        {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
            {
                if (it->second.pending || pinned.contains(it->second.asset) || RetainedForReplacement(it->first)) continue;
                if (oldest == entries_.end() || it->second.used < oldest->second.used) oldest = it;
            }
            // Active icons and in-flight decodes form a working set. Allow it
            // to exceed the soft limit rather than cycling visible icons.
            if (oldest == entries_.end()) break;
            Erase(oldest->first, release);
        }
        const auto ticket = ++serial_;
        entries_.emplace(key, Entry{asset, {}, true, ++clock_, ticket});
        return ticket;
    }
    bool Publish(const std::string& key, std::uint64_t ticket, std::shared_ptr<BackgroundBitmap> bitmap)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.ticket != ticket) return false;
        found->second.bitmap = std::move(bitmap); found->second.pending = false;
        return true;
    }
    std::shared_ptr<BackgroundBitmap> Read(const std::string& key, const std::wstring& identity)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return {};
        found->second.used = ++clock_;
        if (found->second.bitmap && found->second.bitmap->bitmap)
        {
            if (!identity.empty()) displayed_[identity] = {key, key};
            return found->second.bitmap;
        }
        const auto previous = displayed_.find(identity);
        if (!found->second.pending)
        {
            // A confirmed missing/invalid image falls back to the default.
            if (previous != displayed_.end()) displayed_.erase(previous);
            return {};
        }
        if (previous == displayed_.end()) return {};
        previous->second.requested = key;
        const auto ready = entries_.find(previous->second.ready);
        return ready == entries_.end() ? nullptr : ready->second.bitmap;
    }
    void Forget(const std::wstring& identity) { displayed_.erase(identity); }

    template<class Release>
    void Reconcile(const std::vector<IconLibraryEntry>& assets, Release release)
    {
        std::unordered_set<std::string> valid;
        for (const auto& asset : assets) if (!asset.missing) valid.insert(asset.id);
        std::vector<std::string> removed;
        for (const auto& [key, entry] : entries_)
            if (!valid.contains(entry.asset) || (!entry.pending && (!entry.bitmap || !entry.bitmap->bitmap)))
                removed.push_back(key);
        for (const auto& key : removed) Erase(key, release);
    }
    template<class Release>
    void Erase(std::string key, Release release)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return;
        release(key, found->second.bitmap);
        entries_.erase(found);
        std::erase_if(displayed_, [&](const auto& pair) { return pair.second.ready == key; });
    }
private:
    struct Entry
    {
        std::string asset;
        std::shared_ptr<BackgroundBitmap> bitmap;
        bool pending;
        std::uint64_t used, ticket;
    };
    struct Displayed { std::string requested, ready; };
    bool RetainedForReplacement(const std::string& key) const
    {
        for (const auto& [identity, display] : displayed_)
        {
            (void)identity;
            const auto pending = entries_.find(display.requested);
            if (display.ready == key && (pending == entries_.end() || pending->second.pending)) return true;
        }
        return false;
    }
    size_t capacity_;
    std::uint64_t clock_ = 0, serial_ = 0;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::wstring, Displayed> displayed_;
};
}
