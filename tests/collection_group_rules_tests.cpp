#include "widgets/collection_group_rules.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rules =
    snowdesktop::collection_group_rules;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestMarqueeUsesContentCoordinates()
{
    const rules::Rect itemInViewport{10, 20, 90, 60};
    for (const int scrollOffset :
        {1, 40, 100, 500})
    {
        const rules::Rect translated{
            itemInViewport.left,
            itemInViewport.top + scrollOffset,
            itemInViewport.right,
            itemInViewport.bottom + scrollOffset
        };
        Check(
            rules::MarqueeSelectsViewportItem(
                itemInViewport, scrollOffset,
                translated),
            "marquee selection must be invariant under content scrolling");
        Check(
            !rules::MarqueeSelectsViewportItem(
                itemInViewport, scrollOffset,
                {
                    translated.right,
                    translated.top,
                    translated.right + 1,
                    translated.bottom
                }),
            "touching a translated item edge must not select it");
    }
    Check(
        !rules::MarqueeSelectsViewportItem(
            {0, 0, 10, 10}, 0,
            {10, 0, 20, 10}),
        "touching edges must not select an item");
}

void TestViewportClipping()
{
    auto partial = rules::ClipToViewport(
        {-20, 4, 60, 28},
        {0, 0, 100, 30});
    Check(partial.has_value(),
        "a partially visible tab must remain interactive");
    if (partial)
    {
        Check(
            partial->left >= 0 &&
            partial->top >= 0 &&
            partial->right <= 100 &&
            partial->bottom <= 30 &&
            partial->right > partial->left &&
            partial->bottom > partial->top,
            "clipped hit bounds must be a non-empty subset of the viewport");
    }
    Check(
        !rules::ClipToViewport(
            {-30, 4, 0, 28},
            {0, 0, 100, 30}),
        "an off-screen tab must not have a hit target");
    Check(
        !rules::ClipToViewport(
            {100, 4, 120, 28},
            {0, 0, 100, 30}),
        "touching the viewport edge must not create a hit target");
}

void TestActiveItemFallback()
{
    const std::vector<std::string> valid{
        "first", "second"
    };
    Check(
        rules::ResolveActiveItem(
            valid, std::string("second")) == "second",
        "a valid active tab must be preserved");
    Check(
        rules::ResolveActiveItem(
            valid, std::string("missing")) == "first",
        "a stale active tab must fall back to the first tab");
    Check(
        rules::ResolveActiveItem(
            std::vector<std::string>{},
            std::string("missing")).empty(),
        "an empty group must not expose an active tab");
}

void TestTabWidthDistribution()
{
    for (const int available :
        {241, 245, 300})
    {
        const std::vector<int> measured{
            80, 80, 80
        };
        const auto distributed =
            rules::DistributeWidthsToFill(
                measured, available);
        int total = 0;
        for (size_t i = 0;
            i < distributed.size(); ++i)
        {
            total += distributed[i];
            Check(distributed[i] >= measured[i],
                "filling a tab row must not shrink measured labels");
        }
        Check(total == available,
            "distributed tabs must fill the available row");
        const auto [minimum, maximum] =
            std::minmax_element(
                distributed.begin(),
                distributed.end());
        Check(maximum != distributed.end() &&
                minimum != distributed.end() &&
                *maximum - *minimum <= 1,
            "equal labels must share spare width fairly");
    }
    Check(
        rules::DistributeWidthsToFill(
            {120, 120}, 200) ==
            std::vector<int>({120, 120}),
        "overflowing tabs must retain their measured widths");
    Check(
        rules::DistributeWidthsToFill(
            {}, 200).empty(),
        "an empty tab strip must remain empty");
}

