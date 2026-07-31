#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Applies selection mutations consistently across every selectable surface.
 * The controller is data-shape generic so new slot-backed lists can join the
 * same selection lifecycle without depending on DesktopApp.
 */
class SelectionController
{
public:
    template <typename DesktopItems, typename DockEntries,
        typename RunningApps, typename Widgets>
    bool ClearAll(
        DesktopItems& desktopItems,
        DockEntries& dockEntries,
        RunningApps& runningApps,
        Widgets& widgets)
    {
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
};
