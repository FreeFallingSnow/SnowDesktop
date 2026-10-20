#pragma once

#include "common/types.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

namespace snowdesktop::desktop_membership_recovery
{
inline constexpr std::int64_t kRetentionSeconds = 24 * 60 * 60;

inline std::int64_t NowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct KeyLess
{
    bool operator()(const std::wstring& left, const std::wstring& right) const
    {
        return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
            right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

using KeySet = std::set<std::wstring, KeyLess>;

inline bool Expired(std::int64_t missingSince, std::int64_t now)
{
    return missingSince <= 0 ||
        (now >= missingSince && now - missingSince >= kRetentionSeconds);
}

// Reinsert hidden members only into a temporary logical order. The renderer,
// selection, scrolling and popup code continue to see exclusively itemKeys.
inline std::vector<MissingDesktopWidgetItem> LogicalOrder(const DesktopWidget& widget)
{
    std::vector<MissingDesktopWidgetItem> result;
    KeySet seen;
    for (const auto& key : widget.itemKeys)
        if (!key.empty() && seen.insert(key).second)
            result.push_back({key, 0, 0});
    auto missing = widget.missingItems;
    std::stable_sort(missing.begin(), missing.end(),
        [](const auto& left, const auto& right) { return left.index < right.index; });
    for (auto& entry : missing)
        if (!entry.key.empty() && seen.insert(entry.key).second)
            result.insert(result.begin() + static_cast<std::ptrdiff_t>(
                std::min(entry.index, result.size())), std::move(entry));
    return result;
}

// Explicit user operations revoke both live ownership and any pending return.
inline void Forget(DesktopWidget& widget, const std::vector<std::wstring>& keys)
{
    if (widget.type == DesktopWidgetType::FolderMapping || keys.empty()) return;
    const KeySet removed(keys.begin(), keys.end());
    const auto order = LogicalOrder(widget);
    widget.itemKeys.clear();
    widget.missingItems.clear();
    size_t index = 0;
    for (auto entry : order)
    {
        if (removed.contains(entry.key)) continue;
        if (entry.missingSince == 0)
            widget.itemKeys.push_back(std::move(entry.key));
        else
        {
            entry.index = index;
            widget.missingItems.push_back(std::move(entry));
        }
        ++index;
    }
}

inline void MergeMissing(DesktopWidget& target, const DesktopWidget& source,
    const std::vector<std::wstring>& mergedKeys)
{
    auto order = LogicalOrder(target);
    const auto sourceOrder = LogicalOrder(source);
    order.insert(order.end(), sourceOrder.begin(), sourceOrder.end());
    const KeySet active(mergedKeys.begin(), mergedKeys.end());
    KeySet seen;
    target.missingItems.clear();
    size_t index = 0;
    for (auto entry : order)
    {
        if (!seen.insert(entry.key).second) continue;
        if (entry.missingSince != 0 && !active.contains(entry.key))
        {
            entry.index = index;
            target.missingItems.push_back(std::move(entry));
        }
        ++index;
    }
}

inline bool PruneExpired(std::vector<DesktopWidget>& widgets, std::int64_t now)
{
    bool changed = false;
    for (auto& widget : widgets)
    {
        if (widget.type == DesktopWidgetType::FolderMapping || widget.missingItems.empty()) continue;
        if (std::none_of(widget.missingItems.begin(), widget.missingItems.end(),
                [now](const auto& entry) {
                    return Expired(entry.missingSince, now) || entry.missingSince > now;
                }))
            continue;
        const auto before = widget.missingItems;
        size_t index = 0;
        widget.missingItems.clear();
        // Use the old cache to account for expired holes in the logical order.
        DesktopWidget previous;
        previous.itemKeys = widget.itemKeys;
        previous.missingItems = before;
        for (auto entry : LogicalOrder(previous))
        {
            if (entry.missingSince != 0)
            {
                if (Expired(entry.missingSince, now)) continue;
                entry.missingSince = std::min(entry.missingSince, now);
                entry.index = index;
                widget.missingItems.push_back(std::move(entry));
            }
            ++index;
        }
        changed |= before != widget.missingItems;
    }
    return changed;
}

// Only a complete successful desktop snapshot can establish a new absence.
// Incremental startup reads may recover observed members, but keep unobserved
// live ownership. Callers reject failed reads before reaching this transaction.
inline bool Reconcile(std::vector<DesktopWidget>& widgets,
    const std::vector<DesktopItem>& items, const std::vector<DockEntry>& dock,
    std::int64_t now, bool complete)
{
    KeySet available;
    for (const auto& item : items)
        if (!item.layoutKey.empty()) available.insert(item.layoutKey);
    KeySet claimed;
    for (const auto& widget : widgets)
        if (widget.type != DesktopWidgetType::FolderMapping)
            claimed.insert(widget.itemKeys.begin(), widget.itemKeys.end());
    for (const auto& entry : dock)
        if (entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop)
            claimed.insert(entry.reference);

    bool changed = false;
    for (auto& widget : widgets)
    {
        if (widget.type == DesktopWidgetType::FolderMapping) continue;
        const auto previousKeys = widget.itemKeys;
        const auto previousMissing = widget.missingItems;
        const auto order = LogicalOrder(widget);
        widget.itemKeys.clear();
        widget.missingItems.clear();
        size_t index = 0;
        for (auto entry : order)
        {
            const bool recovering = entry.missingSince != 0;
            if (recovering && (Expired(entry.missingSince, now) || claimed.contains(entry.key)))
                continue;
            if (available.contains(entry.key) || (!complete && !recovering))
            {
                claimed.insert(entry.key);
                widget.itemKeys.push_back(std::move(entry.key));
            }
            else
            {
                entry.index = index;
                entry.missingSince = recovering ? std::min(entry.missingSince, now) : now;
                widget.missingItems.push_back(std::move(entry));
            }
            ++index;
        }
        changed |= previousKeys != widget.itemKeys || previousMissing != widget.missingItems;
    }
    return changed;
}
}
