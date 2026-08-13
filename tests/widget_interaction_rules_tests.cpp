#include "widgets/collection_group_rules.h"
#include "desktop_hover_rules.h"
#include "widget_scroll_rules.h"
#include "widget_visibility_rules.h"
#include "widgets/widget_chrome_rules.h"
#include "widgets/guide_widget_rules.h"
#include "pending_drop_rules.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rules =
    snowdesktop::collection_group_rules;
namespace visibilityRules =
    snowdesktop::widget_visibility_rules;
namespace hoverRules =
    snowdesktop::desktop_hover_rules;
namespace chromeRules =
    snowdesktop::widget_chrome_rules;
namespace guideRules =
    snowdesktop::guide_widget_rules;

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
    constexpr size_t maxLength = 7;
    for (size_t length = 0;
        length <= maxLength; ++length)
    {
        std::vector<size_t> source(length);
        for (size_t index = 0;
            index < length; ++index)
            source[index] = index;

        const size_t selectionCount =
            size_t{1} << length;
        for (size_t mask = 0;
            mask < selectionCount; ++mask)
        {
            std::vector<size_t> selected;
            for (size_t index = length;
                index > 0; --index)
            {
                const size_t sourceIndex = index - 1;
                if ((mask &
                        (size_t{1} << sourceIndex)) == 0)
                    continue;
                selected.push_back(sourceIndex);
                selected.push_back(sourceIndex);
            }
            selected.push_back(length + 7);

            for (size_t insertBefore = 0;
                insertBefore <= length + 2;
                ++insertBefore)
            {
                const size_t boundary =
                    std::min(insertBefore, length);
                std::vector<size_t> expected;
                expected.reserve(length);
                for (size_t index = 0;
                    index < boundary; ++index)
                {
                    if ((mask &
                            (size_t{1} << index)) == 0)
                        expected.push_back(index);
                }
                for (size_t index = 0;
                    index < length; ++index)
                {
                    if ((mask &
                            (size_t{1} << index)) != 0)
                        expected.push_back(index);
                }
                for (size_t index = boundary;
                    index < length; ++index)
                {
                    if ((mask &
                            (size_t{1} << index)) == 0)
                        expected.push_back(index);
                }

                Check(
                    rules::ReorderItems(
                        source, selected,
                        insertBefore) == expected,
                    "every selection and insertion boundary must preserve stable reorder semantics");
            }
        }
    }
}

void TestPendingFilePlacementReconciliation()
{
    namespace pendingRules =
        snowdesktop::pending_drop_rules;

    std::vector<std::string> folderEntries{
        "old-a", "new-b", "old-b", "new-a"
    };
    std::vector<std::string> inserted =
        pendingRules::ExtractMatching(
            folderEntries,
            [](const std::string& value) {
                return value.starts_with("new-");
            });
    Check(
        folderEntries == std::vector<std::string>({
            "old-a", "old-b"
        }) &&
        inserted == std::vector<std::string>({
            "new-b", "new-a"
        }),
        "folder reconciliation must isolate newly enumerated members");
    pendingRules::InsertAt(
        folderEntries, 1, std::move(inserted));
    Check(
        folderEntries == std::vector<std::string>({
            "old-a", "new-b", "new-a", "old-b"
        }),
        "new folder members must be restored at the preview boundary");

    std::vector<std::string> autoCollected{
        "old-a", "old-b", "new-a", "new-b"
    };
    std::vector<std::string> first =
        pendingRules::ExtractMatching(
            autoCollected,
            [](const std::string& value) {
                return value == "new-a";
            });
    pendingRules::InsertAt(
        autoCollected, 1, std::move(first));
    std::vector<std::string> second =
        pendingRules::ExtractMatching(
            autoCollected,
            [](const std::string& value) {
                return value == "new-b";
            });
    pendingRules::InsertAt(
        autoCollected, 2, std::move(second));
    Check(
        autoCollected == std::vector<std::string>({
            "old-a", "new-a", "new-b", "old-b"
        }),
        "multiple pending members must preserve their landing order");
}