void TestStableReorder()
{
    const std::vector<std::string> source{
        "A", "B", "C", "D"
    };
    Check(
        rules::ReorderItems(
            source, {1}, 4) ==
            std::vector<std::string>({
                "A", "C", "D", "B"
            }),
        "moving a tab to the end must preserve order");
    Check(
        rules::ReorderItems(
            source, {3, 1, 1}, 0) ==
            std::vector<std::string>({
                "B", "D", "A", "C"
            }),
        "unsorted duplicate indices must be normalized");
    Check(
        rules::ReorderItems(
            source, {1, 2}, 3) == source,
        "dropping a selected range on its own boundary must be stable");
    Check(
        rules::ReorderItems(
            source, {99}, 0) == source,
        "invalid indices must not mutate the list");
}

void TestCollectionLabelTargetMatrix()
{
    using Surface =
        rules::CollectionLabelDropSurface;
    Check(
        rules::AcceptsCollectionLabelDrop(
            Surface::Desktop),
        "desktop must accept a collection label");
    Check(
        rules::AcceptsCollectionLabelDrop(
            Surface::CollectionGroup),
        "collection group must accept a collection label");
    Check(
        !rules::AcceptsCollectionLabelDrop(
            Surface::Other),
        "file and generic item containers must reject a collection label");

    Check(
        rules::AcceptsGroupedDrag(
            rules::GroupedDragKind::FileGroupLabel,
            Surface::Desktop),
        "desktop must accept a file-group source label");
    Check(
        rules::AcceptsGroupedDrag(
            rules::GroupedDragKind::FileGroupLabel,
            Surface::FileGroup),
        "file group must accept its dedicated label drag type");
    Check(
        !rules::AcceptsGroupedDrag(
            rules::GroupedDragKind::FileGroupLabel,
            Surface::FileList),
        "a source label must not use file-entry drop targets");
    Check(
        !rules::AcceptsGroupedDrag(
            rules::GroupedDragKind::FileEntry,
            Surface::FileGroup),
        "a file entry must not use source-label placement");
}

void TestFileGroupRules()
{
    using Kind = rules::FileGroupChildKind;
    Check(
        rules::ShouldOccupyDesktopGrid(false) &&
        !rules::ShouldOccupyDesktopGrid(true),
        "a grouped child must not leave ghost desktop occupancy");

    Check(
        rules::AcceptsFileGroupChild(
            Kind::DesktopFileCategories),
        "desktop file categories must be accepted");
    Check(
        rules::AcceptsFileGroupChild(
            Kind::FolderMapping),
        "folder mappings must be accepted");
    Check(
        !rules::AcceptsFileGroupChild(
            Kind::FileGroup) &&
        !rules::AcceptsFileGroupChild(
            Kind::Collection),
        "nested or unrelated groups must be rejected");

    std::vector<std::string> claimed;
    const auto first =
        rules::ClaimUniqueAllowedItems(
            std::vector<std::string>{
                "desktop", "mapping", "desktop"
            },
            claimed,
            [](const std::string&) { return true; });
    const auto second =
        rules::ClaimUniqueAllowedItems(
            std::vector<std::string>{
                "mapping", "other"
            },
            claimed,
            [](const std::string& value) {
                return value != "other";
            });
    Check(
        first == std::vector<std::string>({
            "desktop", "mapping"
        }) && second.empty(),
        "a child must have one owner and duplicates must be removed");

    Check(
        rules::ShouldShowInnerCategoryTabs(
            true, false, true),
        "category row must be visible when enabled");
    Check(
        !rules::ShouldShowInnerCategoryTabs(
            true, true, false),
        "active search must hide the category row");
    Check(
        rules::ShouldShowInnerCategoryTabs(
            true, true, true),
        "clearing search must restore the category row");
    Check(
        !rules::ShouldShowFileGroupSourceTabs(
            true, false) &&
        !rules::ShouldShowInnerCategoryTabs(
            true, true, false),
        "file-group search results must hide both tab rows");
    Check(
        rules::ShouldShowFileGroupSourceTabs(
            true, true),
        "clearing file-group search must restore the source row");

    Check(
        rules::ClampIndependentTabScroll(
            90, 260, 100) == 90 &&
        rules::ClampIndependentTabScroll(
            90, 140, 100) == 40,
        "each tab row must clamp its own scroll offset");

    Check(
        rules::SelectDragSource(true, true) ==
            rules::DragSourceSelection::Captured &&
        rules::SelectDragSource(true, false) ==
            rules::DragSourceSelection::Rebuild,
        "hover-switching target tabs must retain the "
        "captured source for the same drag session");

    struct Child
    {
        std::string id;
        int columns;
        int rows;
        bool operator==(const Child&) const = default;
    };
    std::vector<Child> grouped{
        {"desktop", 2, 3},
        {"mapping", 4, 2}
    };
    std::string active = "mapping";
    const auto released =
        rules::TakeAllForRelease(grouped, active);
    Check(
        grouped.empty() && active.empty() &&
        released == std::vector<Child>({
            {"desktop", 2, 3},
            {"mapping", 4, 2}
        }),
        "deleting a file group must release every child "
        "with its original span");
}

