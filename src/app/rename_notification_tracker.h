#pragma once

#include "../desktop_item_reference_migration.h"
#include <algorithm>

/** Suppresses only the exact Shell rename already applied by our completion. */
class RenameNotificationTracker
{
public:
    bool Begin(const std::wstring& source, ULONGLONG now)
    {
        Expire(now);
        for (const auto& entry : entries_)
            if (entry.pending && Equal(entry.source, source))
                return false;
        entries_.push_back({ source, {}, {}, true, 0 });
        return true;
    }

    bool Observe(const std::wstring& source, const std::wstring& target,
        ULONGLONG now)
    {
        Expire(now);
        for (auto& entry : entries_)
        {
            if (!Equal(entry.source, source))
                continue;
            if (entry.pending)
            {
                entry.observedTargets.push_back(target);
                return true;
            }
            if (Equal(entry.target, target))
                return true;
        }
        return false;
    }

    // A failure or a different concurrent rename must replay normal refresh.
    bool Finish(const std::wstring& source, const std::wstring& target,
        bool succeeded, ULONGLONG now)
    {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            if (!it->pending || !Equal(it->source, source))
                continue;
            const bool needsRefresh = std::any_of(
                it->observedTargets.begin(), it->observedTargets.end(),
                [&](const auto& observed) {
                    return !succeeded || !Equal(observed, target);
                });
            if (!succeeded || target.empty())
                entries_.erase(it);
            else
            {
                it->pending = false;
                it->target = target;
                it->observedTargets.clear();
                it->expiresAt = now + 5000;
            }
            return needsRefresh;
        }
        return false;
    }

private:
    struct Entry
    {
        std::wstring source;
        std::wstring target;
        std::vector<std::wstring> observedTargets;
        bool pending;
        ULONGLONG expiresAt;
    };
    static bool Equal(const std::wstring& a, const std::wstring& b)
    {
        return snowdesktop::desktop_item_reference_migration::KeysEqual(a, b);
    }
    void Expire(ULONGLONG now)
    {
        std::erase_if(entries_, [now](const auto& entry) {
            return !entry.pending && entry.expiresAt <= now;
        });
    }
    std::vector<Entry> entries_;
};