void TestBottomBarWidthFollowsCornerAndHeight()
{
    Check(
        chromeRules::BottomBarSideInset(12, 24, 4, 2) == 4,
        "default bottom-bar geometry must retain its established width");
    Check(
        chromeRules::BottomBarSideInset(28, 16, 4, 2) == 13,
        "large corners and a short bar must narrow the bottom-bar width");
    Check(
        chromeRules::BottomBarSideInset(28, 24, 4, 2) == 11,
        "a taller bar must recover width that remains inside the corner");
    Check(
        chromeRules::BottomBarSideInset(28, 48, 4, 2) == 7,
        "the maximum bar height must still retain rounded-corner clearance");
    Check(
        chromeRules::BottomBarSideInset(56, 32, 8, 4) == 26,
        "bottom-bar side insets must scale with widget cell size");
}

void TestGuidePlaceholderLifecycle()
{
    Check(
        !guideRules::ShouldRemove(false, false),
        "a guide must remain while it is the page's only visible content");
    Check(
        guideRules::ShouldRemove(true, false),
        "a visible desktop item must replace the guide placeholder");
    Check(
        guideRules::ShouldRemove(false, true),
        "a standalone widget must replace the guide placeholder");
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

    std::vector<std::string> retainedKeys{
        "shared"
    };
    const auto releasedKeys =
        rules::ClaimUniqueAllowedItems(
            std::vector<std::string>{
                "first", "shared", "first", "second"
            },
            retainedKeys,
            [](const std::string& value) {
                return !value.empty();
            });
    Check(
        releasedKeys ==
            std::vector<std::string>({
                "first", "second"
            }),
        "deleting an item-owning widget must release each "
        "unique item except keys retained by another owner");

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
        rules::ResolveCategorizedContentTop(
            108, 500,
            false, 0,
            true, 142,
            1, 38,
            8, 4) == 184,
        "hidden inner categories must reserve the search box "
        "and the file-group source row");
    Check(
        rules::ResolveCategorizedContentTop(
            108, 500,
            false, 180,
            true, 142,
            0, 38,
            8, 4) == 146,
        "active search must skip the category row");
    Check(
        rules::ResolveCategorizedContentTop(
            108, 500,
            true, 180,
            true, 142,
            1, 38,
            8, 4) == 188,
        "visible inner categories already include all preceding rows");

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

void TestHoverOnlyWidgetVisibility()
{
    Check(
        visibilityRules::ShouldRenderWidget(
            false, false, false, false, false, false, false),
        "regular widget remains visible while idle");
    Check(
        !visibilityRules::ShouldRenderWidget(
            true, false, false, false, false, false, false),
        "hover-only widget remains hidden while idle");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, true, false, false, false, false, false),
        "item drag reveals hover-only widget");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, true, false, false, false),
        "widget move reveals hover-only widget");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, false, true, false, false),
        "selected hover-only widget remains visible");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, false, false, true, false),
        "retained inner rename reveals hover-only widget");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, false, false, false, true),
        "pointer hover reveals hover-only widget");
}