void TestGridPlacementInvariants()
{
    std::set<std::pair<int, int>> occupied;
    auto areaOccupied =
        [&](const rules::Placement& placement) {
            for (int column = placement.column;
                column < placement.column +
                    placement.span.columns;
                ++column)
                for (int row = placement.row;
                    row < placement.row +
                        placement.span.rows;
                    ++row)
                    if (occupied.contains(
                            {column, row}))
                        return true;
            return false;
        };
    auto markArea =
        [&](const rules::Placement& placement) {
            for (int column = placement.column;
                column < placement.column +
                    placement.span.columns;
                ++column)
                for (int row = placement.row;
                    row < placement.row +
                        placement.span.rows;
                    ++row)
                    occupied.insert({column, row});
        };

    auto clamped =
        rules::PlanExactPlacements(
            6, 4, 5, 3, {{3, 2}},
            areaOccupied, markArea);
    Check(clamped && clamped->size() == 1,
        "free placement must succeed");
    if (clamped && !clamped->empty())
    {
        const auto& placement = (*clamped)[0];
        Check(
            placement.column >= 0 &&
            placement.row >= 0 &&
            placement.column +
                placement.span.columns <= 6 &&
            placement.row +
                placement.span.rows <= 4 &&
            placement.span.columns == 3 &&
            placement.span.rows == 2,
            "placement must preserve the full span inside page bounds");
    }

    occupied.clear();
    occupied.insert({2, 1});
    auto blocked =
        rules::PlanExactPlacements(
            6, 4, 1, 0, {{2, 2}},
            areaOccupied, markArea);
    Check(!blocked,
        "any occupied cell inside the full span must block placement");

    occupied.clear();
    auto wrapped =
        rules::PlanExactPlacements(
            6, 4, 4, 0,
            {{2, 1}, {2, 1}},
            areaOccupied, markArea);
    Check(wrapped && wrapped->size() == 2,
        "multiple selected labels must receive placements");
    if (wrapped && wrapped->size() == 2)
    {
        const auto& first = (*wrapped)[0];
        const auto& second = (*wrapped)[1];
        const bool overlap =
            first.column <
                second.column +
                    second.span.columns &&
            second.column <
                first.column +
                    first.span.columns &&
            first.row <
                second.row +
                    second.span.rows &&
            second.row <
                first.row +
                    first.span.rows;
        Check(
            !overlap &&
            first.span.columns == 2 &&
            first.span.rows == 1 &&
            second.span.columns == 2 &&
            second.span.rows == 1,
            "multiple placements must preserve spans without overlap");
    }
}
}

int main()
{
    TestMarqueeUsesContentCoordinates();
    TestViewportClipping();
    TestActiveItemFallback();
    TestTabWidthDistribution();
    TestStableReorder();
    TestCollectionLabelTargetMatrix();
    TestFileGroupRules();
    TestGridPlacementInvariants();
    if (failures != 0)
    {
        std::cerr << failures
            << " collection-group rule test(s) failed\n";
        return 1;
    }
    std::cout
        << "All collection-group rule tests passed\n";
    return 0;
}
