#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Applies selection mutations consistently across every selectable surface.
 * The controller is data-shape generic so new slot-backed lists can join the
 * same selection lifecycle without depending on DesktopApp.
 */
class SelectionController
{
public:
    // Targets borrow current model flags only for the duration of an operation.
    // The anchor retains identities, never pointers into rebuilt lists.
    struct Target
    {
        std::wstring key;
        bool* selected = nullptr;
    };

    void RememberAnchor(const std::wstring& scope, const std::wstring& key)
    {
        anchorScope_ = scope;
        anchorKey_ = key;
    }

    template <typename ClearSelection>
    bool SelectRange(const std::wstring& scope,
        const std::vector<Target>& targets, const std::wstring& key,
        bool additive, ClearSelection clearSelection)
    {
        const auto findKey = [&](const std::wstring& candidate) {
            return std::find_if(targets.begin(), targets.end(),
                [&](const Target& target) {
                    return target.selected && target.key == candidate;
                });
        };
        const auto end = findKey(key);
        if (end == targets.end()) return false;
        auto anchor = anchorScope_ == scope
            ? findKey(anchorKey_) : targets.end();
        if (anchor == targets.end())
            anchor = std::find_if(targets.begin(), targets.end(),
                [](const Target& target) {
                    return target.selected && *target.selected;
                });
        if (anchor == targets.end()) anchor = end;
        const std::wstring anchorKey = anchor->key;
        if (!additive) clearSelection();
        bool changed = false;
        const auto first = std::min(anchor, end);
        const auto last = std::max(anchor, end);
        for (auto target = first; target != last + 1; ++target)
            if (target->selected && !*target->selected)
            {
                *target->selected = true;
                changed = true;
            }
        RememberAnchor(scope, anchorKey);
        AdvanceRevisionIf(changed);
        return true;
    }

    bool SelectAll(const std::vector<Target>& targets)
    {
        bool changed = false;
        for (const auto& target : targets)
            if (target.selected && !*target.selected)
            {
                *target.selected = true;
                changed = true;
            }
        AdvanceRevisionIf(changed);
        return changed;
    }

    template <typename DesktopItems, typename DockEntries,
        typename RunningApps, typename Widgets>
    bool ClearAll(
        DesktopItems& desktopItems,
        DockEntries& dockEntries,
        RunningApps& runningApps,
        Widgets& widgets)
    {
        anchorScope_.clear();
        anchorKey_.clear();
        bool changed = ClearRange(desktopItems);
        changed = ClearRange(dockEntries) || changed;
        changed = ClearRange(runningApps) || changed;
        for (auto& widget : widgets)
        {
            changed = ClearValue(widget) || changed;
            changed = ClearRange(widget.folderEntries) || changed;
        }
        AdvanceRevisionIf(changed);
        return changed;
    }

    template <typename DesktopItems>
    bool SelectDesktop(DesktopItems& items, std::size_t index)
    {
        if (index >= items.size() || items[index].selected)
            return false;
        items[index].selected = true;
        AdvanceRevisionIf(true);
        return true;
    }

    template <typename DesktopItems>
    bool ToggleDesktop(DesktopItems& items, std::size_t index)
    {
        if (index >= items.size())
            return false;
        items[index].selected = !items[index].selected;
        AdvanceRevisionIf(true);
        return true;
    }

    template <typename Widgets>
    bool SelectWidget(Widgets& widgets, std::size_t index)
    {
        if (index >= widgets.size())
            return false;
        bool changed = false;
        for (std::size_t current = 0;
            current < widgets.size(); ++current)
        {
            const bool selected = current == index;
            if (widgets[current].selected != selected)
            {
                widgets[current].selected = selected;
                changed = true;
            }
            changed = ClearRange(
                widgets[current].folderEntries) || changed;
        }
        AdvanceRevisionIf(changed);
        return changed;
    }

    std::uint64_t Revision() const { return revision_; }

private:
    template <typename Value>
    static bool ClearValue(Value& value)
    {
        if (!value.selected)
            return false;
        value.selected = false;
        return true;
    }

    template <typename Range>
    static bool ClearRange(Range& range)
    {
        bool changed = false;
        for (auto& value : range)
            changed = ClearValue(value) || changed;
        return changed;
    }

    void AdvanceRevisionIf(bool changed)
    {
        if (!changed)
            return;
        ++revision_;
        if (revision_ == 0)
            revision_ = 1;
    }

    std::uint64_t revision_ = 0;
    std::wstring anchorScope_;
    std::wstring anchorKey_;
};
