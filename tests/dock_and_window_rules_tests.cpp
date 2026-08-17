#include "dock_magnification.h"
#include "dock_launch_animation.h"
#include "dock_rename_layout.h"
#include "dock_drop_rules.h"
#include "dock_folder_rules.h"
#include "dock_collection_icon_rules.h"
#include "collection_popup_layout.h"
#include "folder_sort_rules.h"
#include "shell_item_visibility.h"
#include "popup_drag_rules.h"
#include "item_layout_rules.h"
#include "item_render_layer_rules.h"
#include "dock_window_rules.h"
#include "dock_window_preview.h"
#include "dock_window_transition.h"
#include "dock_settings_rules.h"
#include "desktop_item_reference_migration.h"
#include "app/desktop_backdrop_update_rules.h"
#include "app/native_menu_presentation_rules.h"
#include "desktop_window_discovery_rules.h"
#include "floating_dock_rules.h"
#include "ole_drag_rules.h"
#include "display_topology_refresh.h"
#include "item_visual_metrics.h"
#include "collection_titleless_rules.h"
#include "layout_spacing_rules.h"
#include "grid_spacing_rules.h"
#include "widget_item_layout.h"
#include "app/grid_geometry.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <climits>
#include <iostream>

namespace rules = snowdesktop::dock_window_rules;

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void CheckRowMargins(
    const DockWindowPreviewGrid& grid,
    const std::vector<RECT>& cards,
    size_t rowStart,
    size_t rowCount,
    const char* message)
{
    if (rowCount == 0 || rowStart + rowCount > cards.size())
    {
        Check(false, message);
        return;
    }
    const int leftMargin = cards[rowStart].left;
    const int rightMargin =
        grid.panelWidth - cards[rowStart + rowCount - 1].right;
    Check(std::abs(leftMargin - rightMargin) <= 1, message);
}

} // namespace

