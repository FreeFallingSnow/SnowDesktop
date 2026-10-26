#pragma once

#include "dock_running_animation.h"

#include <string>
#include <initializer_list>
#include <utility>
#include <vector>

namespace snowdesktop::dock_entry_removal
{
enum class Group { Fixed, Folder };
inline constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

// A removed pin owns only a render snapshot. It never remains in the persisted
// model or participates in hit testing. Pending snapshots do not animate until
// the corresponding model entry actually disappears (Shell can cancel deletes).
template<class Snapshot>
class Timeline
{
public:
    struct Entry
    {
        std::wstring key;
        Group group{};
        std::vector<std::wstring> following;
        Snapshot snapshot;
        dock_running_animation::Presence presence;
        bool removed = false;
        bool presented = false;
        double durationScale = 1.0;
    };
    struct Cell
    {
        std::wstring key;
        std::size_t modelIndex = kNoIndex;
        std::size_t removalIndex = kNoIndex;
        float begin = 0.0f;
        float amount = 1.0f;
    };

    void Prepare(const std::vector<std::wstring>& keys, std::size_t index,
        Group group, Snapshot snapshot)
    {
        if (index >= keys.size()) return;
        Cancel(keys[index]);
        Entry entry;
        entry.key = keys[index];
        entry.group = group;
        entry.following.assign(keys.begin() + static_cast<std::ptrdiff_t>(index + 1), keys.end());
        entry.snapshot = std::move(snapshot);
        entry.presence.SetVisible(true, 0.0, false);
        entries_.push_back(std::move(entry));
    }

    void Cancel(const std::wstring& key)
    {
        std::erase_if(entries_, [&](const auto& entry) { return entry.key == key; });
    }

    void Reconcile(const std::vector<std::wstring>& keys, double now,
        bool animate, double durationScale = 1.0)
    {
        for (auto& entry : entries_)
        {
            const bool exists = std::find(keys.begin(), keys.end(), entry.key) != keys.end();
            if (!exists && !entry.removed)
            {
                entry.removed = true;
                entry.durationScale = durationScale;
            }
            if (entry.removed && !animate) entry.presence.SetVisible(false, now, false);
        }
        std::erase_if(entries_, [&](const auto& entry) {
            return entry.removed && (entry.presence.IsHidden() ||
                std::find(keys.begin(), keys.end(), entry.key) != keys.end());
        });
    }

    // Shell completion/rebuild can precede the next paint by an arbitrary
    // amount. Keep the full first frame until a renderer actually presents it.
    void Present(std::size_t index, double now)
    {
        if (index >= entries_.size()) return;
        auto& entry = entries_[index];
        if (!entry.removed || entry.presented) return;
        entry.presented = true;
        entry.presence.SetVisible(false, now, true, entry.durationScale);
    }

    bool Advance(double now, bool animate)
    {
        for (auto& entry : entries_)
        {
            if (!entry.removed) continue;
            if (animate && entry.presented) entry.presence.Advance(now);
            else if (animate) continue;
            else entry.presence.SetVisible(false, now, false);
        }
        std::erase_if(entries_, [](const auto& entry) {
            return entry.removed && entry.presence.IsHidden();
        });
        return IsAnimating();
    }

    bool IsAnimating() const
    {
        return std::any_of(entries_.begin(), entries_.end(), [](const auto& entry) {
            return entry.removed && (!entry.presented || entry.presence.IsAnimating());
        });
    }

    float Units(Group group) const
    {
        float units = 0.0f;
        for (const auto& entry : entries_)
            if (entry.removed && entry.group == group) units += entry.presence.Amount();
        return units;
    }

    std::vector<Cell> Layout(const std::vector<std::wstring>& keys,
        std::size_t first, std::size_t count, Group group) const
    {
        std::vector<Cell> cells;
        for (std::size_t i = first; i < keys.size() && i - first < count; ++i)
            cells.push_back({keys[i], i});
        // Insert against surviving identities, including other exiting pins.
        // This keeps adjacent/batched removals in their original visual order.
        for (std::size_t i = entries_.size(); i-- > 0;)
        {
            const auto& entry = entries_[i];
            if (!entry.removed || entry.group != group) continue;
            auto before = cells.end();
            for (const auto& key : entry.following)
            {
                before = std::find_if(cells.begin(), cells.end(),
                    [&](const auto& cell) { return cell.key == key; });
                if (before != cells.end()) break;
            }
            cells.insert(before, {entry.key, kNoIndex, i, 0.0f, entry.presence.Amount()});
        }
        float units = 0.0f;
        for (auto& cell : cells) { cell.begin = units; units += cell.amount; }
        return cells;
    }

    const std::vector<Entry>& Entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

// Group boundaries shrink with the last exiting icon, just like the running
// section. A full group retains the existing Dock spacing exactly.
inline float GapUnits(float preceding, float following)
{
    return std::min(dock_running_animation::GroupAmount(preceding),
        dock_running_animation::GroupAmount(following));
}

struct AxisGroup
{
    float begin = 0.0f;
    float gaps = 0.0f;
};
struct AxisLayout
{
    std::vector<AxisGroup> groups;
    float units = 0.0f;
    float gaps = 0.0f;
};
inline AxisLayout LayoutAxis(std::initializer_list<float> groups)
{
    AxisLayout result;
    float previous = 0.0f;
    for (const float units : groups)
    {
        if (units > 0.0f)
        {
            result.gaps += GapUnits(previous, units);
            previous = std::max(previous, units);
        }
        result.groups.push_back({result.units, result.gaps});
        result.units += units;
    }
    return result;
}

inline AxisLayout LayoutDockAxis(bool windows, float fixed, float running, float frequent, float trailing)
{
    auto result = LayoutAxis({fixed, running, frequent, trailing});
    // The existing start control keeps a full gap before a running entrance.
    // Pin-only groups can also be the last group and shrink that gap on exit.
    const float first = fixed > 0.0f ? fixed : running > 0.0f ? running : frequent > 0.0f ? frequent : trailing;
    const float leadingGap = windows ? (fixed <= 0.0f && running > 0.0f ? 1.0f :
        dock_running_animation::GroupAmount(first)) : 0.0f;
    for (auto& group : result.groups)
    {
        group.begin += static_cast<float>(windows);
        group.gaps += leadingGap;
    }
    result.groups.insert(result.groups.begin(), AxisGroup{});
    result.units += static_cast<float>(windows);
    result.gaps += leadingGap;
    return result;
}
} // namespace snowdesktop::dock_entry_removal