void TestDesktopHoverDeactivation()
{
    using hoverRules::ReconcileMode;
    Check(
        hoverRules::HasForegroundSettled(false, 0) &&
            !hoverRules::HasForegroundSettled(
                true, hoverRules::kActivationSettleMs - 1) &&
            hoverRules::HasForegroundSettled(
                true, hoverRules::kActivationSettleMs),
        "foreground settling must allow startup and enforce the activation delay boundary");
    Check(
        !hoverRules::OwnsInteractionCapture(0, 1, 0),
        "two null window handles must not imply owned capture");
    Check(
        hoverRules::OwnsInteractionCapture(1, 1, 0),
        "desktop capture belongs to the interaction surface");
    Check(
        hoverRules::OwnsInteractionCapture(2, 1, 2),
        "floating Dock capture belongs to the interaction surface");
    Check(
        hoverRules::CanClearPassiveHover(
            false, false, false, false),
        "idle passive hover can be cleared immediately");
    Check(
        !hoverRules::CanClearPassiveHover(
            true, false, false, false),
        "capture-based pointer interaction must survive foreground changes");
    Check(
        !hoverRules::CanClearPassiveHover(
            false, true, false, false),
        "a pressed pointer must survive foreground changes");
    Check(
        !hoverRules::CanClearPassiveHover(
            false, false, true, false),
        "an active drag must survive foreground changes");
    Check(
        !hoverRules::CanClearPassiveHover(
            false, false, false, true),
        "widget move or resize must survive foreground changes");
    Check(
        hoverRules::ShouldPresentSynchronously(
            true, false),
        "a desktop hover target transition must present in its pointer message");
    Check(
        hoverRules::ShouldPresentSynchronously(
            false, true),
        "a continuous Dock pointer surface must present movement synchronously");
    Check(
        !hoverRules::ShouldPresentSynchronously(
            false, false),
        "unchanged passive hover must not force an extra desktop frame");
    Check(
        hoverRules::ShouldBeginShortcutLaunchSuppression(
            true, true) &&
            !hoverRules::ShouldBeginShortcutLaunchSuppression(
                false, true) &&
            !hoverRules::ShouldBeginShortcutLaunchSuppression(
                true, false),
        "only a shortcut launched from a hover-only widget hides before entering Shell");
    Check(
        hoverRules::ShouldKeepShortcutLaunchSuppression(
            true, true, true) &&
            !hoverRules::ShouldKeepShortcutLaunchSuppression(
                true, false, true) &&
            !hoverRules::ShouldKeepShortcutLaunchSuppression(
                true, true, false),
        "shortcut launch suppression ends after the pointer leaves the source or the desktop surface");
    Check(
        !hoverRules::ShouldActivateFromSurfaceSample(
            true, true, ReconcileMode::DeactivateOnly, true),
        "a transient desktop hit after a foreground change must not reactivate hover");
    Check(
        hoverRules::ShouldActivateFromSurfaceSample(
            true, true, ReconcileMode::AllowImmediateActivation, false),
        "an explicit desktop restoration may activate hover without waiting for foreground settling");
    Check(
        !hoverRules::ShouldActivateFromSurfaceSample(
            true, true,
            ReconcileMode::AllowActivationAfterForegroundSettle, false) &&
            hoverRules::ShouldActivateFromSurfaceSample(
                true, true,
                ReconcileMode::AllowActivationAfterForegroundSettle, true),
        "a periodic desktop sample may activate hover only after the foreground transition settles");
    Check(
        !hoverRules::ShouldActivateFromSurfaceSample(
            false, true, ReconcileMode::AllowImmediateActivation, true) &&
            !hoverRules::ShouldActivateFromSurfaceSample(
                true, false, ReconcileMode::AllowImmediateActivation, true),
        "hover restoration requires both a desktop surface and a cleared state");
}

void TestNestedWidgetScrolling()
{
    using snowdesktop::widget_scroll_rules::
        ApplyWheelDelta;
    const auto innerBoundary =
        ApplyWheelDelta(0, 0, 120);
    Check(!innerBoundary.moved &&
            innerBoundary.offset == 0,
        "wheel at a nested scroll boundary can bubble");
    const auto outerScroll =
        ApplyWheelDelta(48, 240, 120);
    Check(outerScroll.moved &&
            outerScroll.offset == 0,
        "wheel moves the first enclosing scroll area that can move");
    const auto lowerBoundary =
        ApplyWheelDelta(240, 240, -120);
    Check(!lowerBoundary.moved &&
            lowerBoundary.offset == 240,
        "wheel at the lower boundary can bubble");
    const auto precisionWheel =
        ApplyWheelDelta(20, 240, 15);
    Check(precisionWheel.moved &&
            precisionWheel.offset < 20,
        "precision touchpad wheel deltas still scroll");
}
}

int main()
{
    TestMarqueeUsesContentCoordinates();
    TestViewportClipping();
    TestActiveItemFallback();
    TestTabWidthDistribution();
    TestBottomBarWidthFollowsCornerAndHeight();
    TestGuidePlaceholderLifecycle();
    TestStableReorder();
    TestPendingFilePlacementReconciliation();
    TestFileGroupRules();
    TestGridPlacementInvariants();
    TestHoverOnlyWidgetVisibility();
    TestDesktopHoverDeactivation();
    TestNestedWidgetScrolling();
    if (failures != 0)
    {
        std::cerr << failures
            << " widget interaction rule test(s) failed\n";
        return 1;
    }
    std::cout
        << "All widget interaction rule tests passed\n";
    return 0;
}