int main()
{
    namespace dockDrop =
        snowdesktop::dock_drop_rules;
    namespace floatingDock =
        snowdesktop::floating_dock_rules;
    namespace folderRules =
        snowdesktop::dock_folder_rules;
    namespace folderSort =
        snowdesktop::folder_sort_rules;
    namespace popupLayout =
        snowdesktop::collection_popup_layout;
    namespace shellVisibility =
        snowdesktop::shell_item_visibility;
    namespace popupDrag =
        snowdesktop::popup_drag_rules;
    namespace itemLayout =
        snowdesktop::item_layout_rules;
    namespace displayRefresh =
        snowdesktop::display_topology_refresh;
    namespace backdropUpdate =
        snowdesktop::desktop_backdrop_update_rules;
    namespace nativeMenuPresentation =
        snowdesktop::native_menu_presentation_rules;
    namespace desktopWindowDiscovery =
        snowdesktop::desktop_window_discovery_rules;
    namespace oleDrag =
        snowdesktop::ole_drag_rules;

    Check(
        desktopWindowDiscovery::IsExplorerDesktopViewProcess(
            4120, 4120),
        "desktop discovery must accept Explorer-owned DefView windows");
    Check(
        !desktopWindowDiscovery::IsExplorerDesktopViewProcess(
            7280, 4120) &&
        !desktopWindowDiscovery::IsExplorerDesktopViewProcess(
            0, 4120) &&
        !desktopWindowDiscovery::IsExplorerDesktopViewProcess(
            4120, 0),
        "desktop discovery must reject transient in-process Shell views");

    Check(
        !oleDrag::IsExternalDropSurface(
            true, true, true) &&
        !oleDrag::IsExternalDropSurface(
            false, false, true) &&
        !oleDrag::IsExternalDropSurface(
            true, false, false) &&
        oleDrag::IsExternalDropSurface(
            true, false, true),
        "OLE drag routing must keep fixed and floating Dock surfaces internal");
    Check(
        oleDrag::SelectDwellTargetRefreshRoute(
            false, false) ==
            oleDrag::DwellTargetRefreshRoute::NativePointer &&
        oleDrag::SelectDwellTargetRefreshRoute(
            true, false) ==
            oleDrag::DwellTargetRefreshRoute::SelfOleDragOver &&
        oleDrag::SelectDwellTargetRefreshRoute(
            false, true) ==
            oleDrag::DwellTargetRefreshRoute::AwaitExternalOleCallback,
        "Dock dwell must not re-enter native drag handling from an OLE loop");

    Check(
        nativeMenuPresentation::ShouldFlushAfterOwnerMessage(
            true, false, false, false),
        "native Shell menu messages must flush pending composition commits");
    Check(
        !nativeMenuPresentation::ShouldFlushAfterOwnerMessage(
            false, false, false, false) &&
        !nativeMenuPresentation::ShouldFlushAfterOwnerMessage(
            true, true, false, false) &&
        !nativeMenuPresentation::ShouldFlushAfterOwnerMessage(
            true, false, true, false) &&
        !nativeMenuPresentation::ShouldFlushAfterOwnerMessage(
            true, false, false, true),
        "native Shell presentation must wait until every active surface exits BeginDraw");

    const RECT backdropClientRect{0, 0, 1920, 1080};
    const RECT fullBackdropUpdate{0, 0, 1920, 1080};
    const RECT oversizedBackdropUpdate{-20, -20, 1940, 1100};
    const RECT partialBackdropUpdate{0, 0, 1920, 400};
    Check(
        backdropUpdate::ShouldCollectAllPanels(
            false, false, false, false, nullptr, backdropClientRect),
        "an unbounded paint reconciles every backdrop panel");
    Check(
        backdropUpdate::ShouldCollectAllPanels(
            false, false, false, false,
            &fullBackdropUpdate, backdropClientRect),
        "a full WM_PAINT update reconciles every backdrop panel");
    Check(
        backdropUpdate::ShouldCollectAllPanels(
            false, false, false, false, &oversizedBackdropUpdate,
            backdropClientRect),
        "an update covering the client area reconciles backdrop panels");
    Check(
        !backdropUpdate::ShouldCollectAllPanels(
            false, false, false, false, &partialBackdropUpdate,
            backdropClientRect),
        "a partial paint preserves backdrop panels outside the dirty area");
    Check(
        !backdropUpdate::ShouldCollectAllPanels(
            false, true, false, false, &fullBackdropUpdate,
            backdropClientRect) &&
        !backdropUpdate::ShouldCollectAllPanels(
            false, false, true, false, &fullBackdropUpdate,
            backdropClientRect) &&
        !backdropUpdate::ShouldCollectAllPanels(
            false, false, false, true, &fullBackdropUpdate,
            backdropClientRect),
        "interactive preview paints preserve retained backdrop panels");
    Check(
        backdropUpdate::ShouldCollectAllPanels(
            true, true, false, false, &partialBackdropUpdate,
            backdropClientRect) &&
        backdropUpdate::ShouldCollectAllPanels(
            true, false, true, false, &partialBackdropUpdate,
            backdropClientRect),
        "an invalidated drag scene must fully reconcile backdrop panels");

    Check(
        displayRefresh::ResolveAction(false, false, false) ==
            displayRefresh::Action::None,
        "stable display topology must not cause redundant window work");
    Check(
        displayRefresh::ResolveAction(true, false, false) ==
            displayRefresh::Action::ApplyTopology &&
        displayRefresh::ResolveAction(true, true, true) ==
            displayRefresh::Action::ApplyTopology,
        "a changed display topology must rebuild layout before any settle pass");
    Check(
        displayRefresh::ResolveAction(false, true, false) ==
            displayRefresh::Action::ResynchronizeWindow,
        "an unchanged signature must retain Explorer's deferred window "
        "synchronization pass");
    Check(
        displayRefresh::ResolveAction(false, false, true) ==
            displayRefresh::Action::ResynchronizeWindow,
        "a stale desktop window bounds must recover even when the monitor "
        "signature is unchanged");
    Check(
        displayRefresh::ExtendsBeyond(
            { 0, 0, 2560, 1600 }, { 0, 0, 4480, 1600 }) &&
        displayRefresh::ExtendsBeyond(
            { 0, 0, 2560, 1600 }, { -1920, 0, 2560, 1600 }) &&
        !displayRefresh::ExtendsBeyond(
            { 0, 0, 4480, 1600 }, { 0, 0, 2560, 1600 }) &&
        !displayRefresh::ExtendsBeyond(
            { -1920, 0, 2560, 1600 }, { -1280, 0, 2560, 1440 }),
        "only virtual desktops extending beyond the old layered allocation "
        "must recreate the overlay");

    Check(
        itemLayout::ShouldRelayoutDesktopWidget(
            false, false),
        "standalone desktop widgets must participate in grid relayout");
    Check(
        !itemLayout::ShouldRelayoutDesktopWidget(
            true, false),
        "grouped widgets must remain owned by their host during grid relayout");
    Check(
        !itemLayout::ShouldRelayoutDesktopWidget(
            false, true),
        "Dock-exclusive widgets must not be displaced back onto the desktop");

    Check(
        popupLayout::PreferredColumnCount(
            0, 5) == 3 &&
            popupLayout::RequiredRowCount(
                0, 3) == 2,
        "empty collection popups must reserve a comfortable 3x2 content area");
    Check(
        popupLayout::PreferredColumnCount(
            0, 2) == 2 &&
            popupLayout::RequiredRowCount(
                0, 2) == 2,
        "empty collection popups must shrink horizontally on narrow work areas");
    Check(
        popupLayout::PreferredColumnCount(
            1, 5) == 1 &&
            popupLayout::RequiredRowCount(
                1, 1) == 1,
        "non-empty collection popups must retain their content-driven size");
    Check(
        popupLayout::AllowsMarqueeStart(
            true, false, false),
        "popup chrome and inner edges must allow marquee selection to start");
    Check(
        !popupLayout::AllowsMarqueeStart(
            false, false, false) &&
            !popupLayout::AllowsMarqueeStart(
                true, true, false) &&
            !popupLayout::AllowsMarqueeStart(
                true, false, true),
        "outside presses, items and popup controls must not start marquee selection");

    Check(floatingDock::HasAnySummonTrigger(true, false),
        "the floating Dock hotkey must work without edge swipe");
    Check(floatingDock::HasAnySummonTrigger(false, true),
        "the floating Dock edge swipe must work without the hotkey");
    Check(!floatingDock::HasAnySummonTrigger(false, false),
        "the floating Dock must stop its trigger sampler when both triggers are disabled");
    Check(floatingDock::ShouldUseFloatingDockLogicalForeground(
            true, true, false, true) &&
            floatingDock::ShouldUseFloatingDockLogicalForeground(
                true, false, true, false) &&
            floatingDock::ShouldUseFloatingDockLogicalForeground(
                true, false, false, false),
        "internal and Shell-transient foreground changes must retain the floating Dock logical foreground");
    Check(!floatingDock::ShouldUseFloatingDockLogicalForeground(
            true, false, false, true) &&
            !floatingDock::ShouldUseFloatingDockLogicalForeground(
                true, false, true, true) &&
            !floatingDock::ShouldUseFloatingDockLogicalForeground(
                false, true, true, false),
        "a genuine external task switch must replace the logical foreground even while a Shell operation is in flight");
    Check(floatingDock::ShouldRefocusFloatingDockKeyboardSession(
            true, true, 0, 0) &&
            !floatingDock::ShouldRefocusFloatingDockKeyboardSession(
                true, true, 1, 0) &&
            !floatingDock::ShouldRefocusFloatingDockKeyboardSession(
                true, true, 0, 1) &&
            !floatingDock::ShouldRefocusFloatingDockKeyboardSession(
                false, true, 0, 0),
        "floating keyboard input must return only after the final Shell operation and native menu complete");
    Check(floatingDock::ShouldFloatingDockBeTopmost(true, 0) &&
            !floatingDock::ShouldFloatingDockBeTopmost(true, 1) &&
            !floatingDock::ShouldFloatingDockBeTopmost(false, 0),
        "native Shell menu sessions must be the only visible-time topmost override");
    Check(!floatingDock::ShouldChangeFloatingDockTopmost(true, true) &&
            !floatingDock::ShouldChangeFloatingDockTopmost(false, false) &&
            floatingDock::ShouldChangeFloatingDockTopmost(false, true),
        "reapplying the current Dock topmost state must not reorder the topmost band");

    bool showRunningApps = false;
    bool showWindowPreviews = false;
    snowdesktop::dock_settings_rules::
        NormalizeAlwaysEnabledFeatures(
            showRunningApps, showWindowPreviews);
    Check(showRunningApps,
        "the Dock running area must remain enabled after settings normalization");
    Check(showWindowPreviews,
        "Dock window previews must remain enabled after settings normalization");

    namespace itemVisual = snowdesktop;
    namespace gridSpacing = snowdesktop::grid_spacing_rules;
    namespace layoutSpacing = snowdesktop::layout_spacing_rules;
    namespace localLayout = snowdesktop::widget_item_layout;

    Check(layoutSpacing::ResolveDeferredChangeAction(
            false, false) ==
            layoutSpacing::DeferredChangeAction::None &&
        layoutSpacing::ResolveDeferredChangeAction(
            true, false) ==
            layoutSpacing::DeferredChangeAction::Preview &&
        layoutSpacing::ResolveDeferredChangeAction(
            true, true) ==
            layoutSpacing::DeferredChangeAction::Commit,
        "layout-spacing release must supersede any stale preview queued in the same settings frame");

    const auto pageVisual = itemVisual::ResolvePageItemVisualMetrics(
        104, 128, kDefaultItemFontSizeCu);
    const auto smallIconVisual = itemVisual::ResolvePageItemVisualMetrics(
        104, 128, kDefaultItemFontSizeCu,
        kMinimumItemIconSizeScale);
    const auto largeIconVisual = itemVisual::ResolvePageItemVisualMetrics(
        104, 128, kDefaultItemFontSizeCu,
        kMaximumItemIconSizeScale);
    Check(smallIconVisual.iconSize < pageVisual.iconSize &&
            pageVisual.iconSize < largeIconVisual.iconSize,
        "the page-level icon-size control must change the ordinary icon edge length");
    Check(smallIconVisual.listIconSize < pageVisual.listIconSize &&
            pageVisual.listIconSize < largeIconVisual.listIconSize,
        "the page-level icon-size control must scale the compact list icon baseline");
    Check(smallIconVisual.fontSize == pageVisual.fontSize &&
            largeIconVisual.fontSize == pageVisual.fontSize &&
            smallIconVisual.titleHeight == pageVisual.titleHeight &&
            largeIconVisual.titleHeight == pageVisual.titleHeight &&
            smallIconVisual.titleGap == pageVisual.titleGap &&
            largeIconVisual.titleGap == pageVisual.titleGap,
        "icon-size changes must preserve font, two-line title area, and title gap");
    Check(smallIconVisual.minimumGridHeight -
                smallIconVisual.iconSize ==
            pageVisual.minimumGridHeight - pageVisual.iconSize &&
            largeIconVisual.minimumGridHeight -
                largeIconVisual.iconSize ==
            pageVisual.minimumGridHeight - pageVisual.iconSize,
        "the icon plus title region must change height only by the icon edge delta");
    const int referenceGapX = static_cast<int>(std::round(
        104.0f * kGapPercentX));
    const int referenceGapY = static_cast<int>(std::round(
        128.0f * kGapPercentY));
    const RECT referenceCell{
        0, 0, 104 - referenceGapX, 128 - referenceGapY
    };
    const RECT referenceIcon =
        itemVisual::ResolveGridItemIconRect(
            referenceCell, pageVisual);
    const RECT referenceTitle =
        itemVisual::ResolveGridItemTitleRect(
            referenceCell,
            referenceIcon.bottom + pageVisual.titleGap,
            pageVisual.titleHeight);
    const int horizontalVisualGap =
        104 - (referenceTitle.right - referenceTitle.left);
    const int verticalVisualGap =
        128 - (referenceTitle.bottom - referenceCell.top);
    Check(referenceTitle.left == referenceCell.left &&
            referenceTitle.right == referenceCell.right &&
            std::abs(horizontalVisualGap - verticalVisualGap) <= 1,
        "the default title width must make horizontal and vertical visual gaps equal within pixel rounding");
    const RECT roomyLocalCell{
        0, 0, referenceCell.right,
        referenceCell.bottom + 20
    };
    const RECT roomyLocalIcon =
        itemVisual::ResolveGridItemIconRect(
            roomyLocalCell, pageVisual);
    const RECT roomyLocalTitle =
        itemVisual::ResolveGridItemTitleRect(
            roomyLocalCell,
            roomyLocalIcon.bottom + pageVisual.titleGap,
            pageVisual.titleHeight);
    const int roomyTopPadding =
        roomyLocalIcon.top - pageVisual.topInset -
        roomyLocalCell.top;
    const int roomyBottomPadding =
        roomyLocalCell.bottom - roomyLocalTitle.bottom;
    Check(std::abs(roomyTopPadding - roomyBottomPadding) <= 1 &&
            roomyTopPadding >= 9,
        "local grid cells must distribute spare height around the complete icon-and-title block instead of pinning icons to the frame edge");
    const RECT ordinaryItemCells[] = {
        { 0, 0, 104, pageVisual.minimumGridHeight + 8 },
        { 0, 0, 116, pageVisual.minimumGridHeight + 12 },
        { 0, 0, 132, pageVisual.minimumGridHeight + 16 },
        { 0, 0, 148, pageVisual.minimumGridHeight + 20 },
        { 0, 0, 164, pageVisual.minimumGridHeight + 24 },
        { 0, 0, 180, pageVisual.minimumGridHeight + 28 },
        { 0, 0, 196, pageVisual.minimumGridHeight + 32 },
    };
    for (const RECT& cell : ordinaryItemCells)
    {
        const RECT icon = itemVisual::ResolveGridItemIconRect(
            cell, pageVisual);
        Check(icon.right - icon.left == pageVisual.iconSize &&
                icon.bottom - icon.top == pageVisual.iconSize,
            "desktop and all normal file-widget grid contexts must use the exact page icon edge length");
    }
    const RECT listRow{ 0, 0, 420, pageVisual.minimumListHeight + 10 };
    const RECT listIcon = itemVisual::ResolveListItemIconRect(
        listRow, 8, pageVisual);
    Check(listIcon.right - listIcon.left == pageVisual.listIconSize &&
            listIcon.bottom - listIcon.top == pageVisual.listIconSize &&
            pageVisual.listIconSize < pageVisual.iconSize,
        "list rows must retain their compact icon baseline instead of inheriting the desktop-grid icon edge length");
    const RECT shortListRow{ 0, 0, 420,
        std::max(1, pageVisual.listIconSize - 5) };
    const RECT clampedListIcon = itemVisual::ResolveListItemIconRect(
        shortListRow, 8, pageVisual);
    Check(clampedListIcon.bottom - clampedListIcon.top ==
            shortListRow.bottom - shortListRow.top,
        "list icons must clamp to an unexpectedly short row without escaping its bounds");
    const RECT iconOnlyHighlight =
        itemVisual::ResolveIconOnlyHighlightRect(
            ordinaryItemCells[0],
            itemVisual::ResolveGridItemIconRect(
                ordinaryItemCells[0], pageVisual),
            pageVisual.layoutScale);
    Check(iconOnlyHighlight.left >= ordinaryItemCells[0].left &&
            iconOnlyHighlight.top >= ordinaryItemCells[0].top &&
            iconOnlyHighlight.right <= ordinaryItemCells[0].right &&
            iconOnlyHighlight.bottom <= ordinaryItemCells[0].bottom &&
            iconOnlyHighlight.bottom < ordinaryItemCells[0].bottom,
        "titleless selection and hover highlights must stay around the icon instead of retaining the empty title band");

    namespace titleless =
        snowdesktop::collection_titleless_rules;
    Check(titleless::IsLargeFolderMode(false, 1, 2) &&
            titleless::IsLargeFolderMode(false, 3, 1) &&
            !titleless::IsLargeFolderMode(false, 1, 1) &&
            !titleless::IsLargeFolderMode(true, 3, 2) &&
            titleless::IsActive(true, false, 2, 2) &&
            !titleless::IsActive(false, false, 2, 2),
        "the titleless option must affect only fixed non-compact Collection layouts");
    const RECT tooltipFrame{ 0, 0, 240, 180 };
    const RECT titlelessBottomAnchor{ 180, 138, 228, 178 };
    const RECT bottomTooltip = titleless::ResolveTooltipBounds(
        titlelessBottomAnchor, tooltipFrame, 120, 30, 2, 4);
    Check(bottomTooltip.left >= 4 &&
            bottomTooltip.right <= 236 &&
            bottomTooltip.top < titlelessBottomAnchor.top &&
            bottomTooltip.bottom <= 176,
        "a titleless tooltip near the lower edge must flip above its icon and remain inside the Collection frame");

    const RECT roomyViewport{
        0, 0,
        pageVisual.minimumGridWidth * 4 + 160,
        pageVisual.minimumGridHeight * 3 + 150
    };
    const auto compactGaps = localLayout::ResolveGrid(
        roomyViewport, 4, 3,
        pageVisual.minimumGridWidth,
        pageVisual.minimumGridHeight, 0.5f);
    const auto defaultGaps = localLayout::ResolveGrid(
        roomyViewport, 4, 3,
        pageVisual.minimumGridWidth,
        pageVisual.minimumGridHeight, 1.0f);
    const auto expandedGaps = localLayout::ResolveGrid(
        roomyViewport, 4, 3,
        pageVisual.minimumGridWidth,
        pageVisual.minimumGridHeight, 2.0f);
    Check(compactGaps.horizontal.gap <= defaultGaps.horizontal.gap &&
            defaultGaps.horizontal.gap <= expandedGaps.horizontal.gap &&
            compactGaps.vertical.gap <= defaultGaps.vertical.gap &&
            defaultGaps.vertical.gap <= expandedGaps.vertical.gap,
        "the one layout-spacing value must drive both local grid axes");
    Check(expandedGaps.horizontal.cell >= pageVisual.minimumGridWidth &&
            expandedGaps.vertical.cell >= pageVisual.minimumGridHeight,
        "larger requested gaps must never consume the fixed icon and two-line title cell");
    const RECT compactLastTrack = localLayout::ItemRect(
        compactGaps, 3);
    const RECT expandedLastTrack = localLayout::ItemRect(
        expandedGaps, 3);
    Check((compactLastTrack.left + compactLastTrack.right) / 2 ==
            (expandedLastTrack.left + expandedLastTrack.right) / 2,
        "local bounded tracks must keep their centers fixed while spacing changes");

    bool pageTrackCentersStable = true;
    bool pageGapMonotonic = true;
    bool pageCellMonotonic = true;
    std::vector<int> baselineTrackCenters;
    int previousPageGap = -1;
    int previousPageCell = INT_MAX;
    int firstPageCell = 0;
    const auto jitterPageVisual =
        itemVisual::ResolvePageItemVisualMetrics(
            68, 90, kDefaultItemFontSizeCu);
    for (int percent = 50; percent <= 200; ++percent)
    {
        const auto axis = gridSpacing::ResolveAxis(
            1707, 25, 4, kGapPercentX,
            jitterPageVisual.minimumGridWidth,
            static_cast<float>(percent) / 100.0f);
        if (percent == 50) firstPageCell = axis.cell;
        pageGapMonotonic = pageGapMonotonic &&
            axis.gap >= previousPageGap;
        pageCellMonotonic = pageCellMonotonic &&
            axis.cell <= previousPageCell;
        previousPageGap = axis.gap;
        previousPageCell = axis.cell;
        if (baselineTrackCenters.empty())
        {
            for (int index = 0; index < axis.count; ++index)
                baselineTrackCenters.push_back(
                    gridSpacing::TrackCenter(axis, index));
        }
        else
        {
            for (int index = 0; index < axis.count; ++index)
            {
                pageTrackCentersStable = pageTrackCentersStable &&
                    gridSpacing::TrackCenter(axis, index) ==
                        baselineTrackCenters[index];
            }
        }
    }
    Check(pageTrackCentersStable,
        "every one-percent spacing step must preserve page track centers without one-pixel reversals");
    Check(pageGapMonotonic && pageCellMonotonic &&
            previousPageGap > 0 && previousPageCell < firstPageCell,
        "page gaps must grow and cells must shrink monotonically across the full spacing slider");

    GridPage stableScalePage;
    stableScalePage.itemPitchWidth = 68;
    stableScalePage.itemPitchHeight = 90;
    stableScalePage.cellWidth = 62;
    stableScalePage.cellHeight = 82;
    const float stableWidgetScale = GetGridPageCuScale(
        stableScalePage);
    stableScalePage.cellWidth = 44;
    stableScalePage.cellHeight = 70;
    Check(GetGridPageCuScale(stableScalePage) == stableWidgetScale,
        "widget chrome scale must follow the stable page pitch instead of compressed spacing cells");

    const RECT tightViewport{
        0, 0,
        pageVisual.minimumGridWidth * 3 + 6,
        pageVisual.minimumGridHeight * 2 + 4
    };
    const auto tightDefault = localLayout::ResolveGrid(
        tightViewport, 3, 2,
        pageVisual.minimumGridWidth,
        pageVisual.minimumGridHeight, 1.0f);
    const auto tightMaximum = localLayout::ResolveGrid(
        tightViewport, 3, 2,
        pageVisual.minimumGridWidth,
        pageVisual.minimumGridHeight, 2.0f);
    Check(tightMaximum.horizontal.gap == tightDefault.horizontal.gap &&
            tightMaximum.vertical.gap == tightDefault.vertical.gap &&
            tightMaximum.horizontal.cell >= pageVisual.minimumGridWidth &&
            tightMaximum.vertical.cell >= pageVisual.minimumGridHeight,
        "spacing must saturate at the geometry limit instead of shrinking icons or clipping titles");
    const RECT tightLast = localLayout::ItemRect(tightMaximum, 5);
    Check(tightLast.right <= tightViewport.right &&
            tightLast.bottom <= tightViewport.bottom &&
            std::abs(tightMaximum.horizontal.edge -
                (tightViewport.right - tightLast.right)) <= 1 &&
            std::abs(tightMaximum.vertical.edge -
                (tightViewport.bottom - tightLast.bottom)) <= 1,
        "fixed local tracks must retain symmetric edges and keep the final cell inside the viewport");

    const auto narrowTrack = localLayout::ResolveGrid(
        { 0, 0, pageVisual.minimumGridWidth * 2 + 32, 500 },
        2, 0, pageVisual.minimumGridWidth,
        pageVisual.minimumGridHeight, 1.0f);
    const auto wideTrack = localLayout::ResolveGrid(
        { 0, 0, pageVisual.minimumGridWidth * 2 + 120, 500 },
        2, 0, pageVisual.minimumGridWidth,
        pageVisual.minimumGridHeight, 1.0f);
    const RECT narrowCell = localLayout::ItemRect(narrowTrack, 0);
    const RECT wideCell = localLayout::ItemRect(wideTrack, 0);
    Check(wideCell.right - wideCell.left >
            narrowCell.right - narrowCell.left &&
            itemVisual::ResolveGridItemIconRect(
                wideCell, pageVisual).right -
                itemVisual::ResolveGridItemIconRect(
                    wideCell, pageVisual).left == pageVisual.iconSize,
        "local cell and title width may adapt while the page icon size remains fixed");

    const auto listLayout = localLayout::ResolveList(
        { 0, 0, 520, 260 },
        pageVisual.minimumListHeight, 2.0f);
    const size_t listCount = 18;
    const int listContentHeight = localLayout::ContentHeight(
        listLayout, listCount);
    const int listMaxScroll = std::max(
        0, listContentHeight - 260);
    const RECT finalListRow = localLayout::ItemRect(
        listLayout, listCount - 1, listMaxScroll);
    const auto finalVisibleRange = localLayout::VisibleRange(
        listLayout, listCount, listMaxScroll, 260);
    Check(listLayout.vertical.cell >= pageVisual.minimumListHeight &&
            finalListRow.bottom <= 260 &&
            finalVisibleRange.first <= listCount - 1 &&
            finalVisibleRange.second == listCount,
        "list row height, scroll extent, final row, and visible range must share one geometry result");
    Check(localLayout::ScrollOffsetToReveal(
            { 0, 20, 300, 220 },
            { 0, 250, 300, 320 }, 100, 500) == 200 &&
            localLayout::ScrollOffsetToReveal(
                { 0, 20, 300, 220 },
                { 0, -30, 300, 40 }, 100, 500) == 50,
        "keyboard reveal must consume the same item rectangles used for drawing and hit testing");

    const RECT leftInsertionItem{ 10, 20, 90, 120 };
    const RECT rightInsertionItem{ 110, 20, 190, 120 };
    Check(localLayout::SharesInsertionBoundary(
            leftInsertionItem, rightInsertionItem, true) &&
            localLayout::PointInInsertionGap(
                leftInsertionItem, rightInsertionItem,
                { 100, 70 }, true) &&
            localLayout::InsertionBoundaryPad(
                leftInsertionItem, rightInsertionItem, true) == 10.0f,
        "grid item halves and their complete horizontal gap must share one centered insertion boundary");
    const RECT upperInsertionItem{ 20, 10, 220, 50 };
    const RECT lowerInsertionItem{ 20, 62, 220, 102 };
    Check(localLayout::SharesInsertionBoundary(
            upperInsertionItem, lowerInsertionItem, false) &&
            localLayout::PointInInsertionGap(
                upperInsertionItem, lowerInsertionItem,
                { 120, 56 }, false) &&
            localLayout::InsertionBoundaryPad(
                upperInsertionItem, lowerInsertionItem, false) == 6.0f,
        "list row halves and their complete vertical gap must share one centered insertion boundary");
    Check(!localLayout::SharesInsertionBoundary(
            leftInsertionItem,
            { 10, 140, 90, 240 }, true),
        "a wrapped grid row must not be mistaken for a horizontal insertion gap");

    Check(layoutSpacing::ResolveStoredScale(
            1.25f, 1.75f) == 1.25f,
        "iconSpacing must win when both current and legacy spacing keys exist");
    Check(layoutSpacing::ResolveStoredScale(
            std::nullopt, 2.75f) == 2.0f &&
            layoutSpacing::ResolveStoredScale(
                std::nullopt, 0.25f) == 0.5f,
        "legacy componentSpacing-only layouts must migrate into the new supported range");
    Check(layoutSpacing::ComponentVisualGap(
            16, 1.0f, 1.0f) == 12 &&
            layoutSpacing::ComponentFrameOutset(
                16, 1.0f, 1.0f) == 2 &&
            layoutSpacing::ComponentVisualGap(
                16, 1.0f, 0.5f) == 6 &&
            layoutSpacing::ComponentVisualGap(
                16, 1.0f, 2.0f) == 16,
        "widget frames must keep a comfortable compact baseline while following the one layout-spacing value");
    Check(layoutSpacing::ComponentGapResponseScale(1.0f) == 1.0f &&
            layoutSpacing::ComponentGapResponseScale(2.0f) == 3.0f &&
            layoutSpacing::ComponentVisualGap(
                64, 1.0f, 2.0f) == 36,
        "the upper half of the shared spacing range must provide a wider widget-gap adjustment");
    Check(layoutSpacing::ComponentVisualGap(
            5, 1.0f, 2.0f) == 5 &&
            layoutSpacing::ComponentFrameOutset(
                5, 1.0f, 2.0f) == 0 &&
            layoutSpacing::ComponentEdgeMargin(
                14, 16, 1.0f, 1.0f) == 12,
        "widget frame spacing must clamp to the page gap and preserve its matching edge margin");
    Check(localLayout::IsCompactCollectionSpan(1, 1) &&
            !localLayout::IsCompactCollectionSpan(1, 4) &&
            !localLayout::IsCompactCollectionSpan(4, 1) &&
            localLayout::CollectionUsesFullFrame(false, 1, 4) &&
            localLayout::CollectionUsesFullFrame(false, 4, 1) &&
            !localLayout::CollectionUsesFullFrame(true, 1, 4),
        "only a 1x1 Collection is compact; single-row and single-column large-folder layouts must remain valid");

    constexpr float standardLineHeight =
        14.0f * 7.0f / 6.0f;
    constexpr int standardTextHeight =
        itemLayout::CollapsedTextHeight(
            standardLineHeight);
    constexpr int standardTitleGap =
        itemLayout::TitleGap(1.0f);
    Check(
        standardTextHeight >= 34,
        "two-line item titles must reserve both line boxes and anti-aliasing clearance");
    Check(
        standardTitleGap == 4,
        "item icons and titles must retain a readable four-pixel gap at 100% scale");
    Check(
        itemLayout::TextHeightForLineCount(
            standardLineHeight, 12) >
            itemLayout::TextHeightForLineCount(
                standardLineHeight, 3),
        "selected item titles must grow with every wrapped line instead of stopping at three lines");
    constexpr auto normalTitleLayers =
        snowdesktop::item_render_layer_rules::
            ResolveTitleLayerPlan(false);
    constexpr auto selectedTitleLayers =
        snowdesktop::item_render_layer_rules::
            ResolveTitleLayerPlan(true);
    Check(
        normalTitleLayers.drawWithItem &&
            !normalTitleLayers.drawInForeground,
        "normal item titles must remain in the item drawing pass");
    Check(
        !selectedTitleLayers.drawWithItem &&
            selectedTitleLayers.drawInForeground,
        "expanded selected titles must render in the foreground after all icons");
    Check(
        itemLayout::AvailableIconHeight(
            116, 2, standardTitleGap,
            standardTextHeight) +
                2 + standardTitleGap +
                standardTextHeight ==
            116,
        "item icon sizing must reserve the complete title band without overflowing its cell");
    constexpr RECT collectionIconBounds{
        0, 0, 52, 52
    };
    constexpr auto collectionIconLayout =
        snowdesktop::dock_collection_icon_rules::
            CalculateLayout(
                collectionIconBounds);
    Check(EqualRect(
            &collectionIconLayout.background,
            &collectionIconBounds),
        "Dock collections must retain the full control-style background frame");
    Check(collectionIconLayout.content.left == 8 &&
            collectionIconLayout.content.top == 8 &&
            collectionIconLayout.content.right == 44 &&
            collectionIconLayout.content.bottom == 44 &&
            collectionIconLayout.gap == 2 &&
            collectionIconLayout.cellSize == 17,
        "a standard Dock collection must inset its four icons inside the control frame");
    constexpr RECT collectionFirstCell =
        snowdesktop::dock_collection_icon_rules::
            CellRect(
                collectionIconLayout, 0, 0);
    constexpr RECT collectionLastCell =
        snowdesktop::dock_collection_icon_rules::
            CellRect(
                collectionIconLayout, 1, 1);
    Check(collectionFirstCell.left >=
                collectionIconLayout.content.left &&
            collectionFirstCell.top >=
                collectionIconLayout.content.top &&
            collectionLastCell.right <=
                collectionIconLayout.content.right &&
            collectionLastCell.bottom <=
                collectionIconLayout.content.bottom,
        "all four collection cells must stay inside the inset content area");
    constexpr float compactScale =
        92.0f / 116.0f;
    Check(
        itemLayout::TitleGap(
            compactScale) >= 3 &&
            itemLayout::CollapsedTextHeight(
                standardLineHeight *
                    compactScale) >= 27,
        "compact popup cells must preserve scaled title spacing and two complete Chinese lines");

    struct TestLineMetric
    {
        unsigned length;
        unsigned newlineLength;
    };
    constexpr TestLineMetric wrappedLines[]{
        { 5, 0 },
        { 6, 0 },
        { 4, 0 }
    };
    Check(
        itemLayout::
            VisibleTextLengthForLineLimit(
                wrappedLines, 3, 2, 15) ==
            11,
        "collapsed item layout must remove all text belonging to the third visual line");
    constexpr TestLineMetric explicitLines[]{
        { 6, 1 },
        { 7, 1 },
        { 3, 0 }
    };
    Check(
        itemLayout::
            VisibleTextLengthForLineLimit(
                explicitLines, 3, 2, 16) ==
            12,
        "collapsed item layout must remove the second-line newline to avoid creating an empty third line");

    Microsoft::WRL::ComPtr<
        IDWriteFactory> dwriteFactory;
    const HRESULT factoryResult =
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_ISOLATED,
            __uuidof(IDWriteFactory),
            &dwriteFactory);
    Check(
        SUCCEEDED(factoryResult) &&
            dwriteFactory,
        "DirectWrite factory must be available for the collapsed-title layout regression");
    if (dwriteFactory)
    {
        Microsoft::WRL::ComPtr<
            IDWriteTextFormat> format;
        dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.0f, L"zh-CN",
            &format);
        Check(
            format != nullptr,
            "DirectWrite title format must be created");
        if (format)
        {
            format->SetWordWrapping(
                DWRITE_WORD_WRAPPING_WRAP);
            format->SetLineSpacing(
                DWRITE_LINE_SPACING_METHOD_UNIFORM,
                standardLineHeight,
                14.0f * 5.0f / 6.0f);
            const std::wstring longChineseTitle =
                L"一二三四五六七八九十"
                L"一二三四五六七八九十";
            Microsoft::WRL::ComPtr<
                IDWriteTextLayout>
                measuredLayout;
            dwriteFactory->CreateTextLayout(
                longChineseTitle.c_str(),
                static_cast<UINT32>(
                    longChineseTitle.size()),
                format.Get(), 48.0f,
                10000.0f,
                &measuredLayout);
            UINT32 measuredLineCount = 0;
            if (measuredLayout)
            {
                measuredLayout->GetLineMetrics(
                    nullptr, 0,
                    &measuredLineCount);
            }
            Check(
                measuredLineCount > 2,
                "regression title must wrap to more than two DirectWrite visual lines");
            if (measuredLineCount > 2)
            {
                std::vector<
                    DWRITE_LINE_METRICS>
                    measuredLines(
                        measuredLineCount);
                UINT32 actualLineCount = 0;
                const HRESULT lineResult =
                    measuredLayout->
                        GetLineMetrics(
                            measuredLines.data(),
                            measuredLineCount,
                            &actualLineCount);
                Check(
                    SUCCEEDED(lineResult),
                    "DirectWrite visual line metrics must be readable");
                if (SUCCEEDED(lineResult))
                {
                    const std::size_t
                        visibleLength =
                            itemLayout::
                                VisibleTextLengthForLineLimit(
                                    measuredLines.data(),
                                    actualLineCount,
                                    2,
                                    longChineseTitle.
                                        size());
                    const std::wstring
                        visibleTitle =
                            longChineseTitle.substr(
                                0,
                                visibleLength);
                    Microsoft::WRL::ComPtr<
                        IDWriteTextLayout>
                        collapsedLayout;
                    dwriteFactory->
                        CreateTextLayout(
                            visibleTitle.c_str(),
                            static_cast<UINT32>(
                                visibleTitle.size()),
                            format.Get(), 48.0f,
                            static_cast<float>(
                                standardTextHeight),
                            &collapsedLayout);
                    DWRITE_TEXT_METRICS
                        collapsedMetrics{};
                    if (collapsedLayout)
                    {
                        collapsedLayout->
                            GetMetrics(
                                &collapsedMetrics);
                    }
                    Check(
                        collapsedLayout &&
                            collapsedMetrics.
                                lineCount == 2,
                        "collapsed DirectWrite title must contain exactly two visual lines");

                    Microsoft::WRL::ComPtr<
                        IDWriteTextLayout>
                        expandedLayout;
                    const int expandedHeight =
                        itemLayout::
                            TextHeightForLineCount(
                                standardLineHeight,
                                actualLineCount);
                    dwriteFactory->
                        CreateTextLayout(
                            longChineseTitle.c_str(),
                            static_cast<UINT32>(
                                longChineseTitle.size()),
                            format.Get(), 48.0f,
                            static_cast<float>(
                                expandedHeight),
                            &expandedLayout);
                    DWRITE_TEXT_METRICS
                        expandedMetrics{};
                    if (expandedLayout)
                    {
                        expandedLayout->
                            GetMetrics(
                                &expandedMetrics);
                    }
                    Check(
                        expandedLayout &&
                            expandedMetrics.
                                lineCount ==
                                    actualLineCount,
                        "selected DirectWrite title height must preserve every visual line");
                }
            }
        }
    }

    const RECT popupBounds{
        0, 0, 320, 220 };
    const RECT popupContent{
        18, 54, 302, 202 };
    const RECT insertionClip =
        popupDrag::
            ExpandInsertionClipHorizontally(
                popupContent,
                popupBounds, 7);
    Check(
        insertionClip.left == 11 &&
            insertionClip.right == 309,
        "popup insertion clipping must reserve both left and right indicator gutters");
    Check(
        insertionClip.top ==
                popupContent.top &&
            insertionClip.bottom ==
                popupContent.bottom,
        "popup insertion clipping must preserve vertical scroll boundaries");
    constexpr float indicatorWidth = 3.0f;
    constexpr float itemPad = 5.0f;
    const float firstIndicatorLeft =
        static_cast<float>(
            popupContent.left) -
        itemPad - indicatorWidth / 2.0f;
    const float lastIndicatorRight =
        static_cast<float>(
            popupContent.right) +
        itemPad + indicatorWidth / 2.0f;
    Check(
        firstIndicatorLeft >=
                static_cast<float>(
                    insertionClip.left) &&
            lastIndicatorRight <=
                static_cast<float>(
                    insertionClip.right),
        "first-column and last-column popup insertion bars must remain fully visible");

    Check(
        popupDrag::ResolveDropPreviewLayer(false) ==
                popupDrag::DropPreviewLayer::Background &&
            popupDrag::ResolveDropPreviewLayer(true) ==
                popupDrag::DropPreviewLayer::Popup,
        "only popup-owned drop feedback may render above the popup");
    const RECT popupItemBounds{
        120, 160, 220, 280 };
    const RECT popupIconBounds{
        140, 170, 200, 230 };
    const RECT popupHandoffActivation =
        popupDrag::HandoffActivationBounds(
            popupIconBounds);
    Check(
        popupHandoffActivation.left == 136 &&
            popupHandoffActivation.top == 168 &&
            popupHandoffActivation.right == 204 &&
            popupHandoffActivation.bottom == 234,
        "popup handoff activation must use the shared expanded icon bounds");
    Check(
        popupDrag::CanHandoffToItem(true, false) &&
            !popupDrag::CanHandoffToItem(true, true) &&
            !popupDrag::CanHandoffToItem(false, false),
        "popup handoff must accept unselected items and reject missing or selected items");
    const RECT popupHandoffBounds =
        popupDrag::HandoffIndicatorBounds(
            popupItemBounds);
    Check(
        popupHandoffBounds.left ==
                popupItemBounds.left &&
            popupHandoffBounds.top ==
                popupItemBounds.top &&
            popupHandoffBounds.right ==
                popupItemBounds.right &&
            popupHandoffBounds.bottom ==
                popupItemBounds.bottom,
        "popup handoff feedback must cover the full item cell");

    Check(
        shellVisibility::IsAlwaysHidden(
            L"desktop.ini") &&
            shellVisibility::IsAlwaysHidden(
                L"C:\\Users\\Test\\Desktop.INI") &&
            shellVisibility::IsAlwaysHidden(
                L"C:/Mapped/Desktop.ini"),
        "desktop.ini must stay hidden regardless of path or case");
    Check(
        !shellVisibility::IsAlwaysHidden(
            L"desktop.ini.lnk") &&
            !shellVisibility::IsAlwaysHidden(
                L"desktop.json"),
        "desktop.ini filtering must not hide similarly named files");

    struct SortEntry
    {
        std::wstring name;
        std::wstring fullPath;
        std::wstring typeName;
        bool isDirectory = false;
        FILETIME lastWriteTime{};
        std::optional<std::uint64_t> fileSize;
    };
    auto makeSortEntry = [](
        const wchar_t* name,
        bool directory,
        std::uint64_t modified) {
        SortEntry entry;
        entry.name = name;
        entry.fullPath =
            std::wstring(L"C:\\stack\\") +
            name;
        entry.isDirectory = directory;
        entry.typeName = directory
            ? L"File folder"
            : (std::wstring(name).ends_with(L".png")
                ? L"PNG image" : L"Text document");
        if (!directory)
            entry.fileSize = std::wstring(name).ends_with(L".png")
                ? 300u : 20u;
        entry.lastWriteTime.dwLowDateTime =
            static_cast<DWORD>(modified);
        entry.lastWriteTime.dwHighDateTime =
            static_cast<DWORD>(
                modified >> 32);
        return entry;
    };
    std::vector<SortEntry> sortEntries{
        makeSortEntry(
            L"zeta.txt", false, 20),
        makeSortEntry(
            L"alpha.png", false, 30),
        makeSortEntry(
            L"folder-b", true, 40),
        makeSortEntry(
            L"folder-a", true, 10),
    };
    folderSort::StableSort(
        sortEntries,
        folderSort::kName, true);
    Check(
        sortEntries[0].name == L"folder-a" &&
            sortEntries[1].name ==
                L"folder-b" &&
            sortEntries[2].name ==
                L"alpha.png" &&
            sortEntries[3].name ==
                L"zeta.txt",
        "folder popup name sort must keep directories first");
    folderSort::StableSort(
        sortEntries,
        folderSort::kName, false);
    Check(
        sortEntries[0].name == L"folder-b" &&
            sortEntries[1].name ==
                L"folder-a" &&
            sortEntries[2].name ==
                L"zeta.txt" &&
            sortEntries[3].name ==
                L"alpha.png",
        "folder popup descending name sort must keep directories first");
    folderSort::StableSort(
        sortEntries,
        folderSort::kModified, false);
    Check(
        sortEntries[0].name == L"folder-b" &&
            sortEntries[1].name ==
                L"alpha.png" &&
            sortEntries[2].name ==
                L"zeta.txt" &&
            sortEntries[3].name ==
                L"folder-a",
        "folder popup descending date sort must mix files and directories by cached time");
    folderSort::StableSort(
        sortEntries,
        folderSort::kModified, true);
    Check(
        sortEntries[0].name == L"folder-a" &&
            sortEntries[1].name ==
                L"zeta.txt" &&
            sortEntries[2].name ==
                L"alpha.png" &&
            sortEntries[3].name ==
                L"folder-b",
        "folder popup ascending date sort must mix files and directories by cached time");
    folderSort::StableSort(
        sortEntries,
        folderSort::kType, true);
    Check(
        sortEntries[0].isDirectory &&
            sortEntries[1].isDirectory &&
            sortEntries[2].name ==
                L"alpha.png" &&
            sortEntries[3].name ==
                L"zeta.txt",
        "folder popup type sort must compare cached system type names");
    folderSort::StableSort(
        sortEntries,
        folderSort::kType, false);
    Check(
        sortEntries[0].name == L"folder-a" &&
            sortEntries[1].name ==
                L"folder-b" &&
            sortEntries[2].name ==
                L"zeta.txt" &&
            sortEntries[3].name ==
                L"alpha.png",
        "descending type sort keeps directories first and uses stable names for ties");
    folderSort::StableSort(
        sortEntries,
        folderSort::kSize, true);
    Check(
        sortEntries[0].name == L"zeta.txt" &&
            sortEntries[1].name == L"alpha.png" &&
            sortEntries[2].name == L"folder-a" &&
            sortEntries[3].name == L"folder-b",
        "ascending size sort keeps entries with missing sizes last");
    folderSort::StableSort(
        sortEntries,
        folderSort::kSize, false);
    Check(
        sortEntries[0].name == L"alpha.png" &&
            sortEntries[1].name == L"zeta.txt" &&
            sortEntries[2].name == L"folder-a" &&
            sortEntries[3].name == L"folder-b",
        "descending size sort keeps entries with missing sizes last");
    Check(
        folderSort::NormalizeMode(99) ==
            folderSort::kManual,
        "invalid persisted folder sort modes must fall back to manual order");

    struct GroupedEntry
    {
        int id;
        folderRules::EntryGroup group;
    };
    std::vector<GroupedEntry> groupedEntries{
        { 1, folderRules::EntryGroup::Folder },
        { 2, folderRules::EntryGroup::Main },
        { 3, folderRules::EntryGroup::Recycle },
        { 4, folderRules::EntryGroup::Folder },
        { 5, folderRules::EntryGroup::Main },
    };
    folderRules::StableNormalize(
        groupedEntries,
        [](const GroupedEntry& entry) {
            return entry.group;
        });
    Check(groupedEntries[0].id == 2 &&
            groupedEntries[1].id == 5 &&
            groupedEntries[2].id == 1 &&
            groupedEntries[3].id == 4 &&
            groupedEntries[4].id == 3,
        "Dock normalization must preserve order inside main/folder/recycle groups");

    const auto mainRange =
        folderRules::GroupInsertRange(
            false, 3, 5);
    const auto folderRange =
        folderRules::GroupInsertRange(
            true, 3, 5);
    Check(mainRange.begin == 0 &&
            mainRange.end == 3 &&
            folderRange.begin == 3 &&
            folderRange.end == 8,
        "Dock insertion ranges must isolate main and folder ordering");
    Check(
        dockDrop::ShouldPreferMetadataReorder(
            true, false, false) &&
        dockDrop::ShouldPreferMetadataReorder(
            true, true, true) &&
        !dockDrop::ShouldPreferMetadataReorder(
            true, false, true) &&
        !dockDrop::ShouldPreferMetadataReorder(
            true, true, false) &&
        !dockDrop::ShouldPreferMetadataReorder(
            false, false, false),
        "Dock handoff must remain available when a drag crosses entry groups");
    Check(
        dockDrop::SupportsHandoffTarget(
            false, true, false) &&
        dockDrop::SupportsHandoffTarget(
            false, false, true) &&
        dockDrop::SupportsHandoffTarget(
            true, false, false) &&
        !dockDrop::SupportsHandoffTarget(
            true, true, false) &&
        !dockDrop::SupportsHandoffTarget(
            true, false, true),
        "Dock handoff feedback must only advertise executable widget targets");
    Check(
        dockDrop::CanUseCollectionPopup(
            true, false, false,
            false, false, false) &&
        dockDrop::CanUseCollectionPopup(
            true, true, true,
            false, false, false) &&
        !dockDrop::CanUseCollectionPopup(
            false, false, false,
            false, false, false) &&
        !dockDrop::CanUseCollectionPopup(
            true, false, true,
            false, false, false) &&
        !dockDrop::CanUseCollectionPopup(
            true, false, false,
            true, false, false) &&
        !dockDrop::CanUseCollectionPopup(
            true, false, false,
            false, true, false) &&
        !dockDrop::CanUseCollectionPopup(
            true, false, false,
            false, false, true),
        "Collection popup dwell and insertion must share payload compatibility");
    Check(
        folderRules::OpenPopupNeedsRefreshAfterDrop(
            true, false, false, true, false) &&
        folderRules::OpenPopupNeedsRefreshAfterDrop(
            true, false, false, false, true) &&
        folderRules::OpenPopupNeedsRefreshAfterDrop(
            true, true, false, false, false) &&
        !folderRules::OpenPopupNeedsRefreshAfterDrop(
            false, true, true, true, true) &&
        !folderRules::OpenPopupNeedsRefreshAfterDrop(
            true, false, false, false, false),
        "open Dock folder popups must refresh for matching icon and path drops");
    Check(folderRules::SharedScrollableExtent(
            2, 1, 1, 3, 80, 18) ==
            7 * 80 + 3 * 18,
        "folders must contribute to the same Dock scroll extent as main entries");
    Check(folderRules::SharedScrollableExtent(
            0, 0, 0, 3, 80, 18) ==
            3 * 80,
        "folder-only Dock content must not reserve a phantom group separator");
    Check(folderRules::ScrollableExtentForLayout(
            false, 2, 1, 1, 3, 80, 18) ==
            7 * 80 + 3 * 18,
        "floating Dock folders must remain in the shared scrollable strip");
    Check(folderRules::ScrollableExtentForLayout(
            true, 2, 1, 1, 3, 80, 18) ==
            4 * 80 + 2 * 18,
        "edge-attached Dock folders must stay out of the leading scroll strip");
    Check(folderRules::EdgeAttachedTrailingReserve(
            3, 2, true, 80, 18) ==
            5 * 80 + 18,
        "edge-attached Dock must reserve a packed folder/search control area");
    Check(folderRules::FolderAxisStartBeforeSearch(
            1000, 3, 0, 80) == 760 &&
            folderRules::FolderAxisStartBeforeSearch(
                1000, 3, 1, 80) == 840 &&
            folderRules::FolderAxisStartBeforeSearch(
                1000, 3, 2, 80) == 920,
        "edge-attached Dock folders must preserve order immediately before Search");
    Check(dockDrop::ExternalMappingAction() ==
            DropAction::Link,
        "external resources dropped on Dock must create a link mapping");
    using snowdesktop::item_location::FolderTargetKind;
    Check(
        dockDrop::IsFolderSourceTarget(
            FolderTargetKind::Directory) &&
        dockDrop::IsFolderSourceTarget(
            FolderTargetKind::Shortcut) &&
        !dockDrop::IsFolderSourceTarget(
            FolderTargetKind::None),
        "mapped folder entries must route directories and folder shortcuts to the Dock file area");
    const std::unordered_map<size_t, std::wstring>
        materializedPaths{
            { 0, L"C:\\Desktop\\first.lnk" },
            { 2, L"C:\\Desktop\\third.lnk" },
            { 1, L"C:\\Desktop\\second.lnk" },
        };
    const auto orderedMaterializedPaths =
        dockDrop::OrderedMaterializedPaths(
            { 2, 0, 4, 1 }, materializedPaths);
    Check(orderedMaterializedPaths ==
            std::vector<std::wstring>{
                L"C:\\Desktop\\third.lnk",
                L"C:\\Desktop\\first.lnk",
                L"C:\\Desktop\\second.lnk" },
        "Dock completion must use exact created paths in source order without a desktop snapshot diff");
    dockDrop::MaterializedPathReservations pathReservations;
    Check(pathReservations.TryReserve(
              L"C:\\Desktop\\same-name.lnk") &&
            pathReservations.Contains(
                L"c:\\desktop\\SAME-NAME.LNK") &&
            !pathReservations.TryReserve(
                L"c:\\desktop\\same-name.lnk") &&
            pathReservations.TryReserve(
                L"C:\\Desktop\\same-name (2).lnk"),
        "queued Dock materializations must reserve shortcut names across requests");
    pathReservations.Release({
        L"C:\\Desktop\\same-name.lnk",
        L"C:\\Desktop\\same-name (2).lnk" });
    Check(pathReservations.TryReserve(
              L"C:\\Desktop\\same-name.lnk"),
        "completed Dock materializations must release their path reservations");
    Check(dockDrop::ChooseExternalMappingEffect(
            DROPEFFECT_COPY | DROPEFFECT_MOVE |
                DROPEFFECT_LINK) == DROPEFFECT_LINK,
        "Dock mapping must prefer the native link drop effect");
    Check(dockDrop::ChooseExternalMappingEffect(
            DROPEFFECT_COPY | DROPEFFECT_MOVE) ==
            DROPEFFECT_COPY,
        "Dock mapping must fall back to copy without allowing source deletion");
    Check(dockDrop::ChooseExternalMappingEffect(
            DROPEFFECT_MOVE) == DROPEFFECT_NONE,
        "move-only external sources must be rejected by Dock mapping");
    Check(!dockDrop::ShouldDrawSortableInsertionIndicator(
            true),
        "fixed-position Dock items must not show a sortable insertion indicator");
    Check(dockDrop::ShouldDrawSortableInsertionIndicator(
            false),
        "regular Dock items must retain the sortable insertion indicator");
    Check((floatingDock::kWindowExStyle & WS_EX_TOPMOST) == 0,
        "floating Dock uses SetWindowPos to stay topmost instead of fixing WS_EX_TOPMOST to its window style");
    Check((floatingDock::kWindowExStyle & WS_EX_NOACTIVATE) != 0,
        "the floating Dock must not steal foreground activation");
    const DockWindowPreviewZOrderPolicy floatingPreviewZOrder =
        ResolveDockWindowPreviewZOrderPolicy(true, false);
    Check(floatingPreviewZOrder.insertAfter == nullptr &&
            (floatingPreviewZOrder.flags & SWP_NOZORDER) != 0 &&
            (floatingPreviewZOrder.flags & SWP_NOOWNERZORDER) != 0,
        "a preview owned by the floating Dock must preserve its topmost owner Z order");
    const DockWindowPreviewZOrderPolicy desktopPreviewZOrder =
        ResolveDockWindowPreviewZOrderPolicy(false, false);
    Check(desktopPreviewZOrder.insertAfter == HWND_TOPMOST &&
            (desktopPreviewZOrder.flags & SWP_NOZORDER) == 0,
        "a desktop-hosted preview must still enter the topmost band explicitly");
    const RECT floatingDockRect{ 100, 900, 700, 980 };
    const RECT floatingPopupRect{ 240, 500, 560, 892 };
    const RECT floatingHostRect =
        floatingDock::UnionNonEmptyRects(
            floatingDockRect, floatingPopupRect);
    Check(floatingHostRect.left == 100 &&
            floatingHostRect.top == 500 &&
            floatingHostRect.right == 700 &&
            floatingHostRect.bottom == 980,
        "the compact host must contain both Dock and collection popup");
    const RECT bottomTitleHost =
        floatingDock::ExpandHostForTitleLayer(
            floatingDockRect,
            DockPosition::Bottom);
    Check(bottomTitleHost.left < floatingDockRect.left &&
            bottomTitleHost.right > floatingDockRect.right &&
            bottomTitleHost.top < floatingDockRect.top &&
            bottomTitleHost.bottom ==
                floatingDockRect.bottom,
        "the bottom floating host must reserve its Dock-level title layer");
    const RECT leftTitleHost =
        floatingDock::ExpandHostForTitleLayer(
            floatingDockRect,
            DockPosition::Left);
    Check(leftTitleHost.top < floatingDockRect.top &&
            leftTitleHost.bottom > floatingDockRect.bottom &&
            leftTitleHost.left ==
                floatingDockRect.left &&
            leftTitleHost.right >
                floatingDockRect.right,
        "the left floating host must reserve its Dock-level title layer");
    const RECT floatingBorderOverdraw =
        floatingDock::ExpandForBorderOverdraw(
            floatingDockRect);
    Check(floatingBorderOverdraw.left ==
            floatingDockRect.left - 2 &&
            floatingBorderOverdraw.top ==
                floatingDockRect.top - 2 &&
            floatingBorderOverdraw.right ==
                floatingDockRect.right + 2 &&
            floatingBorderOverdraw.bottom ==
                floatingDockRect.bottom + 2,
        "floating layers must preserve the desktop glass-border overdraw");
    const RECT popupReserveWork{
        0, 0, 1920, 1080
    };
    const SIZE popupReserveSize{
        560, 420
    };
    const RECT bottomPopupReserve =
        floatingDock::
            ReserveCollectionPopupEnvelope(
                floatingDockRect,
                popupReserveWork,
                DockPosition::Bottom,
                popupReserveSize);
    Check(bottomPopupReserve.top <
            floatingDockRect.top &&
            bottomPopupReserve.left <
                floatingDockRect.left &&
            bottomPopupReserve.right >
                floatingDockRect.right,
        "the stable bottom host must reserve popup capacity above the Dock");
    const RECT rightPopupReserve =
        floatingDock::
            ReserveCollectionPopupEnvelope(
                floatingDockRect,
                popupReserveWork,
                DockPosition::Right,
                popupReserveSize);
    Check(rightPopupReserve.left <
            floatingDockRect.left &&
            rightPopupReserve.top <
                floatingDockRect.top &&
            rightPopupReserve.bottom >
                floatingDockRect.bottom,
        "the stable right host must reserve popup capacity before the Dock");
    const POINT mappedFloatingPoint =
        floatingDock::WindowPointToDesktopPoint(
            POINT{ 12, 34 }, floatingHostRect);
    Check(mappedFloatingPoint.x == 112 &&
            mappedFloatingPoint.y == 534,
        "floating-window input must map back to desktop coordinates");
    const RECT previewPanelRect{ 260, 620, 540, 860 };
    Check(!floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 150, 930 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "a click in the Dock must keep the floating host open");
    Check(!floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 300, 600 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "a click in the collection popup must keep the host open");
    Check(!floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 300, 700 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "a press on the thumbnail preview panel must keep the floating host open");
    const RECT quickNavigationRect{ 600, 200, 1000, 700 };
    Check(!floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 800, 300 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect, quickNavigationRect),
        "a press in Quick Navigation must keep its floating Dock host open");
    Check(floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 800, 300 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "the floating Dock independently dismisses presses on another surface");
    Check(floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 20, 20 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "an external click must dismiss the floating host");
    Check(floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 300, 400 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "a press between the preview panel and the Dock must still dismiss");
    Check(!floatingDock::ShouldDismissForPointerDown(
            false, true, POINT{ 20, 20 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "an active context menu must suspend floating-host auto dismissal");
    Check(!floatingDock::ShouldDismissForPointerDown(
            true, false, POINT{ 20, 20 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "active drags must suspend floating-host auto dismissal");
    Check(floatingDock::HasNewPointerButtonPress(
            1, 0, 0),
        "a sampled pointer down edge must dismiss an external click");
    Check(floatingDock::HasNewPointerButtonPress(
            0, 0, 1),
        "a fast press released between samples must still be observed");
    Check(!floatingDock::HasNewPointerButtonPress(
            1, 1, 0),
        "a held pointer button must not repeatedly dismiss");
    Check(floatingDock::IsPointInVisibleLayer(
            POINT{ 150, 930 },
            floatingDockRect,
            floatingPopupRect,
            RECT{}),
        "the Dock remains hovered while its window region changes");
    Check(!floatingDock::IsPointInVisibleLayer(
            POINT{ 20, 20 },
            floatingDockRect,
            floatingPopupRect,
            RECT{}),
        "points outside every visible floating layer are genuine leaves");
    Check(!floatingDock::ShouldRenderDesktopDock(
            true, true),
        "only the Dock mirrored by the floating host must be hidden");
    Check(floatingDock::ShouldRenderDesktopDock(
            true, false),
        "Docks on other monitors must remain visible");
    Check(floatingDock::ShouldRenderDesktopDock(
            false, true),
        "the desktop Dock must remain during the floating first-frame hand-off");
    Check(floatingDock::ShouldRetireDesktopDockCopy(
            true, true) &&
            !floatingDock::ShouldRetireDesktopDockCopy(
                false, true) &&
            !floatingDock::ShouldRetireDesktopDockCopy(
                true, false),
        "the desktop Dock copy must survive until a valid floating frame crosses the presentation barrier");
    Check(floatingDock::ShouldRenderFloatingDockFrame(
            true, false) &&
            !floatingDock::ShouldRenderFloatingDockFrame(
                true, true) &&
            !floatingDock::ShouldRenderFloatingDockFrame(
                false, false),
        "a pending close must freeze the floating Dock hand-off surface");
    Check(floatingDock::CanRunPostCloseActionImmediately(
            false, false, false) &&
            !floatingDock::CanRunPostCloseActionImmediately(
                true, false, true) &&
            !floatingDock::CanRunPostCloseActionImmediately(
                false, true, true) &&
            !floatingDock::CanRunPostCloseActionImmediately(
                false, false, true),
        "window commands must wait until every floating Dock close layer is gone");
    Check(!floatingDock::
            ShouldInvalidateDesktopHover(true) &&
            floatingDock::
                ShouldInvalidateDesktopHover(false),
        "floating Dock hover must repaint only its top-level host instead of queueing desktop frames");
    Check(floatingDock::
            NeedsImmediatePointerPresent(
                true, false, false) &&
            floatingDock::
                NeedsImmediatePointerPresent(
                    false, true, false) &&
            floatingDock::
                NeedsImmediatePointerPresent(
                    false, false, true) &&
            !floatingDock::
                NeedsImmediatePointerPresent(
                    false, false, false),
        "item drags, widget previews and marquees must synchronously present pointer frames");
    // 回归保护：f29a882 删掉 ShouldPresentPointerFrame 后，hover/拖拽帧全部
    // 交给 UiAnimationScheduler，快速扫过时 Dock 放大和拖拽虚影晚一帧。
    Check(floatingDock::
            ShouldPresentPointerFrame(
                1000, 0, false) &&
            floatingDock::
                ShouldPresentPointerFrame(
                    1000, 996, true) &&
            !floatingDock::
                ShouldPresentPointerFrame(
                    1000, 996, false) &&
            floatingDock::
                ShouldPresentPointerFrame(
                    1000, 992, false) &&
            floatingDock::
                ShouldPresentPointerFrame(
                    100, 200, false),
        "passive floating Dock hover is rate-limited but pointer feedback stays synchronous");
    Check(floatingDock::RemainingPointerFrameDelay(
                1000, 996) == 4 &&
            floatingDock::RemainingPointerFrameDelay(
                1000, 992) == 0 &&
            floatingDock::RemainingPointerFrameDelay(
                100, 200) == 0,
        "a throttled Dock hover sample schedules its final tail frame at the remaining deadline");
    Check(floatingDock::
            FloatingVisibilityChangesStaticScene(
                false, true) &&
            floatingDock::
                FloatingVisibilityChangesStaticScene(
                    true, false) &&
            !floatingDock::
                FloatingVisibilityChangesStaticScene(
                    true, true),
        "switching a Dock between desktop and floating layers must invalidate the drag static scene");
    Check(floatingDock::ShouldCloseCollectionPopup(
            3, 3),
        "clicking the collection that owns the open popup must close it");
    Check(!floatingDock::ShouldCloseCollectionPopup(
            3, 4),
        "clicking a different collection must replace the open popup");
    Check(!floatingDock::
            ShouldCloseCollectionPopupOnPointerDown(
                3, 3, false),
        "the owning collection button must defer closing until release");
    Check(floatingDock::
            ShouldCloseCollectionPopupOnPointerDown(
                3, 4, false),
        "a different collection button may close the old popup on press");
    Check(!floatingDock::
            ShouldCloseCollectionPopupOnPointerDown(
                3, static_cast<std::size_t>(-1), true),
        "a click inside the collection popup must keep it open");
    Check(floatingDock::
            ShouldCloseCollectionPopupOnPointerDown(
                3, static_cast<std::size_t>(-1), false),
        "an unrelated external press must close the collection popup");

    Check(floatingDock::ScaleEdgeSwipeDip(4, 144) == 6 &&
            floatingDock::ScaleEdgeSwipeDip(72, 144) == 108,
        "edge swipe thresholds must scale with monitor DPI");
    const RECT negativeBottomMonitor{
        -1920, 0, 0, 1080
    };
    Check(floatingDock::IsPointOnDockScreenEdge(
            POINT{ -1200, 1079 },
            negativeBottomMonitor,
            DockPosition::Bottom, 4),
        "negative-coordinate monitors must expose their Dock-facing edge");
    Check(!floatingDock::IsPointOnDockScreenEdge(
            POINT{ -1200, 1070 },
            negativeBottomMonitor,
            DockPosition::Bottom, 4),
        "an inward pointer must not count as an along-edge swipe");

    floatingDock::EdgeSwipeDetector bottomSwipe;
    Check(!bottomSwipe.Update(
            POINT{ -1500, 1079 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            100, 4, 72),
        "touching a Dock-facing edge must only arm the swipe");
    Check(bottomSwipe.Update(
            POINT{ -1420, 1078 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            240, 4, 72),
        "a quick horizontal stroke along the bottom edge must trigger");
    Check(bottomSwipe.IsAwaitingEdgeLeave(),
        "a completed edge swipe must latch until the pointer leaves");
    Check(!bottomSwipe.Update(
            POINT{ -1320, 1079 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            300, 4, 72),
        "one continuous edge stroke must not trigger repeatedly");
    Check(!bottomSwipe.Update(
            POINT{ -1320, 1060 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            320, 4, 72),
        "leaving the edge must reset the completed swipe");
    Check(!bottomSwipe.Update(
            POINT{ -1320, 1079 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            340, 4, 72),
        "returning to the edge must arm a fresh swipe");
    Check(bottomSwipe.Update(
            POINT{ -1400, 1079 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            430, 4, 72),
        "along-edge swipes must work in either direction");

    floatingDock::EdgeSwipeDetector timedOutSwipe;
    Check(!timedOutSwipe.Update(
            POINT{ -1700, 1079 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            100, 4, 72),
        "the timeout test must arm normally");
    Check(!timedOutSwipe.Update(
            POINT{ -1600, 1079 },
            negativeBottomMonitor,
            DockPosition::Bottom,
            700, 4, 72),
        "slow edge movement must restart instead of triggering");

    const RECT rightMonitor{ 0, 0, 2560, 1440 };
    floatingDock::EdgeSwipeDetector rightSwipe;
    Check(!rightSwipe.Update(
            POINT{ 2559, 500 }, rightMonitor,
            DockPosition::Right,
            10, 6, 108),
        "a vertical edge swipe must arm on the right edge");
    Check(rightSwipe.Update(
            POINT{ 2558, 620 }, rightMonitor,
            DockPosition::Right,
            180, 6, 108),
        "left/right Docks must recognize vertical along-edge travel");

    Check(rules::IsTaskWindowStyleEligible(0, false),
        "ordinary unowned windows must remain eligible");
    Check(!rules::IsTaskWindowStyleEligible(WS_EX_TOOLWINDOW, false),
        "tool windows must be excluded");
    Check(!rules::IsTaskWindowStyleEligible(WS_EX_NOACTIVATE, false),
        "no-activate windows must be excluded");
    Check(!rules::IsTaskWindowStyleEligible(0, true),
        "owned windows must be excluded");

    Check(rules::IsTaskWindowStyleEligible(
            WS_EX_APPWINDOW | WS_EX_TOOLWINDOW, false),
        "app windows must override tool-window exclusion");
    Check(rules::IsTaskWindowStyleEligible(
            WS_EX_APPWINDOW | WS_EX_NOACTIVATE, false),
        "app windows must override no-activate exclusion");
    Check(rules::IsTaskWindowStyleEligible(WS_EX_APPWINDOW, true),
        "app windows must override owner exclusion");
    Check(rules::IsTaskWindowPresentationEligible(true, false) &&
            rules::IsTaskWindowPresentationEligible(true, true),
        "visible normal and minimized windows must remain eligible");
    Check(!rules::IsTaskWindowPresentationEligible(false, false) &&
            !rules::IsTaskWindowPresentationEligible(false, true),
        "hidden windows must stay out of the Dock even when still iconic");
    Check(rules::ResolveDockClickAction(false, false, false) ==
            rules::DockClickAction::Launch,
        "a closed application must keep the existing launch gesture");
    Check(rules::ResolveDockClickAction(true, false, false) ==
            rules::DockClickAction::Activate,
        "a short running indicator must activate the application");
    Check(rules::ShouldSuppressDockWindowCommand(true) &&
            !rules::ShouldSuppressDockWindowCommand(false),
        "a pending close must win over Dock activation, restore, and launch commands");
    Check(rules::ResolveDockClickAction(true, false, true) ==
            rules::DockClickAction::Minimize,
        "a long foreground indicator must minimize the application");
    Check(rules::ResolveDockClickAction(true, true, false) ==
            rules::DockClickAction::Restore,
        "a minimized indicator must restore the application");
    Check(rules::ResolveDockClickAction(true, true, true) ==
            rules::DockClickAction::Restore,
        "minimized state must take precedence over stale foreground state");
    Check(rules::ResolveDockWindowPreviewClickAction(false, false) ==
            rules::DockClickAction::Activate,
        "a background preview card must activate its own window");
    Check(rules::ResolveDockWindowPreviewClickAction(false, true) ==
            rules::DockClickAction::Minimize,
        "a foreground preview card must minimize the window");
    Check(rules::ResolveDockWindowPreviewClickAction(true, false) ==
            rules::DockClickAction::Restore,
        "a minimized preview card must restore the window");
    Check(rules::ResolveDockWindowPreviewClickAction(true, true) ==
            rules::DockClickAction::Restore,
        "a minimized preview card must win over foreground state");
    constexpr auto lightTextForegroundIndicator =
        rules::ResolveDockRunningIndicatorColor(
            false, true, false);
    Check(lightTextForegroundIndicator.blue == 1.0f &&
            lightTextForegroundIndicator.blue >
                lightTextForegroundIndicator.green &&
            lightTextForegroundIndicator.green >
                lightTextForegroundIndicator.red,
        "light Dock text must use a saturated blue foreground indicator");
    constexpr auto lightTextMinimizedIndicator =
        rules::ResolveDockRunningIndicatorColor(
            false, false, true);
    Check(lightTextMinimizedIndicator.blue == 1.0f &&
            lightTextMinimizedIndicator.alpha == 0.82f,
        "light Dock text must keep minimized indicators blue with reduced opacity");
    constexpr auto darkTextForegroundIndicator =
        rules::ResolveDockRunningIndicatorColor(
            true, true, false);
    Check(darkTextForegroundIndicator.red == 0.14f &&
            darkTextForegroundIndicator.blue == 0.22f,
        "dark Dock text must retain its high-contrast neutral indicator");
    constexpr std::size_t noDockEntry =
        static_cast<std::size_t>(-1);
    Check(!rules::ShouldSuppressDockClickRelease(
            noDockEntry, noDockEntry),
        "running and frequent areas must not suppress clicks when both "
        "entry indices are sentinel values");
    Check(rules::ShouldSuppressDockClickRelease(4, 4),
        "a matching fixed dock entry must suppress the deferred release");
    Check(!rules::ShouldSuppressDockClickRelease(4, 5),
        "a different fixed dock entry must not suppress the release");
    Check(rules::ShouldDispatchDockDoubleClickPress(false) &&
            !rules::ShouldDispatchDockDoubleClickPress(true),
        "running-app double clicks must replay the missing second press while "
        "launch and folder double-click actions remain single-purpose");
    int foregroundSequence = 0;
    int foregroundChecks = 0;
    int primaryForegroundStep = 0;
    int retryForegroundStep = 0;
    bool foregroundMatched = false;
    const bool foregroundActivated =
        rules::ApplyDockWindowForegroundActivation(
            true,
            [&]() {
                ++foregroundChecks;
                return foregroundMatched;
            },
            [&]() {
                primaryForegroundStep = ++foregroundSequence;
            },
            [&]() {
                retryForegroundStep = ++foregroundSequence;
                foregroundMatched = true;
            });
    Check(foregroundActivated &&
            foregroundChecks == 3 &&
            primaryForegroundStep == 1 &&
            retryForegroundStep == 2,
        "Dock activation must try one foreground request before one attached-input retry");
    int successfulPrimaryRequests = 0;
    int successfulPrimaryRetries = 0;
    bool primaryMatched = false;
    Check(rules::ApplyDockWindowForegroundActivation(
            true,
            [&]() { return primaryMatched; },
            [&]() {
                ++successfulPrimaryRequests;
                primaryMatched = true;
            },
            [&]() { ++successfulPrimaryRetries; }) &&
            successfulPrimaryRequests == 1 &&
            successfulPrimaryRetries == 0,
        "a successful foreground request must not enter the attached-input retry");
    int alreadyForegroundRequests = 0;
    Check(rules::ApplyDockWindowForegroundActivation(
            true,
            []() { return true; },
            [&]() { ++alreadyForegroundRequests; },
            [&]() { ++alreadyForegroundRequests; }) &&
            alreadyForegroundRequests == 0,
        "an already foreground application must not mutate Z-order again");
    int unsafeForegroundRetries = 0;
    Check(!rules::ApplyDockWindowForegroundActivation(
            false,
            []() { return false; },
            []() {},
            [&]() { ++unsafeForegroundRetries; }) &&
            unsafeForegroundRetries == 0,
        "a hung activation target must not enter the attached-input retry");
    Check(rules::NeedsDockMinimizeSystemCommandFallback(false) &&
            !rules::NeedsDockMinimizeSystemCommandFallback(true),
        "a rejected asynchronous minimize must use the system-command fallback");
    Check(rules::NeedsDockCloseSystemCommandFallback(false) &&
            !rules::NeedsDockCloseSystemCommandFallback(true),
        "a rejected graceful close must use the system-command fallback");
    Check(!rules::NeedsDockRestoreRequestFallback(
            false, false) &&
            !rules::NeedsDockRestoreRequestFallback(
                true, true) &&
            rules::NeedsDockRestoreRequestFallback(
                true, false),
        "only a rejected minimized-window restore may use the one-shot system-command fallback");
    int restoreFallbackSequence = 0;
    int restoreSystemCommandStep = 0;
    int restoreStateCheckStep = 0;
    int restoreSwitchStep = 0;
    WPARAM restoreSystemCommand = 0;
    rules::ApplyDockRestoreRequestFallback(
        true,
        [&](WPARAM command) {
            restoreSystemCommandStep = ++restoreFallbackSequence;
            restoreSystemCommand = command;
        },
        [&]() {
            restoreStateCheckStep = ++restoreFallbackSequence;
            return true;
        },
        [&]() {
            restoreSwitchStep = ++restoreFallbackSequence;
        });
    Check(restoreSystemCommand == SC_RESTORE &&
            restoreSystemCommandStep == 1 &&
            restoreStateCheckStep == 2 &&
            restoreSwitchStep == 3,
        "a rejected elevated-window restore must issue SC_RESTORE before the final task-switch fallback");
    int completedRestoreSwitches = 0;
    rules::ApplyDockRestoreRequestFallback(
        true,
        [](WPARAM) {},
        []() { return false; },
        [&]() { ++completedRestoreSwitches; });
    Check(completedRestoreSwitches == 0,
        "a completed system restore must not issue a duplicate task-switch restore");
    int unnecessaryRestoreCallbacks = 0;
    rules::ApplyDockRestoreRequestFallback(
        false,
        [&](WPARAM) { ++unnecessaryRestoreCallbacks; },
        [&]() {
            ++unnecessaryRestoreCallbacks;
            return true;
        },
        [&]() { ++unnecessaryRestoreCallbacks; });
    Check(unnecessaryRestoreCallbacks == 0,
        "a successful asynchronous restore must not enter the system-command fallback");
    Check(rules::ShouldSwitchDockWindowAfterShow(
            false, true),
        "a visible background window must always switch to the foreground");
    Check(!rules::ShouldSwitchDockWindowAfterShow(
            true, false),
        "every asynchronous restore must defer foreground switching until the window is visible");
    Check(rules::ShouldSwitchDockWindowAfterShow(
            true, true),
        "a restored window must switch above an existing maximized foreground application");
    Check(rules::IsDockWindowActivationPopupEligible(
            true, true, false, false) &&
            !rules::IsDockWindowActivationPopupEligible(
                true, false, false, false) &&
            !rules::IsDockWindowActivationPopupEligible(
                true, true, true, false) &&
            !rules::IsDockWindowActivationPopupEligible(
                true, true, false, true),
        "only a visible restorable popup may replace the root activation target");
    Check(rules::ShouldRetryDockWindowForegroundActivation(
            false, true) &&
            !rules::ShouldRetryDockWindowForegroundActivation(
                true, true) &&
            !rules::ShouldRetryDockWindowForegroundActivation(
                false, false),
        "input queues may be shared only after a safe ordinary foreground request fails");
    Check(rules::IsDockWindowSynchronousActivationSafe(
            true, true) &&
            !rules::IsDockWindowSynchronousActivationSafe(
                false, true) &&
            !rules::IsDockWindowSynchronousActivationSafe(
                true, false),
        "synchronous activation must require both the root and actual popup threads to respond");
    using ObservationAction =
        rules::DockWindowActivationObservationAction;
    Check(rules::ResolveDockWindowActivationObservationAction(
            true, false, true, true, true, false, false) ==
            ObservationAction::WaitForRestore,
        "a valid asynchronous restore must remain observed while the window is iconic");
    Check(rules::ResolveDockWindowActivationObservationAction(
            true, false, true, true, false, false, false) ==
            ObservationAction::Activate &&
            rules::ResolveDockWindowActivationObservationAction(
                true, false, true, false, false, false, false) ==
            ObservationAction::Activate,
        "restored and already-visible requests must use the same foreground activation path");
    Check(rules::ResolveDockWindowActivationObservationAction(
            false, false, true, true, true, false, false) ==
            ObservationAction::Stop &&
            rules::ResolveDockWindowActivationObservationAction(
                true, true, true, true, true, false, false) ==
            ObservationAction::Stop &&
            rules::ResolveDockWindowActivationObservationAction(
                true, false, false, true, true, false, false) ==
            ObservationAction::Stop &&
            rules::ResolveDockWindowActivationObservationAction(
                true, false, true, false, false, true, false) ==
            ObservationAction::Stop &&
            rules::ResolveDockWindowActivationObservationAction(
                true, false, true, false, false, false, true) ==
            ObservationAction::Stop,
        "activation observation must stop for stale, closing, hung, foreground or superseded requests");
    Check(rules::ResolveDockWindowActivationObservationAction(
            true, false, true, false, true, false, false) ==
            ObservationAction::Stop,
        "a visible-window activation request must not wait forever if the window becomes minimized");
    Check(rules::RequiresFloatingDockMinimizeCaptureIsolation(
            true, rules::DockClickAction::Minimize),
        "floating minimize animations must exclude the top-level Dock");
    Check(!rules::RequiresFloatingDockMinimizeCaptureIsolation(
            false, rules::DockClickAction::Minimize) &&
            !rules::RequiresFloatingDockMinimizeCaptureIsolation(
                true, rules::DockClickAction::Activate) &&
            !rules::RequiresFloatingDockMinimizeCaptureIsolation(
                true, rules::DockClickAction::Restore) &&
            !rules::RequiresFloatingDockMinimizeCaptureIsolation(
                true, rules::DockClickAction::Launch),
        "desktop-layer Docks, restore, foreground activation and launches must keep the floating Dock visible");
    Check(rules::ResolveDockRestoreShowCommand(
            WPF_RESTORETOMAXIMIZED,
            SW_SHOWMINIMIZED) == SW_SHOWMAXIMIZED,
        "a window minimized from maximized must return to maximized");
    Check(rules::ResolveDockRestoreShowCommand(
            0, SW_SHOWMAXIMIZED) == SW_SHOWMAXIMIZED,
        "an explicitly maximized placement must remain maximized");
    Check(rules::ResolveDockRestoreShowCommand(
            0, SW_SHOWMINIMIZED) == SW_RESTORE,
        "an ordinary minimized window must restore to its normal rectangle");
    Check(rules::ShouldRestoreDockWindowMaximized(
            WPF_RESTORETOMAXIMIZED, SW_SHOWMINIMIZED) &&
            rules::ShouldRestoreDockWindowMaximized(
                0, SW_SHOWMAXIMIZED) &&
            !rules::ShouldRestoreDockWindowMaximized(
                0, SW_SHOWMINIMIZED),
        "restore animation geometry and the real show command must agree on maximized placement");

    Check(EaseDockWindowTransition(-1.0) == 0.0 &&
            EaseDockWindowTransition(2.0) == 1.0,
        "window transition easing must clamp its input");
    Check(EaseDockWindowTransition(0.25) < 0.25 &&
            EaseDockWindowTransition(0.75) > 0.75 &&
            EaseDockWindowTransition(0.5) == 0.5,
        "window transition easing must accelerate and decelerate smoothly");
    Check(ResolveDockWindowTransitionOpacity(
            DockWindowTransitionDirection::Minimize,
            0.0) == 255 &&
            ResolveDockWindowTransitionOpacity(
                DockWindowTransitionDirection::Minimize,
                1.0) == 0,
        "minimize transition opacity must fade the snapshot into the Dock");
    Check(ResolveDockWindowTransitionOpacity(
            DockWindowTransitionDirection::Restore,
            0.0) == 0 &&
            ResolveDockWindowTransitionOpacity(
                DockWindowTransitionDirection::Restore,
                1.0) == 255,
        "restore transition opacity must reveal the snapshot before handoff");
    const RECT transitionFrom{ 100, 100, 900, 700 };
    const RECT transitionTo{ 460, 1000, 540, 1080 };
    const int transitionCornerRadius =
        ResolveDockWindowTransitionCornerRadius(
            transitionFrom, transitionTo);
    Check(transitionCornerRadius > 0 &&
            transitionCornerRadius <
                (transitionTo.right -
                    transitionTo.left) / 2,
        "window transitions must retain a rounded mask sized from the Dock target");
    const RECT highDpiDockTarget{
        400, 900, 560, 1060
    };
    Check(ResolveDockWindowTransitionCornerRadius(
            transitionFrom,
            highDpiDockTarget) >
            transitionCornerRadius,
        "window transition corner rounding must scale with Dock DPI geometry");
    const RECT tinyTransitionFrame{
        0, 0, 10, 8
    };
    Check(ResolveDockWindowTransitionCornerRadius(
            tinyTransitionFrame,
            transitionTo) <= 4,
        "rounded transition masks must remain valid near their smallest frame");
    const RECT transitionStart =
        InterpolateDockWindowTransitionRect(
            transitionFrom, transitionTo, 0.0);
    const RECT transitionEnd =
        InterpolateDockWindowTransitionRect(
            transitionFrom, transitionTo, 1.0);
    Check(EqualRect(
            &transitionStart, &transitionFrom),
        "window transition must begin at the source rectangle");
    Check(EqualRect(
            &transitionEnd, &transitionTo),
        "window transition must end at the Dock icon rectangle");
    const RECT snapshotHost =
        ResolveDockWindowSnapshotHostRect(
            transitionFrom, transitionTo);
    Check(snapshotHost.left == 100 &&
            snapshotHost.top == 100 &&
            snapshotHost.right == 900 &&
            snapshotHost.bottom == 1080,
        "snapshot animation must use one fixed host surface covering both endpoints");
    const SIZE fullHdSnapshot =
        ConstrainDockWindowSnapshotSize(
            { 1920, 1080 });
    Check(fullHdSnapshot.cx == 1920 &&
            fullHdSnapshot.cy == 1080,
        "ordinary high-resolution windows must retain native snapshot detail");
    const SIZE portraitSnapshot =
        ConstrainDockWindowSnapshotSize(
            { 2160, 3840 });
    Check(portraitSnapshot.cx == 2160 &&
            portraitSnapshot.cy == 3840,
        "portrait windows up to 4K must retain native snapshot detail");
    const SIZE eightKSnapshot =
        ConstrainDockWindowSnapshotSize(
            { 7680, 4320 });
    Check(eightKSnapshot.cx == 4096 &&
            eightKSnapshot.cy == 2304,
        "extreme snapshots must remain bounded while preserving aspect ratio");
    const SIZE compactSnapshot =
        ConstrainDockWindowSnapshotSize(
            { 800, 600 });
    Check(compactSnapshot.cx == 800 &&
            compactSnapshot.cy == 600,
        "small window snapshots must not be enlarged");
    Check(kDockWindowSnapshotRenderDpi == 96.0f,
        "snapshot render coordinates must remain physical pixels at every monitor DPI");
    Check(kDockWindowSnapshotUsesComposition,
        "normal snapshot frames must use the composition visual path");
    Check(kDockWindowTransitionCornerPreference ==
                DWMWCP_DONOTROUND &&
            kDockWindowTransitionNcRenderingPolicy ==
                DWMNCRP_DISABLED &&
            kDockWindowTransitionBorderColor ==
                DWMWA_COLOR_NONE,
        "the transition host must not draw a DWM frame, shadow, or rounded border");
    Check((kDockWindowTransitionExStyle &
                WS_EX_LAYERED) != 0 &&
            (kDockWindowTransitionExStyle &
                WS_EX_NOREDIRECTIONBITMAP) != 0 &&
            (kDockWindowTransitionExStyle &
                WS_EX_TRANSPARENT) != 0 &&
            (kDockWindowTransitionExStyle &
                WS_EX_NOACTIVATE) != 0,
        "the transition host must use a transparent no-redirection composition surface without a DWM shadow");
    Check(ResolveDockWindowTransitionSurface(
            true, true) ==
            DockWindowTransitionSurface::Snapshot,
        "a captured frame must be preferred over a live DWM thumbnail");
    Check(ResolveDockWindowTransitionSurface(
            false, true) ==
            DockWindowTransitionSurface::LiveThumbnail,
        "live DWM rendering must remain available when no snapshot exists");
    Check(ResolveDockWindowTransitionSurface(
            false, false) ==
            DockWindowTransitionSurface::None,
        "a transition must stop safely when neither rendering surface is available");
    Check(ResolveDockWindowTransitionSurface(
            true, true,
            DockWindowTransitionCapturePolicy::LiveThumbnailOnly) ==
            DockWindowTransitionSurface::LiveThumbnail,
        "floating minimize must prefer the target-only DWM thumbnail over a screen snapshot");
    Check(ResolveDockWindowTransitionSurface(
            true, false,
            DockWindowTransitionCapturePolicy::LiveThumbnailOnly) ==
            DockWindowTransitionSurface::None,
        "floating minimize must reject a screen snapshot when no DWM thumbnail is available");
    Check(rules::ResolveDockWindowIconSource(
            true, true, false, true) ==
            rules::DockWindowIconSource::AppUserModel,
        "packaged applications must retain their stable AppUserModel icon");
    Check(rules::ResolveDockWindowIconSource(
            false, true, false, true) ==
            rules::DockWindowIconSource::Executable,
        "a dedicated executable icon must take precedence over a window icon");
    Check(rules::ResolveDockWindowIconSource(
            false, true, true, true) ==
            rules::DockWindowIconSource::Window,
        "a valid window icon must replace the generic executable icon");
    Check(rules::ResolveDockWindowIconSource(
            false, true, true, false) ==
            rules::DockWindowIconSource::GenericExecutable,
        "the generic executable icon must remain the final fallback");
    Check(rules::ResolveDockWindowIconSource(
            false, false, false, false) ==
            rules::DockWindowIconSource::None,
        "icon resolution must fail safely when no source is available");
    Check(RequiresDockWindowTransitionCompositionBarrier(
            DockWindowTransitionDirection::Minimize),
        "snapshot minimize must commit disabled native transitions before changing window state");
    Check(!RequiresDockWindowTransitionCompositionBarrier(
            DockWindowTransitionDirection::Restore),
        "snapshot restore changes the native window state only after its custom animation");
    Check(ResolveDockWindowTransitionStartAction(
            false, false, false) ==
            DockWindowTransitionStartAction::StartNew &&
            ResolveDockWindowTransitionStartAction(
                true, false, false) ==
                DockWindowTransitionStartAction::StartNew,
        "inactive or different-window requests must start a new transition");
    Check(ResolveDockWindowTransitionStartAction(
            true, true, true) ==
            DockWindowTransitionStartAction::ContinueActive,
        "a repeated same-direction request must not restart its transition");
    Check(ResolveDockWindowTransitionStartAction(
            true, true, false) ==
            DockWindowTransitionStartAction::ReverseActive,
        "an opposite request for the active window must reverse in place");
    Check(ResolveDockWindowTransitionStartAction(
            true, true, false, true) ==
            DockWindowTransitionStartAction::
                InterruptRestoreHandoff,
        "an opposite request during restore handoff must release the old "
        "transition immediately");
    Check(ResolveDockWindowTransitionStartAction(
            true, true, true, true) ==
            DockWindowTransitionStartAction::ContinueActive,
        "a repeated restore request must keep waiting for its real window");

    namespace launchAnimation =
        snowdesktop::dock_launch_animation;
    Check(launchAnimation::NormalizedOffset(0) == 0.0 &&
            launchAnimation::NormalizedOffset(
                launchAnimation::kMaximumDurationMs) == 0.0,
        "Dock launch bounce must begin and end at rest");
    Check(launchAnimation::NormalizedOffset(
            launchAnimation::kBouncePeriodMs / 2) >
            launchAnimation::NormalizedOffset(
                launchAnimation::kBouncePeriodMs +
                launchAnimation::kBouncePeriodMs / 2),
        "Dock launch bounce must decay after each cycle");
    Check(launchAnimation::OffsetPixels(
            launchAnimation::kBouncePeriodMs / 2, 64) > 0,
        "Dock launch bounce must move a visible icon");
    const double nearTakeoff =
        launchAnimation::NormalizedOffset(1.0);
    const double quarterStep =
        launchAnimation::NormalizedOffset(
            launchAnimation::kBouncePeriodMs / 4.0 + 1.0) -
        launchAnimation::NormalizedOffset(
            launchAnimation::kBouncePeriodMs / 4.0);
    Check(nearTakeoff < quarterStep,
        "Dock launch bounce must ease smoothly away from rest");
    Check(launchAnimation::OffsetPixels(
            launchAnimation::kBouncePeriodMs / 7.0, 64) !=
            std::round(launchAnimation::OffsetPixels(
                launchAnimation::kBouncePeriodMs / 7.0, 64)),
        "Dock launch bounce must preserve subpixel motion");
    Check(!launchAnimation::IsRestingPoint(
            launchAnimation::kBouncePeriodMs) &&
            launchAnimation::IsRestingPoint(
                launchAnimation::kMinimumDurationMs),
        "Dock launch bounce must complete at least two cycles");

    namespace magnification = snowdesktop::dock_magnification;
    Check(std::abs(
            magnification::ScaleForAxisDistance(0, 76) -
            magnification::kFocusScale) < 0.001f,
        "the focused Dock element must receive the maximum scale");
    Check(std::abs(
            magnification::ScaleForAxisDistance(76, 76) -
            magnification::kFirstNeighborScale) < 0.001f,
        "the first Dock neighbor must receive the medium scale");
    Check(std::abs(
            magnification::ScaleForAxisDistance(152, 76) -
            magnification::kSecondNeighborScale) < 0.001f,
        "the second Dock neighbor must receive the subtle scale");
    Check(magnification::ScaleForAxisDistance(228, 76) == 1.0f,
        "distant Dock elements must retain their normal scale");
    Check(magnification::FocusSwitchHysteresisPixels(76) == 4 &&
            magnification::FocusSwitchHysteresisPixels(16) == 3 &&
            magnification::FocusSwitchHysteresisPixels(256) == 8,
        "Dock focus hysteresis must scale within a small responsive range");
    Check(!magnification::HasCrossedFocusSwitchBoundary(
            100, 176, 141, 76) &&
            magnification::HasCrossedFocusSwitchBoundary(
                100, 176, 142, 76),
        "forward focus changes must wait until the pointer clears the Schmitt boundary");
    Check(!magnification::HasCrossedFocusSwitchBoundary(
            176, 100, 135, 76) &&
            magnification::HasCrossedFocusSwitchBoundary(
                176, 100, 134, 76),
        "reverse focus changes must use the mirrored Schmitt boundary");
    const RECT retainedFocus =
        magnification::ExpandFocusRetentionBounds(
            RECT{ 100, 200, 176, 288 });
    Check(retainedFocus.left == 95 &&
            retainedFocus.top == 195 &&
            retainedFocus.right == 181 &&
            retainedFocus.bottom == 293,
        "Dock focus must retain a small exit margin around its visual bounds");
    const float quarterScale =
        magnification::ScaleForAxisDistance(19, 76);
    const float halfScale =
        magnification::ScaleForAxisDistance(38, 76);
    const float threeQuarterScale =
        magnification::ScaleForAxisDistance(57, 76);
    Check(quarterScale < magnification::kFocusScale &&
            quarterScale > halfScale &&
            halfScale > threeQuarterScale &&
            threeQuarterScale >
                magnification::kFirstNeighborScale,
        "Dock magnification must vary continuously inside each icon pitch");
    Check(magnification::AxisShiftForDistance(19, 76, 64) <
            magnification::AxisShiftForDistance(38, 76, 64) &&
            magnification::AxisShiftForDistance(38, 76, 64) <
            magnification::AxisShiftForDistance(57, 76, 64),
        "Dock displacement must grow continuously with pointer distance");
    const int firstNeighborShift =
        magnification::AxisShiftForDistance(76, 76, 64);
    const int secondNeighborShift =
        magnification::AxisShiftForDistance(152, 76, 64);
    const int tailShift =
        magnification::AxisShiftForDistance(228, 76, 64);
    Check(firstNeighborShift > 0 &&
            secondNeighborShift > firstNeighborShift &&
            tailShift > secondNeighborShift,
        "Dock neighbors and the remaining tail must be pushed outward");
    Check(magnification::AxisShiftForDistance(-76, 76, 64) ==
            -firstNeighborShift,
        "Dock displacement must be symmetric around the focused element");

    const std::vector<float> leadingZoneScales{
        magnification::kFocusScale,
        magnification::kFirstNeighborScale,
        magnification::kSecondNeighborScale,
        1.0f
    };
    const int leadingFocusShift =
        magnification::PackedAxisShift(
            leadingZoneScales, 0, 64, true);
    const int leadingNeighborShift =
        magnification::PackedAxisShift(
            leadingZoneScales, 1, 64, true);
    Check(leadingFocusShift > 0 &&
            leadingNeighborShift > leadingFocusShift,
        "edge-attached leading zone must accumulate growth toward center");

    const RECT baseDockElement{ 100, 200, 176, 288 };
    const RECT bottomMagnified = magnification::MagnifyRect(
        baseDockElement, DockPosition::Bottom,
        magnification::kFocusScale, 64);
    Check(bottomMagnified.top < baseDockElement.top &&
            bottomMagnified.bottom == baseDockElement.bottom,
        "bottom Dock magnification must grow toward the desktop");
    Check(bottomMagnified.left < baseDockElement.left &&
            bottomMagnified.right > baseDockElement.right,
        "horizontal Dock magnification must remain centered on its slot");

    constexpr int tooltipWidth = 120;
    constexpr int tooltipHeight = 30;
    constexpr int tooltipGap = 8;
    const RECT bottomBaseTooltip =
        magnification::AnchorTooltipBounds(
            baseDockElement, DockPosition::Bottom,
            tooltipWidth, tooltipHeight, tooltipGap);
    const RECT bottomMagnifiedTooltip =
        magnification::AnchorTooltipBounds(
            bottomMagnified, DockPosition::Bottom,
            tooltipWidth, tooltipHeight, tooltipGap);
    Check(bottomMagnifiedTooltip.top ==
            bottomMagnified.top - tooltipGap - tooltipHeight &&
            bottomMagnifiedTooltip.top < bottomBaseTooltip.top,
        "the Dock title tooltip must follow perpendicular icon magnification");

    const RECT shiftedBottomMagnified =
        magnification::MagnifyRect(
            baseDockElement, DockPosition::Bottom,
            magnification::kFocusScale, 64, 13);
    const RECT shiftedBottomTooltip =
        magnification::AnchorTooltipBounds(
            shiftedBottomMagnified, DockPosition::Bottom,
            tooltipWidth, tooltipHeight, tooltipGap);
    Check(shiftedBottomTooltip.left ==
            bottomMagnifiedTooltip.left + 13,
        "the Dock title tooltip must follow icon displacement along the Dock axis");

    const RECT leftMagnified = magnification::MagnifyRect(
        baseDockElement, DockPosition::Left,
        magnification::kFocusScale, 64);
    Check(leftMagnified.left == baseDockElement.left &&
            leftMagnified.right > baseDockElement.right,
        "left Dock magnification must grow toward the desktop");

    const RECT neighborDockElement{ 176, 200, 252, 288 };
    const RECT shiftedNeighbor = magnification::MagnifyRect(
        neighborDockElement, DockPosition::Bottom,
        magnification::kFirstNeighborScale, 64, firstNeighborShift);
    Check(shiftedNeighbor.left >= bottomMagnified.right,
        "neighbor displacement must preserve spacing beside the magnified focus");

    const RECT leadingPackedFocus =
        magnification::MagnifyRect(
            baseDockElement, DockPosition::Bottom,
            leadingZoneScales[0], 64, leadingFocusShift);
    const RECT leadingPackedNeighbor =
        magnification::MagnifyRect(
            neighborDockElement, DockPosition::Bottom,
            leadingZoneScales[1], 64, leadingNeighborShift);
    Check(leadingPackedFocus.left == baseDockElement.left,
        "edge-attached leading zone must preserve its outer boundary");
    Check(leadingPackedNeighbor.left >= leadingPackedFocus.right,
        "edge-attached leading zone must keep icon spacing while packing inward");

    const std::vector<float> trailingZoneScales{
        magnification::kFirstNeighborScale,
        magnification::kFocusScale
    };
    const int trailingInnerShift =
        magnification::PackedAxisShift(
            trailingZoneScales, 0, 64, false);
    const int trailingFocusShift =
        magnification::PackedAxisShift(
            trailingZoneScales, 1, 64, false);
    const RECT trailingSearchBase{ 100, 200, 176, 288 };
    const RECT trailingRecycleBase{ 176, 200, 252, 288 };
    const RECT trailingPackedInner =
        magnification::MagnifyRect(
            trailingSearchBase, DockPosition::Bottom,
            trailingZoneScales[0], 64, trailingInnerShift);
    const RECT trailingPackedFocus =
        magnification::MagnifyRect(
            trailingRecycleBase, DockPosition::Bottom,
            trailingZoneScales[1], 64, trailingFocusShift);
    Check(trailingFocusShift < 0 &&
            trailingPackedFocus.right == trailingRecycleBase.right,
        "edge-attached trailing zone must preserve its outer boundary");
    Check(trailingPackedInner.right == trailingPackedFocus.left,
        "search and recycle bin must remain packed in physical order");
    const RECT trailingSearchVertical{ 100, 200, 188, 276 };
    const RECT trailingRecycleVertical{ 100, 276, 188, 352 };
    const RECT trailingPackedSearchVertical =
        magnification::MagnifyRect(
            trailingSearchVertical, DockPosition::Left,
            trailingZoneScales[0], 64, trailingInnerShift);
    const RECT trailingPackedRecycleVertical =
        magnification::MagnifyRect(
            trailingRecycleVertical, DockPosition::Left,
            trailingZoneScales[1], 64, trailingFocusShift);
    Check(trailingPackedRecycleVertical.bottom ==
            trailingRecycleVertical.bottom &&
            trailingPackedSearchVertical.bottom ==
            trailingPackedRecycleVertical.top,
        "vertical edge-attached trailing controls must pack upward from the edge");

    const RECT baseIsland{ 80, 190, 300, 300 };
    const RECT expandedIsland = magnification::ExpandInteractionBounds(
        baseIsland, DockPosition::Bottom, 64);
    Check(expandedIsland.left < baseIsland.left &&
            expandedIsland.right > baseIsland.right &&
            expandedIsland.top < baseIsland.top,
        "the Dock island interaction area must cover the expanded wave");
    const RECT bottomViewport =
        magnification::ExpandPerpendicularBounds(
            baseIsland, DockPosition::Bottom, 64);
    Check(bottomViewport.left == baseIsland.left &&
            bottomViewport.right == baseIsland.right &&
            bottomViewport.top < baseIsland.top &&
            bottomViewport.bottom == baseIsland.bottom,
        "horizontal overflow clipping must preserve its Dock-axis boundaries");
    const RECT bottomSeparatorHover =
        magnification::ExpandSeparatorHoverBounds(
            baseIsland, DockPosition::Bottom, 64);
    Check(bottomSeparatorHover.left == baseIsland.left &&
            bottomSeparatorHover.right == baseIsland.right &&
            bottomSeparatorHover.top == bottomViewport.top &&
            bottomSeparatorHover.bottom == baseIsland.bottom &&
            PtInRect(
                &bottomSeparatorHover,
                POINT{ 190, baseIsland.top - 1 }),
        "the separator hover corridor must continue above a bottom Dock");
    const POINT desktopSidePoint{
        190, baseIsland.top - 1
    };
    const RECT inactiveFocusBounds =
        magnification::ResolveFocusInteractionBounds(
            baseIsland, DockPosition::Bottom, 64, false);
    const RECT activeFocusBounds =
        magnification::ResolveFocusInteractionBounds(
            baseIsland, DockPosition::Bottom, 64, true);
    Check(!PtInRect(
                &inactiveFocusBounds,
                desktopSidePoint) &&
            PtInRect(
                &activeFocusBounds,
                desktopSidePoint),
        "desktop-side magnification bounds must retain active focus without acquiring it at a distance");
    const RECT leftViewport =
        magnification::ExpandPerpendicularBounds(
            baseIsland, DockPosition::Left, 64);
    Check(leftViewport.left == baseIsland.left &&
            leftViewport.right > baseIsland.right &&
            leftViewport.top == baseIsland.top &&
            leftViewport.bottom == baseIsland.bottom,
        "vertical overflow clipping must preserve its Dock-axis boundaries");
    const RECT horizontalOverflowViewport{ 80, 190, 300, 300 };
    const RECT leadingVisual{ 20, 190, 92, 300 };
    const RECT trailingVisual{ 260, 190, 340, 300 };
    const RECT fittedHorizontalViewport =
        magnification::FitOverflowViewportToFixedVisuals(
            horizontalOverflowViewport, DockPosition::Bottom,
            leadingVisual, trailingVisual, 12);
    Check(fittedHorizontalViewport.left == 104 &&
            fittedHorizontalViewport.right == 248,
        "horizontal overflow clipping must follow magnified fixed controls");
    const RECT verticalOverflowViewport{ 80, 190, 300, 500 };
    const RECT topVisual{ 80, 120, 300, 215 };
    const RECT bottomVisual{ 80, 450, 300, 560 };
    const RECT fittedVerticalViewport =
        magnification::FitOverflowViewportToFixedVisuals(
            verticalOverflowViewport, DockPosition::Left,
            topVisual, bottomVisual, 12);
    Check(fittedVerticalViewport.top == 227 &&
            fittedVerticalViewport.bottom == 438,
        "vertical overflow clipping must follow magnified fixed controls");
    const RECT scrollWaveViewport{ 80, 190, 300, 500 };
    const RECT firstScrollableBase{ 80, 200, 160, 276 };
    const RECT firstScrollableVisual{ 80, 209, 176, 294 };
    const RECT lastScrollableBase{ 80, 400, 160, 476 };
    const RECT lastScrollableVisual{ 80, 428, 176, 508 };
    const RECT movedScrollWaveViewport =
        magnification::MoveOverflowViewportWithScrollableVisuals(
            scrollWaveViewport, DockPosition::Left,
            firstScrollableBase, firstScrollableVisual,
            lastScrollableBase, lastScrollableVisual);
    Check(movedScrollWaveViewport.top == 199 &&
            movedScrollWaveViewport.bottom == 532,
        "overflow clipping must move with the expanded scrollable wave");
    const RECT horizontalScrollWaveViewport{ 80, 190, 400, 300 };
    const RECT firstHorizontalBase{ 100, 190, 176, 300 };
    const RECT firstHorizontalVisual{ 82, 176, 167, 300 };
    const RECT lastHorizontalBase{ 300, 190, 376, 300 };
    const RECT lastHorizontalVisual{ 324, 176, 409, 300 };
    const RECT movedHorizontalScrollWaveViewport =
        magnification::MoveOverflowViewportWithScrollableVisuals(
            horizontalScrollWaveViewport, DockPosition::Bottom,
            firstHorizontalBase, firstHorizontalVisual,
            lastHorizontalBase, lastHorizontalVisual);
    Check(movedHorizontalScrollWaveViewport.left == 62 &&
            movedHorizontalScrollWaveViewport.right == 433,
        "island overflow clipping must follow both ends of the hover wave");
    const RECT waveBounds{ 50, 150, 330, 330 };
    const RECT horizontalIsland =
        magnification::ExtendPanelAlongDockAxis(
            baseIsland, waveBounds, DockPosition::Bottom, 6);
    Check(horizontalIsland.left == waveBounds.left - 6 &&
            horizontalIsland.right == waveBounds.right + 6 &&
            horizontalIsland.top == baseIsland.top &&
            horizontalIsland.bottom == baseIsland.bottom,
        "horizontal Dock islands must preserve their end padding");
    const RECT verticalIsland =
        magnification::ExtendPanelAlongDockAxis(
            baseIsland, waveBounds, DockPosition::Left, 6);
    Check(verticalIsland.top == waveBounds.top - 6 &&
            verticalIsland.bottom == waveBounds.bottom + 6 &&
            verticalIsland.left == baseIsland.left &&
            verticalIsland.right == baseIsland.right,
        "vertical Dock islands must preserve their end padding");

    const DockWindowPreviewGrid single =
        CalculateDockWindowPreviewGrid(1, 1200, 700, 96);
    Check(single.columns == 1 && single.rows == 1,
        "a single window preview must use one card");
    Check(single.panelWidth <= 1200 && single.panelHeight <= 700,
        "single preview layout must stay inside the available area");
    const std::vector<RECT> singleCards =
        CalculateDockWindowPreviewCardRects(1, single, 96);
    Check(singleCards.size() == 1,
        "single preview layout must return one card rectangle");
    CheckRowMargins(single, singleCards, 0, 1,
        "single preview must have equal left and right margins");
    const RECT previewCloseButton =
        CalculateDockWindowPreviewCloseButtonRect(
            singleCards.front(), 96);
    Check(!IsRectEmpty(&previewCloseButton) &&
            previewCloseButton.left >
                singleCards.front().left &&
            previewCloseButton.right <
                singleCards.front().right &&
            previewCloseButton.top >=
                singleCards.front().top &&
            previewCloseButton.bottom <
                singleCards.front().bottom,
        "the preview close button must stay inside the title area at 100% DPI");
    const POINT previewCloseCenter{
        (previewCloseButton.left +
            previewCloseButton.right) / 2,
        (previewCloseButton.top +
            previewCloseButton.bottom) / 2
    };
    Check(IsPointInDockWindowPreviewCloseButton(
            previewCloseCenter,
            singleCards.front(), 96),
        "the close glyph center must use the dedicated close hit target");
    Check(!IsPointInDockWindowPreviewCloseButton(
            POINT{
                singleCards.front().left + 2,
                singleCards.front().bottom - 2
            },
            singleCards.front(), 96),
        "thumbnail content must not be mistaken for the close button");

    const DockWindowPreviewGrid multi =
        CalculateDockWindowPreviewGrid(2, 1200, 700, 96);
    Check(multi.columns >= 2 && multi.rows >= 1,
        "multiple windows must be arranged as a preview grid");
    Check(multi.columns * multi.rows >= 2,
        "preview grid must allocate a card for every window");
    Check(multi.panelWidth <= 1200 && multi.panelHeight <= 700,
        "multi-window preview layout must stay inside the available area");
    Check(multi.cardWidth <= 210 && multi.cardHeight <= 156,
        "default preview cards must use the compact dimensions");
    const std::vector<RECT> multiCards =
        CalculateDockWindowPreviewCardRects(2, multi, 96);
    CheckRowMargins(multi, multiCards, 0, 2,
        "two-window preview must have equal outer margins");

    const DockWindowPreviewGrid constrained =
        CalculateDockWindowPreviewGrid(5, 700, 420, 96);
    Check(constrained.rows > 1,
        "constrained multi-window previews must wrap to multiple rows");
    Check(constrained.panelWidth <= 700 &&
            constrained.panelHeight <= 420,
        "wrapped preview layout must stay inside the available area");
    const std::vector<RECT> constrainedCards =
        CalculateDockWindowPreviewCardRects(5, constrained, 96);
    CheckRowMargins(constrained, constrainedCards, 0,
        static_cast<size_t>(constrained.columns),
        "full preview row must have equal outer margins");
    const size_t finalRowStart =
        static_cast<size_t>(constrained.columns);
    CheckRowMargins(constrained, constrainedCards, finalRowStart,
        constrainedCards.size() - finalRowStart,
        "incomplete preview row must be centered");

    const DockWindowPreviewGrid highDpi =
        CalculateDockWindowPreviewGrid(5, 1050, 630, 144);
    const std::vector<RECT> highDpiCards =
        CalculateDockWindowPreviewCardRects(5, highDpi, 144);
    const size_t highDpiFinalRowStart =
        static_cast<size_t>(highDpi.columns);
    Check(highDpi.panelWidth <= 1050 &&
            highDpi.panelHeight <= 630,
        "high-DPI preview layout must stay inside the available area");
    CheckRowMargins(highDpi, highDpiCards,
        highDpiFinalRowStart,
        highDpiCards.size() - highDpiFinalRowStart,
        "high-DPI incomplete row must be centered");
    const RECT highDpiCloseButton =
        CalculateDockWindowPreviewCloseButtonRect(
            highDpiCards.front(), 144);
    Check(highDpiCloseButton.right -
                highDpiCloseButton.left >
            previewCloseButton.right -
                previewCloseButton.left,
        "the preview close target must scale with monitor DPI");

    namespace renameLayout =
        snowdesktop::dock_rename_layout;
    const RECT renameWorkArea{ 0, 0, 1920, 1080 };
    const RECT bottomRename =
        renameLayout::CalculateAdjacentEditRect(
            { 900, 1000, 980, 1080 },
            renameWorkArea, DockPosition::Bottom,
            180, 30, 6, 5);
    Check(bottomRename.bottom <= 994 &&
            bottomRename.right - bottomRename.left == 180 &&
            bottomRename.bottom - bottomRename.top == 30 &&
            (bottomRename.left + bottomRename.right) / 2 == 940,
        "bottom Dock rename editor must use the compact size above its icon");
    const RECT topRename =
        renameLayout::CalculateAdjacentEditRect(
            { 900, 0, 980, 80 },
            renameWorkArea, DockPosition::Top,
            180, 30, 6, 5);
    Check(topRename.top >= 86,
        "top Dock rename editor must appear below its icon");
    const RECT leftRename =
        renameLayout::CalculateAdjacentEditRect(
            { 0, 500, 80, 580 },
            renameWorkArea, DockPosition::Left,
            180, 30, 6, 5);
    Check(leftRename.left >= 86,
        "left Dock rename editor must appear to the icon's right");
    const RECT rightRename =
        renameLayout::CalculateAdjacentEditRect(
            { 1840, 500, 1920, 580 },
            renameWorkArea, DockPosition::Right,
            180, 30, 6, 5);
    Check(rightRename.right <= 1834,
        "right Dock rename editor must appear to the icon's left");
    const RECT clampedRename =
        renameLayout::CalculateAdjacentEditRect(
            { 0, 0, 40, 40 },
            renameWorkArea, DockPosition::Bottom,
            4000, 2000, 6, 5);
    Check(clampedRename.left >= renameWorkArea.left + 5 &&
            clampedRename.top >= renameWorkArea.top + 5 &&
            clampedRename.right <= renameWorkArea.right - 5 &&
            clampedRename.bottom <= renameWorkArea.bottom - 5,
        "Dock rename editor must remain inside the monitor work area");

    namespace keyMigration =
        snowdesktop::desktop_item_reference_migration;
    std::vector<DesktopWidget> mappedWidgets(2);
    mappedWidgets[0].itemKeys = {
        L"C:\\Users\\Test\\Desktop\\OLD.LNK",
        L"C:\\Users\\Test\\Desktop\\Other.lnk"
    };
    mappedWidgets[1].itemKeys = {
        L"c:\\users\\test\\desktop\\old.lnk"
    };
    std::vector<DockEntry> mappedDockEntries{
        { DockEntryType::DesktopItem,
            L"C:\\Users\\Test\\Desktop\\Old.lnk", false },
        { DockEntryType::Collection,
            L"C:\\Users\\Test\\Desktop\\Old.lnk", false }
    };
    const auto migratedReferences =
        keyMigration::MigrateReferences(
            mappedWidgets, mappedDockEntries,
            L"C:\\Users\\Test\\Desktop\\old.lnk",
            L"C:\\Users\\Test\\Desktop\\Renamed.lnk");
    Check(migratedReferences.widgetReferences == 2 &&
            migratedReferences.dockReferences == 1,
        "desktop rename must migrate every widget and Dock item reference");
    Check(mappedWidgets[0].itemKeys[0].ends_with(L"Renamed.lnk") &&
            mappedWidgets[1].itemKeys[0].ends_with(L"Renamed.lnk") &&
            mappedDockEntries[0].reference.ends_with(L"Renamed.lnk"),
        "desktop rename migration must write the new stable key");
    Check(mappedWidgets[0].itemKeys[1].ends_with(L"Other.lnk") &&
            mappedDockEntries[1].reference.ends_with(L"Old.lnk"),
        "desktop rename migration must preserve unrelated and collection references");

    std::vector<DockEntry> removableMappings{
        { DockEntryType::DesktopItem,
            L"C:\\Desktop\\Mapped.lnk", true },
        { DockEntryType::DesktopItem,
            L"C:\\Desktop\\Exclusive.lnk", false },
        { DockEntryType::Collection,
            L"collection-id", true }
    };
    Check(keyMigration::RemoveDockMappingAt(
            removableMappings, 0),
        "mapped Dock items must be removable without touching their source");
    Check(removableMappings.size() == 2 &&
            removableMappings[0].reference.ends_with(
                L"Exclusive.lnk"),
        "removing a Dock mapping must only erase the mapping entry");
    Check(!keyMigration::RemoveDockMappingAt(
            removableMappings, 0) &&
            !keyMigration::RemoveDockMappingAt(
                removableMappings, 1),
        "exclusive Dock items and collections must not use mapping removal");

    const RECT bottomAnchor{ 100, 300, 180, 380 };
    const RECT bottomPreview{ 20, 100, 300, 250 };
    Check(IsPointInDockPreviewTransitionRegion(
            { 140, 275 }, { 140, 340 }, bottomAnchor, bottomPreview,
            DockPosition::Bottom, 4),
        "bottom Dock preview must keep a triangular pointer path open");
    Check(!IsPointInDockPreviewTransitionRegion(
            { 10, 275 }, { 140, 340 }, bottomAnchor, bottomPreview,
            DockPosition::Bottom, 4),
        "bottom Dock preview triangle must reject distant side points");
    Check(IsPointInDockPreviewTransitionRegion(
            { 80, 275 }, { 105, 305 }, bottomAnchor, bottomPreview,
            DockPosition::Bottom, 12),
        "bottom Dock preview triangle must follow the actual icon exit point");

    const RECT topAnchor{ 100, 100, 180, 180 };
    const RECT topPreview{ 20, 230, 300, 380 };
    Check(IsPointInDockPreviewTransitionRegion(
            { 140, 205 }, { 140, 140 }, topAnchor, topPreview,
            DockPosition::Top, 4),
        "top Dock preview must keep a triangular pointer path open");

    const RECT leftAnchor{ 100, 100, 180, 180 };
    const RECT leftPreview{ 230, 20, 430, 300 };
    Check(IsPointInDockPreviewTransitionRegion(
            { 205, 140 }, { 140, 140 }, leftAnchor, leftPreview,
            DockPosition::Left, 4),
        "left Dock preview must keep a triangular pointer path open");
    Check(IsPointInDockPreviewTransitionRegion(
            { 205, 225 }, { 180, 100 }, leftAnchor, leftPreview,
            DockPosition::Left, 12),
        "left Dock preview must cover the complete icon-facing edge");
    Check(!IsPointInDockPreviewTransitionRegion(
            { 205, 10 }, { 140, 140 }, leftAnchor, leftPreview,
            DockPosition::Left, 4),
        "left Dock preview triangle must reject distant side points");

    const RECT rightAnchor{ 300, 100, 380, 180 };
    const RECT rightPreview{ 50, 20, 250, 300 };
    Check(IsPointInDockPreviewTransitionRegion(
            { 275, 140 }, { 340, 140 }, rightAnchor, rightPreview,
            DockPosition::Right, 4),
        "right Dock preview must keep a triangular pointer path open");

    DockPreviewHoverController hover;
    DockPreviewHoverTransition transition =
        hover.UpdateTarget(L"WORD@PRIMARY", false, false);
    Check(transition.armTimer && hover.TimerArmed(),
        "entering a preview target must arm the hover timer");
    transition = hover.UpdateTarget(
        L"WORD@PRIMARY", false, false);
    Check(!transition.armTimer,
        "moving inside the same icon must not restart the timer");
    Check(hover.ConsumeTimer(L"WORD@PRIMARY"),
        "matching hover timer must be accepted");
    hover.MarkPreviewShown(L"WORD@PRIMARY");
    transition = hover.UpdateTarget(
        L"WORD@PRIMARY", true, true);
    Check(transition.keepPreviewVisible,
        "matching visible preview must be kept open");

    Check(!hover.SuppressForActivation(),
        "activation after a shown preview must not report a pending timer");
    transition = hover.UpdateTarget(
        L"WORD@PRIMARY", false, false);
    Check(!transition.armTimer &&
            hover.SuppressedTarget() == L"WORD@PRIMARY",
        "activation must suppress reopening while pointer remains");
    hover.UpdateTarget(L"", false, false);
    Check(hover.SuppressedTarget().empty(),
        "leaving the icon must clear activation suppression");
    transition = hover.UpdateTarget(
        L"WORD@PRIMARY", false, false);
    Check(transition.armTimer,
        "re-entering after leave must arm a fresh hover timer");

    hover.Reset();
    hover.UpdateTarget(L"WORD@PRIMARY", false, false);
    transition = hover.UpdateTarget(
        L"EDGE@PRIMARY", false, false);
    Check(transition.cancelTimer && transition.armTimer,
        "switching icons must cancel and replace the hover timer");
    Check(hover.SuppressForActivation(),
        "activation while pending must cancel the hover timer");
    transition = hover.UpdateTarget(
        L"EDGE@PRIMARY", false, false);
    Check(!transition.armTimer,
        "pending activation suppression must block immediate reopening");

    // Restore must reuse the Dock icon's minimize/restore transition
    // animation; without a transition or an anchor only plain activation
    // works.
    Check(rules::ShouldAnimateDockWindowRestore(true, true, true),
        "minimized restore with transition and anchor must animate");
    Check(!rules::ShouldAnimateDockWindowRestore(false, true, true),
        "non-minimized restore must not animate");
    Check(!rules::ShouldAnimateDockWindowRestore(true, false, true),
        "missing transition must fall back to plain restore");
    Check(!rules::ShouldAnimateDockWindowRestore(true, true, false),
        "missing anchor must fall back to plain restore");

    // Dock magnification shifts the icon anchor while the preview is open;
    // the visible preview must follow in place instead of rebuilding.
    Check(rules::ShouldFollowDockPreviewAnchor(true, true, true),
        "visible matching preview must follow an anchor move");
    Check(!rules::ShouldFollowDockPreviewAnchor(false, true, true),
        "hidden preview must not follow anchors");
    Check(!rules::ShouldFollowDockPreviewAnchor(true, false, true),
        "different identity must not reuse the visible preview");
    Check(!rules::ShouldFollowDockPreviewAnchor(true, true, false),
        "stable anchor must not move the preview");

    if (failures == 0)
        std::cout << "All Dock and window rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
