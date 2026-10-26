#pragma once

#include "dock/dock_entry_removal.h"
#include <memory>

namespace snowdesktop::tests::dock_entry_removal
{
template<class Check>
void Run(Check check)
{
    namespace removal = snowdesktop::dock_entry_removal;
    using Timeline = removal::Timeline<std::shared_ptr<int>>;
    using Group = removal::Group;
    const std::vector<std::wstring> original{L"a", L"b", L"c", L"folder"};
    Timeline timeline;
    auto snapshot = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = snapshot;
    timeline.Prepare(original, 1, Group::Fixed, std::move(snapshot));
    timeline.Reconcile(original, 100.0, true);
    check(!timeline.IsAnimating() && timeline.Units(Group::Fixed) == 0.0f,
        "a prepared/cancelled Shell delete does not hide or reserve a second pin");

    const std::vector<std::wstring> remaining{L"a", L"c", L"folder"};
    timeline.Reconcile(remaining, 200.0, true);
    auto layout = timeline.Layout(remaining, 0, 2, Group::Fixed);
    check(remaining.size() == 3 && layout.size() == 3 && layout[1].key == L"b" &&
        layout[1].modelIndex == removal::kNoIndex && layout[1].removalIndex != removal::kNoIndex &&
        layout[2].begin == 2.0f && !lifetime.expired(),
        "removing a mapping commits its model immediately but retains a noninteractive snapshot in its old slot");
    timeline.Advance(1000.0, true);
    check(timeline.IsAnimating() && timeline.Units(Group::Fixed) == 1.0f && !lifetime.expired(),
        "a delayed first paint cannot consume the exit while Shell completion is still blocking presentation");
    timeline.Present(layout[1].removalIndex, 1000.0);
    timeline.Advance(1110.0, true);
    layout = timeline.Layout(remaining, 0, 2, Group::Fixed);
    check(timeline.Units(Group::Fixed) == 0.5f && layout[2].begin == 1.5f &&
        removal::GapUnits(timeline.Units(Group::Fixed), 1.0f) == 0.5f,
        "pin size, following position and last-group separator shrink on the running-area exit timeline");
    timeline.Advance(1220.0, true);
    check(!timeline.IsAnimating() && lifetime.expired() &&
        timeline.Layout(remaining, 0, 2, Group::Fixed).size() == 2,
        "completed exits release snapshots and leave only live model slots");

    timeline.Prepare(original, 1, Group::Fixed, std::make_shared<int>(1));
    timeline.Prepare(original, 2, Group::Fixed, std::make_shared<int>(2));
    const std::vector<std::wstring> batchRemaining{L"a", L"folder"};
    timeline.Reconcile(batchRemaining, 500.0, true);
    layout = timeline.Layout(batchRemaining, 0, 1, Group::Fixed);
    check(layout.size() == 3 && layout[1].key == L"b" && layout[2].key == L"c",
        "adjacent removals keep their independent snapshots in original order");
    timeline.Reconcile({L"a", L"b", L"folder"}, 510.0, true);
    layout = timeline.Layout({L"a", L"b", L"folder"}, 0, 2, Group::Fixed);
    check(layout.size() == 3 && layout[1].modelIndex == 1 && layout[2].key == L"c",
        "readding a pin cancels its outgoing duplicate without cancelling its neighbour");
    timeline.Advance(511.0, false);
    check(timeline.Entries().empty(), "disabling animations retires outgoing snapshots immediately");

    timeline.Prepare(original, 3, Group::Folder, std::make_shared<int>(3));
    timeline.Reconcile({L"a", L"b", L"c"}, 600.0, false);
    check(timeline.Entries().empty(), "reduced motion removes mappings without a retained exit frame");
    timeline.Prepare(original, 3, Group::Folder, std::make_shared<int>(3));
    timeline.Reconcile({L"a", L"b", L"c"}, 700.0, true, 2.0);
    timeline.Present(0, 700.0);
    timeline.Advance(920.0, true);
    check(timeline.Units(Group::Folder) == 0.5f && timeline.Units(Group::Fixed) == 0.0f &&
        timeline.Layout({L"a", L"b", L"c"}, 3, 0, Group::Folder).size() == 1,
        "the last file-area icon keeps its own shrinking slot and follows the duration preference");
    timeline.Cancel(L"folder");
    check(timeline.Entries().empty(), "explicit cancellation releases a prepared snapshot");
    const auto axis = removal::LayoutAxis({1.0f, 0.5f, 0.0f, 2.0f, 1.0f});
    check(axis.units == 4.5f && axis.gaps == 2.5f &&
        axis.groups[3].begin == 1.5f && axis.groups[3].gaps == 1.5f,
        "weighted pin groups keep start, running/frequent and file controls on one continuous axis");
}
} // namespace snowdesktop::tests::dock_entry_removal
