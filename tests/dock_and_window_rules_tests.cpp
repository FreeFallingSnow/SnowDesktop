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
#include "dock_app_identity_rules.h"
#include "page_navigation_rules.h"
#include "page_layout_settings.h"
#include "dock_settings_rules.h"
#include "desktop_item_reference_migration.h"
#include "app/desktop_backdrop_update_rules.h"
#include "app/native_menu_presentation_rules.h"
#include "app/popup_window_pair_z_order.h"
#include "desktop_window_discovery_rules.h"
#include "floating_dock_rules.h"
#include "floating_popup_rules.h"
#include "drag_visual_rules.h"
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

#include <algorithm>
#include <array>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace rules = snowdesktop::dock_window_rules;
namespace identityRules = snowdesktop::dock_app_identity_rules;

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string source = contents.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
}

std::size_t CountOccurrences(
    const std::string& text,
    const std::string& needle)
{
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) !=
        std::string::npos)
    {
        ++count;
        offset += needle.size();
    }
    return count;
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

void CheckPopupWindowPairZOrderTransitions()
{
    constexpr DWORD extendedStyle =
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    HWND content = CreateWindowExW(
        extendedStyle, L"STATIC", L"popup-pair-content",
        WS_POPUP, 0, 0, 32, 32,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND backdrop = CreateWindowExW(
        extendedStyle, L"STATIC", L"popup-pair-backdrop",
        WS_POPUP, 0, 0, 32, 32,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND menu = CreateWindowExW(
        extendedStyle, L"STATIC", L"popup-pair-menu",
        WS_POPUP, 0, 0, 32, 32,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(content && backdrop && menu,
        "popup pair transition test windows are created");
    if (!content || !backdrop || !menu)
    {
        if (menu) DestroyWindow(menu);
        if (backdrop) DestroyWindow(backdrop);
        if (content) DestroyWindow(content);
        return;
    }

    const POINT origin{ 0, 0 };
    const SIZE size{ 32, 32 };
    const auto pairMatches = [&](bool topmost) {
        return snowdesktop::popup_window_pair_z_order::
                IsTopmost(content) == topmost &&
            snowdesktop::popup_window_pair_z_order::
                IsTopmost(backdrop) == topmost &&
            snowdesktop::popup_window_pair_z_order::
                IsPaired(content, backdrop);
    };

    Check(snowdesktop::popup_window_pair_z_order::Apply(
            content, backdrop, HWND_TOPMOST, true,
            origin, size) &&
            pairMatches(true),
        "popup pair promotion keeps content above its topmost backdrop");
    Check(snowdesktop::popup_window_pair_z_order::Apply(
            content, backdrop, HWND_NOTOPMOST, false,
            origin, size) &&
            pairMatches(false),
        "popup pair demotion moves both windows out of TOPMOST and preserves adjacency");
    Check(snowdesktop::popup_window_pair_z_order::Apply(
            content, backdrop, HWND_TOPMOST, true,
            origin, size) &&
            pairMatches(true),
        "popup pair can return to TOPMOST without exposing its backdrop");

    const auto isAbove = [](HWND upper, HWND lower) {
        for (HWND current = upper; current;
             current = GetWindow(current, GW_HWNDNEXT))
        {
            if (current == lower)
                return true;
        }
        return false;
    };
    Check(SetWindowPos(
            menu, HWND_TOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE &&
            isAbove(menu, content),
        "a menu probe can be placed above the synchronized popup pair");
    Check(snowdesktop::popup_window_pair_z_order::Apply(
            content, backdrop, HWND_TOPMOST, true,
            origin, size) &&
            pairMatches(true) &&
            isAbove(menu, content),
        "an idempotent popup layer refresh must not raise the pair above an existing menu");

    DestroyWindow(menu);
    DestroyWindow(backdrop);
    DestroyWindow(content);
}

} // namespace

int main(int argc, char** argv)
{
    CheckPopupWindowPairZOrderTransitions();
    const std::vector<std::wstring> savedPageOrder{
        L"page-1", L"page-2", L"page-3"};
    Check(snowdesktop::IsValidPageOrder(savedPageOrder,
              {L"page-3", L"page-1", L"page-2"}),
        "page settings must accept a complete permutation");
    Check(!snowdesktop::IsValidPageOrder(savedPageOrder,
              {L"page-1", L"page-1", L"page-3"}) &&
            !snowdesktop::IsValidPageOrder(savedPageOrder,
              {L"page-1", L"page-2"}) &&
            !snowdesktop::IsValidPageOrder(savedPageOrder,
              {L"page-1", L"page-2", L"unknown"}),
        "page settings must reject duplicate, missing, or unknown page ids");
    namespace dockDrop =
        snowdesktop::dock_drop_rules;
    namespace floatingDock =
        snowdesktop::floating_dock_rules;
    namespace floatingPopup =
        snowdesktop::floating_popup_rules;
    namespace dragVisual =
        snowdesktop::drag_visual_rules;
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
    namespace pageNavigation =
        snowdesktop::page_navigation_rules;

    {
        RECT previousEdge{};
        RECT nextEdge{};
        pageNavigation::BuildHotEdgeRects(
            { 100, 50, 1100, 850 }, 144,
            previousEdge, nextEdge);
        Check(previousEdge.left == 100 &&
                previousEdge.right == 112 &&
                previousEdge.top == 50 &&
                previousEdge.bottom == 850 &&
                nextEdge.left == 1088 &&
                nextEdge.right == 1100 &&
                pageNavigation::HotEdgeWidth(96) == 8 &&
                pageNavigation::HotEdgeWidth(144) == 12,
            "page navigation hot edges must span the supplied monitor bounds and scale from eight DIPs");

        Check(pageNavigation::HitTestPointerTarget(
                { 105, 100 },
                previousEdge, nextEdge) ==
                    pageNavigation::PointerTarget::PreviousEdge &&
                pageNavigation::HitTestPointerTarget(
                    { 1095, 700 },
                    previousEdge, nextEdge) ==
                    pageNavigation::PointerTarget::NextEdge &&
                pageNavigation::HitTestPointerTarget(
                    { 500, 400 },
                    previousEdge, nextEdge) ==
                    pageNavigation::PointerTarget::None &&
                pageNavigation::PointerTargetDirection(
                    pageNavigation::PointerTarget::PreviousEdge) == -1 &&
                pageNavigation::PointerTargetDirection(
                    pageNavigation::PointerTarget::NextEdge) == 1 &&
                pageNavigation::kHotEdgeHintDelayMs == 500,
            "page navigation must expose only full-height edge targets with a half-second hint delay");
        const auto dragRails =
            pageNavigation::ResolveHotEdgeRailVisibility(
                true, 0, true, true);
        const auto boundaryDragRails =
            pageNavigation::ResolveHotEdgeRailVisibility(
                true, 0, true, false);
        const auto passiveRail =
            pageNavigation::ResolveHotEdgeRailVisibility(
                false, 1, true, true);
        Check(dragRails.previous && dragRails.next &&
                boundaryDragRails.previous &&
                !boundaryDragRails.next &&
                !passiveRail.previous && passiveRail.next,
            "dragging must reveal every available page rail before the pointer reaches an edge");
        const RECT dragHintAtTop{ 120, 100, 480, 144 };
        const RECT dragHintLower{ 120, 130, 480, 174 };
        const RECT hiddenDragHint{};
        Check(!pageNavigation::NeedsDragHintPresent(
                    true, -1, dragHintAtTop,
                    -1, dragHintAtTop) &&
                pageNavigation::NeedsDragHintPresent(
                    true, -1, dragHintLower,
                    -1, dragHintAtTop) &&
                pageNavigation::NeedsDragHintPresent(
                    true, 0, hiddenDragHint,
                    -1, dragHintAtTop) &&
                pageNavigation::NeedsDragHintPresent(
                    false, 0, hiddenDragHint,
                    -1, dragHintAtTop),
            "drag dwell hints must redraw when their bounds move or when the hint disappears");
        Check(pageNavigation::ShortcutMatches(
                MOD_CONTROL, VK_PRIOR,
                MOD_CONTROL, VK_PRIOR) &&
                !pageNavigation::ShortcutMatches(
                    MOD_CONTROL, VK_PRIOR,
                    MOD_CONTROL | MOD_SHIFT, VK_PRIOR) &&
                !pageNavigation::ShortcutMatches(
                    0, VK_PRIOR, 0, VK_NEXT),
            "page navigation shortcuts must match the exact modifier set and virtual key");
        Check(pageNavigation::IsReservedDesktopSingleKey(
                    0, VK_RETURN) &&
                pageNavigation::IsReservedDesktopSingleKey(
                    0, VK_LEFT) &&
                pageNavigation::IsReservedDesktopSingleKey(
                    0, VK_F2) &&
                !pageNavigation::IsReservedDesktopSingleKey(
                    MOD_CONTROL, VK_LEFT) &&
                !pageNavigation::IsReservedDesktopSingleKey(
                    0, VK_PRIOR),
            "page navigation must reject unmodified desktop action keys but allow modified arrows and Page Up");
    }

    int maximumPageChecks = 0;
    const int maximumOffset = pageNavigation::MaximumOffset(
        8, 3,
        [&maximumPageChecks](std::size_t pageIndex) {
            ++maximumPageChecks;
            return pageIndex == 7;
        });
    Check(maximumOffset == 5 && maximumPageChecks == 1,
        "maximum page offset must stop after finding the last populated overflow page");
    int sparsePageChecks = 0;
    const int sparseMaximumOffset = pageNavigation::MaximumOffset(
        8, 3,
        [&sparsePageChecks](std::size_t pageIndex) {
            ++sparsePageChecks;
            return pageIndex == 4;
        });
    Check(sparseMaximumOffset == 2 && sparsePageChecks == 4,
        "maximum page offset must preserve sparse-page results while scanning backward");
    Check(pageNavigation::NextNonEmptyOffset(
            1, 1, 8, 3,
            [](std::size_t pageIndex) {
                return pageIndex == 4;
            }) == 2 &&
        pageNavigation::NextNonEmptyOffset(
            5, -1, 8, 3,
            [](std::size_t pageIndex) {
                return pageIndex == 4;
            }) == 2 &&
        pageNavigation::NextNonEmptyOffset(
            2, 1, 8, 3,
            [](std::size_t) { return false; }) == 2,
        "page navigation must find sparse pages in either direction and preserve the source offset on a miss");

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
        oleDrag::DwellTargetRefreshRoute::ExternalLocalFeedback,
        "dwell completion must locally refresh stationary external OLE feedback without re-entering the source drag loop");
    Check(
        oleDrag::SelectQueryContinueDragAction(
            true, true, true, true, true) ==
            oleDrag::QueryContinueDragAction::Cancel &&
        oleDrag::SelectQueryContinueDragAction(
            false, true, true, true, true) ==
            oleDrag::QueryContinueDragAction::ResumeNative &&
        oleDrag::SelectQueryContinueDragAction(
            false, false, true, true, true) ==
            oleDrag::QueryContinueDragAction::ResumeNative,
        "self OLE source continuation must prioritize Escape, then hand pressed or released internal returns back to native input");
    Check(
        oleDrag::SelectQueryContinueDragAction(
            false, true, true, true, false) ==
            oleDrag::QueryContinueDragAction::ContinueOle &&
        oleDrag::SelectQueryContinueDragAction(
            false, true, true, false, true) ==
            oleDrag::QueryContinueDragAction::ContinueOle &&
        oleDrag::SelectQueryContinueDragAction(
            false, true, false, true, true) ==
            oleDrag::QueryContinueDragAction::ContinueOle &&
        oleDrag::SelectQueryContinueDragAction(
            false, false, true, true, false) ==
            oleDrag::QueryContinueDragAction::Drop,
        "self OLE source continuation must wait for a live internal return and drop only after an external release");

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

    const RECT firstBackdropFrame{100, 900, 600, 980};
    const RECT shiftedBackdropFrame{96, 870, 608, 980};
    constexpr std::uintptr_t firstBackdropOwner = 0x101;
    constexpr std::uintptr_t secondBackdropOwner = 0x202;
    Check(
        backdropUpdate::PanelIdentityMatches(
            firstBackdropOwner, firstBackdropFrame,
            firstBackdropOwner, shiftedBackdropFrame) &&
        !backdropUpdate::PanelIdentityMatches(
            firstBackdropOwner, firstBackdropFrame,
            secondBackdropOwner, firstBackdropFrame) &&
        !backdropUpdate::PanelIdentityMatches(
            firstBackdropOwner, firstBackdropFrame,
            0, firstBackdropFrame) &&
        backdropUpdate::PanelIdentityMatches(
            0, firstBackdropFrame,
            0, firstBackdropFrame) &&
        !backdropUpdate::PanelIdentityMatches(
            0, firstBackdropFrame,
            0, shiftedBackdropFrame),
        "stable backdrop owners must update across geometry changes without colliding with another owner or legacy rectangle identity");
    RECT simulatedDockPanel = firstBackdropFrame;
    std::size_t simulatedDockPanelCount = 1;
    for (int step = 0; step < 1000; ++step)
    {
        const RECT nextFrame{
            100 - step % 17,
            900 - step % 31,
            600 + step % 19,
            980
        };
        if (!backdropUpdate::PanelIdentityMatches(
                firstBackdropOwner, simulatedDockPanel,
                firstBackdropOwner, nextFrame))
        {
            ++simulatedDockPanelCount;
        }
        simulatedDockPanel = nextFrame;
    }
    Check(simulatedDockPanelCount == 1,
        "one thousand Dock geometry updates with one stable owner must keep one backdrop panel identity");

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

    const displayRefresh::PageIdSet twoMappedPages{
        L"page-1", L"page-2" };
    const displayRefresh::PageIdSet oneMappedPage{ L"page-1" };
    const displayRefresh::PageIdSet noHiddenPages;
    const auto pageHiddenByContraction =
        displayRefresh::ReconcileHiddenPages(
            noHiddenPages, twoMappedPages, oneMappedPage);
    Check(pageHiddenByContraction.contains(L"page-2") &&
            !pageHiddenByContraction.contains(L"page-1") &&
            !pageHiddenByContraction.contains(L"virtual-page"),
        "display contraction must retain only pages that were previously mapped");
    const auto pageRestoredByExpansion =
        displayRefresh::ReconcileHiddenPages(
            pageHiddenByContraction, oneMappedPage, twoMappedPages);
    Check(pageRestoredByExpansion.empty(),
        "display expansion must clear the topology-hidden classification when a page is mapped again");

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

    GridPage dockWidgetTargetPage;
    dockWidgetTargetPage.id = L"dock-widget-target";
    dockWidgetTargetPage.workArea = { 0, 0, 640, 480 };
    dockWidgetTargetPage.columns = 8;
    dockWidgetTargetPage.rows = 6;
    dockWidgetTargetPage.cellWidth = 80;
    dockWidgetTargetPage.cellHeight = 80;
    dockWidgetTargetPage.itemPitchWidth = 80;
    dockWidgetTargetPage.itemPitchHeight = 80;
    dockWidgetTargetPage.marginX = 0;
    dockWidgetTargetPage.marginY = 0;
    const GridSpan dockWidgetSpan{ 3, 2 };
    const GridCell clampedDockWidgetCell =
        ClampGridCellToFitPage(
            dockWidgetTargetPage,
            { dockWidgetTargetPage.id, 7, 5 },
            dockWidgetSpan);
    Check(clampedDockWidgetCell.column == 5 &&
            clampedDockWidgetCell.row == 4 &&
            GridAreaFitsPage(
                dockWidgetTargetPage,
                clampedDockWidgetCell,
                dockWidgetSpan),
        "Dock widget drops at the page edge must move the anchor instead of shrinking the original span");

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
        popupLayout::RequiredListRowCount(0) == 5 &&
            popupLayout::RequiredListRowCount(3) == 5 &&
            popupLayout::RequiredListRowCount(8) == 8,
        "list collection popups must retain a five-row minimum height without clipping larger lists");
    const auto standardPopupMetrics =
        popupLayout::ResolveMetrics(
            92, 116, 84, 108, 1.0f, 34);
    Check(
        standardPopupMetrics.cellWidth == 92 &&
            standardPopupMetrics.cellHeight == 116 &&
            standardPopupMetrics.minimumListHeight == 34 &&
            standardPopupMetrics.paddingX == 18 &&
            standardPopupMetrics.headerHeight == 54 &&
            standardPopupMetrics.gapX == 10 &&
            standardPopupMetrics.gapY == 8 &&
            standardPopupMetrics.maximumWidth == 560 &&
            standardPopupMetrics.maximumHeight == 640,
        "standard collection popup geometry must retain its baseline dimensions");
    Check(
        popupLayout::ResolveMaximumHeight(
            standardPopupMetrics, 1080) == 640 &&
            popupLayout::ResolveMaximumHeight(
                standardPopupMetrics, 480) == 456,
        "collection popups must scroll at a scaled 640-pixel cap while still fitting smaller work areas");
    Check(
        popupLayout::DetailsVisible(
            true, true, false, false) &&
            !popupLayout::DetailsVisible(
                false, true, true, true) &&
            !popupLayout::DetailsVisible(
                true, false, false, false) &&
            popupLayout::ResolveListRowHeight(
                standardPopupMetrics, 16.0f) >= 38 &&
            popupLayout::ResolveDetailsHeaderHeight(
                standardPopupMetrics) == 30,
        "popup detail headers require list mode and list rows retain the shared compact metrics");
    const auto enlargedPopupMetrics =
        popupLayout::ResolveMetrics(
            138, 174, 132, 168, 1.5f);
    Check(
        enlargedPopupMetrics.cellWidth == 138 &&
            enlargedPopupMetrics.cellHeight == 174 &&
            enlargedPopupMetrics.paddingX == 27 &&
            enlargedPopupMetrics.headerHeight == 81 &&
            enlargedPopupMetrics.gapX == 15 &&
            enlargedPopupMetrics.gapY == 12 &&
            enlargedPopupMetrics.maximumWidth == 840 &&
            enlargedPopupMetrics.maximumHeight == 960 &&
            popupLayout::ResolveMaximumHeight(
                enlargedPopupMetrics, 1440) == 960,
        "enlarged page popups must preserve page cell size and scale their chrome");
    const auto constrainedPopupMetrics =
        popupLayout::ResolveMetrics(
            70, 90, 86, 112, 0.75f);
    Check(
        constrainedPopupMetrics.cellWidth == 86 &&
            constrainedPopupMetrics.cellHeight == 112 &&
            constrainedPopupMetrics.gapY == 6,
        "popup rows must remain large enough for the complete icon and title block");
    const auto invalidScalePopupMetrics =
        popupLayout::ResolveMetrics(
            -10, -20, 0, 0,
            std::numeric_limits<float>::quiet_NaN());
    Check(
        invalidScalePopupMetrics.scale == 1.0f &&
            invalidScalePopupMetrics.cellWidth == 1 &&
            invalidScalePopupMetrics.cellHeight == 1 &&
            invalidScalePopupMetrics.gapY == 8,
        "invalid popup page metrics must fall back to finite baseline chrome");
    for (const float scale : { 0.75f, 1.0f, 1.25f, 1.5f, 2.0f })
    {
        const auto headerBounds =
            popupLayout::ResolveHeaderVerticalBounds(scale);
        Check(
            std::abs(
                headerBounds.titleTop + headerBounds.titleBottom -
                headerBounds.sortButtonTop -
                headerBounds.sortButtonBottom) <= 1,
            "folder popup title and sort button must share a vertical center");
        Check(
            headerBounds.sortLabelOffsetY ==
                popupLayout::ScaleDimension(2, scale),
            "folder popup sort labels must retain their downward optical offset");
    }
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
    Check(snowdesktop::dock_settings_rules::
              ShouldReserveDesktopWorkArea(false, false) &&
            !snowdesktop::dock_settings_rules::
              ShouldReserveDesktopWorkArea(false, true) &&
            !snowdesktop::dock_settings_rules::
              ShouldReserveDesktopWorkArea(true, false),
        "desktop work-area reservation must use effective overlap while summon-only display is active");
    Check(snowdesktop::dock_settings_rules::
              IsFloatingEdgeSwipeEnabled(false, true) &&
            snowdesktop::dock_settings_rules::
              IsFloatingEdgeSwipeEnabled(true, false) &&
            !snowdesktop::dock_settings_rules::
              IsFloatingEdgeSwipeEnabled(false, false),
        "the low-level edge observer must follow effective edge-swipe enablement, including summon-only mode");

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
    const RECT ordinaryTitlelessIcon =
        itemVisual::ResolveVerticallyCenteredIconRect(
            ordinaryItemCells[0],
            itemVisual::ResolveGridItemIconRect(
                ordinaryItemCells[0], pageVisual));
    Check(ordinaryTitlelessIcon.right - ordinaryTitlelessIcon.left ==
                pageVisual.iconSize &&
            ordinaryTitlelessIcon.bottom - ordinaryTitlelessIcon.top ==
                pageVisual.iconSize &&
            std::abs(
                ordinaryTitlelessIcon.top +
                ordinaryTitlelessIcon.bottom -
                ordinaryItemCells[0].top -
                ordinaryItemCells[0].bottom) <= 1,
        "titleless Collection icons must keep the page icon edge length while centering vertically in their local cells");
    const RECT iconOnlyHighlight =
        itemVisual::ResolveIconOnlyHighlightRect(
            ordinaryItemCells[0],
            ordinaryTitlelessIcon,
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
            !titleless::IsActive(false, false, 2, 2) &&
            titleless::ResolveStoredMode(std::nullopt, true) &&
            !titleless::ResolveStoredMode(false, true) &&
            titleless::ResolveStoredMode(true, false),
        "the titleless option must affect only fixed non-compact Collection layouts");
    const auto denseTwoByTwo = titleless::ResolveDenseLayout(
        RECT{ 0, 0, 184, 232 }, 2, 2,
        57, 2, 2, 1.0f);
    Check(denseTwoByTwo.columns >= 3 &&
            denseTwoByTwo.rows >= 3 &&
            denseTwoByTwo.columns * denseTwoByTwo.rows > 4 &&
            denseTwoByTwo.iconSize >=
                denseTwoByTwo.minimumIconSize,
        "a default 2x2 titleless Collection must gain at least a 3x3 dense grid without shrinking icons below 80 percent");
    const auto denseLandscape = titleless::ResolveDenseLayout(
        RECT{ 0, 0, 300, 150 }, 2, 2,
        50, 2, 2, 1.0f);
    const auto densePortrait = titleless::ResolveDenseLayout(
        RECT{ 0, 0, 150, 300 }, 2, 2,
        50, 2, 2, 1.0f);
    Check(denseLandscape.columns > denseLandscape.rows &&
            densePortrait.rows > densePortrait.columns &&
            denseLandscape.columns * denseLandscape.rows ==
                densePortrait.columns * densePortrait.rows &&
            denseLandscape.iconSize >=
                denseLandscape.minimumIconSize &&
            densePortrait.iconSize >=
                densePortrait.minimumIconSize,
        "dense titleless layouts must maximize capacity without forcing rectangular widgets into square grids");
    const auto denseLargeIcons = titleless::ResolveDenseLayout(
        RECT{ 0, 0, 184, 232 }, 2, 2,
        70, 2, 2, 2.0f);
    Check(denseLargeIcons.iconSize >=
            denseLargeIcons.minimumIconSize &&
            denseLargeIcons.columns * denseLargeIcons.rows <=
                denseTwoByTwo.columns * denseTwoByTwo.rows,
        "larger icon and spacing settings must recompute density while preserving the 80 percent floor");
    const auto denseFallback = titleless::ResolveDenseLayout(
        RECT{ 0, 0, 60, 60 }, 2, 2,
        50, 2, 2, 1.0f);
    Check(denseFallback.columns == 2 &&
            denseFallback.rows == 2 &&
            denseFallback.iconSize <
                denseFallback.minimumIconSize,
        "an undersized frame with no qualified dense candidate must fall back to the original row and column counts");
    const RECT denseIconRect = itemVisual::ResolveCenteredIconRect(
        localLayout::ItemRect(denseTwoByTwo.geometry, 0),
        denseTwoByTwo.iconSize);
    Check(denseIconRect.right - denseIconRect.left ==
                denseTwoByTwo.iconSize &&
            denseIconRect.bottom - denseIconRect.top ==
                denseTwoByTwo.iconSize,
        "dense items, highlights, placeholders, and tooltip anchors must share one centered icon edge length");
    const auto denseVisualGaps = [](
        const titleless::DenseLayout& dense, bool horizontal) {
        const int count = horizontal
            ? dense.columns : dense.rows;
        std::vector<RECT> icons;
        icons.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            const size_t index = horizontal
                ? static_cast<size_t>(i)
                : static_cast<size_t>(i * dense.columns);
            icons.push_back(itemVisual::ResolveCenteredIconRect(
                localLayout::ItemRect(dense.geometry, index),
                dense.iconSize));
        }
        std::vector<int> gaps;
        gaps.reserve(static_cast<size_t>(count + 1));
        gaps.push_back(horizontal
            ? icons.front().left - dense.geometry.viewport.left
            : icons.front().top - dense.geometry.viewport.top);
        for (int i = 1; i < count; ++i)
        {
            gaps.push_back(horizontal
                ? icons[static_cast<size_t>(i)].left -
                    icons[static_cast<size_t>(i - 1)].right
                : icons[static_cast<size_t>(i)].top -
                    icons[static_cast<size_t>(i - 1)].bottom);
        }
        gaps.push_back(horizontal
            ? dense.geometry.viewport.right - icons.back().right
            : dense.geometry.viewport.bottom - icons.back().bottom);
        return gaps;
    };
    const auto denseOuterGaps = [&](
        const titleless::DenseLayout& dense) {
        const auto horizontal = denseVisualGaps(dense, true);
        const auto vertical = denseVisualGaps(dense, false);
        return std::array<int, 4>{
            horizontal.front(), horizontal.back(),
            vertical.front(), vertical.back()
        };
    };
    const auto hasUnifiedOuterPadding = [&](
        const titleless::DenseLayout& dense) {
        const auto gaps = denseOuterGaps(dense);
        const auto [minimum, maximum] =
            std::minmax_element(gaps.begin(), gaps.end());
        return *maximum - *minimum <= 1 &&
            std::all_of(gaps.begin(), gaps.end(),
                [&](int gap) {
                    return std::abs(gap - dense.outerPadding) <= 1;
                });
    };
    const auto hasEvenInternalDistribution = [&](
        const titleless::DenseLayout& dense, bool horizontal) {
        const auto gaps = denseVisualGaps(dense, horizontal);
        if (gaps.size() <= 3)
            return true;
        const auto [minimum, maximum] = std::minmax_element(
            gaps.begin() + 1, gaps.end() - 1);
        return *maximum - *minimum <= 1;
    };
    Check(hasUnifiedOuterPadding(denseTwoByTwo) &&
            hasUnifiedOuterPadding(denseLandscape) &&
            hasUnifiedOuterPadding(densePortrait) &&
            hasEvenInternalDistribution(denseTwoByTwo, true) &&
            hasEvenInternalDistribution(denseTwoByTwo, false) &&
            hasEvenInternalDistribution(denseLandscape, true) &&
            hasEvenInternalDistribution(denseLandscape, false) &&
            hasEvenInternalDistribution(densePortrait, true) &&
            hasEvenInternalDistribution(densePortrait, false),
        "dense compact grids must use one visual padding on all four edges and distribute each axis remainder evenly");
    const auto denseSingleColumn = titleless::ResolveDenseLayout(
        RECT{ 0, 0, 60, 150 }, 1, 2,
        50, 2, 2, 1.0f);
    const auto denseSingleRow = titleless::ResolveDenseLayout(
        RECT{ 0, 0, 150, 60 }, 2, 1,
        50, 2, 2, 1.0f);
    Check(denseSingleColumn.columns == 1 &&
            denseSingleRow.rows == 1 &&
            hasUnifiedOuterPadding(denseSingleColumn) &&
            hasUnifiedOuterPadding(denseSingleRow),
        "single-column and single-row compact Collections must share the same horizontal and vertical edge padding");
    const auto ordinaryGrid = localLayout::ResolveGrid(
        RECT{ 0, 0, 184, 232 }, 2, 2,
        61, 80, 1.0f);
    Check(ordinaryGrid.horizontal.visualItemSize == 0 &&
            ordinaryGrid.horizontal.visualOuterPadding < 0 &&
            ordinaryGrid.vertical.visualItemSize == 0 &&
            ordinaryGrid.vertical.visualOuterPadding < 0,
        "ordinary Collection grids must keep the original bounded-track geometry");
    Check(!titleless::IsHandoffDwellReady(
              false, true, 600, 520) &&
            !titleless::IsHandoffDwellReady(
              true, false, 519, 520) &&
            titleless::IsHandoffDwellReady(
              true, false, 520, 520) &&
            titleless::IsHandoffDwellReady(
              true, true, 0, 520),
        "compact Collection handoff must require one stable target until the dwell delay expires");
    Check(titleless::CollectionOwnsHandoffDwell(
              L"collection-a", L"collection-a") &&
            !titleless::CollectionOwnsHandoffDwell(
              L"collection-a", L"collection-b") &&
            !titleless::CollectionOwnsHandoffDwell(
              L"", L"collection-a"),
        "only the Collection that owns titleless handoff dwell may reset the shared timer state");
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
    const RECT listInsertionClip =
        popupDrag::ExpandInsertionClipVertically(
            popupContent, popupBounds, 7);
    Check(
        listInsertionClip.top == 47 &&
            listInsertionClip.bottom == 209 &&
            listInsertionClip.left == popupContent.left &&
            listInsertionClip.right == popupContent.right,
        "list popup insertion clipping must reserve top and bottom indicator gutters");
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
    const RECT listRowBounds{ 18, 80, 302, 118 };
    Check(
        !popupDrag::IsAfterInsertionMidpoint(
            listRowBounds, POINT{ 280, 90 }, true) &&
            popupDrag::IsAfterInsertionMidpoint(
                listRowBounds, POINT{ 20, 110 }, true) &&
            !popupDrag::IsAfterInsertionMidpoint(
                listRowBounds, POINT{ 40, 110 }, false) &&
            popupDrag::IsAfterInsertionMidpoint(
                listRowBounds, POINT{ 280, 90 }, false),
        "list popup rows must resolve insertion halves vertically while grids remain horizontal");
    const long long listLeadingDistance =
        popupDrag::InsertionEdgeDistanceSquared(
            listRowBounds, listRowBounds,
            POINT{ 160, 125 }, true, false, 3);
    const long long listTrailingDistance =
        popupDrag::InsertionEdgeDistanceSquared(
            listRowBounds, listRowBounds,
            POINT{ 160, 125 }, true, true, 3);
    Check(
        listTrailingDistance < listLeadingDistance,
        "the blank area below the last list row must resolve to its trailing insertion edge");

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
        dockDrop::IsDockHandoffDwellIdle(
            static_cast<size_t>(-1), 0, false) &&
        !dockDrop::IsDockHandoffDwellIdle(0, 0, false) &&
        !dockDrop::IsDockHandoffDwellIdle(
            static_cast<size_t>(-1), 1, false) &&
        !dockDrop::IsDockHandoffDwellIdle(
            static_cast<size_t>(-1), 0, true),
        "Dock handoff cleanup may be skipped only for the complete canonical idle state");
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
    const std::array<long, 2> insertionMidpoints{ 40, 80 };
    const auto insertionMidpointAt =
        [&](size_t index) {
            return insertionMidpoints[index - 3];
        };
    Check(dockDrop::ResolveRedirectedInsertionIndex(
              20, 3, 5, insertionMidpointAt) == 3 &&
            dockDrop::ResolveRedirectedInsertionIndex(
              40, 3, 5, insertionMidpointAt) == 4 &&
            dockDrop::ResolveRedirectedInsertionIndex(
              79, 3, 5, insertionMidpointAt) == 4 &&
            dockDrop::ResolveRedirectedInsertionIndex(
              80, 3, 5, insertionMidpointAt) == 5 &&
            dockDrop::ResolveRedirectedInsertionIndex(
              20, 7, 7, insertionMidpointAt) == 7,
        "non-sortable Dock areas must redirect to the nearest real insertion boundary");
    Check((floatingDock::kWindowExStyle & WS_EX_TOPMOST) == 0,
        "floating Dock uses SetWindowPos to stay topmost instead of fixing WS_EX_TOPMOST to its window style");
    Check((floatingDock::kWindowExStyle & WS_EX_NOACTIVATE) != 0,
        "the floating Dock must not steal foreground activation");
    Check(floatingDock::ShouldSummonForDockSurface(
            true, false) &&
            !floatingDock::ShouldSummonForDockSurface(
                true, true) &&
            !floatingDock::ShouldSummonForDockSurface(
                false, false),
        "a Dock-associated popup must summon only a hidden floating Dock");
    Check(floatingDock::ShouldDispatchDockContextMenu(
              false, false) &&
            floatingDock::ShouldDispatchDockContextMenu(
              true, true) &&
            !floatingDock::ShouldDispatchDockContextMenu(
              true, false),
        "an active persistent DockHost must require a right-button press that began on the same Host while the desktop fallback remains usable");
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
    Check((dragVisual::kPreviewWindowExStyle &
              WS_EX_NOACTIVATE) != 0 &&
            (dragVisual::kPreviewWindowExStyle &
              WS_EX_TRANSPARENT) != 0 &&
            (dragVisual::kPreviewWindowExStyle &
              WS_EX_TOPMOST) == 0,
        "the drag preview must pass input without activating and enter the topmost band explicitly");
    Check(dragVisual::ShouldShowPreview(
              true, true, true) &&
            !dragVisual::ShouldShowPreview(
              false, true, true) &&
            !dragVisual::ShouldShowPreview(
              true, false, true) &&
            !dragVisual::ShouldShowPreview(
              true, true, false),
        "the independent drag preview must follow the drag session visibility contract");
    Check(dragVisual::ShouldSyncPreviewBeforePresentation(false) &&
            !dragVisual::ShouldSyncPreviewBeforePresentation(true),
        "presentation must sync the drag preview exactly when the current input path has not already done so");
    Check(!dragVisual::ShouldCompactPreview(1) &&
            dragVisual::ShouldCompactPreview(2) &&
            dragVisual::kMaximumStackedPreviewItems == 4 &&
            dragVisual::kStackedPreviewOffset > 0,
        "multi-item drag previews must use a bounded compact stack");
    Check(!dragVisual::ShouldSkipPreviewFallbackCandidate(
              true, true, false, false) &&
            dragVisual::ShouldSkipPreviewFallbackCandidate(
              false, true, false, false) &&
            dragVisual::ShouldSkipPreviewFallbackCandidate(
              true, false, false, false) &&
            dragVisual::ShouldSkipPreviewFallbackCandidate(
              true, true, true, false) &&
            dragVisual::ShouldSkipPreviewFallbackCandidate(
              true, true, false, true),
        "manual preview fallback must ignore unavailable and presentation-only windows");
    Check(dragVisual::IsLayeredTransparentPresentationWindow(
              WS_EX_LAYERED | WS_EX_TRANSPARENT) &&
            dragVisual::IsLayeredTransparentPresentationWindow(
              kDockWindowTransitionExStyle) &&
            !dragVisual::IsLayeredTransparentPresentationWindow(
              WS_EX_LAYERED) &&
            !dragVisual::IsLayeredTransparentPresentationWindow(
              WS_EX_TRANSPARENT),
        "manual preview fallback must match the system passthrough semantics of layered transparent presentation windows");
    Check(dragVisual::PreviewFallbackRegionContainsPoint(
              ERROR, false) &&
            !dragVisual::PreviewFallbackRegionContainsPoint(
              NULLREGION, true) &&
            dragVisual::PreviewFallbackRegionContainsPoint(
              SIMPLEREGION, true) &&
            !dragVisual::PreviewFallbackRegionContainsPoint(
              SIMPLEREGION, false) &&
            dragVisual::PreviewFallbackRegionContainsPoint(
              COMPLEXREGION, true) &&
            !dragVisual::PreviewFallbackRegionContainsPoint(
              COMPLEXREGION, false),
        "manual preview fallback must preserve rectangular windows and reject points outside explicit or empty regions");
    Check(!dragVisual::PreviewFallbackNeedsExactRegionCheck(ERROR) &&
            !dragVisual::PreviewFallbackNeedsExactRegionCheck(NULLREGION) &&
            !dragVisual::PreviewFallbackNeedsExactRegionCheck(SIMPLEREGION) &&
            dragVisual::PreviewFallbackNeedsExactRegionCheck(COMPLEXREGION),
        "only complex window regions require allocating and testing an exact fallback region");
    const RECT appliedPreviewBounds{100, 200, 180, 280};
    const RECT movedPreviewBounds{101, 200, 181, 280};
    const RECT resizedPreviewBounds{100, 200, 181, 280};
    Check(dragVisual::ShouldApplyPreviewWindowPlacement(
              true, false,
              appliedPreviewBounds, appliedPreviewBounds) &&
            dragVisual::ShouldApplyPreviewWindowPlacement(
              false, true,
              appliedPreviewBounds, appliedPreviewBounds) &&
            !dragVisual::ShouldApplyPreviewWindowPlacement(
              true, true,
              appliedPreviewBounds, appliedPreviewBounds) &&
            dragVisual::ShouldApplyPreviewWindowPlacement(
              true, true,
              appliedPreviewBounds, movedPreviewBounds) &&
            dragVisual::ShouldApplyPreviewWindowPlacement(
              true, true,
              appliedPreviewBounds, resizedPreviewBounds),
        "drag preview placement must reapply only for first show, re-show, movement, or resize");
    const auto hiddenPreviewZOrder =
        dragVisual::ResolvePreviewWindowZOrderPolicy(false);
    const auto visiblePreviewZOrder =
        dragVisual::ResolvePreviewWindowZOrderPolicy(true);
    Check(hiddenPreviewZOrder.insertAfter == HWND_TOPMOST &&
            (hiddenPreviewZOrder.flags & SWP_NOZORDER) == 0 &&
            visiblePreviewZOrder.insertAfter == HWND_TOPMOST &&
            (visiblePreviewZOrder.flags & SWP_NOZORDER) == 0,
        "every drag preview placement must reassert topmost above Dock popups opened later");
    Check(dragVisual::DropPreviewBelongsToRenderSurface(
              true, true, true) &&
            !dragVisual::DropPreviewBelongsToRenderSurface(
              false, true, true) &&
            dragVisual::DropPreviewBelongsToRenderSurface(
              false, true, false) &&
            !dragVisual::DropPreviewBelongsToRenderSurface(
              true, true, false),
        "Dock drop guidance must have exactly one owning render surface");
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
        "floating layers must preserve the default edge-highlight overdraw");
    const RECT maximumFloatingBorderOverdraw =
        floatingDock::ExpandForBorderOverdraw(
            floatingDockRect,
            kMaximumWidgetBorderWidth);
    Check(maximumFloatingBorderOverdraw.left ==
            floatingDockRect.left - 3 &&
            maximumFloatingBorderOverdraw.top ==
                floatingDockRect.top - 3 &&
            maximumFloatingBorderOverdraw.right ==
                floatingDockRect.right + 3 &&
            maximumFloatingBorderOverdraw.bottom ==
                floatingDockRect.bottom + 3,
        "floating layers must expand for the maximum configured visual-edge width");
    Check((floatingPopup::kWindowExStyle &
            WS_EX_TOPMOST) == 0 &&
            (floatingPopup::kWindowExStyle &
                WS_EX_NOACTIVATE) != 0 &&
            (floatingPopup::kWindowExStyle &
                WS_EX_TOOLWINDOW) != 0,
        "the shared popup host must be no-activate and enter the topmost band dynamically");
    Check(floatingPopup::HostsCollectionPopup(true) &&
            !floatingPopup::HostsCollectionPopup(false) &&
            floatingPopup::HostsLuaPanel(true) &&
            !floatingPopup::HostsLuaPanel(false),
        "the shared popup host must own every collection popup and Lua panel");
    Check(floatingPopup::ShouldShow(true, false) &&
            floatingPopup::ShouldShow(false, true) &&
            !floatingPopup::ShouldShow(false, false),
        "the shared popup host must remain visible while either hosted layer is open");
    Check(floatingPopup::ShouldRevealHost(false, true) &&
            !floatingPopup::ShouldRevealHost(false, false) &&
            !floatingPopup::ShouldRevealHost(true, true) &&
            !floatingPopup::ShouldRevealHost(true, false),
        "a hidden shared popup host must remain concealed during staging and reveal only after a present request");
    Check(!floatingPopup::
              ShouldCancelPointerPressForHostMessage(
                  true, 101u, 202u, true, false) &&
            floatingPopup::
              ShouldCancelPointerPressForHostMessage(
                  true, 101u, 101u, true, false) &&
            floatingPopup::
              ShouldCancelPointerPressForHostMessage(
                  true, 101u, 0u, false, false) &&
            !floatingPopup::
              ShouldCancelPointerPressForHostMessage(
                  false, 101u, 101u, true, true) &&
            floatingPopup::
              ShouldCancelPointerPressForHostMessage(
                  false, 101u, 101u, true, false),
        "an old popup host cancel message must not clear a press whose capture has transferred to another owned host");
    Check(floatingPopup::ShouldBeTopmost(true, 0) &&
            !floatingPopup::ShouldBeTopmost(true, 1) &&
            !floatingPopup::ShouldBeTopmost(false, 0),
        "native menus must temporarily outrank the shared popup host");
    Check(floatingPopup::ResolveMenuZOrderOwner(
              true, 101, true, false, 202) == 101 &&
            floatingPopup::ResolveMenuZOrderOwner(
              false, 101, true, true, 202) == 202 &&
            floatingPopup::ResolveMenuZOrderOwner(
              true, 0, true, true, 202) == 202 &&
            floatingPopup::ResolveMenuZOrderOwner(
              false, 101, true, false, 202) == 0 &&
            floatingPopup::ResolveMenuZOrderOwner(
              false, 101, false, true, 202) == 0,
        "modern menus must prefer the shared popup as their Z-order owner, fall back only to an effectively floating visible Dock, and exclude desktop-band or hidden DockHosts");
    const POINT popupAnimationOffset =
        floatingPopup::AnimationVisualOffset(
            RECT{ 460, 280, 1260, 880 },
            RECT{ 440, 250, 1280, 900 });
    Check(popupAnimationOffset.x == 20 &&
            popupAnimationOffset.y == 30,
        "popup animation snapshots must use shared-host local coordinates");
    const RECT survivingAnimationBounds{ 920, 260, 1320, 660 };
    const RECT unionPopupHost{ 400, 180, 1340, 900 };
    const RECT survivingPopupHost{ 900, 240, 1340, 680 };
    const POINT unionAnimationOffset =
        floatingPopup::AnimationVisualOffset(
            survivingAnimationBounds, unionPopupHost);
    const POINT rebasedAnimationOffset =
        floatingPopup::AnimationVisualOffset(
            survivingAnimationBounds, survivingPopupHost);
    Check(unionPopupHost.left + unionAnimationOffset.x ==
                survivingPopupHost.left +
                    rebasedAnimationOffset.x &&
            unionPopupHost.top + unionAnimationOffset.y ==
                survivingPopupHost.top +
                    rebasedAnimationOffset.y,
        "a surviving popup animation must keep its desktop position when the shared host shrinks after the other popup closes");
    const POINT externalPointerPoint{ -123456, 234567 };
    const std::uint64_t externalPointerPayload =
        floatingPopup::PackScreenPoint(
            externalPointerPoint);
    const POINT restoredExternalPointerPoint =
        floatingPopup::UnpackScreenPoint(
            externalPointerPayload);
    Check(restoredExternalPointerPoint.x ==
                externalPointerPoint.x &&
            restoredExternalPointerPoint.y ==
                externalPointerPoint.y,
        "each popup hook notification must preserve its full signed screen coordinate snapshot");
    Check(floatingPopup::IsCurrentPointerNotification(41, 41) &&
            !floatingPopup::IsCurrentPointerNotification(41, 42) &&
            !floatingPopup::IsCurrentPointerNotification(0, 0),
        "a queued outside-click may affect only the popup content generation that existed at button-down");
    const RECT collectionPopupBounds{ 10, 20, 110, 120 };
    const RECT luaPanelBounds{ 200, 220, 320, 360 };
    Check(floatingPopup::IsPointOnHostedPopupSurface(
            POINT{ 40, 60 }, collectionPopupBounds,
            luaPanelBounds) &&
            floatingPopup::IsPointOnHostedPopupSurface(
                POINT{ 250, 300 }, collectionPopupBounds,
                luaPanelBounds) &&
            floatingPopup::IsPointOnHostedPopupSurface(
                POINT{ 8, 60 }, collectionPopupBounds,
                luaPanelBounds),
        "collection content, Lua panels and their host shadow margin must count as owned popup input");
    Check(!floatingPopup::IsPointOnHostedPopupSurface(
            POINT{ 150, 180 }, collectionPopupBounds,
            luaPanelBounds) &&
            !floatingPopup::IsPointOnHostedPopupSurface(
                POINT{ 0, 0 }, RECT{}, RECT{}),
        "points outside every non-empty hosted region must remain external popup input");
    Check(floatingPopup::IsInternalPointerTarget(true, false) &&
            !floatingPopup::IsInternalPointerTarget(false, false) &&
            !floatingPopup::IsInternalPointerTarget(true, true),
        "an application-level settings window must remain external to desktop popup ownership even in the current process");
    Check(floatingPopup::ShouldDismissForExternalPointerDown(
            true, false, false),
        "an external application press must dismiss a visible shared popup");
    Check(!floatingPopup::ShouldDismissForExternalPointerDown(
            true, true, false) &&
            !floatingPopup::ShouldDismissForExternalPointerDown(
                false, false, false) &&
            !floatingPopup::ShouldDismissForExternalPointerDown(
                true, false, true),
        "own-process presses, hidden popups and active drags must not trigger external dismissal");
    Check(floatingPopup::ShouldReleaseRecordedPanelCapture(
              101u, 101u) &&
            !floatingPopup::ShouldReleaseRecordedPanelCapture(
                101u, 202u) &&
            !floatingPopup::ShouldReleaseRecordedPanelCapture(
                0u, 0u),
        "Lua panel teardown may release only its recorded non-null capture host");
    Check(floatingPopup::
            ShouldDismissLuaPanelForExternalPointerDown(
                true, true, false, false) &&
            !floatingPopup::
                ShouldDismissLuaPanelForExternalPointerDown(
                    true, false, false, false),
        "Lua panels must preserve their dismiss-on-outside contract for external presses");
    const POINT mappedFloatingPoint =
        floatingDock::WindowPointToDesktopPoint(
            POINT{ 12, 34 }, floatingDockRect);
    Check(mappedFloatingPoint.x == 112 &&
            mappedFloatingPoint.y == 934,
        "floating-window input must map back to desktop coordinates");
    const RECT associatedPopupRect =
        floatingDock::DockAssociatedPopupInteractionRect(
            true, floatingPopupRect);
    const RECT unrelatedPopupRect =
        floatingDock::DockAssociatedPopupInteractionRect(
            false, floatingPopupRect);
    Check(EqualRect(
            &associatedPopupRect,
            &floatingPopupRect) != FALSE &&
            IsRectEmpty(&unrelatedPopupRect) != FALSE,
        "only a shared popup anchored to the Dock may extend the floating Dock interaction surface");
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
        "one external press may independently dismiss both a Dock-associated popup and its floating Dock");
    Check(floatingDock::ShouldDismissForPointerDown(
            false, false, POINT{ 20, 20 },
            floatingDockRect, floatingPopupRect,
            previewPanelRect),
        "an external click must dismiss the floating host");
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
    Check(floatingDock::HasPointerButtonActivity(
              1, 0, 0) &&
            floatingDock::HasPointerButtonActivity(
              1, 1, 0) &&
            floatingDock::HasPointerButtonActivity(
              0, 1, 0) &&
            floatingDock::HasPointerButtonActivity(
              0, 0, 1) &&
            !floatingDock::HasPointerButtonActivity(
              0, 0, 0),
        "edge gestures must exclude pointer press, hold, release and between-sample click activity");
    Check(floatingDock::HasPointerButtonActivity(
              0, 0, 0, true) &&
            !floatingDock::HasPointerButtonActivity(
              0, 0, 0, false),
        "a low-level mouse event must preserve pointer-button activity even when every sampled key state is idle");
    Check(floatingDock::IsGuiMenuModeActive(GUI_INMENUMODE) &&
            floatingDock::IsGuiMenuModeActive(
              GUI_SYSTEMMENUMODE) &&
            floatingDock::IsGuiMenuModeActive(
              GUI_POPUPMENUMODE) &&
            !floatingDock::IsGuiMenuModeActive(0),
        "native menu, system-menu and popup-menu loops must suspend edge gestures");
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
    const RECT floatingTooltipRect{ 120, 840, 280, 890 };
    Check(floatingDock::IsTooltipOnlyPoint(
              POINT{ 180, 860 },
              floatingDockRect,
              floatingPopupRect,
              floatingTooltipRect) &&
            !floatingDock::IsTooltipOnlyPoint(
                POINT{ 150, 930 },
                floatingDockRect,
                floatingPopupRect,
                floatingTooltipRect) &&
            !floatingDock::IsTooltipOnlyPoint(
                POINT{ 300, 600 },
                floatingDockRect,
                floatingPopupRect,
                RECT{ 260, 560, 360, 660 }) &&
            !floatingDock::IsTooltipOnlyPoint(
                POINT{ 20, 20 },
                floatingDockRect,
                floatingPopupRect,
                RECT{}),
        "only the visual title chip outside interactive Dock and popup regions may pass through input");
    Check(!floatingDock::ShouldRenderDesktopDock(
            true, true),
        "only the Dock mirrored by the floating host must be hidden");
    Check(floatingDock::ShouldRenderDesktopDock(
            true, false),
        "Docks on other monitors must remain visible");
    Check(floatingDock::ShouldRenderDesktopDock(
            false, true),
        "the desktop Dock remains visible until its persistent Host owns the visual");
    Check(floatingDock::ShouldRenderFloatingDockFrame(
            true) &&
            !floatingDock::ShouldRenderFloatingDockFrame(
                false),
        "the persistent DockHost renders in both desktop and floating bands while active");
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
        "drag feedback changes, widget previews and marquees must synchronously present pointer frames");
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
    Check(floatingDock::ShouldCloseCollectionPopup(
            3, 3),
        "clicking the collection that owns the open popup must close it");
    Check(!floatingDock::ShouldCloseCollectionPopup(
            3, 3, false),
        "the same collection on another DockHost must switch the popup instead of toggling the old owner");
    Check(!floatingDock::ShouldCloseCollectionPopup(
            3, 4),
        "clicking a different collection must replace the open popup");
    Check(!floatingDock::
            ShouldCloseCollectionPopupOnPointerDown(
                3, 3, false),
        "the owning collection button must defer closing until release");
    Check(floatingDock::
            ShouldCloseCollectionPopupOnPointerDown(
                3, 3, false, false),
        "the same collection on another DockHost must begin replacing the old popup on press");
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

    floatingDock::EdgeSwipeDetector buttonSuppressedSwipe;
    buttonSuppressedSwipe.SuppressUntilEdgeLeave();
    Check(!buttonSuppressedSwipe.Update(
              POINT{ -1500, 1079 },
              negativeBottomMonitor,
              DockPosition::Bottom,
              100, 4, 72) &&
            buttonSuppressedSwipe.IsAwaitingEdgeLeave(),
        "a pointer-button interaction at the edge must suppress swipe arming");
    Check(!buttonSuppressedSwipe.Update(
              POINT{ -1400, 1079 },
              negativeBottomMonitor,
              DockPosition::Bottom,
              180, 4, 72) &&
            buttonSuppressedSwipe.IsAwaitingEdgeLeave(),
        "movement along the same edge must not clear button suppression");
    Check(!buttonSuppressedSwipe.Update(
              POINT{ -1400, 1060 },
              negativeBottomMonitor,
              DockPosition::Bottom,
              200, 4, 72) &&
            !buttonSuppressedSwipe.IsAwaitingEdgeLeave(),
        "leaving the edge must clear pointer-button suppression");
    Check(!buttonSuppressedSwipe.Update(
              POINT{ -1500, 1079 },
              negativeBottomMonitor,
              DockPosition::Bottom,
              220, 4, 72) &&
            buttonSuppressedSwipe.Update(
              POINT{ -1400, 1079 },
              negativeBottomMonitor,
              DockPosition::Bottom,
              300, 4, 72),
        "a fresh buttonless edge stroke must work after leaving and returning");

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

    Check(!floatingDock::IsDockEffectivelyPromoted(
              false, true, false) &&
            floatingDock::IsDockEffectivelyPromoted(
              false, true, true) &&
            floatingDock::IsDockEffectivelyPromoted(
              true, false, false),
        "passive drag reveal must borrow the floating band only in summon-only mode while manual promotion remains unconditional");
    Check(floatingDock::ShouldShowPersistentDockHost(
              true, false, false, true, false, false) &&
            !floatingDock::ShouldShowPersistentDockHost(
              true, false, true, true, false, false) &&
            floatingDock::ShouldShowPersistentDockHost(
              true, true, true, false, true, false),
        "summon-only mode must hide idle Hosts but retain every manually or passively floating Host");
    Check(!floatingDock::ShouldPassivelyRevealDockForDragAtEdge(
              true, false, false) &&
            floatingDock::ShouldPassivelyRevealDockForDragAtEdge(
              true, true, false) &&
            floatingDock::ShouldPassivelyRevealDockForDragAtEdge(
              true, false, true) &&
            !floatingDock::ShouldPassivelyRevealDockForDragAtEdge(
              false, true, true),
        "edge contact must passively reveal a summon-only Dock only during an internal or OLE drag");

    const RECT bottomDockScreen{ -1500, 1000, -900, 1060 };
    Check(floatingDock::IsPointInDockEdgeProjection(
              POINT{ -1200, 1079 }, negativeBottomMonitor,
              bottomDockScreen, DockPosition::Bottom, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ -1700, 1079 }, negativeBottomMonitor,
              bottomDockScreen, DockPosition::Bottom, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ -1200, 1070 }, negativeBottomMonitor,
              bottomDockScreen, DockPosition::Bottom, 6),
        "a summon-only Dock must accept passive drag reveal only from its own projection on the monitor edge");
    Check(floatingDock::IsPointInDockEdgeCorridor(
              POINT{ -1200, 1070 }, negativeBottomMonitor,
              bottomDockScreen, DockPosition::Bottom) &&
            floatingDock::IsPointInDockEdgeCorridor(
              POINT{ -1200, 1010 }, negativeBottomMonitor,
              bottomDockScreen, DockPosition::Bottom) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ -1600, 1070 }, negativeBottomMonitor,
              bottomDockScreen, DockPosition::Bottom) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ -1200, 990 }, negativeBottomMonitor,
              bottomDockScreen, DockPosition::Bottom),
        "the edge corridor must bridge an island Dock to its edge without covering unrelated desktop space");
    const RECT leftDockScreen{ 24, 360, 84, 960 };
    Check(floatingDock::IsPointInDockEdgeProjection(
              POINT{ 0, 600 }, rightMonitor,
              leftDockScreen, DockPosition::Left, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ 0, 1100 }, rightMonitor,
              leftDockScreen, DockPosition::Left, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ 12, 600 }, rightMonitor,
              leftDockScreen, DockPosition::Left, 6) &&
            floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 12, 600 }, rightMonitor,
              leftDockScreen, DockPosition::Left) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 12, 1100 }, rightMonitor,
              leftDockScreen, DockPosition::Left) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 90, 600 }, rightMonitor,
              leftDockScreen, DockPosition::Left),
        "vertical passive drag projection and corridor rules must follow the configured Dock edge");

    const RECT topDockScreen{ 800, 24, 1400, 84 };
    Check(floatingDock::IsPointInDockEdgeProjection(
              POINT{ 1000, 0 }, rightMonitor,
              topDockScreen, DockPosition::Top, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ 200, 0 }, rightMonitor,
              topDockScreen, DockPosition::Top, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ 1000, 12 }, rightMonitor,
              topDockScreen, DockPosition::Top, 6) &&
            floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 1000, 12 }, rightMonitor,
              topDockScreen, DockPosition::Top) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 200, 12 }, rightMonitor,
              topDockScreen, DockPosition::Top) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 1000, 90 }, rightMonitor,
              topDockScreen, DockPosition::Top),
        "top passive drag projection and corridor must reject points outside its axis and beyond its inner edge");

    const RECT rightDockScreen{ 2476, 360, 2536, 960 };
    Check(floatingDock::IsPointInDockEdgeProjection(
              POINT{ 2559, 600 }, rightMonitor,
              rightDockScreen, DockPosition::Right, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ 2559, 1100 }, rightMonitor,
              rightDockScreen, DockPosition::Right, 6) &&
            !floatingDock::IsPointInDockEdgeProjection(
              POINT{ 2540, 600 }, rightMonitor,
              rightDockScreen, DockPosition::Right, 6) &&
            floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 2548, 600 }, rightMonitor,
              rightDockScreen, DockPosition::Right) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 2548, 1100 }, rightMonitor,
              rightDockScreen, DockPosition::Right) &&
            !floatingDock::IsPointInDockEdgeCorridor(
              POINT{ 2460, 600 }, rightMonitor,
              rightDockScreen, DockPosition::Right),
        "right passive drag projection and corridor must reject points outside its axis and beyond its inner edge");

    using PassiveDragAction =
        floatingDock::PassiveDragRevealAction;
    Check(floatingDock::ResolvePassiveDragRevealUpdate(
              true, false, false, true,
              false, false, false) ==
                PassiveDragAction::Reveal &&
            floatingDock::ResolvePassiveDragRevealUpdate(
              true, false, true, false,
              true, true, false) ==
                PassiveDragAction::CancelLeave &&
            floatingDock::ResolvePassiveDragRevealUpdate(
              true, false, true, false,
              false, false, false) ==
                PassiveDragAction::BeginLeave &&
            floatingDock::ResolvePassiveDragRevealUpdate(
              true, false, true, false,
              false, true, true) ==
                PassiveDragAction::Hide,
        "passive drag state must reveal at the edge, cancel leave on re-entry and hide only after the delay");
    Check(floatingDock::ResolvePassiveDragRevealUpdate(
              true, true, true, false,
              false, true, true) ==
                PassiveDragAction::CancelLeave,
        "a manually summoned Dock must never be collapsed by passive drag cleanup");
    Check(floatingDock::ResolvePassiveDragRevealUpdate(
              false, false, true, false,
              false, true, true) ==
                PassiveDragAction::CancelLeave,
        "disabling summon-only mode must cancel passive cleanup instead of hiding an ordinarily visible Dock");
    Check(floatingDock::ResolvePassiveDragRevealUpdate(
              true, false, false, false,
              false, false, false) ==
                PassiveDragAction::None,
        "a summon-only Dock must ignore an edge sample that was not authorized by a drag");
    Check(!floatingDock::HasPassiveDragLeaveDelayElapsed(
              100, 459, 360) &&
            floatingDock::HasPassiveDragLeaveDelayElapsed(
              100, 460, 360),
        "passive drag leave delay must expire at its exact monotonic deadline");

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
    Check(rules::IsTaskWindowProcessEligible(false, false) &&
            rules::IsTaskWindowProcessEligible(true, true) &&
            !rules::IsTaskWindowProcessEligible(true, false),
        "Dock discovery must include the application-level settings window while excluding other current-process surfaces");
    constexpr LONG_PTR taskbarProxyStyle =
        WS_POPUP | WS_CAPTION;
    constexpr LONG_PTR taskbarProxyExtendedStyle =
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_WINDOWEDGE;
    Check(rules::IsTaskbarDocumentProxyCandidateEligible(
            true, false, false, true,
            taskbarProxyStyle, taskbarProxyExtendedStyle),
        "a titled hidden ownerless popup with taskbar proxy styles must become a class-independent candidate");
    Check(!rules::IsTaskbarDocumentProxyCandidateEligible(
            true, false, false, false,
            taskbarProxyStyle, taskbarProxyExtendedStyle) &&
            !rules::IsTaskbarDocumentProxyCandidateEligible(
                true, true, false, true,
                taskbarProxyStyle, taskbarProxyExtendedStyle) &&
            !rules::IsTaskbarDocumentProxyCandidateEligible(
                true, false, true, true,
                taskbarProxyStyle, taskbarProxyExtendedStyle) &&
            !rules::IsTaskbarDocumentProxyCandidateEligible(
                true, false, false, true,
                WS_POPUP, taskbarProxyExtendedStyle) &&
            !rules::IsTaskbarDocumentProxyCandidateEligible(
                true, false, false, true,
                taskbarProxyStyle,
                taskbarProxyExtendedStyle | WS_EX_TOPMOST) &&
            !rules::IsTaskbarDocumentProxyCandidateEligible(
                true, false, false, true,
                taskbarProxyStyle,
                WS_EX_TOOLWINDOW | WS_EX_WINDOWEDGE),
        "untitled, visible, owned, captionless, topmost, and activatable helpers must not become proxy candidates");
    Check(rules::IsTaskbarDocumentProxyCohortEligible(2, 2) &&
            rules::IsTaskbarDocumentProxyCohortEligible(3, 3) &&
            !rules::IsTaskbarDocumentProxyCohortEligible(1, 1) &&
            !rules::IsTaskbarDocumentProxyCohortEligible(2, 1),
        "only multi-window cohorts with distinct document titles may replace the main frame");
    Check(rules::ShouldPreferTaskbarDocumentProxyCohort(1) &&
            !rules::ShouldPreferTaskbarDocumentProxyCohort(0) &&
            !rules::ShouldPreferTaskbarDocumentProxyCohort(2),
        "ambiguous or absent proxy cohorts must fall back to ordinary task windows");
    Check(rules::ResolveDockClickAction(false, false, false) ==
            rules::DockClickAction::Launch,
        "a closed application must keep the existing launch gesture");
    Check(identityRules::MatchesRunningApp(
            DockAppIdentityKind::Executable,
            L"C:\\APPS\\EDITOR.EXE", L"", L"",
            L"C:\\APPS\\EDITOR.EXE", L"") &&
            !identityRules::MatchesRunningApp(
                DockAppIdentityKind::Executable,
                L"C:\\APPS\\EDITOR.EXE", L"", L"",
                L"C:\\APPS\\OTHER.EXE", L""),
        "running executable identities must match only the same normalized executable path");
    const std::vector<std::wstring> launcherAncestors{
        L"C:\\PROGRAMS\\SUITE\\LAUNCHER.EXE",
        L"C:\\WINDOWS\\EXPLORER.EXE",
    };
    Check(identityRules::MatchesRunningApp(
            DockAppIdentityKind::Executable,
            L"C:\\PROGRAMS\\SUITE\\LAUNCHER.EXE",
            L"", L"",
            L"C:\\PROGRAMS\\SUITE\\UI\\HELPER.EXE",
            L"", launcherAncestors) &&
            !identityRules::MatchesRunningApp(
                DockAppIdentityKind::Executable,
                L"C:\\PROGRAMS\\SUITE\\LAUNCHER.EXE",
                L"", L"",
                L"D:\\APPLICATIONS\\HELPER.EXE",
                L"", launcherAncestors) &&
            !identityRules::MatchesRunningApp(
                DockAppIdentityKind::Executable,
                L"C:\\PROGRAMS\\SUITE\\LAUNCHER.EXE",
                L"", L"",
                L"C:\\PROGRAMS\\SUITE\\UI\\HELPER.EXE",
                L"", std::span<const std::wstring>{}),
        "an executable launcher must match a descendant window process only inside the same installation tree");
    Check(identityRules::MatchesRunningApp(
            DockAppIdentityKind::Applications,
            L"", L"CONTOSO.EDITOR_123!APP", L"",
            L"C:\\WINDOWS\\SYSTEM32\\APPLICATIONFRAMEHOST.EXE",
            L"CONTOSO.EDITOR_123!APP") &&
            !identityRules::MatchesRunningApp(
                DockAppIdentityKind::Applications,
                L"", L"CONTOSO.EDITOR_123!APP", L"",
                L"C:\\WINDOWS\\SYSTEM32\\APPLICATIONFRAMEHOST.EXE",
                L"CONTOSO.OTHER_123!APP"),
        "packaged applications must match by normalized application user model ID");
    Check(identityRules::MatchesRunningApp(
            DockAppIdentityKind::Steam,
            L"", L"", L"D:\\STEAM\\COMMON\\GAME",
            L"D:\\STEAM\\COMMON\\GAME\\BIN\\GAME.EXE", L"") &&
            !identityRules::MatchesRunningApp(
                DockAppIdentityKind::Steam,
                L"", L"", L"D:\\STEAM\\COMMON\\GAME",
                L"D:\\STEAM\\COMMON\\GAME2\\GAME.EXE", L"") &&
            identityRules::MatchesRunningApp(
                DockAppIdentityKind::Steam,
                L"", L"STEAM.APP.123", L"",
                L"C:\\GAMES\\GAME.EXE", L"STEAM.APP.123"),
        "Steam identities must match their AUMID or a path below the install directory without accepting sibling prefixes");
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
    Check(rules::IsMatchingPendingDockDoubleClickRelease(
                4, noDockEntry, 4, noDockEntry,
                250, 500) &&
            rules::IsMatchingPendingDockDoubleClickRelease(
                noDockEntry, 7, noDockEntry, 7,
                500, 500) &&
            !rules::IsMatchingPendingDockDoubleClickRelease(
                noDockEntry, noDockEntry,
                noDockEntry, noDockEntry,
                100, 500) &&
            !rules::IsMatchingPendingDockDoubleClickRelease(
                4, noDockEntry, 4, noDockEntry,
                501, 500),
        "release-based Dock double-click fallback must match one real item within the system interval");
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
    Check(!magnification::ShouldSuppressMagnification(
              false, false, false) &&
            magnification::ShouldSuppressMagnification(
              true, false, false) &&
            magnification::ShouldSuppressMagnification(
              false, true, false) &&
            magnification::ShouldSuppressMagnification(
              false, false, true),
        "Dock magnification must be disabled for item drags and widget move or resize gestures");
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
    const std::array<DockPosition, 4> dockPositions{
        DockPosition::Bottom,
        DockPosition::Top,
        DockPosition::Left,
        DockPosition::Right,
    };
    const std::array<int, 8> magnificationIconSizes{
        1, 32, 64, 77, 96, 128, 192, 256,
    };
    const std::array<float, 4> magnificationScales{
        1.0f,
        magnification::kSecondNeighborScale,
        magnification::kFirstNeighborScale,
        magnification::kFocusScale,
    };
    bool hoverPresentationCoversRetainedFocus = true;
    for (const int iconSize : magnificationIconSizes)
    {
        const int maximumAxisShift =
            magnification::MaximumAxisShift(iconSize);
        for (const DockPosition position : dockPositions)
        {
            const RECT interactionBounds =
                magnification::ExpandInteractionBounds(
                    baseIsland, position, iconSize);
            const RECT presentationBounds =
                magnification::ExpandHoverPresentationBounds(
                    interactionBounds);
            for (const float scale : magnificationScales)
            {
                for (const int axisShift : {
                        -maximumAxisShift,
                        maximumAxisShift })
                {
                    const RECT maximumVisual =
                        magnification::MagnifyRect(
                            baseIsland, position,
                            scale, iconSize,
                            axisShift);
                    const RECT retainedVisual =
                        magnification::ExpandFocusRetentionBounds(
                            maximumVisual);
                    hoverPresentationCoversRetainedFocus =
                        hoverPresentationCoversRetainedFocus &&
                        presentationBounds.left <= retainedVisual.left &&
                        presentationBounds.top <= retainedVisual.top &&
                        presentationBounds.right >= retainedVisual.right &&
                        presentationBounds.bottom >= retainedVisual.bottom;
                }
            }
        }
    }
    Check(hoverPresentationCoversRetainedFocus,
        "Dock hover presentation must continue through every retained maximum-focus pixel in all four orientations");
    const RECT bottomPresentationBounds =
        magnification::ExpandHoverPresentationBounds(
            expandedIsland);
    const LONG bottomPresentationCenter =
        (bottomPresentationBounds.left +
            bottomPresentationBounds.right) / 2;
    const POINT firstRetentionOnlyPoint{
        bottomPresentationCenter,
        expandedIsland.top - 1,
    };
    const POINT lastTrackedPoint{
        bottomPresentationCenter,
        bottomPresentationBounds.top,
    };
    const POINT firstFullyExitedPoint{
        bottomPresentationCenter,
        bottomPresentationBounds.top - 1,
    };
    Check(!PtInRect(
              &expandedIsland,
              firstRetentionOnlyPoint) &&
            PtInRect(
              &bottomPresentationBounds,
              firstRetentionOnlyPoint) &&
            magnification::ShouldTrackHoverPresentation(
              expandedIsland,
              firstRetentionOnlyPoint,
              POINT{
                  firstRetentionOnlyPoint.x,
                  firstRetentionOnlyPoint.y - 1 }) &&
            magnification::ShouldTrackHoverPresentation(
              expandedIsland,
              lastTrackedPoint,
              firstFullyExitedPoint) &&
            !magnification::ShouldTrackHoverPresentation(
              expandedIsland,
              firstFullyExitedPoint,
              POINT{
                  firstFullyExitedPoint.x,
                  firstFullyExitedPoint.y - 1 }),
        "slow upward exit must present every retention-shell sample and the final clearing transition exactly once");
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

    DockWindowPreview clearedPreview;
    Check(clearedPreview.IsCleared(),
        "a new Dock preview must already satisfy its cleared invariant");

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
    Check(hover.IsIdle(),
        "a new Dock preview hover controller must be idle");
    DockPreviewHoverTransition transition =
        hover.UpdateTarget(L"WORD@PRIMARY", false, false);
    Check(transition.armTimer && hover.TimerArmed() &&
            !hover.IsIdle(),
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
    Check(hover.IsIdle(),
        "resetting Dock preview hover state must make cleanup idempotent");
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

    Check(argc == 2, "source root argument is provided");
    if (argc == 2)
    {
        const std::string lifecycleSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_lifecycle.cpp");
        const std::size_t createBegin = lifecycleSource.find(
            "bool DesktopApp::CreateDesktopOverlayWindow()");
        const std::size_t recoverBegin = lifecycleSource.find(
            "void DesktopApp::RecoverDesktopHostAfterExplorerRestart()",
            createBegin);
        const std::string createOverlay =
            createBegin == std::string::npos ||
                recoverBegin == std::string::npos
            ? std::string{}
            : lifecycleSource.substr(
                createBegin, recoverBegin - createBegin);
        const std::size_t hostReady = createOverlay.find(
            "StartDockForegroundMonitor();");
        const std::size_t rebind = createOverlay.find(
            "widgetEngine_->RebindHostTimers();");
        const std::size_t show = createOverlay.find(
            "ShowWindow(hwnd_, SW_SHOWNOACTIVATE);");
        Check(!lifecycleSource.empty(),
            "application lifecycle source is readable");
        Check(hostReady != std::string::npos &&
                rebind != std::string::npos &&
                show != std::string::npos &&
                hostReady < rebind && rebind < show,
            "replacement overlays rebind Lua widget timers after host setup and before display");

        const std::string floatingPopupSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_floating_popup_window.cpp");
        const std::string popupRenderSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_popup_render.cpp");
        const std::string panelRenderPrimitivesSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_render_primitives.cpp");
        const std::string compositionAnimationSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_composition_animation_overlay.cpp");
        const std::string luaPanelSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_lua_panel.cpp");
        const std::string floatingDockSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_floating_dock_window.cpp");
        const std::string floatingDockRenderSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_floating_dock_render.cpp");
        const std::string floatingDockInteractionSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_floating_dock_interaction.cpp");
        const std::string floatingDockLifecycleSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_floating_dock_lifecycle.cpp");
        const std::string popupTransitionSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_popup_transition.cpp");
        const std::string popupLifecycleSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_popup_lifecycle.cpp");
        const std::string dockPreviewSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_dock_window_control.cpp");
        const std::string dockPlatformHelpersSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "dock_platform_helpers.h");
        const std::string pointerContextSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_pointer_context.cpp");

        const std::size_t floatingDockMessageHandler =
            floatingDockRenderSource.find(
                "LRESULT DesktopApp::HandleFloatingDockMessage(");
        const std::size_t floatingPopupMessageHandler =
            floatingPopupSource.find(
                "LRESULT DesktopApp::HandleFloatingPopupMessage(");
        Check(floatingDockMessageHandler != std::string::npos &&
                floatingDockRenderSource.find(
                    "app.FlushNativeMenuPresentation();",
                    floatingDockMessageHandler) != std::string::npos &&
                floatingPopupMessageHandler != std::string::npos &&
                floatingPopupSource.find(
                    "app.FlushNativeMenuPresentation();",
                    floatingPopupMessageHandler) != std::string::npos,
            "floating Dock and popup hosts must flush content composition "
            "after native-menu modal-loop messages");
        Check(popupRenderSource.find(
                  "popupBackgroundAppearance.widgetEdgeHighlightEnabled = false;") !=
                    std::string::npos &&
                popupRenderSource.find(
                  "DrawWidgetPanelEdgeHighlight(") !=
                    std::string::npos &&
                popupRenderSource.find(
                  "&collectionPopupAppearance_, popupMetrics.scale") !=
                    std::string::npos &&
                panelRenderPrimitivesSource.find(
                  "std::max(0.0f, effectScale)") !=
                    std::string::npos,
            "collection popup edge highlights must render above content and "
            "scale with the popup while ordinary border scaling stays independent");

        const std::string menuIconsSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_menu_icons.cpp");
        const std::string pointerDownSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_pointer_down.cpp");
        const std::string pointerReleaseSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_pointer_release.cpp");
        const std::string itemMenuSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_item_menu.cpp");
        const std::string messageDispatchSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_message_dispatch.cpp");
        const std::string appRunSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_run.cpp");
        const std::string dragHintWindowSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_drag_hint_window.cpp");
        const std::string dockDropSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_dock_drop.cpp");
        const std::string desktopLayoutSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_desktop_layout.cpp");
        const std::string widgetPlacementSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_widget_placement.cpp");
        const std::string widgetGroupingSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_widget_grouping.cpp");
        const std::string dockLayoutSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_dock_layout.cpp");
        const std::string desktopReloadSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_desktop_reload.cpp");
        const std::string iconLoaderSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_icon_loader.cpp");
        const std::string appHeaderSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app.h");
        const std::string dockWindowTrackingSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_dock_window_tracking.cpp");
        const std::string sceneSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_scene_render.cpp");
        const std::string navigationRenderSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_navigation_render.cpp");
        const std::string pointerMoveSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_pointer_move.cpp");
        const std::string hitTestingSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_hit_testing.cpp");
        const std::string desktopGridSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "core" /
                "desktop.cpp");
        const std::string dockContainerSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "core" /
                "dock.cpp");
        const std::string backdropCompositorSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "desktop_backdrop_compositor.cpp");
        const std::string popupPairZOrderSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "popup_window_pair_z_order.h");
        const std::string quickNavigationWindowSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_quick_navigation_window.cpp");
        const std::string quickNavigationInteractionSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_quick_navigation_interaction.cpp");
        const std::string settingsApplySource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_settings_apply.cpp");
        const std::string quickNavigationRenderSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_quick_navigation_render.cpp");
        const std::string animationSchedulerSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_animation_scheduler.cpp");
        const std::string shellMenuSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_shell_menu.cpp");
        const std::string timerDispatchSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_timer_dispatch.cpp");
        const std::string renderPrimitivesSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_render_primitives.cpp");
        const std::size_t liveNativeMenuPumpBegin =
            shellMenuSource.find(
                "UINT DesktopApp::TrackShellPopupMenuWithDesktopPump(");
        const std::size_t firstShellMenuEntry =
            shellMenuSource.find(
                "void DesktopApp::ShowNewMenuAndInvoke(",
                liveNativeMenuPumpBegin);
        const std::string liveNativeMenuPumpSource =
            liveNativeMenuPumpBegin != std::string::npos &&
                    firstShellMenuEntry != std::string::npos
                ? shellMenuSource.substr(
                    liveNativeMenuPumpBegin,
                    firstShellMenuEntry - liveNativeMenuPumpBegin)
                : std::string{};
        Check(!liveNativeMenuPumpSource.empty() &&
                liveNativeMenuPumpSource.find(
                    "std::thread") != std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "CreateWindowExW(") != std::string::npos &&
                shellMenuSource.find(
                    "ShellMenuTrackerWindowProc") !=
                    std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "SetForegroundWindow(trackerOwner);") !=
                    std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "trackerOwner, nullptr);") !=
                    std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "MsgWaitForMultipleObjectsEx(") !=
                    std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "PeekMessageW(") != std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "uiAnimationScheduler_.DispatchDue();") !=
                    std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "FlushPendingCompositionCommit();") !=
                    std::string::npos &&
                CountOccurrences(
                    shellMenuSource,
                    "TrackShellPopupMenuWithDesktopPump(") == 5 &&
                CountOccurrences(
                    itemMenuSource,
                    "TrackShellPopupMenuWithDesktopPump(") == 1 &&
                CountOccurrences(
                    shellMenuSource,
                    "TrackPopupMenuEx(") ==
                    1 &&
                CountOccurrences(
                    itemMenuSource,
                    "TrackPopupMenuEx(") == 0 &&
                popupLifecycleSource.find(
                    "shellPopupTrackerOwnerHwnd_.load(") !=
                    std::string::npos &&
                popupLifecycleSource.find(
                    "SendMessageW(trackerOwner, WM_CANCELMODE") !=
                    std::string::npos &&
                appHeaderSource.find(
                    "std::atomic<HWND> shellPopupTrackerOwnerHwnd_") !=
                    std::string::npos &&
                shellMenuSource.find(
                    "message == WM_INITMENUPOPUP") !=
                    std::string::npos &&
                shellMenuSource.find(
                    "message == WM_DRAWITEM") !=
                    std::string::npos &&
                shellMenuSource.find(
                    "message == WM_MEASUREITEM") !=
                    std::string::npos &&
                shellMenuSource.find(
                    "message == WM_MENUCHAR") !=
                    std::string::npos &&
                liveNativeMenuPumpSource.find(
                    "synchronous fallback") == std::string::npos &&
                popupLifecycleSource.find(
                    "EndMenu();") == std::string::npos,
            "native Shell menus must use a tracker-thread-owned window while leaving the desktop pump live");
        const std::size_t dockSurfacePrepareBegin =
            floatingDockInteractionSource.find(
                "HRESULT DesktopApp::\n"
                "CreateOrResizeFloatingDockCompositionSurface(");
        const std::size_t dockSurfacePrepareEnd =
            floatingDockInteractionSource.find(
                "void DesktopApp::\n"
                "RecoverFloatingDockCompositionFailure(",
                dockSurfacePrepareBegin);
        const std::string dockSurfacePrepareSource =
            dockSurfacePrepareBegin != std::string::npos &&
                    dockSurfacePrepareEnd != std::string::npos
                ? floatingDockInteractionSource.substr(
                    dockSurfacePrepareBegin,
                    dockSurfacePrepareEnd - dockSurfacePrepareBegin)
                : std::string{};
        const std::size_t dockFrameRenderBegin =
            floatingDockRenderSource.find(
                "bool DesktopApp::RenderFloatingDockCompositionFrame(");
        const std::size_t dockFrameRenderEnd =
            floatingDockRenderSource.find(
                "void DesktopApp::PaintFloatingDockWindow(",
                dockFrameRenderBegin);
        const std::string dockFrameRenderSource =
            dockFrameRenderBegin != std::string::npos &&
                    dockFrameRenderEnd != std::string::npos
                ? floatingDockRenderSource.substr(
                    dockFrameRenderBegin,
                    dockFrameRenderEnd - dockFrameRenderBegin)
                : std::string{};
        const std::size_t dockFrameBeginDraw =
            dockFrameRenderSource.find(
                "frameSurface->BeginDraw(");
        const std::size_t dockFrameEndDraw =
            dockFrameRenderSource.find(
                "frameSurface->EndDraw();",
                dockFrameBeginDraw);
        const std::size_t dockFrameAttach =
            dockFrameRenderSource.find(
                "host.dcompVisual->SetContent(",
                dockFrameEndDraw);
        const std::size_t dockFrameCommit =
            dockFrameRenderSource.find(
                "CommitCompositionAnimationFrame()",
                dockFrameAttach);
        Check(!dockSurfacePrepareSource.empty() &&
                dockSurfacePrepareSource.find(
                    "ComPtr<IDCompositionSurface>& frameSurface") !=
                    std::string::npos &&
                dockSurfacePrepareSource.find(
                    "SetContent(") == std::string::npos &&
                dockSurfacePrepareSource.find(
                    "FlushPendingCompositionCommit()") ==
                    std::string::npos &&
                dockFrameBeginDraw != std::string::npos &&
                dockFrameEndDraw != std::string::npos &&
                dockFrameAttach != std::string::npos &&
                dockFrameCommit != std::string::npos &&
                dockFrameBeginDraw < dockFrameEndDraw &&
                dockFrameEndDraw < dockFrameAttach &&
                dockFrameAttach < dockFrameCommit &&
                floatingDockSource.find(
                    "RenderFloatingDockCompositionFrame(host) &&\n"
                    "            FlushPendingCompositionCommit();") !=
                    std::string::npos,
            "a resized Dock surface must be fully drawn before one atomic content switch and resize-path commit");

        const std::size_t widgetCompletionBegin =
            pointerReleaseSource.find(
                "// ── Widget action completion");
        const std::size_t widgetCompletionEnd =
            pointerReleaseSource.find(
                "if (!dragSession_.IsActive())",
                widgetCompletionBegin);
        const std::string widgetCompletionSource =
            widgetCompletionBegin != std::string::npos &&
                    widgetCompletionEnd != std::string::npos
                ? pointerReleaseSource.substr(
                    widgetCompletionBegin,
                    widgetCompletionEnd - widgetCompletionBegin)
                : std::string{};
        const std::size_t widgetFinalDockHit =
            widgetCompletionSource.find(
                "GetDockContainerAtPoint(upPoint)");
        const std::size_t widgetMoveStateCleared =
            widgetCompletionSource.find(
                "widgetAction_ = WidgetAction::None;",
                widgetFinalDockHit);
        const std::size_t widgetGroupCommit =
            widgetCompletionSource.find(
                "AddCollectionToGroup(",
                widgetMoveStateCleared);
        const std::size_t widgetFileGroupCommit =
            widgetCompletionSource.find(
                "AddWidgetToFileGroup(",
                widgetMoveStateCleared);
        const std::size_t widgetDockCommit =
            widgetCompletionSource.find(
                "CommitDockDrop(",
                widgetMoveStateCleared);
        const std::size_t widgetMovePlacement =
            widgetCompletionSource.find(
                "PlaceWidgetWithDisplacement(",
                widgetMoveStateCleared);
        const std::size_t widgetResizeStateCleared =
            widgetCompletionSource.find(
                "widgetAction_ = WidgetAction::None;",
                widgetMovePlacement);
        const std::size_t widgetResizePlacement =
            widgetCompletionSource.find(
                "PlaceWidgetWithDisplacement(",
                widgetResizeStateCleared);
        Check(!widgetCompletionSource.empty() &&
                widgetFinalDockHit != std::string::npos &&
                widgetMoveStateCleared != std::string::npos &&
                widgetGroupCommit != std::string::npos &&
                widgetFileGroupCommit != std::string::npos &&
                widgetDockCommit != std::string::npos &&
                widgetMovePlacement != std::string::npos &&
                widgetResizeStateCleared != std::string::npos &&
                widgetResizePlacement != std::string::npos &&
                widgetFinalDockHit < widgetMoveStateCleared &&
                widgetMoveStateCleared < widgetGroupCommit &&
                widgetMoveStateCleared < widgetFileGroupCommit &&
                widgetMoveStateCleared < widgetDockCommit &&
                widgetMoveStateCleared < widgetMovePlacement &&
                widgetMovePlacement < widgetResizeStateCleared &&
                widgetResizeStateCleared < widgetResizePlacement &&
                CountOccurrences(
                    widgetCompletionSource,
                    "widgetAction_ = WidgetAction::None;") == 2 &&
                widgetCompletionSource.find(
                    "RebuildContainersAndItems();") ==
                    std::string::npos,
            "widget release must retain drag hit geometry through final targeting, then clear suppression before one runtime rebuild");

        const std::size_t addWidgetToFileGroupBegin =
            widgetGroupingSource.find(
                "bool DesktopApp::AddWidgetToFileGroup(");
        const std::size_t addWidgetToFileGroupEnd =
            widgetGroupingSource.find(
                "bool DesktopApp::MoveFolderMappingsToFileGroup(",
                addWidgetToFileGroupBegin);
        const std::string addWidgetToFileGroupSource =
            addWidgetToFileGroupBegin != std::string::npos &&
                    addWidgetToFileGroupEnd != std::string::npos
                ? widgetGroupingSource.substr(
                    addWidgetToFileGroupBegin,
                    addWidgetToFileGroupEnd - addWidgetToFileGroupBegin)
                : std::string{};
        Check(widgetPlacementSource.find(
                    "LayoutItems();") != std::string::npos &&
                widgetPlacementSource.find(
                    "RebuildContainersAndItems();") ==
                    std::string::npos &&
                !addWidgetToFileGroupSource.empty() &&
                addWidgetToFileGroupSource.find(
                    "LayoutItems();") != std::string::npos &&
                addWidgetToFileGroupSource.find(
                    "RebuildContainersAndItems();") ==
                    std::string::npos,
            "widget placement and file-group completion must rely on LayoutItems for exactly one runtime rebuild");

        const std::size_t committedItemDropBegin =
            pointerReleaseSource.find(
                "targetContainer->OnItemsDropped(");
        const std::size_t committedItemDropEnd =
            pointerReleaseSource.find(
                "cleanup:",
                committedItemDropBegin);
        const std::string committedItemDropSource =
            committedItemDropBegin != std::string::npos &&
                    committedItemDropEnd != std::string::npos
                ? pointerReleaseSource.substr(
                    committedItemDropBegin,
                    committedItemDropEnd - committedItemDropBegin)
                : std::string{};
        Check(!committedItemDropSource.empty() &&
                committedItemDropSource.find(
                    "ApplyPageMapping();\n"
                    "            LayoutItems();") !=
                    std::string::npos &&
                committedItemDropSource.find(
                    "RebuildContainersAndItems();\n"
                    "            LayoutItems();") ==
                    std::string::npos,
            "a committed item drop must not rebuild the DockHost immediately before LayoutItems rebuilds it again");

        const std::size_t dockReservationBegin =
            dockLayoutSource.find(
                "void DesktopApp::ApplyDockWorkAreaReservation()");
        const std::string dockReservationSource =
            dockReservationBegin == std::string::npos
                ? std::string{}
                : dockLayoutSource.substr(dockReservationBegin);
        const std::size_t preserveDockArea =
            dockReservationSource.find(
                "dockAreas_.push_back(dockArea)");
        const std::size_t overlapRestore =
            dockReservationSource.find(
                "if (!reserveDesktopWorkArea)",
                preserveDockArea);
        const std::size_t restorePageWorkArea =
            dockReservationSource.find(
                "targetPage.workArea = originalWorkArea;",
                overlapRestore);
        Check(!dockReservationSource.empty() &&
                dockReservationSource.find(
                  "ShouldReserveDesktopWorkArea(\n"
                  "                dockSettings_.showOnlyWhenSummoned,\n"
                  "                dockSettings_.allowDesktopContentOverlap)") !=
                    std::string::npos &&
                preserveDockArea != std::string::npos &&
                overlapRestore != std::string::npos &&
                restorePageWorkArea != std::string::npos &&
                preserveDockArea < overlapRestore &&
                overlapRestore < restorePageWorkArea &&
                desktopLayoutSource.find(
                  "for (const RECT& dockArea : dockAreas_)") !=
                    std::string::npos &&
                desktopLayoutSource.find(
                  "std::make_unique<DockContainer>(this, &dockEntries_, dockArea)") !=
                    std::string::npos,
            "effective overlap must preserve Dock geometry and container creation while restoring only the desktop work area");
        Check(CountOccurrences(
                  dragHintWindowSource,
                  "SetWindowPos(hintHwnd_, HWND_TOPMOST") == 2 &&
                dragHintWindowSource.find(
                  "SetWindowPos(hintHwnd_, nullptr") ==
                    std::string::npos,
            "drag hints must be raised above the floating Dock on both cached and rebuilt frames");
        Check(pointerReleaseSource.find(
                  "IsMatchingPendingDockDoubleClickRelease(") !=
                    std::string::npos,
            "Dock releases must retain a cross-HWND double-click fallback");
        Check(pointerMoveSource.find(
                  "cell = ClampGridCellToFitPage(") !=
                    std::string::npos &&
                desktopGridSource.find(
                  "landing = ClampGridCellToFitPage(") !=
                    std::string::npos &&
                dockDropSource.find(
                  "freeCell = ClampGridCellToFitPage(") !=
                    std::string::npos,
            "desktop widget moves and Dock widget previews/commits must share full-span edge anchoring");
        const std::size_t dockHoverDirtyBegin =
            pointerMoveSource.find(
                "if (dockHoverActive && !marqueeActive_)");
        const std::size_t dockHoverDirtyEnd =
            pointerMoveSource.find(
                "using namespace\n                snowdesktop::widget_composition_layer_rules;",
                dockHoverDirtyBegin);
        const std::string dockHoverDirtySource =
            dockHoverDirtyBegin == std::string::npos ||
                dockHoverDirtyEnd == std::string::npos
            ? std::string{}
            : pointerMoveSource.substr(
                dockHoverDirtyBegin,
                dockHoverDirtyEnd - dockHoverDirtyBegin);
        Check(!dockHoverDirtySource.empty() &&
                dockHoverDirtySource.find(
                    "ExpandHostForTitleLayer(") !=
                    std::string::npos &&
                dockHoverDirtySource.find(
                    "dock->GetInteractiveBounds()") !=
                    std::string::npos &&
                dockHoverDirtySource.find(
                    "GetVisualPanelBounds(oldMouse)") ==
                    std::string::npos &&
                dockHoverDirtySource.find(
                    "GetHoveredTitleBounds(oldMouse)") ==
                    std::string::npos,
            "desktop Dock hover must clear one focus-neutral envelope instead of reconstructing stale geometry after hysteresis changes");
        Check(pointerMoveSource.find(
                  "ShouldTrackHoverPresentation(") !=
                    std::string::npos &&
                pointerMoveSource.find(
                  "dockHoverPresentationTracked;") !=
                    std::string::npos &&
                pointerMoveSource.find(
                  "NeedsForegroundPaint(newVisual.layer) ||\n                dockHoverActive;") !=
                    std::string::npos,
            "desktop Dock hover must keep presenting the foreground until focus exit retention has fully cleared");
        Check(backdropCompositorSource.find(
                  "PanelIdentityMatches(") !=
                    std::string::npos &&
                backdropCompositorSource.find(
                  "existing->frame = frame;") !=
                    std::string::npos &&
                dockContainerSource.find(
                  "reinterpret_cast<std::uintptr_t>(this)") !=
                    std::string::npos &&
                CountOccurrences(
                  renderPrimitivesSource,
                  "backdropOwnerKey") >= 3,
            "Dock backdrop rendering in the persistent Host must reuse a stable owner while updating the panel rectangle in place");
        const std::size_t widgetGestureGuard =
            pointerMoveSource.find(
                "const bool activeWidgetGesture =");
        const std::size_t widgetGestureInactiveBranch =
            pointerMoveSource.find(
                "if (!activeWidgetGesture && !marqueePointerGesture)",
                widgetGestureGuard);
        const std::size_t guardedDockPreview =
            pointerMoveSource.find(
                "UpdateDockWindowPreview(current);",
                widgetGestureInactiveBranch);
        const std::size_t guardedLuaHover =
            pointerMoveSource.find(
                "if (widgetEngine_)",
                guardedDockPreview);
        const std::size_t guardedPopupDwell =
            pointerMoveSource.find(
                "UpdateCollectionPopupDwell(current);",
                guardedLuaHover);
        const std::size_t widgetGestureThreshold =
            pointerMoveSource.find(
                "WidgetAction::PendingMove ||",
                guardedPopupDwell);
        Check(widgetGestureGuard != std::string::npos &&
                widgetGestureInactiveBranch != std::string::npos &&
                guardedDockPreview != std::string::npos &&
                guardedLuaHover != std::string::npos &&
                guardedPopupDwell != std::string::npos &&
                widgetGestureThreshold != std::string::npos &&
                widgetGestureGuard < widgetGestureInactiveBranch &&
                widgetGestureInactiveBranch < guardedDockPreview &&
                guardedDockPreview < guardedLuaHover &&
                guardedLuaHover < guardedPopupDwell &&
                guardedPopupDwell < widgetGestureThreshold,
            "active widget drags and marquees must bypass unrelated Dock, Lua hover and popup dwell work");
        const std::size_t runLatencyGesture = appRunSource.find(
            "IsLatencySensitivePointerGesture(");
        const std::size_t runWidgetAction = appRunSource.rfind(
            "widgetAction_ != WidgetAction::None", runLatencyGesture);
        const std::size_t runWidgetTarget = appRunSource.find(
            "mouseDownWidgetIndex_ < widgets_.size()", runLatencyGesture);
        const std::size_t dispatchLatencyGesture =
            messageDispatchSource.find(
                "IsLatencySensitivePointerGesture(");
        const std::size_t dispatchWidgetAction =
            messageDispatchSource.rfind(
                "widgetAction_ != WidgetAction::None",
                dispatchLatencyGesture);
        const std::size_t dispatchWidgetTarget =
            messageDispatchSource.find(
                "mouseDownWidgetIndex_ < widgets_.size()",
                dispatchLatencyGesture);
        const std::size_t dispatchMiddleButton =
            messageDispatchSource.find(
                "GetAsyncKeyState(VK_MBUTTON)",
                dispatchLatencyGesture);
        const std::size_t dispatchPrimaryButton =
            messageDispatchSource.find(
                "GetAsyncKeyState(VK_LBUTTON)",
                dispatchLatencyGesture);
        const std::size_t dispatchPrimaryLatencyGate =
            messageDispatchSource.rfind(
                "latencySensitivePointerActive &&",
                dispatchPrimaryButton);
        const std::size_t dispatchPrimaryOwnershipGate =
            messageDispatchSource.rfind(
                "!middleButtonWidgetMove_ &&",
                dispatchPrimaryButton);
        const std::size_t dispatchMiddleLatencyGate =
            messageDispatchSource.rfind(
                "latencySensitivePointerActive &&",
                dispatchMiddleButton);
        const std::size_t dispatchMiddleOwnershipGate =
            messageDispatchSource.rfind(
                "middleButtonWidgetMove_ &&",
                dispatchMiddleButton);
        const std::size_t dispatchOwningButton =
            messageDispatchSource.find(
                "IsPointerGestureButtonDown(",
                dispatchMiddleButton);
        Check(runLatencyGesture != std::string::npos &&
                runWidgetAction != std::string::npos &&
                runWidgetTarget != std::string::npos &&
                runWidgetAction < runLatencyGesture &&
                runLatencyGesture < runWidgetTarget &&
                dispatchLatencyGesture != std::string::npos &&
                dispatchWidgetAction != std::string::npos &&
                dispatchWidgetTarget != std::string::npos &&
                dispatchWidgetAction < dispatchLatencyGesture &&
                dispatchLatencyGesture < dispatchWidgetTarget &&
                dispatchPrimaryButton != std::string::npos &&
                dispatchPrimaryLatencyGate != std::string::npos &&
                dispatchPrimaryOwnershipGate != std::string::npos &&
                dispatchPrimaryLatencyGate < dispatchPrimaryOwnershipGate &&
                dispatchPrimaryOwnershipGate < dispatchPrimaryButton &&
                dispatchMiddleButton != std::string::npos &&
                dispatchMiddleLatencyGate != std::string::npos &&
                dispatchMiddleOwnershipGate != std::string::npos &&
                dispatchMiddleLatencyGate < dispatchMiddleOwnershipGate &&
                dispatchMiddleOwnershipGate < dispatchMiddleButton &&
                dispatchOwningButton != std::string::npos &&
                dispatchMiddleButton < dispatchOwningButton,
            "the message pump and dispatcher must route valid widget gestures through shared low-latency coalescing and query only the owning mouse button");
        const std::size_t middleReleaseHandler =
            pointerMoveSource.find(
                "void DesktopApp::OnMiddleButtonUpAt(");
        const std::size_t middleMoveClear = pointerMoveSource.find(
            "middleButtonWidgetMove_ = false;", middleReleaseHandler);
        const std::size_t middleReleaseDelegate = pointerMoveSource.find(
            "OnLeftButtonUpAt(wp, point);", middleMoveClear);
        Check(middleReleaseHandler != std::string::npos &&
                middleMoveClear != std::string::npos &&
                middleReleaseDelegate != std::string::npos &&
                middleReleaseHandler < middleMoveClear &&
                middleMoveClear < middleReleaseDelegate,
            "middle-button widget release must clear its ownership flag before delegating to the primary widget completion path");
        const std::string oleDropSessionSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_ole_drop_session.cpp");
        const std::string dragPreviewSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_drag_preview_window.cpp");
        const std::string dragLifecycleSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_drag_lifecycle.cpp");
        const std::string keyboardInputSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_keyboard_input.cpp");
        const std::string oleDropRoutingSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_ole_drop_routing.cpp");
        const std::string dragTargetUpdateSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_drag_target_update.cpp");
        const std::string pageGridSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_page_grid.cpp");
        const std::string popupDwellInteractionSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "app" /
                "app_popup_dwell_interaction.cpp");
        const std::string collectionSource = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "widgets" /
                "collection.cpp");
        const std::size_t collectionDragHitBegin =
            collectionSource.find(
                "HitRegion Collection::HitTestDrag(");
        const std::size_t collectionDragHintBegin =
            collectionSource.find(
                "std::wstring Collection::GetDragHint(",
                collectionDragHitBegin);
        const std::string collectionDragHit =
            collectionDragHitBegin != std::string::npos &&
                    collectionDragHintBegin != std::string::npos
                ? collectionSource.substr(
                    collectionDragHitBegin,
                    collectionDragHintBegin - collectionDragHitBegin)
                : std::string{};
        const std::size_t denseIconHit =
            collectionDragHit.find("ResolveCenteredIconRect(");
        const std::size_t genericSlotHit =
            collectionDragHit.find("WidgetContainer::HitTestDrag(");
        const std::size_t directoryTarget =
            collectionDragHit.find(
                "CollectionItemIsDirectory(targetItem)");
        const std::size_t immediateDirectoryHandoff =
            collectionDragHit.find(
                "return HitRegion::Handoff;", directoryTarget);
        const std::size_t dwellIdentity =
            collectionDragHit.find(
                "compactCollectionHandoffWidgetId_ != data_->id");
        Check(!collectionDragHit.empty() &&
                denseIconHit != std::string::npos &&
                genericSlotHit != std::string::npos &&
                denseIconHit < genericSlotHit &&
                directoryTarget != std::string::npos &&
                immediateDirectoryHandoff != std::string::npos &&
                dwellIdentity != std::string::npos &&
                directoryTarget < immediateDirectoryHandoff &&
                immediateDirectoryHandoff < dwellIdentity &&
                collectionDragHit.find(
                    "dragSession_.TargetContainer()") ==
                    std::string::npos,
            "titleless Collection dwell must lock the actual dense icon before generic insertion canonicalization and keep folders on immediate handoff");
        Check(floatingPopupSource.find("CreateTargetForHwnd") !=
                    std::string::npos &&
                floatingPopupSource.find("RegisterDragDrop") !=
                    std::string::npos,
            "the shared popup host must own an independent DComp target and OLE drop surface");
        Check(floatingPopupSource.find(
                  "collectionPopupBackdropCompositor_.InitializePopup(") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "collectionPopupBackdropCompositor_.Reattach(") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "collectionPopupBackdropCompositor_.Reset();") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "collectionPopupBackdropCompositor_.SetPopupTopmost(") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "collectionPopupBackdropCompositor_.SetVisible(false);") !=
                    std::string::npos,
            "the collection popup backdrop must follow the shared host lifecycle, cache hidden resources, and preserve z-order");
        const std::size_t popupLayerPolicyBegin =
            floatingPopupSource.find(
                "void DesktopApp::ApplyFloatingPopupLayerPolicy()");
        const std::size_t popupLayerPolicyEnd =
            floatingPopupSource.find(
                "void DesktopApp::ApplyCollectionPopupBackdropAnimationFrame()",
                popupLayerPolicyBegin);
        const std::string popupLayerPolicySource =
            popupLayerPolicyBegin != std::string::npos &&
                    popupLayerPolicyEnd != std::string::npos
                ? floatingPopupSource.substr(
                    popupLayerPolicyBegin,
                    popupLayerPolicyEnd - popupLayerPolicyBegin)
                : std::string{};
        Check(!popupLayerPolicySource.empty() &&
                popupLayerPolicySource.find(
                  "SetPopupWindowPairZOrder(") !=
                    std::string::npos &&
                popupLayerPolicySource.find(
                  "SetWindowPos(") == std::string::npos &&
                popupLayerPolicySource.find(
                  "SetPopupTopmost(") == std::string::npos,
            "native menu layer changes must move the floating popup content and backdrop through the shared pair policy");
        Check(compositionAnimationSource.find(
                  "ApplyCollectionPopupBackdropAnimationFrame();") !=
                    std::string::npos &&
                compositionAnimationSource.find(
                  "StartVisualScaleAnimation(") !=
                    std::string::npos &&
                compositionAnimationSource.find(
                  "if (collectionPopupGlassTheme_)\n        return false;") ==
                    std::string::npos &&
                backdropCompositorSource.find(
                  "CreateVector3KeyFrameAnimation()") !=
                    std::string::npos &&
                backdropCompositorSource.find(
                  "root.StartAnimation(L\"Scale\", animation)") !=
                    std::string::npos,
            "acrylic popup scale must run on the compositor and use scheduler frames only as fallback");
        Check(compositionAnimationSource.find(
                  "bool DesktopApp::StartQuickNavigationCompositionAnimation()") !=
                    std::string::npos &&
                compositionAnimationSource.find(
                  "StartVisualTransformAnimation(") !=
                    std::string::npos &&
                backdropCompositorSource.find(
                  "CreateScalarKeyFrameAnimation()") !=
                    std::string::npos &&
                backdropCompositorSource.find(
                  "root.StartAnimation(L\"Opacity\", opacityAnimation)") !=
                    std::string::npos &&
                CountOccurrences(
                  quickNavigationWindowSource,
                  "StartQuickNavigationCompositionAnimation()") >= 3 &&
                animationSchedulerSource.find(
                  "!quickNavigationAnimationCompositorDriven_") !=
                    std::string::npos,
            "acrylic quick navigation transforms must run on both compositors and use scheduler frames only as fallback");
        const std::size_t quickNavigationRegionBegin =
            quickNavigationWindowSource.find(
                "void DesktopApp::UpdateQuickNavigationWindowRegion(");
        const std::size_t quickNavigationRegionEnd =
            quickNavigationWindowSource.find(
                "void DesktopApp::InvalidateQuickNavigationWindow(",
                quickNavigationRegionBegin);
        const std::string quickNavigationRegionSource =
            quickNavigationRegionBegin != std::string::npos &&
                    quickNavigationRegionEnd != std::string::npos
                ? quickNavigationWindowSource.substr(
                    quickNavigationRegionBegin,
                    quickNavigationRegionEnd -
                        quickNavigationRegionBegin)
                : std::string{};
        const std::size_t backdropRegionBegin =
            backdropCompositorSource.find(
                "bool SyncPanelWindowRegion()");
        const std::size_t backdropRegionEnd =
            backdropCompositorSource.find(
                "void SetAnimationPathRegionExpanded(",
                backdropRegionBegin);
        const std::string backdropRegionSource =
            backdropRegionBegin != std::string::npos &&
                    backdropRegionEnd != std::string::npos
                ? backdropCompositorSource.substr(
                    backdropRegionBegin,
                    backdropRegionEnd - backdropRegionBegin)
                : std::string{};
        Check(!quickNavigationRegionSource.empty() &&
                quickNavigationRegionSource.find(
                    "CreateRectRgn(") != std::string::npos &&
                quickNavigationRegionSource.find(
                    "CreateRoundRectRgn(") == std::string::npos &&
                quickNavigationWindowSource.find(
                    "CreateRoundRectRgn(") == std::string::npos &&
                !backdropRegionSource.empty() &&
                backdropRegionSource.find(
                    "CreateRectRgn(") != std::string::npos &&
                backdropRegionSource.find(
                    "CreateRoundRectRgn(") == std::string::npos,
            "quick navigation content and backdrop HWND regions must preserve composition-antialiased rounded edges");
        Check(quickNavigationRenderSource.find(
                  "windowBorderStrokeWidth * 0.5f") !=
                    std::string::npos &&
                quickNavigationRenderSource.find(
                  "static_cast<float>(overlay.left) +\n                windowBorderInset") !=
                    std::string::npos &&
                quickNavigationRenderSource.find(
                  "static_cast<float>(overlay.right) -\n                windowBorderInset") !=
                    std::string::npos &&
                quickNavigationRenderSource.find(
                  "windowCornerRadius - windowBorderInset") !=
                    std::string::npos &&
                quickNavigationRenderSource.find(
                  "overlay.right - 1") ==
                    std::string::npos,
            "quick navigation border must inset symmetrically and keep its outer radius aligned with the background edge");
        Check(compositionAnimationSource.find(
                  "ScaleSegmentNormalizedStartSlope(") !=
                    std::string::npos &&
                compositionAnimationSource.find(
                  "uiAnimationScheduler_.Cancel(\n            popupAnimationFrameToken_);") !=
                    std::string::npos &&
                compositionAnimationSource.find(
                  "uiAnimationScheduler_.Cancel(\n            luaPanelAnimationFrameToken_);") !=
                    std::string::npos &&
                backdropCompositorSource.find(
                  "clampedStartSlope / 3.0f") !=
                    std::string::npos &&
                backdropCompositorSource.find(
                  "L\"backdrop.restore_scale_animation\"") <
                backdropCompositorSource.find(
                  "impl_->Reset();",
                  backdropCompositorSource.find(
                    "L\"backdrop.restore_scale_animation\"")),
            "rapid popup reversals must retain the global easing curve, retire fallback frames, and destroy a faulted backdrop");
        const std::size_t hidePopupPairBegin =
            backdropCompositorSource.find(
                "void DesktopBackdropCompositor::HidePopupWindowPair(");
        const std::size_t hidePopupPairEnd =
            backdropCompositorSource.find(
                "void DesktopBackdropCompositor::SetVisualTransform(",
                hidePopupPairBegin);
        const std::string hidePopupPairSource =
            hidePopupPairBegin != std::string::npos &&
                    hidePopupPairEnd != std::string::npos
                ? backdropCompositorSource.substr(
                    hidePopupPairBegin,
                    hidePopupPairEnd - hidePopupPairBegin)
                : std::string{};
        const std::size_t pairModeGuard =
            hidePopupPairSource.find("impl_->popupMode &&");
        const std::size_t pairIdentityGuard =
            hidePopupPairSource.find(
                "impl_->contentWindow == contentWindow");
        const std::size_t pairBegin =
            hidePopupPairSource.find("BeginDeferWindowPos(2)");
        const std::size_t pairContent =
            hidePopupPairSource.find(
                "deferred, contentWindow, nullptr", pairBegin);
        const std::size_t pairBackdrop =
            hidePopupPairSource.find(
                "deferred, backdropWindow, nullptr", pairContent);
        const std::size_t pairEnd =
            hidePopupPairSource.find(
                "EndDeferWindowPos(deferred)", pairBackdrop);
        const std::size_t pairFallback =
            hidePopupPairSource.find(
                "if (!hiddenTogether)", pairEnd);
        const std::size_t fallbackContent =
            hidePopupPairSource.find(
                "ShowWindow(contentWindow, SW_HIDE)",
                pairFallback);
        const std::size_t fallbackBackdrop =
            hidePopupPairSource.find(
                "ShowWindow(backdropWindow, SW_HIDE)",
                fallbackContent);
        const std::size_t pairHiddenState =
            hidePopupPairSource.find(
                "impl_->visible = false;", fallbackBackdrop);
        Check(!hidePopupPairSource.empty() &&
                pairModeGuard != std::string::npos &&
                pairIdentityGuard != std::string::npos &&
                pairBegin != std::string::npos &&
                pairContent != std::string::npos &&
                pairBackdrop != std::string::npos &&
                pairEnd != std::string::npos &&
                pairFallback != std::string::npos &&
                fallbackContent != std::string::npos &&
                fallbackBackdrop != std::string::npos &&
                pairHiddenState != std::string::npos &&
                pairModeGuard < pairBegin &&
                pairIdentityGuard < pairBegin &&
                pairBegin < pairContent &&
                pairContent < pairBackdrop &&
                pairBackdrop < pairEnd &&
                pairEnd < pairFallback &&
                pairFallback < fallbackContent &&
                fallbackContent < fallbackBackdrop &&
                fallbackBackdrop < pairHiddenState &&
                hidePopupPairSource.find(
                  "SWP_HIDEWINDOW") != std::string::npos,
            "a popup backdrop must hide with its content host in one popup-only window transaction");
        const std::size_t showPopupPairBegin =
            backdropCompositorSource.find(
                "void DesktopBackdropCompositor::ShowPopupWindowPair(");
        const std::size_t showPopupPairEnd =
            backdropCompositorSource.find(
                "void DesktopBackdropCompositor::HidePopupWindowPair(",
                showPopupPairBegin);
        const std::string showPopupPairSource =
            showPopupPairBegin != std::string::npos &&
                    showPopupPairEnd != std::string::npos
                ? backdropCompositorSource.substr(
                    showPopupPairBegin,
                    showPopupPairEnd - showPopupPairBegin)
                : std::string{};
        Check(!showPopupPairSource.empty() &&
                showPopupPairSource.find(
                  "impl_->contentWindow == contentWindow") !=
                    std::string::npos &&
                showPopupPairSource.find(
                  "BeginDeferWindowPos(2)") !=
                    std::string::npos &&
                showPopupPairSource.find(
                  "deferred, backdropWindow, nullptr") !=
                    std::string::npos &&
                showPopupPairSource.find(
                  "deferred, contentWindow, nullptr") !=
                    std::string::npos &&
                showPopupPairSource.find(
                  "SWP_SHOWWINDOW") != std::string::npos,
            "a persistent popup host must reveal its content and backdrop in one window transaction");
        const std::size_t pairZOrderBegin =
            backdropCompositorSource.find(
                "void DesktopBackdropCompositor::SetPopupWindowPairZOrder(");
        const std::size_t pairZOrderEnd =
            backdropCompositorSource.find(
                "void DesktopBackdropCompositor::SetPopupTopmost(",
                pairZOrderBegin);
        const std::string pairZOrderSource =
            pairZOrderBegin != std::string::npos &&
                    pairZOrderEnd != std::string::npos
                ? backdropCompositorSource.substr(
                    pairZOrderBegin,
                    pairZOrderEnd - pairZOrderBegin)
                : std::string{};
        Check(!pairZOrderSource.empty() &&
                pairZOrderSource.find(
                  "popup_window_pair_z_order::Apply(") !=
                    std::string::npos,
            "popup backdrop compositor Z-order changes must use the shared pair transition policy");
        const std::size_t bandChange =
            popupPairZOrderSource.find(
                "const bool changesZOrderBand");
        const std::size_t demoteBackdrop =
            popupPairZOrderSource.find(
                "backdropWindow, HWND_NOTOPMOST", bandChange);
        const std::size_t moveContent =
            popupPairZOrderSource.find(
                "contentWindow, contentInsertAfter", demoteBackdrop);
        const std::size_t crossBandPairBackdrop =
            popupPairZOrderSource.find(
                "backdropWindow, contentWindow", moveContent);
        const std::size_t sameBandBatch =
            popupPairZOrderSource.find(
                "BeginDeferWindowPos(2)", crossBandPairBackdrop);
        Check(!popupPairZOrderSource.empty() &&
                bandChange != std::string::npos &&
                demoteBackdrop != std::string::npos &&
                moveContent != std::string::npos &&
                crossBandPairBackdrop != std::string::npos &&
                sameBandBatch != std::string::npos &&
                bandChange < demoteBackdrop &&
                demoteBackdrop < moveContent &&
                moveContent < crossBandPairBackdrop &&
                crossBandPairBackdrop < sameBandBatch,
            "cross-band popup demotion must move glass first, content second, and only batch windows already in one Z-order band");
        const std::size_t syncBackdropPlacementBegin =
            backdropCompositorSource.find(
                "bool SyncWindowPlacement()");
        const std::size_t syncBackdropPlacementEnd =
            backdropCompositorSource.find(
                "bool SyncPanelWindowRegion()",
                syncBackdropPlacementBegin);
        const std::string syncBackdropPlacementSource =
            syncBackdropPlacementBegin != std::string::npos &&
                    syncBackdropPlacementEnd != std::string::npos
                ? backdropCompositorSource.substr(
                    syncBackdropPlacementBegin,
                    syncBackdropPlacementEnd -
                        syncBackdropPlacementBegin)
                : std::string{};
        Check(!syncBackdropPlacementSource.empty() &&
                syncBackdropPlacementSource.find(
                  "const bool placementMatches =") !=
                    std::string::npos &&
                syncBackdropPlacementSource.find(
                  "const bool pairedZOrder =") !=
                    std::string::npos &&
                syncBackdropPlacementSource.find(
                  "const bool visibilityMatches =") !=
                    std::string::npos &&
                syncBackdropPlacementSource.find(
                  "if (placementMatches && pairedZOrder &&\n"
                  "            visibilityMatches)") !=
                    std::string::npos &&
                syncBackdropPlacementSource.find(
                  "if (!visibilityMatches)\n"
                  "            flags |= visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;") !=
                    std::string::npos &&
                backdropCompositorSource.find(
                  "DWMWA_TRANSITIONS_FORCEDISABLED") !=
                    std::string::npos,
            "an aligned backdrop helper must not receive a second Z-order/show transaction during paint, and it must disable native DWM transitions");
        Check(luaPanelSource.find("ReleaseCapture();") ==
                    std::string::npos &&
                CountOccurrences(
                  luaPanelSource,
                  "ReleaseCapturePreservingPointerState();") == 1 &&
                CountOccurrences(
                  luaPanelSource,
                  "ReleaseLuaWidgetPanelCaptureIfOwned();") >= 2 &&
                pointerDownSource.find(
                  "luaWidgetPanelCaptureHwnd_ = panelCaptureHost;") !=
                    std::string::npos &&
                luaPanelSource.find(
                  "widgetEngine_->CancelInteractionPointerPress(\n                luaWidgetPanelRequest_.surface);") !=
                    std::string::npos,
            "an old Lua panel finalizer must not release a later component press capture");
        const std::size_t advancePopupGeneration =
            floatingPopupSource.find(
                "void DesktopApp::AdvanceFloatingPopupContentGeneration()");
        const std::size_t updateActiveGeneration =
            floatingPopupSource.find(
                "floatingPopupMouseHookActiveGeneration_.store(",
                advancePopupGeneration);
        const std::size_t luaResumeClosingPanel =
            luaPanelSource.find(
                "AdvanceFloatingPopupContentGeneration();");
        const std::size_t luaPublishFreshPanel =
            luaPanelSource.find(
                "AdvanceFloatingPopupContentGeneration();\n    luaWidgetPanelRequest_ = request;",
                luaResumeClosingPanel);
        Check(advancePopupGeneration != std::string::npos &&
                updateActiveGeneration != std::string::npos &&
                floatingPopupSource.find(
                  "IsCurrentPointerNotification(") !=
                    std::string::npos &&
                luaResumeClosingPanel != std::string::npos &&
                luaPublishFreshPanel != std::string::npos,
            "publishing a new or resumed Lua panel must invalidate outside-click notifications queued for older popup contents");
        const std::size_t collectionReopenGeneration =
            popupTransitionSource.find(
                "ExistingSourceAction::ReopenExisting:\n"
                "        pendingCollectionPopupOpen_.reset();\n"
                "        AdvanceFloatingPopupContentGeneration();");
        const std::size_t collectionPublishGeneration =
            popupTransitionSource.find(
                "AdvanceFloatingPopupContentGeneration();\n\n"
                "    if (DockContainer* dock =",
                collectionReopenGeneration);
        const std::size_t folderPopupGeneration =
            popupLifecycleSource.find(
                "AdvanceFloatingPopupContentGeneration();\n"
                "    dockFolderPopupOpen_ = true;");
        Check(collectionReopenGeneration != std::string::npos &&
                collectionPublishGeneration != std::string::npos &&
                folderPopupGeneration != std::string::npos,
            "publishing a new, resumed, or Dock-folder collection popup must invalidate outside-click notifications queued for older popup contents");
        const std::size_t luaFinalizeBegin = luaPanelSource.find(
            "void DesktopApp::FinalizeCloseLuaWidgetPanel(");
        const std::size_t luaFinalizeEnd = luaPanelSource.find(
            "void DesktopApp::CloseLuaWidgetPanel(", luaFinalizeBegin);
        const std::string luaFinalizeHandler =
            luaFinalizeBegin == std::string::npos ||
                luaFinalizeEnd == std::string::npos
            ? std::string{}
            : luaPanelSource.substr(
                luaFinalizeBegin,
                luaFinalizeEnd - luaFinalizeBegin);
        const std::size_t luaDetachRequest =
            luaFinalizeHandler.find(
                "luaWidgetPanelRequest_ = {};");
        const std::size_t luaCloseSurface =
            luaFinalizeHandler.find(
                "widgetEngine_->CloseWidgetPanelSurface(");
        const std::size_t luaClosedCallback =
            luaFinalizeHandler.find(
                "widgetEngine_->InvokeMouseEvent(",
                luaCloseSurface);
        Check(luaPanelSource.find(
                  "if (luaWidgetPanelFinalizing_)") !=
                    std::string::npos &&
                luaPanelSource.find(
                  "pendingLuaWidgetPanelOpen_ = request;") !=
                    std::string::npos &&
                luaPanelSource.find(
                  "luaWidgetPanelRequest_.surface ==\n                request.surface") !=
                    std::string::npos &&
                luaDetachRequest != std::string::npos &&
                luaCloseSurface != std::string::npos &&
                luaClosedCallback != std::string::npos &&
                luaDetachRequest < luaCloseSurface &&
                luaCloseSurface < luaClosedCallback &&
                lifecycleSource.find(
                  "FinalizeCloseLuaWidgetPanel(false);") !=
                    std::string::npos,
            "Lua panel replacement must detach the old surface before callbacks, preserve latest-wins requests, and suppress reopen during teardown");
        Check(popupRenderSource.find(
                  "DrawWidgetPanelBackground(") !=
                    std::string::npos &&
                popupRenderSource.find(
                  "collectionPopupAppearance_") !=
                    std::string::npos &&
                CountOccurrences(
                  popupRenderSource,
                  "collectionPopupLightTheme_") >= 7 &&
                popupRenderSource.find(
                  "IsLightContentTheme()") == std::string::npos,
            "collection and Dock popups must render every foreground state from their independent theme");
        Check(dragPreviewSource.find(
                  "collectionPopupBackdropCompositor_.IsBackdropWindow(window)") !=
                    std::string::npos &&
                CountOccurrences(
                  oleDropRoutingSource,
                  "collectionPopupBackdropCompositor_.IsBackdropWindow(") == 2,
            "the collection popup backdrop must remain presentation-only for drag and OLE routing");
        Check(floatingPopupSource.find(
                  "PackScreenPoint(event->pt)") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "UnpackScreenPoint(screenPointPayload)") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "floatingPopupMouseHookScreenX_") ==
                    std::string::npos &&
                floatingPopupSource.find(
                  "floatingPopupMouseHookScreenY_") ==
                    std::string::npos,
            "each queued popup outside-click notification must own its pointer coordinate payload");
        const std::size_t popupDpiChangeBegin =
            floatingPopupSource.find("case WM_DPICHANGED:");
        const std::size_t popupDisplayChangeBegin =
            floatingPopupSource.find(
                "case WM_DISPLAYCHANGE:", popupDpiChangeBegin);
        const std::size_t popupCloseBegin =
            floatingPopupSource.find(
                "case WM_CLOSE:", popupDisplayChangeBegin);
        const std::string popupDpiChangeHandler =
            popupDpiChangeBegin != std::string::npos &&
                    popupDisplayChangeBegin != std::string::npos
                ? floatingPopupSource.substr(
                    popupDpiChangeBegin,
                    popupDisplayChangeBegin - popupDpiChangeBegin)
                : std::string{};
        const std::string popupDisplayChangeHandler =
            popupDisplayChangeBegin != std::string::npos &&
                    popupCloseBegin != std::string::npos
                ? floatingPopupSource.substr(
                    popupDisplayChangeBegin,
                    popupCloseBegin - popupDisplayChangeBegin)
                : std::string{};
        const std::size_t dockDpiChangeBegin =
            floatingDockRenderSource.find("case WM_DPICHANGED:");
        const std::size_t dockDisplayChangeBegin =
            floatingDockRenderSource.find(
                "case WM_DISPLAYCHANGE:", dockDpiChangeBegin);
        const std::size_t dockCloseBegin =
            floatingDockRenderSource.find(
                "case WM_CLOSE:", dockDisplayChangeBegin);
        const std::string dockDpiChangeHandler =
            dockDpiChangeBegin != std::string::npos &&
                    dockDisplayChangeBegin != std::string::npos
                ? floatingDockRenderSource.substr(
                    dockDpiChangeBegin,
                    dockDisplayChangeBegin - dockDpiChangeBegin)
                : std::string{};
        const std::string dockDisplayChangeHandler =
            dockDisplayChangeBegin != std::string::npos &&
                    dockCloseBegin != std::string::npos
                ? floatingDockRenderSource.substr(
                    dockDisplayChangeBegin,
                    dockCloseBegin - dockDisplayChangeBegin)
                : std::string{};
        Check(!popupDpiChangeHandler.empty() &&
                popupDpiChangeHandler.find(
                  "InvalidateFloatingPopupWindow(false);") !=
                    std::string::npos &&
                popupDpiChangeHandler.find(
                  "CloseCollectionPopup(") == std::string::npos &&
                popupDpiChangeHandler.find(
                  "CloseLuaWidgetPanel(") == std::string::npos &&
                popupDisplayChangeHandler.find(
                  "CloseCollectionPopup(false);") !=
                    std::string::npos &&
                popupDisplayChangeHandler.find(
                  "CloseLuaWidgetPanel(") != std::string::npos &&
                !dockDpiChangeHandler.empty() &&
                dockDpiChangeHandler.find(
                  "InvalidateFloatingDockWindow(host, false);") !=
                    std::string::npos &&
                dockDpiChangeHandler.find(
                  "CloseFloatingDock(") == std::string::npos &&
                dockDisplayChangeHandler.find(
                  "CloseAllFloatingDocks();") != std::string::npos,
            "cross-monitor DPI migration must preserve reusable popup and Dock hosts while a real display change may dismiss them");
        Check(floatingDockSource.find(
                  "ReserveCollectionPopupEnvelope") ==
                    std::string::npos &&
                floatingDockSource.find(
                  "const RECT nextPopupRect{};") !=
                    std::string::npos,
            "the floating Dock host must not reserve or render collection popup space");
        Check(floatingDockInteractionSource.find(
                  "EnsureFloatingDockVisibleForAssociatedSurface(") !=
                    std::string::npos &&
                floatingDockInteractionSource.find(
                  "ShowFloatingDock(monitor);") !=
                    std::string::npos &&
                floatingDockSource.find(
                  "SelectFloatingDockContainerForMonitor(") !=
                    std::string::npos,
            "Dock-associated surfaces must promote the persistent DockHost selected for their monitor");
        Check(floatingDockInteractionSource.find(
                  "CloseCollectionPopup();") ==
                    std::string::npos &&
                floatingDockInteractionSource.find(
                  "FinalizeCloseCollectionPopup();") ==
                    std::string::npos,
            "closing the floating Dock must not own or finalize a shared popup");
        Check(floatingPopupSource.find(
                  "CloseFloatingDock(") ==
                    std::string::npos &&
                pointerDownSource.find(
                  "ClaimDockAssociatedPopupPointerPress") ==
                    std::string::npos &&
                floatingPopupSource.find(
                  "ClaimDockAssociatedPopupPointerPress") ==
                    std::string::npos &&
                floatingDockLifecycleSource.find(
                  "DockAssociatedPopupOwnsPointerDown") ==
                    std::string::npos &&
                appHeaderSource.find(
                  "dockAssociatedPopupPointerPressClaimed_") ==
                    std::string::npos,
            "popup and floating Dock lifecycles must observe one outside press independently without ownership claims or cross-close calls");
        const std::size_t edgeMouseHookCallbackBegin =
            floatingDockLifecycleSource.find(
                "LRESULT CALLBACK DesktopApp::FloatingDockEdgeSwipeMouseHookProc(");
        const std::size_t edgeMouseHookStartBegin =
            floatingDockLifecycleSource.find(
                "bool DesktopApp::StartFloatingDockEdgeSwipeMouseMonitor()",
                edgeMouseHookCallbackBegin);
        const std::size_t edgeMouseHookStopBegin =
            floatingDockLifecycleSource.find(
                "void DesktopApp::StopFloatingDockEdgeSwipeMouseMonitor()",
                edgeMouseHookStartBegin);
        const std::size_t floatingDockUnregisterBegin =
            floatingDockLifecycleSource.find(
                "void DesktopApp::UnregisterFloatingDockHotkey()",
                edgeMouseHookStopBegin);
        const std::size_t floatingDockApplyBegin =
            floatingDockLifecycleSource.find(
                "void DesktopApp::ApplyFloatingDockHotkey()",
                floatingDockUnregisterBegin);
        const std::size_t floatingDockPassiveRevealBegin =
            floatingDockLifecycleSource.find(
                "bool DesktopApp::UpdatePassiveDragRevealHosts(",
                floatingDockApplyBegin);
        const std::string edgeMouseHookCallbackSource =
            edgeMouseHookCallbackBegin != std::string::npos &&
                    edgeMouseHookStartBegin != std::string::npos
                ? floatingDockLifecycleSource.substr(
                    edgeMouseHookCallbackBegin,
                    edgeMouseHookStartBegin - edgeMouseHookCallbackBegin)
                : std::string{};
        const std::string edgeMouseHookStartSource =
            edgeMouseHookStartBegin != std::string::npos &&
                    edgeMouseHookStopBegin != std::string::npos
                ? floatingDockLifecycleSource.substr(
                    edgeMouseHookStartBegin,
                    edgeMouseHookStopBegin - edgeMouseHookStartBegin)
                : std::string{};
        const std::string edgeMouseHookStopSource =
            edgeMouseHookStopBegin != std::string::npos &&
                    floatingDockUnregisterBegin != std::string::npos
                ? floatingDockLifecycleSource.substr(
                    edgeMouseHookStopBegin,
                    floatingDockUnregisterBegin - edgeMouseHookStopBegin)
                : std::string{};
        const std::string floatingDockUnregisterSource =
            floatingDockUnregisterBegin != std::string::npos &&
                    floatingDockApplyBegin != std::string::npos
                ? floatingDockLifecycleSource.substr(
                    floatingDockUnregisterBegin,
                    floatingDockApplyBegin - floatingDockUnregisterBegin)
                : std::string{};
        const std::string floatingDockApplySource =
            floatingDockApplyBegin != std::string::npos &&
                    floatingDockPassiveRevealBegin != std::string::npos
                ? floatingDockLifecycleSource.substr(
                    floatingDockApplyBegin,
                    floatingDockPassiveRevealBegin - floatingDockApplyBegin)
                : std::string{};
        Check(!edgeMouseHookCallbackSource.empty() &&
                edgeMouseHookCallbackSource.find("WM_LBUTTONDOWN") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("WM_LBUTTONUP") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("WM_RBUTTONDOWN") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("WM_RBUTTONUP") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("WM_MBUTTONDOWN") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("WM_MBUTTONUP") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("WM_XBUTTONDOWN") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("WM_XBUTTONUP") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find(
                  "floatingDockEdgeSwipeMouseActivity_.store(") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find(
                  "CallNextHookEx(nullptr, code, message, data)") !=
                    std::string::npos &&
                edgeMouseHookCallbackSource.find("PostMessage") ==
                    std::string::npos,
            "the edge-swipe mouse hook must only record low-level L/R/M/X button down/up activity and continue the hook chain");
        const std::size_t edgeMouseHookInstall =
            edgeMouseHookStartSource.find("SetWindowsHookExW(");
        const std::size_t edgeMouseHookKind =
            edgeMouseHookStartSource.find(
                "WH_MOUSE_LL", edgeMouseHookInstall);
        const std::size_t edgeMouseHookUninstall =
            edgeMouseHookStopSource.find("UnhookWindowsHookEx(");
        const std::size_t edgeMouseHookClear =
            edgeMouseHookStopSource.find(
                "floatingDockEdgeSwipeMouseActivity_.store(",
                edgeMouseHookUninstall);
        Check(!edgeMouseHookStartSource.empty() &&
                !edgeMouseHookStopSource.empty() &&
                edgeMouseHookInstall != std::string::npos &&
                edgeMouseHookKind != std::string::npos &&
                edgeMouseHookUninstall != std::string::npos &&
                edgeMouseHookClear != std::string::npos &&
                edgeMouseHookInstall < edgeMouseHookKind &&
                edgeMouseHookUninstall < edgeMouseHookClear &&
                appHeaderSource.find(
                  "HHOOK floatingDockEdgeSwipeMouseHook_ = nullptr;") !=
                    std::string::npos &&
                appHeaderSource.find(
                  "floatingDockEdgeSwipeMouseActivity_{ false };") !=
                    std::string::npos,
            "the edge-swipe activity observer must be a lightweight WH_MOUSE_LL hook with explicit handle and activity cleanup");
        const std::size_t repeatedApplyCleanup =
            floatingDockApplySource.find(
                "UnregisterFloatingDockHotkey();");
        const std::size_t effectiveEdgeState =
            floatingDockApplySource.find(
                "const bool edgeSwipeEnabled =",
                repeatedApplyCleanup);
        const std::size_t effectiveEdgeRule =
            floatingDockApplySource.find(
                "IsFloatingEdgeSwipeEnabled(", effectiveEdgeState);
        const std::size_t edgeSamplerTimerInstall =
            floatingDockApplySource.find(
                "SetTimer(", effectiveEdgeRule);
        const std::size_t effectiveEdgeHookGuard =
            floatingDockApplySource.find(
                "if (edgeSwipeEnabled)", edgeSamplerTimerInstall);
        const std::size_t effectiveEdgeHookStart =
            floatingDockApplySource.find(
                "StartFloatingDockEdgeSwipeMouseMonitor();",
                effectiveEdgeHookGuard);
        Check(!floatingDockApplySource.empty() &&
                repeatedApplyCleanup != std::string::npos &&
                effectiveEdgeState != std::string::npos &&
                effectiveEdgeRule != std::string::npos &&
                edgeSamplerTimerInstall != std::string::npos &&
                effectiveEdgeHookGuard != std::string::npos &&
                effectiveEdgeHookStart != std::string::npos &&
                repeatedApplyCleanup < effectiveEdgeState &&
                effectiveEdgeState < effectiveEdgeRule &&
                effectiveEdgeRule < edgeSamplerTimerInstall &&
                edgeSamplerTimerInstall < effectiveEdgeHookGuard &&
                effectiveEdgeHookGuard < effectiveEdgeHookStart &&
                CountOccurrences(
                  floatingDockApplySource,
                  "StartFloatingDockEdgeSwipeMouseMonitor();") == 1 &&
                floatingDockUnregisterSource.find(
                  "StopFloatingDockEdgeSwipeMouseMonitor();") !=
                    std::string::npos &&
                lifecycleSource.find(
                  "DesktopApp::~DesktopApp()") !=
                    std::string::npos &&
                lifecycleSource.find(
                  "UnregisterFloatingDockHotkey();",
                  lifecycleSource.find(
                    "DesktopApp::~DesktopApp()")) !=
                    std::string::npos &&
                messageDispatchSource.find(
                  "UnregisterFloatingDockHotkey();",
                  messageDispatchSource.find("case WM_DESTROY:")) !=
                    std::string::npos,
            "effective edge-swipe setup must install one observer after timer setup while repeated apply, unregister and destruction tear it down first");
        const std::size_t dockPointerSamplerBegin =
            floatingDockLifecycleSource.find(
                "void DesktopApp::UpdateFloatingDockEdgeSwipe()");
        const std::string dockPointerSamplerSource =
            dockPointerSamplerBegin == std::string::npos
                ? std::string{}
                : floatingDockLifecycleSource.substr(
                    dockPointerSamplerBegin);
        const std::size_t outsideDockClose =
            dockPointerSamplerSource.find(
                "CloseAllFloatingDocks(");
        const std::size_t preserveOutsideForeground =
            dockPointerSamplerSource.find(
                "FloatingDockCloseFocusPolicy::PreserveCurrent",
                outsideDockClose);
        Check(!dockPointerSamplerSource.empty() &&
                outsideDockClose != std::string::npos &&
                dockPointerSamplerSource.find(
                  "IsPointOnPromotedDock(desktopPoint)") !=
                    std::string::npos &&
                preserveOutsideForeground != std::string::npos,
            "outside-click dismissal must ignore every promoted Dock and preserve the foreground selected by a true external click");
        const std::size_t passiveDragUpdateBegin =
            floatingDockLifecycleSource.find(
                "bool DesktopApp::UpdatePassiveDragRevealHosts(");
        const std::size_t passiveDragUpdateEnd =
            floatingDockLifecycleSource.find(
                "void DesktopApp::UpdateFloatingDockEdgeSwipe()",
                passiveDragUpdateBegin);
        const std::string passiveDragUpdateSource =
            passiveDragUpdateBegin != std::string::npos &&
                    passiveDragUpdateEnd != std::string::npos
                ? floatingDockLifecycleSource.substr(
                    passiveDragUpdateBegin,
                    passiveDragUpdateEnd - passiveDragUpdateBegin)
                : std::string{};
        const std::size_t passiveDragSamplerCall =
            dockPointerSamplerSource.find(
                "UpdatePassiveDragRevealHosts(cursor)");
        const std::size_t pointerActivityReducer =
            dockPointerSamplerSource.find(
                "const bool pointerButtonActivity =",
                passiveDragSamplerCall);
        const std::size_t edgeMouseHookActivityConsume =
            dockPointerSamplerSource.find(
                "floatingDockEdgeSwipeMouseActivity_.exchange(",
                pointerActivityReducer);
        const std::size_t foregroundGuiMenuQuery =
            dockPointerSamplerSource.find(
                "GetGUIThreadInfo(0, &foregroundGuiThreadInfo)",
                edgeMouseHookActivityConsume);
        const std::size_t foregroundGuiMenuRule =
            dockPointerSamplerSource.find(
                "IsGuiMenuModeActive(",
                foregroundGuiMenuQuery);
        const std::size_t edgeSwipeSuppressionState =
            dockPointerSamplerSource.find(
                "const bool suppressEdgeSwipeUntilLeave =",
                foregroundGuiMenuRule);
        const std::size_t contextMenuGestureGuard =
            dockPointerSamplerSource.find(
                "HasActiveContextMenuSession() ||",
                edgeSwipeSuppressionState);
        const std::size_t foregroundGuiMenuGuard =
            dockPointerSamplerSource.find(
                "foregroundGuiMenuActive ||",
                contextMenuGestureGuard);
        const std::size_t edgeSwipeSuppressionCall =
            dockPointerSamplerSource.find(
                "SuppressUntilEdgeLeave();",
                contextMenuGestureGuard);
        const std::size_t edgeSwipeDetectorUpdate =
            dockPointerSamplerSource.find(
                "floatingDockEdgeSwipeDetector_.Update(",
                edgeSwipeSuppressionCall);
        const std::size_t edgeSwipeTriggerBranch =
            dockPointerSamplerSource.find(
                "if (triggered &&",
                edgeSwipeDetectorUpdate);
        const std::size_t edgeSwipeSummon =
            dockPointerSamplerSource.find(
                "ShowFloatingDock(monitor);",
                edgeSwipeTriggerBranch);
        Check(passiveDragSamplerCall != std::string::npos &&
                pointerActivityReducer != std::string::npos &&
                edgeMouseHookActivityConsume != std::string::npos &&
                foregroundGuiMenuQuery != std::string::npos &&
                foregroundGuiMenuRule != std::string::npos &&
                edgeSwipeSuppressionState != std::string::npos &&
                contextMenuGestureGuard != std::string::npos &&
                foregroundGuiMenuGuard != std::string::npos &&
                edgeSwipeSuppressionCall != std::string::npos &&
                edgeSwipeDetectorUpdate != std::string::npos &&
                passiveDragSamplerCall < pointerActivityReducer &&
                pointerActivityReducer < edgeMouseHookActivityConsume &&
                edgeMouseHookActivityConsume <
                    foregroundGuiMenuQuery &&
                foregroundGuiMenuQuery < foregroundGuiMenuRule &&
                foregroundGuiMenuRule <
                    edgeSwipeSuppressionState &&
                edgeSwipeSuppressionState <
                    contextMenuGestureGuard &&
                contextMenuGestureGuard < foregroundGuiMenuGuard &&
                foregroundGuiMenuGuard <
                    edgeSwipeSuppressionCall &&
                edgeSwipeSuppressionCall < edgeSwipeDetectorUpdate,
            "the sampler must preserve passive drag reveal ordering, consume low-level button activity, hold suppression throughout native menu loops, and suppress before edge detection");
        const std::string edgeSwipeTriggerSource =
            edgeSwipeTriggerBranch != std::string::npos &&
                    edgeSwipeSummon != std::string::npos
                ? dockPointerSamplerSource.substr(
                    edgeSwipeTriggerBranch,
                    edgeSwipeSummon - edgeSwipeTriggerBranch +
                        sizeof("ShowFloatingDock(monitor);") - 1)
                : std::string{};
        const std::size_t directRevealGate =
            passiveDragUpdateSource.find(
                "ShouldPassivelyRevealDockForDragAtEdge(");
        const std::size_t passiveDragReducer =
            passiveDragUpdateSource.find(
                "ResolvePassiveDragRevealUpdate(",
                directRevealGate);
        const std::size_t gatedRevealRequest =
            passiveDragUpdateSource.find(
                "passiveDragRevealRequested,",
                passiveDragReducer);
        const std::size_t passiveDragReducerEnd =
            passiveDragUpdateSource.find(
                "leaveDelayElapsed);",
                passiveDragReducer);
        const std::string passiveDragReducerCall =
            passiveDragReducer != std::string::npos &&
                    passiveDragReducerEnd != std::string::npos
                ? passiveDragUpdateSource.substr(
                    passiveDragReducer,
                    passiveDragReducerEnd - passiveDragReducer)
                : std::string{};
        const std::size_t prepareRevealFrame =
            passiveDragUpdateSource.find(
                "RenderFloatingDockCompositionFrame(host)");
        const std::size_t flushRevealFrame =
            passiveDragUpdateSource.find(
                "FlushPendingCompositionCommit()",
                prepareRevealFrame);
        const std::size_t revealPairVisibility =
            passiveDragUpdateSource.find(
                "UpdatePersistentDockHostVisibility(host);",
                flushRevealFrame);
        Check(!passiveDragUpdateSource.empty() &&
                appHeaderSource.find(
                  "bool UpdatePassiveDragRevealHosts(\n"
                  "        POINT cursorScreen);") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "IsPointInDockEdgeProjection(") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "IsPointInDockEdgeCorridor(") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "dragSession_.IsActive()") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "dragDropController_.IsTransportActive()") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "const bool keepPassiveDragReveal =") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "(dragRevealActive &&\n"
                  "                pointerInEdgeCorridor);") !=
                    std::string::npos &&
                directRevealGate != std::string::npos &&
                passiveDragReducer != std::string::npos &&
                gatedRevealRequest != std::string::npos &&
                directRevealGate < passiveDragReducer &&
                passiveDragReducer < gatedRevealRequest &&
                passiveDragReducerCall.find(
                  "passiveDragRevealRequested,") !=
                    std::string::npos &&
                passiveDragReducerCall.find(
                  "pointerInEdgeProjection,") ==
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "collectionPopupDockHost_ == &host") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "quickNavigationDockHost_ == &host") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "dockWindowPreview_->IsVisible()") !=
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "HasActiveContextMenuSession()") ==
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "ShowFloatingDock(") ==
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "BeginFloatingDockKeyboardSession(") ==
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "SetForegroundWindow(") ==
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "RefocusFloatingDockKeyboardSession(") ==
                    std::string::npos &&
                passiveDragUpdateSource.find(
                  "EnsureFloatingDockInputWindow(") ==
                    std::string::npos &&
                prepareRevealFrame != std::string::npos &&
                flushRevealFrame != std::string::npos &&
                revealPairVisibility != std::string::npos &&
                prepareRevealFrame < flushRevealFrame &&
                flushRevealFrame < revealPairVisibility,
            "passive drag reveal must be per-Host, focusless, pre-rendered and retained only by an active drag corridor or associated Dock surface");
        Check(passiveDragSamplerCall != std::string::npos &&
                dockPointerSamplerSource.find(
                  "IsFloatingEdgeSwipeEnabled(\n"
                  "                dockSettings_.showOnlyWhenSummoned,\n"
                  "                dockSettings_.floatingEdgeSwipeEnabled)") !=
                    std::string::npos &&
                contextMenuGestureGuard != std::string::npos &&
                edgeSwipeSuppressionState != std::string::npos &&
                edgeSwipeSuppressionCall != std::string::npos &&
                edgeSwipeDetectorUpdate != std::string::npos &&
                edgeSwipeTriggerBranch != std::string::npos &&
                edgeSwipeSummon != std::string::npos &&
                edgeSwipeTriggerSource.find(
                  "ShowFloatingDock(monitor);") !=
                    std::string::npos &&
                edgeSwipeTriggerSource.find(
                  "showOnlyWhenSummoned") ==
                    std::string::npos &&
                passiveDragSamplerCall < edgeSwipeSuppressionState &&
                edgeSwipeSuppressionState < contextMenuGestureGuard &&
                contextMenuGestureGuard < edgeSwipeSuppressionCall &&
                edgeSwipeSuppressionCall < edgeSwipeDetectorUpdate &&
                edgeSwipeDetectorUpdate < edgeSwipeTriggerBranch &&
                edgeSwipeTriggerBranch < edgeSwipeSummon,
            "ordinary edge swipe must keep the manual summon path while pointer-button activity, context menus, internal drags and OLE drags remain suppressed until the pointer leaves the edge");
        const std::size_t containsPointBegin =
            dockContainerSource.find(
                "bool DockContainer::ContainsInteractivePoint(");
        const std::size_t containsPointEnd =
            dockContainerSource.find(
                "RECT DockContainer::GetInteractiveBounds() const",
                containsPointBegin);
        const std::string containsPointSource =
            containsPointBegin != std::string::npos &&
                    containsPointEnd != std::string::npos
                ? dockContainerSource.substr(
                    containsPointBegin,
                    containsPointEnd - containsPointBegin)
                : std::string{};
        const std::size_t hitTestDragBegin =
            dockContainerSource.find(
                "HitRegion DockContainer::HitTestDrag(");
        const std::size_t hitTestDragEnd =
            dockContainerSource.find(
                "std::wstring DockContainer::GetDragHint(",
                hitTestDragBegin);
        const std::string hitTestDragSource =
            hitTestDragBegin != std::string::npos &&
                    hitTestDragEnd != std::string::npos
                ? dockContainerSource.substr(
                    hitTestDragBegin,
                    hitTestDragEnd - hitTestDragBegin)
                : std::string{};
        const std::size_t containsVisibilityGate =
            containsPointSource.find(
                "IsDockContainerInteractionVisible(this)");
        const std::size_t containsBoundsRead =
            containsPointSource.find("GetInteractiveBounds()");
        const std::size_t dragVisibilityGate =
            hitTestDragSource.find(
                "IsDockContainerInteractionVisible(this)");
        const std::size_t dragBoundsRead =
            hitTestDragSource.find("GetBounds()");
        Check(appHeaderSource.find(
                  "bool passivelyRevealed = false;") !=
                    std::string::npos &&
                appHeaderSource.find(
                  "ULONGLONG passiveLeaveStartTick = 0;") !=
                    std::string::npos &&
                floatingDockSource.find(
                  "ShouldShowPersistentDockHost(host)") !=
                    std::string::npos &&
                containsVisibilityGate != std::string::npos &&
                containsBoundsRead != std::string::npos &&
                containsVisibilityGate < containsBoundsRead &&
                dragVisibilityGate != std::string::npos &&
                dragBoundsRead != std::string::npos &&
                dragVisibilityGate < dragBoundsRead &&
                floatingDockSource.find(
                  "return !host || !host->active ||\n"
                  "        ShouldShowPersistentDockHost(*host);") !=
                    std::string::npos,
            "each DockHost must own independent passive state and hidden Dock containers must reject pointer and drag hit tests");
        const std::size_t manualPromotionBegin =
            floatingDockSource.find(
                "bool DesktopApp::IsPersistentDockHostPromoted(");
        const std::size_t effectiveFloatingBegin =
            floatingDockSource.find(
                "bool DesktopApp::IsPersistentDockHostEffectivelyFloating(",
                manualPromotionBegin);
        const std::size_t effectiveFloatingEnd =
            floatingDockSource.find(
                "bool DesktopApp::ShouldShowPersistentDockHost(",
                effectiveFloatingBegin);
        const std::string manualPromotionSource =
            manualPromotionBegin != std::string::npos &&
                    effectiveFloatingBegin != std::string::npos
                ? floatingDockSource.substr(
                    manualPromotionBegin,
                    effectiveFloatingBegin - manualPromotionBegin)
                : std::string{};
        const std::string effectiveFloatingSource =
            effectiveFloatingBegin != std::string::npos &&
                    effectiveFloatingEnd != std::string::npos
                ? floatingDockSource.substr(
                    effectiveFloatingBegin,
                    effectiveFloatingEnd - effectiveFloatingBegin)
                : std::string{};
        Check(passiveDragUpdateSource.find(
                  "host.promoted,") !=
                    std::string::npos &&
                manualPromotionSource.find(
                  "return host.active && host.promoted;") !=
                    std::string::npos &&
                manualPromotionSource.find(
                  "passivelyRevealed") ==
                    std::string::npos &&
                effectiveFloatingSource.find(
                  "IsDockEffectivelyPromoted(") !=
                    std::string::npos &&
                dockPreviewSource.find(
                  "IsDockContainerEffectivelyFloating(dock)") !=
                    std::string::npos &&
                pointerDownSource.find(
                  "IsDockContainerEffectivelyFloating(dock)") !=
                    std::string::npos &&
                CountOccurrences(
                  pointerReleaseSource,
                  "IsDockContainerEffectivelyFloating(dock)") >= 2 &&
                popupLifecycleSource.find(
                  "IsPersistentDockHostEffectivelyFloating(*host)") !=
                    std::string::npos &&
                popupTransitionSource.find(
                  "IsPersistentDockHostEffectivelyFloating(*host)") !=
                    std::string::npos &&
                sceneSource.find(
                  "IsPersistentDockHostEffectivelyFloating(") !=
                    std::string::npos &&
                CountOccurrences(
                  settingsApplySource,
                  "IsDockContainerEffectivelyFloating(") >= 2 &&
                settingsApplySource.find(
                  "IsPersistentDockHostEffectivelyFloating(") !=
                    std::string::npos &&
                CountOccurrences(
                  dockWindowTrackingSource,
                  "IsPersistentDockHostEffectivelyFloating(") >= 2 &&
                shellMenuSource.find(
                  "IsPersistentDockHostEffectivelyFloating(host)") !=
                    std::string::npos &&
                floatingDockInteractionSource.find(
                  "host.passivelyRevealed = false;") !=
                    std::string::npos &&
                floatingDockInteractionSource.find(
                  "host->passivelyRevealed = false;") !=
                    std::string::npos,
            "manual promotion must be distinct from passive reveal while explicit close clears both states");
        const std::size_t boundsUpdateBegin =
            floatingDockSource.find(
                "void DesktopApp::UpdateFloatingDockWindowBounds(\n"
                "    PersistentDockHost& host,");
        const std::string boundsUpdateSource =
            boundsUpdateBegin == std::string::npos
                ? std::string{}
                : floatingDockSource.substr(boundsUpdateBegin);
        const std::size_t visibilityUpdateBegin =
            floatingDockSource.find(
                "void DesktopApp::UpdatePersistentDockHostVisibility(\n"
                "    PersistentDockHost& host)");
        const std::size_t visibilityUpdateEnd =
            floatingDockSource.find(
                "void DesktopApp::UpdatePersistentDockHostVisibility()",
                visibilityUpdateBegin);
        const std::string visibilityUpdateSource =
            visibilityUpdateBegin != std::string::npos &&
                    visibilityUpdateEnd != std::string::npos
                ? floatingDockSource.substr(
                    visibilityUpdateBegin,
                    visibilityUpdateEnd - visibilityUpdateBegin)
                : std::string{};
        Check(!boundsUpdateSource.empty() &&
                boundsUpdateSource.find("SWP_SHOWWINDOW") ==
                    std::string::npos &&
                boundsUpdateSource.find(
                  "InitializePopup(\n"
                  "                    dockHostHwnd,\n"
                  "                    floatingLayerTopmost,\n"
                  "                    false)") !=
                    std::string::npos &&
                boundsUpdateSource.find(
                  "UpdatePersistentDockHostVisibility(host);") !=
                    std::string::npos &&
                visibilityUpdateSource.find(
                  "ShouldShowPersistentDockHost(host)") !=
                    std::string::npos &&
                visibilityUpdateSource.find(
                  "ShowPopupWindowPair(host.hwnd)") !=
                    std::string::npos,
            "idle summon-only Hosts must remain hidden across bounds, region and backdrop layout refreshes");
        const std::size_t closeFloatingDockBegin =
            floatingDockInteractionSource.find(
                "void DesktopApp::CloseFloatingDock(\n"
                "    PersistentDockHost& host");
        const std::size_t closeFloatingDockEnd =
            floatingDockInteractionSource.find(
                "void DesktopApp::CloseAllFloatingDocks(",
                closeFloatingDockBegin);
        const std::string closeFloatingDockSource =
            closeFloatingDockBegin != std::string::npos &&
                    closeFloatingDockEnd != std::string::npos
                ? floatingDockInteractionSource.substr(
                    closeFloatingDockBegin,
                    closeFloatingDockEnd - closeFloatingDockBegin)
                : std::string{};
        const std::size_t closeDemoted =
            closeFloatingDockSource.find(
                "host.promoted = false;");
        const std::size_t closeRightButtonPressCleared =
            closeFloatingDockSource.find(
                "rightButtonDownDockHost_ = nullptr;");
        const std::size_t closeAggregateUpdated =
            closeFloatingDockSource.find(
                "RefreshFloatingDockVisibilityState();",
                closeDemoted);
        const std::size_t closeVisibilityUpdated =
            closeFloatingDockSource.find(
                "UpdatePersistentDockHostVisibility(",
                closeAggregateUpdated);
        const std::size_t closeKeyboardEnded =
            closeFloatingDockSource.find(
                "EndFloatingDockKeyboardSession(focusPolicy);",
                closeVisibilityUpdated);
        const std::size_t closeActionRun =
            closeFloatingDockSource.find(
                "if (action)",
                closeVisibilityUpdated);
        Check(!closeFloatingDockSource.empty() &&
                closeRightButtonPressCleared != std::string::npos &&
                closeDemoted != std::string::npos &&
                closeAggregateUpdated != std::string::npos &&
                closeVisibilityUpdated != std::string::npos &&
                closeKeyboardEnded != std::string::npos &&
                closeActionRun != std::string::npos &&
                closeRightButtonPressCleared < closeDemoted &&
                closeDemoted < closeAggregateUpdated &&
                closeAggregateUpdated < closeVisibilityUpdated &&
                closeVisibilityUpdated < closeKeyboardEnded &&
                closeKeyboardEnded < closeActionRun &&
                closeFloatingDockSource.find(
                  "ApplyFloatingDockLayerPolicy();") ==
                    std::string::npos &&
                closeFloatingDockSource.find(
                  "SetOpacity(") == std::string::npos &&
                closeFloatingDockSource.find(
                  "HidePopupWindowPair(") == std::string::npos &&
                closeFloatingDockSource.find(
                  "WaitForCompositionPresentation(") ==
                    std::string::npos &&
                closeFloatingDockSource.find(
                  "DwmFlush()") == std::string::npos,
            "closing a persistent DockHost must cancel its pending right-button press and demote its window pair before changing foreground focus or running the queued command");
        const std::size_t closeAllFloatingDocksBegin =
            floatingDockInteractionSource.find(
                "void DesktopApp::CloseAllFloatingDocks(");
        const std::size_t closeAllFloatingDocksEnd =
            floatingDockInteractionSource.find(
                "void DesktopApp::CloseFloatingDockThen(",
                closeAllFloatingDocksBegin);
        const std::string closeAllFloatingDocksSource =
            closeAllFloatingDocksBegin != std::string::npos &&
                    closeAllFloatingDocksEnd != std::string::npos
                ? floatingDockInteractionSource.substr(
                    closeAllFloatingDocksBegin,
                    closeAllFloatingDocksEnd -
                        closeAllFloatingDocksBegin)
                : std::string{};
        const std::size_t closeAllDemoted =
            closeAllFloatingDocksSource.find(
                "host->promoted = false;");
        const std::size_t closeAllRightButtonPressCleared =
            closeAllFloatingDocksSource.find(
                "rightButtonDownDockHost_ = nullptr;");
        const std::size_t closeAllVisibilityUpdated =
            closeAllFloatingDocksSource.find(
                "UpdatePersistentDockHostVisibility(*host);",
                closeAllDemoted);
        const std::size_t closeAllKeyboardEnded =
            closeAllFloatingDocksSource.find(
                "EndFloatingDockKeyboardSession(focusPolicy);",
                closeAllVisibilityUpdated);
        Check(!closeAllFloatingDocksSource.empty() &&
                closeAllRightButtonPressCleared !=
                    std::string::npos &&
                closeAllDemoted != std::string::npos &&
                closeAllVisibilityUpdated != std::string::npos &&
                closeAllKeyboardEnded != std::string::npos &&
                closeAllRightButtonPressCleared < closeAllDemoted &&
                closeAllDemoted < closeAllVisibilityUpdated &&
                closeAllVisibilityUpdated < closeAllKeyboardEnded,
            "closing every promoted Dock must cancel pending right-button ownership and finish all pair demotions before the foreground input proxy is hidden");

        const std::size_t showFloatingDockBegin =
            floatingDockInteractionSource.find(
                "void DesktopApp::ShowFloatingDock(");
        const std::size_t showFloatingDockEnd =
            floatingDockInteractionSource.find(
                "EnsureFloatingDockVisibleForAssociatedSurface(",
                showFloatingDockBegin);
        const std::string persistentShowSource =
            showFloatingDockBegin != std::string::npos &&
                    showFloatingDockEnd != std::string::npos
                ? floatingDockInteractionSource.substr(
                    showFloatingDockBegin,
                    showFloatingDockEnd - showFloatingDockBegin)
                : std::string{};
        const std::size_t showPreviousMonitorCaptured =
            persistentShowSource.find(
                "const HMONITOR previouslySelectedMonitor =");
        const std::size_t showHostSynced =
            persistentShowSource.find(
                "SyncPersistentDockHost(targetMonitor)",
                showPreviousMonitorCaptured);
        const std::size_t showHostVisibilityCaptured =
            persistentShowSource.find(
                "const bool hostWasVisible =",
                showHostSynced);
        const std::size_t showPhysicalVisibilityRead =
            persistentShowSource.find(
                "IsWindowVisible(floatingDockHost_->hwnd)",
                showHostVisibilityCaptured);
        const std::size_t showPromoted =
            persistentShowSource.find(
                "floatingDockHost_->promoted = true;",
                showPhysicalVisibilityRead);
        const std::size_t showAggregateUpdated =
            persistentShowSource.find(
                "RefreshFloatingDockVisibilityState();",
                showPromoted);
        const std::size_t showHiddenHostBranch =
            persistentShowSource.find(
                "if (!hostWasVisible)",
                showAggregateUpdated);
        const std::size_t showFrameRendered =
            persistentShowSource.find(
                "RenderFloatingDockCompositionFrame(*floatingDockHost_)",
                showHiddenHostBranch);
        const std::size_t showFrameFlushed =
            persistentShowSource.find(
                "FlushPendingCompositionCommit()",
                showFrameRendered);
        const std::size_t showFrameFailureGuard =
            persistentShowSource.find(
                "if (!revealFramePrepared)",
                showFrameFlushed);
        const std::size_t showFailureDemoted =
            persistentShowSource.find(
                "floatingDockHost_->promoted = false;",
                showFrameFailureGuard);
        const std::size_t showFailureAggregateUpdated =
            persistentShowSource.find(
                "RefreshFloatingDockVisibilityState();",
                showFailureDemoted);
        const std::size_t showFailureRepaintQueued =
            persistentShowSource.find(
                "InvalidateFloatingDockWindow(",
                showFailureAggregateUpdated);
        const std::size_t showPreviousMonitorRestored =
            persistentShowSource.find(
                "SyncPersistentDockHost(",
                showFailureRepaintQueued);
        const std::size_t showPreviousMonitorRestoreArgument =
            persistentShowSource.find(
                "previouslySelectedMonitor);",
                showPreviousMonitorRestored);
        const std::size_t showFailureReturned =
            persistentShowSource.find(
                "return;",
                showPreviousMonitorRestoreArgument);
        const std::size_t showVisibilityUpdated =
            persistentShowSource.find(
                "UpdatePersistentDockHostVisibility(",
                showFrameFailureGuard);
        const std::size_t showVisibleHostBranch =
            persistentShowSource.find(
                "if (hostWasVisible)",
                showVisibilityUpdated);
        const std::size_t showVisibleHostRepaint =
            persistentShowSource.find(
                "InvalidateFloatingDockWindow(",
                showVisibleHostBranch);
        const std::size_t showKeyboardSessionStarted =
            persistentShowSource.find(
                "BeginFloatingDockKeyboardSession();",
                showVisibleHostRepaint);
        Check(!persistentShowSource.empty() &&
                showPreviousMonitorCaptured != std::string::npos &&
                showHostSynced != std::string::npos &&
                showHostVisibilityCaptured != std::string::npos &&
                showPhysicalVisibilityRead != std::string::npos &&
                showPromoted != std::string::npos &&
                showAggregateUpdated != std::string::npos &&
                showHiddenHostBranch != std::string::npos &&
                showFrameRendered != std::string::npos &&
                showFrameFlushed != std::string::npos &&
                showFrameFailureGuard != std::string::npos &&
                showFailureDemoted != std::string::npos &&
                showFailureAggregateUpdated != std::string::npos &&
                showFailureRepaintQueued != std::string::npos &&
                showPreviousMonitorRestored != std::string::npos &&
                showPreviousMonitorRestoreArgument != std::string::npos &&
                showFailureReturned != std::string::npos &&
                showVisibilityUpdated != std::string::npos &&
                showVisibleHostBranch != std::string::npos &&
                showVisibleHostRepaint != std::string::npos &&
                showKeyboardSessionStarted != std::string::npos &&
                showPreviousMonitorCaptured < showHostSynced &&
                showHostSynced < showHostVisibilityCaptured &&
                showHostVisibilityCaptured < showPhysicalVisibilityRead &&
                showPhysicalVisibilityRead < showPromoted &&
                showPromoted < showAggregateUpdated &&
                showAggregateUpdated < showHiddenHostBranch &&
                showHiddenHostBranch < showFrameRendered &&
                showFrameRendered < showFrameFlushed &&
                showFrameFlushed < showFrameFailureGuard &&
                showFrameFailureGuard < showFailureDemoted &&
                showFailureDemoted < showFailureAggregateUpdated &&
                showFailureAggregateUpdated < showFailureRepaintQueued &&
                showFailureRepaintQueued < showPreviousMonitorRestored &&
                showPreviousMonitorRestored <
                    showPreviousMonitorRestoreArgument &&
                showPreviousMonitorRestoreArgument < showFailureReturned &&
                showFailureReturned < showVisibilityUpdated &&
                showVisibilityUpdated < showVisibleHostBranch &&
                showVisibleHostBranch < showVisibleHostRepaint &&
                showVisibleHostRepaint < showKeyboardSessionStarted &&
                persistentShowSource.find(
                  "ApplyFloatingDockLayerPolicy();") ==
                    std::string::npos &&
                floatingDockSource.find(
                  "ApplyFloatingDockLayerPolicy(host);") !=
                    std::string::npos &&
                floatingDockSource.find(
                  "if (IsWindowVisible(host.hwnd))") !=
                    std::string::npos &&
                persistentShowSource.find(
                  "SetOpacity(") == std::string::npos &&
                persistentShowSource.find(
                  "WaitForCompositionPresentation(") ==
                    std::string::npos,
            "summoning a hidden persistent DockHost must render and flush before its atomic show, defer on frame-preparation failure, and start keyboard input only after visibility is updated");
        Check(floatingDockInteractionSource.find(
                  "CompleteFloatingDockCloseHandoff") ==
                    std::string::npos &&
                floatingDockInteractionSource.find(
                  "FinishFloatingDockCloseHandoff") ==
                    std::string::npos &&
                floatingDockInteractionSource.find(
                  "floatingDockDesktopCache") ==
                    std::string::npos &&
                floatingDockSource.find(
                  "EnsureFloatingDockDesktopCacheVisual") ==
                    std::string::npos &&
                appHeaderSource.find(
                  "floatingDockBackdropCommitToken_") ==
                    std::string::npos &&
                appHeaderSource.find(
                  "floatingDockClosePending_") ==
                    std::string::npos,
            "the persistent DockHost must not retain the old dual-copy hand-off or asynchronous close state");

        const std::size_t desktopBandPolicyBegin =
            shellMenuSource.find(
                "if (!promoted)");
        const std::size_t floatingBandPolicyBegin =
            shellMenuSource.find(
                "const bool shouldBeTopmost =",
                desktopBandPolicyBegin);
        const std::string desktopBandPolicySource =
            desktopBandPolicyBegin != std::string::npos &&
                    floatingBandPolicyBegin != std::string::npos
                ? shellMenuSource.substr(
                    desktopBandPolicyBegin,
                    floatingBandPolicyBegin - desktopBandPolicyBegin)
                : std::string{};
        Check(!desktopBandPolicySource.empty() &&
                desktopBandPolicySource.find(
                  "desktopWindows_.host") != std::string::npos &&
                desktopBandPolicySource.find(
                  "GW_HWNDPREV") != std::string::npos &&
                desktopBandPolicySource.find(
                  "host.backdrop.SetPopupWindowPairZOrder(") !=
                    std::string::npos,
            "desktop mode must place the persistent DockHost pair above WorkerW through the shared policy");
        Check(shellMenuSource.find(
                  "host.backdrop.SetPopupTopmost(") ==
                    std::string::npos &&
                shellMenuSource.find(
                  "host.backdrop.Reattach(host.hwnd)") ==
                    std::string::npos,
            "Dock callers must not bypass the shared pair policy with separate content and backdrop restacks");
        Check(appHeaderSource.find(
                  "struct PersistentDockHost") !=
                    std::string::npos &&
                appHeaderSource.find(
                  "bool promoted = false;") !=
                    std::string::npos &&
                appHeaderSource.find(
                  "persistentDockHosts_") !=
                    std::string::npos &&
                floatingDockSource.find(
                  "existing->monitor == monitor") !=
                    std::string::npos &&
                floatingDockSource.find(
                  "created->monitor = monitor;") !=
                    std::string::npos &&
                floatingDockSource.find(
                  "CreateWindowExW(") !=
                    std::string::npos &&
                persistentShowSource.find(
                  "CreateWindowExW(") ==
                    std::string::npos,
            "each monitor must retain its own persistent DockHost and independent promotion state");
        const std::size_t toggleFloatingDockBegin =
            floatingDockInteractionSource.find(
                "void DesktopApp::ToggleFloatingDock()");
        const std::string toggleFloatingDockSource =
            toggleFloatingDockBegin == std::string::npos
                ? std::string{}
                : floatingDockInteractionSource.substr(
                    toggleFloatingDockBegin);
        Check(!toggleFloatingDockSource.empty() &&
                toggleFloatingDockSource.find(
                  "SyncPersistentDockHost(monitor)") !=
                    std::string::npos &&
                toggleFloatingDockSource.find(
                  "IsPersistentDockHostPromoted(*floatingDockHost_)") !=
                    std::string::npos &&
                toggleFloatingDockSource.find(
                  "CloseFloatingDock(*floatingDockHost_)") !=
                    std::string::npos &&
                toggleFloatingDockSource.find(
                  "ShowFloatingDock(monitor)") !=
                    std::string::npos,
            "the shortcut must toggle the DockHost on the cursor monitor instead of treating any promoted host as a global singleton");
        const std::size_t syncDockHostsBegin =
            floatingDockSource.find(
                "bool DesktopApp::SyncPersistentDockHosts()");
        const std::size_t syncDockHostsEnd =
            floatingDockSource.find(
                "bool DesktopApp::SyncPersistentDockHost(",
                syncDockHostsBegin);
        const std::string syncDockHostsSource =
            syncDockHostsBegin != std::string::npos &&
                    syncDockHostsEnd != std::string::npos
                ? floatingDockSource.substr(
                    syncDockHostsBegin,
                    syncDockHostsEnd - syncDockHostsBegin)
                : std::string{};
        const std::size_t graphicsReadyGuard =
            syncDockHostsSource.find(
                "if (!d2dDevice_ || !dcompDevice_)");
        const std::size_t dockHostCreated =
            syncDockHostsSource.find(
                "CreateFloatingDockWindow(host)");
        Check(!syncDockHostsSource.empty() &&
                graphicsReadyGuard != std::string::npos &&
                dockHostCreated != std::string::npos &&
                graphicsReadyGuard < dockHostCreated,
            "persistent DockHosts must not be created before the graphics devices are ready");
        Check(floatingDockSource.find(
                  "if (floatingDockVisible_ && previous != selected)") ==
                    std::string::npos &&
                popupLifecycleSource.find(
                  "collectionPopupDockHost_") !=
                    std::string::npos &&
                popupTransitionSource.find(
                  "collectionPopupDockHost_") !=
                    std::string::npos,
            "selecting another DockHost must not restack siblings, and a shared popup must retain its originating DockHost");
        Check(pointerDownSource.find(
                  "popupOwnedByPointDock") != std::string::npos &&
                pointerReleaseSource.find(
                  "collectionPopupDockHost_ ==") !=
                    std::string::npos &&
                popupTransitionSource.find(
                  "collectionPopupDockHost_ == requestedDockHost") !=
                    std::string::npos,
            "popup toggle identity must include the originating DockHost when switching between monitors");
        const std::size_t quickNavigationRetargetBegin =
            quickNavigationWindowSource.find(
                "if (quickNavigationOpen_)\n    {");
        const std::size_t quickNavigationRetargetEnd =
            quickNavigationWindowSource.find(
                "quickNavigationPostCloseAction_ = {};",
                quickNavigationRetargetBegin);
        const std::string quickNavigationRetargetSource =
            quickNavigationRetargetBegin != std::string::npos &&
                    quickNavigationRetargetEnd != std::string::npos
                ? quickNavigationWindowSource.substr(
                    quickNavigationRetargetBegin,
                    quickNavigationRetargetEnd -
                        quickNavigationRetargetBegin)
                : std::string{};
        Check(appHeaderSource.find(
                  "PersistentDockHost* quickNavigationDockHost_") !=
                    std::string::npos &&
                quickNavigationInteractionSource.find(
                  "requestedDockHost != quickNavigationDockHost_") !=
                    std::string::npos &&
                quickNavigationRetargetSource.find(
                  "quickNavigationDockHost_ = requestedDockHost;") !=
                    std::string::npos &&
                quickNavigationRetargetSource.find(
                  "PositionQuickNavigationWindow();") !=
                    std::string::npos &&
                quickNavigationRetargetSource.find(
                  "StartQuickNavigationCompositionAnimation();") !=
                    std::string::npos,
            "an open Dock-search panel must retarget its owner, anchor and monitor instead of consuming another Dock's search press");
        Check(floatingDockSource.find(
                  "void DesktopApp::ApplyPersistentDockHostAppearance()") !=
                    std::string::npos &&
                floatingDockSource.find(
                  "*ownedHost, false, true") !=
                    std::string::npos &&
                CountOccurrences(
                  settingsApplySource,
                  "ApplyPersistentDockHostAppearance();") >= 2 &&
                messageDispatchSource.find(
                  "ApplyPersistentDockHostAppearance();") !=
                    std::string::npos &&
                desktopReloadSource.find(
                  "ApplyPersistentDockHostAppearance();") !=
                    std::string::npos,
            "personalization and system theme changes must repaint every persistent DockHost and force its rounded region to refresh");
        const std::size_t finalizePopupBegin =
            popupLifecycleSource.find(
                "void DesktopApp::FinalizeCloseCollectionPopup()");
        const std::size_t finalizePopupEnd =
            popupLifecycleSource.find(
                "void DesktopApp::ClearDockFolderPopupEntries()",
                finalizePopupBegin);
        const std::string finalizePopupSource =
            finalizePopupBegin != std::string::npos &&
                    finalizePopupEnd != std::string::npos
                ? popupLifecycleSource.substr(
                    finalizePopupBegin,
                    finalizePopupEnd - finalizePopupBegin)
                : std::string{};
        Check(!finalizePopupSource.empty() &&
                finalizePopupSource.find(
                    "UpdateFloatingDockWindowBounds(") ==
                    std::string::npos &&
                finalizePopupSource.find(
                    "InvalidateFloatingDockWindow(") ==
                    std::string::npos &&
                finalizePopupSource.find(
                    "RenderFloatingDockCompositionFrame(") ==
                    std::string::npos &&
                finalizePopupSource.find(
                    "floatingDockBackdropCompositor_") ==
                    std::string::npos &&
                finalizePopupSource.find(
                    "floatingDockHwnd_") ==
                    std::string::npos,
            "finalizing a shared popup must not repaint or resize the floating Dock");
        const std::size_t updatePopupBoundsBegin =
            floatingPopupSource.find(
                "void DesktopApp::UpdateFloatingPopupWindowBounds(");
        const std::size_t updatePopupBoundsEnd =
            floatingPopupSource.find(
                "void DesktopApp::PaintFloatingPopupWindow(",
                updatePopupBoundsBegin);
        const std::string updatePopupBoundsSource =
            updatePopupBoundsBegin != std::string::npos &&
                    updatePopupBoundsEnd != std::string::npos
                ? floatingPopupSource.substr(
                    updatePopupBoundsBegin,
                    updatePopupBoundsEnd - updatePopupBoundsBegin)
                : std::string{};
        const std::size_t noPopupSurfaces =
            updatePopupBoundsSource.find(
                "if (!ShouldShowFloatingPopupWindow())");
        const std::size_t stopPopupMonitor =
            updatePopupBoundsSource.find(
                "StopFloatingPopupOutsideClickMonitor();",
                noPopupSurfaces);
        const std::size_t hidePopupPair =
            updatePopupBoundsSource.find(
                "collectionPopupBackdropCompositor_.HidePopupWindowPair(\n            floatingPopupHwnd_);",
                stopPopupMonitor);
        const std::size_t clearPopupBounds =
            updatePopupBoundsSource.find(
                "floatingPopupWindowBounds_ = {};",
                hidePopupPair);
        const std::size_t createPopupWindow =
            updatePopupBoundsSource.find(
                "if (!CreateFloatingPopupWindow())",
                clearPopupBounds);
        Check(!updatePopupBoundsSource.empty() &&
                noPopupSurfaces != std::string::npos &&
                stopPopupMonitor != std::string::npos &&
                hidePopupPair != std::string::npos &&
                clearPopupBounds != std::string::npos &&
                createPopupWindow != std::string::npos &&
                noPopupSurfaces < stopPopupMonitor &&
                stopPopupMonitor < hidePopupPair &&
                hidePopupPair < clearPopupBounds &&
                clearPopupBounds < createPopupWindow,
            "the shared popup host may hide only after its own live surface set becomes empty");
        const std::size_t nativePopupAnimationBegin =
            compositionAnimationSource.find(
                "bool DesktopApp::StartCollectionPopupCompositionAnimation()");
        const std::size_t nativePopupAnimationEnd =
            compositionAnimationSource.find(
                "bool DesktopApp::StartLuaWidgetPanelCompositionAnimation()",
                nativePopupAnimationBegin);
        const std::string nativePopupAnimationSource =
            nativePopupAnimationBegin != std::string::npos &&
                    nativePopupAnimationEnd != std::string::npos
                ? compositionAnimationSource.substr(
                    nativePopupAnimationBegin,
                    nativePopupAnimationEnd - nativePopupAnimationBegin)
                : std::string{};
        const std::size_t nativePopupCompletion =
            nativePopupAnimationSource.find(
                "uiAnimationScheduler_.ScheduleOnce(");
        const std::size_t nativePopupAdvance =
            nativePopupAnimationSource.find(
                "popupAnimation_.Advance(",
                nativePopupCompletion);
        const std::size_t nativePopupTerminalGuard =
            nativePopupAnimationSource.find(
                "if (!popupAnimation_.IsAnimating() &&",
                nativePopupAdvance);
        const std::size_t nativePopupHidden =
            nativePopupAnimationSource.find(
                "popupAnimation_.IsHidden())",
                nativePopupTerminalGuard);
        const std::size_t nativePopupBackdropApply =
            nativePopupAnimationSource.find(
                "ApplyCollectionPopupBackdropAnimationFrame();",
                nativePopupAdvance);
        const std::size_t nativePopupFinalize =
            nativePopupAnimationSource.find(
                "FinalizeCloseCollectionPopup();",
                nativePopupHidden);
        const std::size_t nativePopupHiddenReturn =
            nativePopupAnimationSource.find(
                "return;", nativePopupFinalize);
        Check(!nativePopupAnimationSource.empty() &&
                nativePopupCompletion != std::string::npos &&
                nativePopupAdvance != std::string::npos &&
                nativePopupTerminalGuard != std::string::npos &&
                nativePopupHidden != std::string::npos &&
                nativePopupFinalize != std::string::npos &&
                nativePopupHiddenReturn != std::string::npos &&
                nativePopupBackdropApply != std::string::npos &&
                nativePopupAdvance < nativePopupTerminalGuard &&
                nativePopupTerminalGuard < nativePopupHidden &&
                nativePopupHidden < nativePopupFinalize &&
                nativePopupFinalize < nativePopupHiddenReturn &&
                nativePopupHiddenReturn < nativePopupBackdropApply,
            "a native close completion must reach popup finalization before any helper-only hidden frame");
        const std::size_t refreshPopupGeometryBegin =
            popupTransitionSource.find(
                "void DesktopApp::RefreshDockFolderPopupGeometry()");
        const std::size_t commitPopupStateBegin =
            popupTransitionSource.find(
                "CommitDockFolderPopupStateToSource()",
                refreshPopupGeometryBegin);
        const std::string refreshPopupGeometrySource =
            refreshPopupGeometryBegin != std::string::npos &&
                    commitPopupStateBegin != std::string::npos
                ? popupTransitionSource.substr(
                    refreshPopupGeometryBegin,
                    commitPopupStateBegin -
                        refreshPopupGeometryBegin)
                : std::string{};
        Check(!refreshPopupGeometrySource.empty() &&
                refreshPopupGeometrySource.find(
                    "UpdateFloatingDockWindowBounds();") ==
                    std::string::npos &&
                refreshPopupGeometrySource.find(
                    "InvalidateFloatingDockWindow(true);") ==
                    std::string::npos,
            "shared popup geometry refresh must not resize or repaint the floating Dock");
        Check(popupTransitionSource.find(
                  "EnsureFloatingDockVisibleForAssociatedSurface(") !=
                    std::string::npos &&
                popupLifecycleSource.find(
                  "EnsureFloatingDockVisibleForAssociatedSurface(") !=
                    std::string::npos,
            "collection and folder popups opened from the Dock must reveal its floating host");
        Check(dockPreviewSource.find(
                  "EnsureFloatingDockVisibleForAssociatedSurface(") !=
                    std::string::npos &&
                dockPreviewSource.find(
                  "ResolveDockWindowPreviewTarget(") !=
                    std::string::npos &&
                dockPreviewSource.find(
                  "target = std::move(floatingTarget)") !=
                    std::string::npos,
            "task thumbnails must reveal the floating Dock and refresh their moved anchor");
        Check(dockPreviewSource.find(
                  "IsDockTaskbarDocumentProxyCandidate(window)") !=
                    std::string::npos &&
                dockPreviewSource.find(
                  "IsTaskbarDocumentProxyCohortEligible(") !=
                    std::string::npos &&
                dockPreviewSource.find(
                  "ShouldPreferTaskbarDocumentProxyCohort(") !=
                    std::string::npos &&
                dockPreviewSource.find(
                  "identity, true, false") !=
                    std::string::npos,
            "a unique multi-document proxy cohort must replace duplicate main-frame previews without changing application-close discovery");
        Check(dockPlatformHelpersSource.find(
                  "IsDockTaskbarDocumentProxyCandidate(HWND window)") !=
                    std::string::npos &&
                dockPlatformHelpersSource.find("swThumbnailWnd") ==
                    std::string::npos &&
                dockPlatformHelpersSource.find("PROME-TASKBAR") ==
                    std::string::npos,
            "taskbar document proxy discovery must not depend on per-application window-class allowlists");
        Check(dockPlatformHelpersSource.find(
                  "if (preferDirectResource)\n    {\n"
                  "        bitmap = GetDirectIconResourceBitmap(") !=
                    std::string::npos &&
                dockPlatformHelpersSource.find(
                  "executablePath, executableBitmapSize, requestedSize, true,") !=
                    std::string::npos,
            "classic Dock executables must bypass the Shell compatibility plate before using fallback icon sources");
        const std::size_t proxyActivationBegin =
            dockWindowTrackingSource.find(
                "if (IsDockTaskbarDocumentProxyCandidate(target))");
        const std::size_t proxyForegroundRequest =
            dockWindowTrackingSource.find(
                "SetForegroundWindow(target);",
                proxyActivationBegin);
        const std::size_t proxyActivationReturn =
            dockWindowTrackingSource.find(
                "return;", proxyForegroundRequest);
        const std::size_t ordinaryActivationRequest =
            dockWindowTrackingSource.find(
                "RequestDockWindowActivation(target, minimized);",
                proxyActivationBegin);
        Check(proxyActivationBegin != std::string::npos &&
                proxyForegroundRequest != std::string::npos &&
                proxyActivationReturn != std::string::npos &&
                ordinaryActivationRequest != std::string::npos &&
                proxyActivationBegin < proxyForegroundRequest &&
                proxyForegroundRequest < proxyActivationReturn &&
                proxyActivationReturn < ordinaryActivationRequest,
            "taskbar document proxies must activate directly without showing the hidden helper window");
        const std::size_t modernMenuBegin =
            menuIconsSource.find(
                "UINT DesktopApp::ShowModernMenu(");
        const std::size_t dockMenuOwnerWindowVisible =
            menuIconsSource.find(
                "const bool floatingDockHostWindowVisible =",
                modernMenuBegin);
        const std::size_t dockMenuOwnerPhysicalVisibility =
            menuIconsSource.find(
                "IsWindowVisible(floatingDockHwnd_)",
                dockMenuOwnerWindowVisible);
        const std::size_t dockMenuOwnerEffectiveFloating =
            menuIconsSource.find(
                "IsPersistentDockHostEffectivelyFloating(",
                dockMenuOwnerPhysicalVisibility);
        const std::size_t dockMenuOwnerResolve =
            menuIconsSource.find(
                "ResolveMenuZOrderOwner(",
                dockMenuOwnerEffectiveFloating);
        const std::size_t dockMenuOwnerVisibleArgument =
            menuIconsSource.find(
                "floatingDockHostWindowVisible,",
                dockMenuOwnerResolve);
        const std::size_t dockMenuOwnerFloatingArgument =
            menuIconsSource.find(
                "floatingDockHostEffectivelyFloating,",
                dockMenuOwnerVisibleArgument);
        const std::size_t modernMenuEnd =
            menuIconsSource.find(
                "void DesktopApp::ConfigureModernMenuEventPump(",
                modernMenuBegin);
        const std::string modernMenuSource =
            modernMenuBegin != std::string::npos &&
                    modernMenuEnd != std::string::npos
                ? menuIconsSource.substr(
                    modernMenuBegin,
                    modernMenuEnd - modernMenuBegin)
                : std::string{};
        Check(!modernMenuSource.empty() &&
                dockMenuOwnerWindowVisible != std::string::npos &&
                dockMenuOwnerPhysicalVisibility != std::string::npos &&
                dockMenuOwnerEffectiveFloating != std::string::npos &&
                dockMenuOwnerResolve != std::string::npos &&
                dockMenuOwnerVisibleArgument != std::string::npos &&
                dockMenuOwnerFloatingArgument != std::string::npos &&
                dockMenuOwnerWindowVisible <
                    dockMenuOwnerPhysicalVisibility &&
                dockMenuOwnerPhysicalVisibility <
                    dockMenuOwnerEffectiveFloating &&
                dockMenuOwnerEffectiveFloating <
                    dockMenuOwnerResolve &&
                dockMenuOwnerResolve <
                    dockMenuOwnerVisibleArgument &&
                dockMenuOwnerVisibleArgument <
                    dockMenuOwnerFloatingArgument &&
                modernMenuSource.find(
                  "floatingDockHostActive_") == std::string::npos,
            "modern menus must never use an active desktop-band or hidden DockHost as their Z-order owner; only the selected visible, effectively floating Host is eligible");
        const std::size_t dockContextHit =
            pointerContextSource.find(
                "DockContainer* dock = GetDockContainerAtPoint(pt);");
        const std::size_t rightButtonDownHandler =
            pointerContextSource.find(
                "void DesktopApp::OnRightButtonDown(");
        const std::size_t synchronousGestureCancel =
            pointerContextSource.find(
                "SuppressUntilEdgeLeave();",
                rightButtonDownHandler);
        const std::size_t rightButtonPressConsume =
            pointerContextSource.find(
                "std::exchange(rightButtonDownDockHost_, nullptr)");
        const std::size_t dockContextSourceGate =
            pointerContextSource.find(
                "ShouldDispatchDockContextMenu(",
                dockContextHit);
        const std::size_t dockContextHostMatch =
            pointerContextSource.find(
                "rightButtonPressDockHost ==",
                dockContextSourceGate);
        const std::size_t dockContextRejectedLog =
            pointerContextSource.find(
                "Floating Dock context summon ignored:",
                dockContextHostMatch);
        const std::size_t dockContextRejectedReturn =
            pointerContextSource.find(
                "return;",
                dockContextRejectedLog);
        const std::size_t dockContextBranch =
            pointerContextSource.find(
                "if (dockOwnsContextInput)",
                dockContextHostMatch);
        const std::size_t dockContextSummon =
            pointerContextSource.find(
                "EnsureFloatingDockVisibleForAssociatedSurface(",
                dockContextBranch);
        const std::size_t dockContextEnd =
            pointerContextSource.find(
                "const size_t standaloneInputWidget",
                dockContextHit);
        Check(dockContextHit != std::string::npos &&
                rightButtonDownHandler != std::string::npos &&
                synchronousGestureCancel != std::string::npos &&
                rightButtonPressConsume != std::string::npos &&
                dockContextSourceGate != std::string::npos &&
                dockContextHostMatch != std::string::npos &&
                dockContextRejectedLog != std::string::npos &&
                dockContextRejectedReturn != std::string::npos &&
                dockContextBranch != std::string::npos &&
                dockContextSummon != std::string::npos &&
                dockContextEnd != std::string::npos &&
                dockContextHit < dockContextSourceGate &&
                dockContextSourceGate < dockContextHostMatch &&
                dockContextHostMatch < dockContextRejectedLog &&
                dockContextRejectedLog < dockContextRejectedReturn &&
                dockContextRejectedReturn < dockContextBranch &&
                dockContextHostMatch < dockContextBranch &&
                dockContextBranch < dockContextSummon &&
                dockContextSummon < dockContextEnd &&
                rightButtonDownHandler < synchronousGestureCancel &&
                rightButtonPressConsume < dockContextHit &&
                pointerContextSource.find(
                  "EnsureFloatingDockVisibleForAssociatedSurface(",
                  dockContextSummon + 1) == std::string::npos,
            "only a right-button press that began on the persistent Host owning a Dock hit may summon its floating host, rejected releases must not fall through, and every press cancels an armed edge gesture synchronously");
        const std::size_t dockRightButtonDown =
            floatingDockRenderSource.find(
                "case WM_RBUTTONDOWN:");
        const std::size_t dockRightButtonPressRecord =
            floatingDockRenderSource.find(
                "OnRightButtonDown(&host);",
                dockRightButtonDown);
        const std::size_t dockRightButtonDoubleClick =
            floatingDockRenderSource.find(
                "case WM_RBUTTONDBLCLK:",
                dockRightButtonDown);
        const std::size_t dockRightButtonUp =
            floatingDockRenderSource.find(
                "case WM_RBUTTONUP:",
                dockRightButtonPressRecord);
        Check(dockRightButtonDown != std::string::npos &&
                dockRightButtonDoubleClick != std::string::npos &&
                dockRightButtonPressRecord != std::string::npos &&
                dockRightButtonUp != std::string::npos &&
                dockRightButtonDown < dockRightButtonPressRecord &&
                dockRightButtonDoubleClick <
                    dockRightButtonPressRecord &&
                dockRightButtonPressRecord < dockRightButtonUp &&
                messageDispatchSource.find(
                  "case WM_RBUTTONDOWN:\n"
                  "    case WM_RBUTTONDBLCLK:\n"
                  "        OnRightButtonDown(nullptr);") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "case WM_RBUTTONDOWN:\n"
                  "    case WM_RBUTTONDBLCLK:\n"
                  "        OnRightButtonDown(nullptr);") !=
                    std::string::npos,
            "Dock, desktop and floating-popup surfaces must record right-button press ownership for single and double clicks before release dispatch");
        Check(pointerContextSource.find(
                  "if (mouseDownHit_ == popupItem)") !=
                    std::string::npos &&
                pointerContextSource.find(
                  "popupMouseDownItem_.reset();") !=
                    std::string::npos &&
                popupLifecycleSource.find(
                  "ClearPopupMouseDownItem();") !=
                    std::string::npos &&
                pointerDownSource.find(
                  "ClearPopupMouseDownItem();") !=
                    std::string::npos &&
                pointerReleaseSource.find(
                  "ClearPopupMouseDownItem();") !=
                    std::string::npos &&
                desktopLayoutSource.find(
                  "ClearPopupMouseDownItem();") !=
                    std::string::npos,
            "popup item teardown must clear borrowed pointer state before releasing its wrapper");
        Check(sceneSource.find(
                  "collectionHostedByFloatingPopup") !=
                    std::string::npos &&
                sceneSource.find(
                  "luaPanelBelongsToCurrentSurface") !=
                    std::string::npos,
            "collection popups and Lua panels must render through the shared popup surface");
        const std::size_t dockPreviewHide =
            dockPreviewSource.find(
                "void DesktopApp::HideDockWindowPreview()");
        const std::size_t dockPreviewIdleGuard =
            dockPreviewSource.find(
                "dockWindowPreviewHover_.IsIdle()",
                dockPreviewHide);
        const std::size_t dockPreviewClearedGuard =
            dockPreviewSource.find(
                "dockWindowPreview_->IsCleared()",
                dockPreviewHide);
        const std::size_t dockPreviewTimerKill =
            dockPreviewSource.find(
                "KillTimer(hwnd_, kDockWindowPreviewHoverTimerId)",
                dockPreviewHide);
        Check(dockPreviewHide != std::string::npos &&
                dockPreviewIdleGuard != std::string::npos &&
                dockPreviewClearedGuard != std::string::npos &&
                dockPreviewTimerKill != std::string::npos &&
                dockPreviewIdleGuard < dockPreviewTimerKill &&
                dockPreviewClearedGuard < dockPreviewTimerKill,
            "repeated drag movement must skip Dock preview teardown after its presentation state is already idle");
        Check(sceneSource.find(
                  "ResolveDraggedBounds(") ==
                    std::string::npos &&
                sceneSource.find(
                  "ExcludeRect(") ==
                    std::string::npos &&
                sceneSource.find(
                  "DropPreviewBelongsToRenderSurface(") !=
                    std::string::npos,
            "desktop, Dock and popup surfaces must render guidance without another drag ghost copy");
        Check(dragPreviewSource.find(
                  "CreateTargetForHwnd") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "ResolveDraggedBounds(") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "dragPreviewRenderRevision_") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "kMaximumStackedPreviewItems") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "SetWindowPos(") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "SetWindowRgn(") ==
                    std::string::npos &&
                dragPreviewSource.find(
                  "RGN_DIFF") ==
                    std::string::npos &&
                dragPreviewSource.find(
                  "ShouldApplyPreviewWindowPlacement(") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "ResolvePreviewWindowZOrderPolicy(") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "dragPreviewWindowBounds_") !=
                    std::string::npos &&
                dragPreviewSource.find(
                  "void DesktopApp::ApplyDragPreviewLayerPolicy()") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "ApplyDragPreviewLayerPolicy();") !=
                    std::string::npos &&
                shellMenuSource.find(
                  "ApplyDragPreviewLayerPolicy();") !=
                    std::string::npos,
            "the drag ghost must use one cached compact DComp surface, cached placement, and reassert its layer above Dock popups");
        const std::size_t fallbackResolver =
            dragPreviewSource.find(
                "ResolveWindowBelowDragPreviewAt(");
        const std::size_t fallbackGeometry =
            dragPreviewSource.find(
                "GetWindowRect(candidate, &windowRect)",
                fallbackResolver);
        const std::size_t fallbackCloak =
            dragPreviewSource.find(
                "DwmGetWindowAttribute(",
                fallbackGeometry);
        const std::size_t fallbackRegionBounds =
            dragPreviewSource.find(
                "GetWindowRgnBox(candidate, &regionBounds)",
                fallbackCloak);
        const std::size_t fallbackExactRegion =
            dragPreviewSource.find(
                "CreateRectRgn(0, 0, 0, 0)",
                fallbackRegionBounds);
        Check(fallbackResolver != std::string::npos &&
                fallbackGeometry != std::string::npos &&
                fallbackCloak != std::string::npos &&
                fallbackRegionBounds != std::string::npos &&
                fallbackExactRegion != std::string::npos &&
                fallbackGeometry < fallbackCloak &&
                fallbackCloak < fallbackRegionBounds &&
                fallbackRegionBounds < fallbackExactRegion,
            "preview fallback must reject off-point windows before DWM queries and allocate exact regions only after a bounding-box hit");
        Check(dragLifecycleSource.find(
                  "SyncDragPreviewWindow();") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                  "PresentationRevision()") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                  "PresentOleDragInteractionFrame()") !=
                    std::string::npos &&
                oleDropRoutingSource.find(
                  "ResolveWindowBelowDragPreviewAt(screenPoint)") !=
                    std::string::npos &&
                oleDropRoutingSource.find(
                  "belongsTo(dragPreviewHwnd_)") !=
                    std::string::npos,
            "pointer and OLE presentation must separate ghost movement from changed drop feedback");
        const std::size_t resetDockDwell =
            dragLifecycleSource.find(
                "void DesktopApp::ResetDockHandoffDwell()");
        const std::size_t dockDwellIdleGuard =
            dragLifecycleSource.find(
                "IsDockHandoffDwellIdle(",
                resetDockDwell);
        const std::size_t dockDwellTimerKill =
            dragLifecycleSource.find(
                "KillTimer(hwnd_, kDockHandoffDwellTimerId)",
                resetDockDwell);
        Check(resetDockDwell != std::string::npos &&
                dockDwellIdleGuard != std::string::npos &&
                dockDwellTimerKill != std::string::npos &&
                dockDwellIdleGuard < dockDwellTimerKill,
            "idle Dock handoff cleanup must return before touching the window timer");
        Check(CountOccurrences(
                  popupDwellInteractionSource,
                  "SetTimer(") == 2 &&
                CountOccurrences(
                  popupDwellInteractionSource,
                  "KillTimer(") == 2 &&
                popupDwellInteractionSource.find(
                  "EnsureCollectionPopupDwellTimerArmed()") !=
                    std::string::npos &&
                popupDwellInteractionSource.find(
                  "CancelCollectionPopupDwell()") !=
                    std::string::npos &&
                popupDwellInteractionSource.find(
                  "EnsureCollectionGroupTabDwellTimerArmed()") !=
                    std::string::npos &&
                popupDwellInteractionSource.find(
                  "CancelCollectionGroupTabDwell()") !=
                    std::string::npos,
            "collection dwell timers must be armed and canceled only through their centralized state helpers");
        const std::size_t tryOpenPopupDwell =
            popupDwellInteractionSource.find(
                "bool DesktopApp::TryOpenDwellCollectionPopup(");
        const std::size_t hiddenPopupCandidate =
            popupDwellInteractionSource.find(
                "if (desktopIconsHidden_ &&",
                tryOpenPopupDwell);
        const std::size_t hiddenPopupCancel =
            popupDwellInteractionSource.find(
                "CancelCollectionPopupDwell();",
                hiddenPopupCandidate);
        const std::size_t popupReadiness =
            popupDwellInteractionSource.find(
                "popupDwellController_.IsReady(",
                hiddenPopupCandidate);
        Check(tryOpenPopupDwell != std::string::npos &&
                hiddenPopupCandidate != std::string::npos &&
                hiddenPopupCancel != std::string::npos &&
                popupReadiness != std::string::npos &&
                hiddenPopupCancel < popupReadiness,
            "an ineligible hidden popup candidate must cancel its repeating timer instead of polling forever");
        const std::size_t popupTimerDispatch =
            timerDispatchSource.find(
                "timerId == kCollectionPopupDwellTimerId");
        const std::size_t popupTimerStaleGuard =
            timerDispatchSource.find(
                "if (!collectionPopupDwellTimerArmed_)",
                popupTimerDispatch);
        const std::size_t popupTimerOpen =
            timerDispatchSource.find(
                "TryOpenDwellCollectionPopup(",
                popupTimerDispatch);
        const std::size_t popupTimerRefresh =
            timerDispatchSource.find(
                "RefreshDwellDragTarget(lastMousePoint_);",
                popupTimerOpen);
        const std::size_t groupTimerDispatch =
            timerDispatchSource.find(
                "timerId == kCollectionGroupTabDwellTimerId");
        const std::size_t groupTimerStaleGuard =
            timerDispatchSource.find(
                "if (!collectionGroupTabDwellTimerArmed_)",
                groupTimerDispatch);
        const std::size_t groupTimerActivate =
            timerDispatchSource.find(
                "TryActivateCollectionGroupTab(",
                groupTimerDispatch);
        Check(popupTimerDispatch != std::string::npos &&
                popupTimerStaleGuard != std::string::npos &&
                popupTimerOpen != std::string::npos &&
                popupTimerRefresh != std::string::npos &&
                popupTimerStaleGuard < popupTimerOpen &&
                popupTimerOpen < popupTimerRefresh &&
                popupTimerRefresh < groupTimerDispatch &&
                groupTimerDispatch != std::string::npos &&
                groupTimerStaleGuard != std::string::npos &&
                groupTimerActivate != std::string::npos &&
                groupTimerStaleGuard < groupTimerActivate,
            "queued dwell timer messages must stop after cancellation and refresh the opened popup through the active drag transport");
        Check(dragLifecycleSource.find(
                  "CancelCollectionPopupDwell();") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                  "CancelCollectionGroupTabDwell();") !=
                    std::string::npos &&
                popupTransitionSource.find(
                  "CancelCollectionPopupDwell();") !=
                    std::string::npos,
            "drag-session teardown and popup opening must close both dwell timer state loops");
        const std::size_t dragPageNavigation =
            dragTargetUpdateSource.find(
                "bool DesktopApp::UpdateDragPageNavigation(");
        const std::size_t dragPageNextHit =
            dragTargetUpdateSource.find(
                "HitTestPointerTarget(",
                dragPageNavigation);
        const std::size_t dragPageMaximum =
            dragTargetUpdateSource.find(
                "MaxPageOffset()",
                dragPageNavigation);
        const std::size_t dragPageScan =
            dragTargetUpdateSource.find(
                "NextNonEmptyOffset(pageOffset_, navSide)",
                dragPageNavigation);
        const std::size_t dragPageRetryThrottle =
            dragTargetUpdateSource.find(
                "navAutoFlipTick_ = now;",
                dragPageScan);
        const std::size_t dragPageNoTarget =
            dragTargetUpdateSource.find(
                "if (newOffset == pageOffset_)",
                dragPageScan);
        Check(dragPageNavigation != std::string::npos &&
                dragPageNextHit != std::string::npos &&
                dragPageMaximum != std::string::npos &&
                dragPageNextHit < dragPageMaximum,
            "drag page navigation must hit-test its hot edges before scanning page content");
        Check(dragPageScan != std::string::npos &&
                dragPageRetryThrottle != std::string::npos &&
                dragPageNoTarget != std::string::npos &&
                dragPageRetryThrottle < dragPageNoTarget,
            "an empty drag page-navigation scan must be throttled before the next pointer sample");
        const std::size_t pageHasContent =
            pageGridSource.find(
                "bool DesktopApp::PageHasContent(");
        const std::size_t widgetPageMatch =
            pageGridSource.find(
                "w.gridCell.pageId == pageId",
                pageHasContent);
        const std::size_t groupedWidgetCheck =
            pageGridSource.find(
                "IsGroupedWidget(w)",
                pageHasContent);
        Check(pageHasContent != std::string::npos &&
                widgetPageMatch != std::string::npos &&
                groupedWidgetCheck != std::string::npos &&
                widgetPageMatch < groupedWidgetCheck,
            "page content checks must reject widgets on other pages before resolving group membership");
        const std::size_t widgetPageNavigation =
            pointerMoveSource.find(
                "void DesktopApp::UpdateWidgetDragPageNavigation(");
        const std::size_t widgetPageNavigationEnd =
            pointerMoveSource.find(
                "void DesktopApp::OnMouseMoveAt(",
                widgetPageNavigation);
        const std::string widgetPageNavigationSource =
            widgetPageNavigation != std::string::npos &&
                    widgetPageNavigationEnd != std::string::npos
                ? pointerMoveSource.substr(
                    widgetPageNavigation,
                    widgetPageNavigationEnd -
                        widgetPageNavigation)
                : std::string{};
        const std::size_t widgetPageHit =
            widgetPageNavigationSource.find(
                "HitTestPointerTarget(");
        const std::size_t widgetPageMaximum =
            widgetPageNavigationSource.find(
                "MaxPageOffset()");
        const std::size_t repeatedWidgetPageMaximum =
            widgetPageNavigationSource.find(
                "MaxPageOffset()",
                widgetPageMaximum == std::string::npos
                    ? 0 : widgetPageMaximum + 1);
        const std::size_t emptyWidgetPageGuard =
            widgetPageNavigationSource.find(
                "if (maximumOffset <= 0 ||",
                widgetPageMaximum);
        const std::size_t clearWidgetPageSide =
            widgetPageNavigationSource.find(
                "navSide = 0;",
                emptyWidgetPageGuard);
        Check(!widgetPageNavigationSource.empty() &&
                widgetPageHit != std::string::npos &&
                widgetPageMaximum != std::string::npos &&
                widgetPageHit < widgetPageMaximum &&
                repeatedWidgetPageMaximum == std::string::npos &&
                emptyWidgetPageGuard != std::string::npos &&
                clearWidgetPageSide != std::string::npos,
            "widget drag page navigation must scan page content only once and only after a hot-edge hit");
        Check(pointerMoveSource.find(
                  "*dragPreviewSynced = true;") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                  "ShouldSyncPreviewBeforePresentation(") !=
                    std::string::npos &&
                messageDispatchSource.find(
                  "PresentPointerInteractionFrame(dragPreviewSynced);") !=
                    std::string::npos &&
                floatingDockRenderSource.find(
                  "&dragPreviewSynced") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "&dragPreviewSynced") !=
                    std::string::npos,
            "each WM_MOUSEMOVE surface must reuse the preview sync already performed for that input sample");
        const std::size_t drawPageHotEdges =
            navigationRenderSource.find(
                "void DesktopApp::DrawPageNavHotEdgeHint(");
        const std::size_t getPageHotEdges =
            navigationRenderSource.find(
                "void DesktopApp::GetNavHotEdgeRects(");
        const std::size_t getPageHotEdgesEnd =
            navigationRenderSource.find(
                "RECT DesktopApp::GetPageNavHotEdgeHintBounds(",
                getPageHotEdges);
        const std::string getPageHotEdgesSource =
            getPageHotEdges != std::string::npos &&
                    getPageHotEdgesEnd != std::string::npos
                ? navigationRenderSource.substr(
                    getPageHotEdges,
                    getPageHotEdgesEnd - getPageHotEdges)
                : std::string{};
        Check(!getPageHotEdgesSource.empty() &&
                getPageHotEdgesSource.find(
                    "targetPage->bounds") != std::string::npos &&
                getPageHotEdgesSource.find(
                    "targetPage->workArea") == std::string::npos,
            "page navigation hot edges must extend through the Dock to the full monitor bounds");
        const std::size_t resolveDragRails =
            navigationRenderSource.find(
                "ResolveHotEdgeRailVisibility(",
                drawPageHotEdges);
        const std::size_t immediateDragHint =
            navigationRenderSource.find(
                "(!dragging && !navHotEdgeHintVisible_)",
                resolveDragRails);
        const std::size_t dragDwellText =
            navigationRenderSource.find(
                "app.navigation.edge_drag_dwell",
                immediateDragHint);
        Check(drawPageHotEdges != std::string::npos &&
                resolveDragRails != std::string::npos &&
                immediateDragHint != std::string::npos &&
                dragDwellText != std::string::npos,
            "dragging must show available rails before edge entry and reveal the dwell-to-page hint immediately on entry");
        const std::size_t movingDragHint =
            dragLifecycleSource.find(
                "NeedsDragHintPresent(");
        const std::size_t clearOldDragHint =
            dragLifecycleSource.find(
                "addDirty(presentedDragNavHintBounds_);",
                movingDragHint);
        const std::size_t drawNewDragHint =
            dragLifecycleSource.find(
                "addDirty(pageNavDragHintBounds);",
                clearOldDragHint);
        const std::size_t presentMovingDragHint =
            dragLifecycleSource.find(
                "PresentDesktopForegroundComposition(dirty);",
                drawNewDragHint);
        Check(movingDragHint != std::string::npos &&
                clearOldDragHint != std::string::npos &&
                drawNewDragHint != std::string::npos &&
                presentMovingDragHint != std::string::npos,
            "moving a drag dwell hint must redraw both its old and new bounds to prevent trails");
        const std::size_t desktopLeaveBegin =
            messageDispatchSource.find(
                "case WM_MOUSELEAVE:");
        const std::size_t desktopLeaveEnd =
            messageDispatchSource.find(
                "case WM_LBUTTONUP:",
                desktopLeaveBegin);
        const std::string desktopLeaveHandler =
            desktopLeaveBegin == std::string::npos ||
                desktopLeaveEnd == std::string::npos
            ? std::string{}
            : messageDispatchSource.substr(
                desktopLeaveBegin,
                desktopLeaveEnd - desktopLeaveBegin);
        const std::size_t floatingLeaveBegin =
            floatingDockRenderSource.find(
                "case WM_MOUSELEAVE:");
        const std::size_t floatingLeaveEnd =
            floatingDockRenderSource.find(
                "case WM_LBUTTONDOWN:",
                floatingLeaveBegin);
        const std::string floatingLeaveHandler =
            floatingLeaveBegin == std::string::npos ||
                floatingLeaveEnd == std::string::npos
            ? std::string{}
            : floatingDockRenderSource.substr(
                floatingLeaveBegin,
                floatingLeaveEnd - floatingLeaveBegin);
        Check(!desktopLeaveHandler.empty() &&
                desktopLeaveHandler.find(
                    "tracking.dwFlags = TME_LEAVE;") ==
                    std::string::npos &&
                desktopLeaveHandler.find(
                    "OnMouseMoveAt(") ==
                    std::string::npos &&
                desktopLeaveHandler.find(
                    "PresentPointerInteractionFrame(") ==
                    std::string::npos &&
                desktopLeaveHandler.find(
                    "ShouldPresentRetainedMouseLeave(") !=
                    std::string::npos &&
                desktopLeaveHandler.find(
                    "PresentPassiveHoverVisualChange();") !=
                    std::string::npos,
            "a retained desktop-surface leave must converge once without rearming or replaying input");
        Check(!floatingLeaveHandler.empty() &&
                floatingLeaveHandler.find(
                    "WindowFromPoint(cursorScreen) == hwnd") !=
                    std::string::npos &&
                floatingLeaveHandler.find(
                    "tracking.dwFlags = TME_LEAVE;") !=
                    std::string::npos &&
                floatingLeaveHandler.find(
                    "tracking.hwndTrack = hwnd;") !=
                    std::string::npos &&
                floatingLeaveHandler.find(
                    "OnMouseMoveAt(") ==
                    std::string::npos &&
                floatingLeaveHandler.find(
                    "PresentPointerInteractionFrame(") ==
                    std::string::npos &&
                floatingLeaveHandler.find(
                    "ShouldPresentRetainedMouseLeave(") ==
                    std::string::npos &&
                floatingLeaveHandler.find(
                    "UpdateFloatingDockWindowBounds(") ==
                    std::string::npos &&
                floatingLeaveHandler.find(
                    "InvalidateFloatingDockWindow(") ==
                    std::string::npos,
            "a retained floating-Dock leave must rearm only after an exact HWND hit and have no state, region, or presentation side effects");
        const std::size_t floatingHitTestBegin =
            floatingDockRenderSource.find(
                "case WM_NCHITTEST:");
        const std::size_t floatingHitTestEnd =
            floatingDockRenderSource.find(
                "case WM_ERASEBKGND:",
                floatingHitTestBegin);
        const std::string floatingHitTestHandler =
            floatingHitTestBegin == std::string::npos ||
                floatingHitTestEnd == std::string::npos
            ? std::string{}
            : floatingDockRenderSource.substr(
                floatingHitTestBegin,
                floatingHitTestEnd - floatingHitTestBegin);
        Check(!floatingHitTestHandler.empty() &&
                floatingHitTestHandler.find(
                    "IsTooltipOnlyPoint(") !=
                    std::string::npos &&
                floatingHitTestHandler.find(
                    "return HTTRANSPARENT;") !=
                    std::string::npos,
            "the floating Dock title must remain visual-only and pass mouse input to the paired desktop surface");
        const std::size_t onMouseLeaveBegin =
            pointerReleaseSource.find(
                "void DesktopApp::OnMouseLeave()");
        const std::size_t onMouseLeaveEnd =
            pointerReleaseSource.find(
                "void DesktopApp::ReconcileDesktopHoverState(",
                onMouseLeaveBegin);
        const std::string onMouseLeaveHandler =
            onMouseLeaveBegin == std::string::npos ||
                onMouseLeaveEnd == std::string::npos
            ? std::string{}
            : pointerReleaseSource.substr(
                onMouseLeaveBegin,
                onMouseLeaveEnd - onMouseLeaveBegin);
        const std::size_t clearPointer =
            onMouseLeaveHandler.find(
                "lastMousePoint_ = { LONG_MIN, LONG_MIN };");
        const std::size_t holdNativeMenuHover =
            onMouseLeaveHandler.find(
                "ShouldHoldHoverDuringNativeShellPopup(");
        const std::size_t shrinkFloatingRegion =
            onMouseLeaveHandler.find(
                "UpdateFloatingDockWindowBounds(false);");
        const std::size_t presentClearedHover =
            onMouseLeaveHandler.find(
                "PresentPassiveHoverVisualChange();");
        Check(!onMouseLeaveHandler.empty() &&
                holdNativeMenuHover != std::string::npos &&
                clearPointer != std::string::npos &&
                shrinkFloatingRegion != std::string::npos &&
                presentClearedHover != std::string::npos &&
                holdNativeMenuHover < clearPointer &&
                clearPointer < shrinkFloatingRegion &&
                shrinkFloatingRegion < presentClearedHover,
            "native menu capture must hold the paired hover frame before ordinary leave clears and presents it");
        Check(oleDropRoutingSource.find(
                  "bool DesktopApp::IsBaseDesktopHoverSurfaceWindow(") !=
                    std::string::npos &&
                oleDropRoutingSource.find(
                  "bool DesktopApp::TryGetBaseDesktopHoverPointFromCursor(") !=
                    std::string::npos &&
                pointerReleaseSource.find(
                  "ShouldRefreshActiveHoverFromSurfaceSample(") !=
                    std::string::npos &&
                pointerReleaseSource.find(
                  "GetCapture() == nullptr &&") !=
                    std::string::npos &&
                pointerReleaseSource.find(
                  "TryGetBaseDesktopHoverPointFromCursor(") !=
                    std::string::npos &&
                pointerReleaseSource.find(
                  "if (refreshActiveHover)\n                UpdateFloatingDockWindowBounds(false);") !=
                    std::string::npos,
            "periodic hover recovery must refresh active coordinates and the floating title region only from uncaptured base desktop surfaces");
        Check(dockContainerSource.find(
                  "bool DockContainer::IsMagnificationSuppressed() const") !=
                    std::string::npos &&
                dockContainerSource.find(
                  "ShouldSuppressMagnification(") !=
                    std::string::npos &&
                dockContainerSource.find(
                  "DesktopApp::WidgetAction::Move") !=
                    std::string::npos &&
                dockContainerSource.find(
                  "DesktopApp::WidgetAction::Resize") !=
                    std::string::npos &&
                CountOccurrences(
                  dockContainerSource,
                  "IsMagnificationSuppressed()") >= 9,
            "Dock geometry, hit testing, and titles must share drag and widget-gesture magnification suppression");
        Check(pointerMoveSource.find(
                  "ShowDragHintWindow(current, hint);\n        InvalidateRect(hwnd_, nullptr, FALSE);") ==
                    std::string::npos,
            "ordinary drag movement must not invalidate the full desktop after every pointer pixel");
        const std::size_t marqueeMoveBegin =
            pointerMoveSource.find(
                "if (mouseDown_ && !mouseDownHit_");
        const std::size_t marqueeMoveEnd =
            pointerMoveSource.find(
                "    {\n        int oldHover = navHoverSide_;",
                marqueeMoveBegin);
        const std::string marqueeMoveHotPath =
            marqueeMoveBegin != std::string::npos &&
                    marqueeMoveEnd != std::string::npos
                ? pointerMoveSource.substr(
                    marqueeMoveBegin,
                    marqueeMoveEnd - marqueeMoveBegin)
                : std::string{};
        Check(!marqueeMoveHotPath.empty() &&
                marqueeMoveHotPath.find(
                    "const bool startingMarquee = !marqueeActive_;") !=
                    std::string::npos &&
                marqueeMoveHotPath.find(
                    "QueueDesktopWidgetComposition(") !=
                    std::string::npos &&
                marqueeMoveHotPath.find(
                    "if (startingMarquee)\n                InvalidateRect(") !=
                    std::string::npos &&
                CountOccurrences(
                    marqueeMoveHotPath,
                    "InvalidateRect(hwnd_, nullptr, FALSE);") == 1 &&
                dragLifecycleSource.find(
                    "marqueeInteractionPresented =\n            PresentDesktopForegroundComposition(client);") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                    "!marqueeInteractionPresented &&") !=
                    std::string::npos &&
                messageDispatchSource.find(
                    "const bool marqueePointerActive =") !=
                    std::string::npos &&
                messageDispatchSource.find(
                    "const bool sampleLivePointer =") !=
                    std::string::npos,
            "marquee moves must use live pointer input and redraw only the foreground plus their target widget after the first frame");
        const std::size_t widgetTransitionPaint =
            pointerMoveSource.find(
                "PresentDesktopPointerUpdate();",
                widgetGestureThreshold);
        const std::size_t widgetResizePreview =
            pointerMoveSource.find(
                "// Widget resize preview",
                widgetTransitionPaint);
        const std::size_t widgetMovePreview =
            pointerMoveSource.find(
                "// Widget drag preview",
                widgetResizePreview);
        const std::string widgetResizeHotPath =
            widgetResizePreview != std::string::npos &&
                    widgetMovePreview != std::string::npos
                ? pointerMoveSource.substr(
                    widgetResizePreview,
                    widgetMovePreview - widgetResizePreview)
                : std::string{};
        const std::size_t widgetForegroundPresent =
            dragLifecycleSource.find(
                "PresentDesktopForegroundComposition(client);");
        const std::size_t widgetFeedbackGate =
            dragLifecycleSource.rfind(
                "if (widgetDragFeedbackChanged)",
                widgetForegroundPresent);
        const std::size_t widgetRootPresentFallback =
            dragLifecycleSource.find(
                "if (immediateDesktopPresent &&",
                widgetForegroundPresent);
        Check(widgetTransitionPaint != std::string::npos &&
                widgetResizePreview != std::string::npos &&
                widgetTransitionPaint < widgetResizePreview &&
                !widgetResizeHotPath.empty() &&
                widgetResizeHotPath.find(
                    "InvalidateRect(hwnd_, nullptr") ==
                    std::string::npos &&
                widgetFeedbackGate != std::string::npos &&
                widgetForegroundPresent != std::string::npos &&
                widgetRootPresentFallback != std::string::npos &&
                widgetFeedbackGate < widgetForegroundPresent &&
                widgetForegroundPresent < widgetRootPresentFallback &&
                dragLifecycleSource.find(
                    "immediateFloatingDockPresent ||") !=
                    std::string::npos,
            "widget gestures must redraw shared feedback and force Dock presentation only when their target changes");
        const std::size_t desktopGridHitTest =
            desktopGridSource.find(
                "HitRegion DesktopGrid::HitTestAtPoint(");
        const std::size_t desktopGridCellFilter =
            desktopGridSource.find(
                "item.gridCell.pageId != page->id",
                desktopGridHitTest);
        const std::size_t desktopGridKeyNormalize =
            desktopGridSource.find(
                "ToUpperInvariant(item.layoutKey)",
                desktopGridHitTest);
        const std::size_t iconHitTest =
            hitTestingSource.find(
                "DesktopIcon* DesktopApp::HitTestIcon(");
        const std::size_t iconGeometryFilter =
            hitTestingSource.find(
                "if (!PtInRect(&selRect, pt)) continue;",
                iconHitTest);
        const std::size_t iconKeyNormalize =
            hitTestingSource.find(
                "ToUpperInvariant(di->layoutKey)",
                iconHitTest);
        Check(desktopGridHitTest != std::string::npos &&
                desktopGridCellFilter != std::string::npos &&
                desktopGridKeyNormalize != std::string::npos &&
                desktopGridCellFilter < desktopGridKeyNormalize &&
                iconHitTest != std::string::npos &&
                iconGeometryFilter != std::string::npos &&
                iconKeyNormalize != std::string::npos &&
                iconGeometryFilter < iconKeyNormalize,
            "desktop hit tests must reject nonmatching geometry before allocating normalized layout keys");
        const std::size_t pointerLiveUpdate =
            pointerMoveSource.find(
                "dragSession_.UpdatePoint(current);");
        const std::size_t pointerPreviewSync =
            pointerMoveSource.find(
                "SyncDragPreviewWindow();",
                pointerLiveUpdate);
        const std::size_t pointerExternalHit =
            pointerMoveSource.find(
                "IsExternalDropWindowAt(current)",
                pointerPreviewSync);
        Check(pointerLiveUpdate != std::string::npos &&
                pointerPreviewSync != std::string::npos &&
                pointerExternalHit != std::string::npos &&
                pointerLiveUpdate < pointerPreviewSync &&
                pointerPreviewSync < pointerExternalHit,
            "the drag preview and OLE input hole must reach the live point before external-window hit testing");
        const std::size_t popupHitBegin =
            dragTargetUpdateSource.find(
                "bool DesktopApp::HitTestPopupForDrag(");
        const std::size_t popupHitEnd =
            dragTargetUpdateSource.find(
                "bool DesktopApp::UpdateDragPageNavigation(",
                popupHitBegin);
        const std::string popupHitHandler =
            popupHitBegin == std::string::npos ||
                popupHitEnd == std::string::npos
            ? std::string{}
            : dragTargetUpdateSource.substr(
                popupHitBegin,
                popupHitEnd - popupHitBegin);
        const std::size_t firstOwnedHandoff =
            popupHitHandler.find(
                "popupDragTarget_.BindHandoff(");
        const std::size_t secondOwnedHandoff =
            popupHitHandler.find(
                "popupDragTarget_.BindHandoff(",
                firstOwnedHandoff == std::string::npos
                    ? 0 : firstOwnedHandoff + 1);
        Check(!popupHitHandler.empty() &&
                firstOwnedHandoff != std::string::npos &&
                secondOwnedHandoff != std::string::npos &&
                popupHitHandler.find("GetMemberItem(") ==
                    std::string::npos,
            "popup handoff hit testing must reuse owned target wrappers instead of appending drag-source wrappers per mouse sample");
        const std::size_t clearPopupTargetBegin =
            dragLifecycleSource.find(
                "void DesktopApp::ClearPopupDragTarget()");
        const std::size_t clearPopupTargetEnd =
            dragLifecycleSource.find(
                "void DesktopApp::EndDragSession()",
                clearPopupTargetBegin);
        const std::string clearPopupTargetHandler =
            clearPopupTargetBegin == std::string::npos ||
                clearPopupTargetEnd == std::string::npos
            ? std::string{}
            : dragLifecycleSource.substr(
                clearPopupTargetBegin,
                clearPopupTargetEnd - clearPopupTargetBegin);
        const std::size_t detachPopupTarget =
            clearPopupTargetHandler.find(
                "dragSession_.UpdateTarget(");
        const std::size_t resetPopupTarget =
            clearPopupTargetHandler.find(
                "popupDragTarget_.Reset();");
        Check(!clearPopupTargetHandler.empty() &&
                clearPopupTargetHandler.find(
                    "if (popupTarget &&") !=
                    std::string::npos &&
                detachPopupTarget != std::string::npos &&
                resetPopupTarget != std::string::npos &&
                detachPopupTarget < resetPopupTarget &&
                clearPopupTargetHandler.find(
                    "dragSession_.IsActive()") ==
                    std::string::npos,
            "popup target teardown must detach active or deactivated drop context before releasing owned wrappers");
        const std::size_t openDockPopupBegin =
            popupLifecycleSource.find(
                "void DesktopApp::OpenDockFolderPopupAt(");
        const std::size_t openDockPopupClear =
            popupLifecycleSource.find(
                "ClearPopupDragTarget();",
                openDockPopupBegin);
        const std::size_t openDockPopupRewrite =
            popupLifecycleSource.find(
                "dockFolderPopupWidget_ = DesktopWidget{};",
                openDockPopupBegin);
        const std::size_t openDockPopupIconClear =
            popupLifecycleSource.find(
                "ClearDockFolderPopupEntries();",
                openDockPopupBegin);
        const std::size_t openDockPopupLoadCancel =
            popupLifecycleSource.find(
                "CancelDockFolderPopupIconLoads();",
                openDockPopupBegin);
        const std::size_t finalizePopupClear =
            popupLifecycleSource.find(
                "ClearPopupDragTarget();",
                finalizePopupBegin);
        const std::size_t finalizePopupReset =
            popupLifecycleSource.find(
                "dockFolderPopupContainer_.reset();",
                finalizePopupBegin);
        const std::size_t finalizePopupIconClear =
            popupLifecycleSource.find(
                "ClearDockFolderPopupEntries();",
                finalizePopupBegin);
        const std::size_t finalizePopupLoadCancel =
            popupLifecycleSource.find(
                "CancelDockFolderPopupIconLoads();",
                finalizePopupBegin);
        const std::size_t finalizePopupEmptyGuard =
            popupLifecycleSource.find(
                "!dockFolderPopupOpen_)",
                finalizePopupBegin);
        const std::size_t openCollectionPopupBegin =
            popupTransitionSource.find(
                "void DesktopApp::OpenCollectionPopupAt(");
        const std::size_t openCollectionPopupClear =
            popupTransitionSource.find(
                "ClearPopupDragTarget();",
                openCollectionPopupBegin);
        const std::size_t openCollectionPopupReset =
            popupTransitionSource.find(
                "dockFolderPopupContainer_.reset();",
                openCollectionPopupBegin);
        const std::size_t openCollectionPopupIconClear =
            popupTransitionSource.find(
                "ClearDockFolderPopupEntries();",
                openCollectionPopupBegin);
        const std::size_t openCollectionPopupLoadCancel =
            popupTransitionSource.find(
                "CancelDockFolderPopupIconLoads();",
                openCollectionPopupBegin);
        const std::size_t refreshFolderPopupBegin =
            popupTransitionSource.find(
                "void DesktopApp::RefreshDockFolderPopup()");
        const std::size_t refreshFolderPopupClear =
            popupTransitionSource.find(
                "ClearPopupDragTarget();",
                refreshFolderPopupBegin);
        const std::size_t refreshFolderPopupShellGuard =
            popupTransitionSource.find(
                "if (shellFileOperationInFlight_ > 0)",
                refreshFolderPopupBegin);
        const std::size_t refreshFolderPopupOpenGuard =
            popupTransitionSource.find(
                "if (!dockFolderPopupOpen_) return;",
                refreshFolderPopupBegin);
        const std::size_t refreshFolderPopupLoadCancel =
            popupTransitionSource.find(
                "CancelDockFolderPopupIconLoads();",
                refreshFolderPopupOpenGuard);
        const std::size_t refreshFolderPopupRewrite =
            popupTransitionSource.find(
                "EnumerateFolderMappingEntries(",
                refreshFolderPopupBegin);
        const std::size_t refreshFolderPopupIconClear =
            popupTransitionSource.find(
                "ClearDockFolderPopupEntries();",
                refreshFolderPopupRewrite);
        const std::size_t refreshFolderPopupContainer =
            popupTransitionSource.find(
                "dockFolderPopupContainer_ =",
                refreshFolderPopupIconClear);
        const std::size_t clearPopupEntriesBegin =
            popupLifecycleSource.find(
                "void DesktopApp::ClearDockFolderPopupEntries()");
        const std::size_t clearPopupEntriesD2D =
            popupLifecycleSource.find(
                "EraseD2DIconCacheForBitmap(entry.iconBitmap);",
                clearPopupEntriesBegin);
        const std::size_t clearPopupEntriesModel =
            popupLifecycleSource.find(
                "dockFolderPopupWidget_.folderEntries.clear();",
                clearPopupEntriesBegin);
        const std::size_t closePopupIconLifecycleBegin =
            popupLifecycleSource.find(
                "void DesktopApp::CloseCollectionPopup(");
        const std::size_t closePopupClosingGuard =
            popupLifecycleSource.find(
                "if (popupAnimation_.IsClosing())",
                closePopupIconLifecycleBegin);
        const std::size_t closePopupLoadCancel =
            popupLifecycleSource.find(
                "CancelDockFolderPopupIconLoads();",
                closePopupClosingGuard);
        const std::size_t closePopupAnimation =
            popupLifecycleSource.find(
                "SystemAnimationsEnabled()",
                closePopupLoadCancel);
        Check(openDockPopupBegin != std::string::npos &&
                openDockPopupClear != std::string::npos &&
                openDockPopupRewrite != std::string::npos &&
                openDockPopupIconClear != std::string::npos &&
                openDockPopupLoadCancel != std::string::npos &&
                openDockPopupClear < openDockPopupRewrite &&
                openDockPopupLoadCancel < openDockPopupIconClear &&
                openDockPopupIconClear < openDockPopupRewrite &&
                finalizePopupBegin != std::string::npos &&
                finalizePopupClear != std::string::npos &&
                finalizePopupReset != std::string::npos &&
                finalizePopupIconClear != std::string::npos &&
                finalizePopupLoadCancel != std::string::npos &&
                finalizePopupEmptyGuard != std::string::npos &&
                finalizePopupEmptyGuard < finalizePopupLoadCancel &&
                finalizePopupLoadCancel < finalizePopupClear &&
                finalizePopupClear < finalizePopupReset &&
                finalizePopupReset < finalizePopupIconClear &&
                openCollectionPopupBegin != std::string::npos &&
                openCollectionPopupClear != std::string::npos &&
                openCollectionPopupReset != std::string::npos &&
                openCollectionPopupIconClear != std::string::npos &&
                openCollectionPopupLoadCancel != std::string::npos &&
                openCollectionPopupLoadCancel <
                    openCollectionPopupClear &&
                openCollectionPopupClear <
                    openCollectionPopupReset &&
                openCollectionPopupReset <
                    openCollectionPopupIconClear &&
                refreshFolderPopupBegin != std::string::npos &&
                refreshFolderPopupClear != std::string::npos &&
                refreshFolderPopupRewrite != std::string::npos &&
                refreshFolderPopupShellGuard != std::string::npos &&
                refreshFolderPopupOpenGuard != std::string::npos &&
                refreshFolderPopupLoadCancel != std::string::npos &&
                refreshFolderPopupIconClear != std::string::npos &&
                refreshFolderPopupContainer != std::string::npos &&
                refreshFolderPopupShellGuard <
                    refreshFolderPopupOpenGuard &&
                refreshFolderPopupOpenGuard <
                    refreshFolderPopupLoadCancel &&
                refreshFolderPopupLoadCancel <
                    refreshFolderPopupClear &&
                refreshFolderPopupClear <
                    refreshFolderPopupRewrite &&
                refreshFolderPopupRewrite <
                    refreshFolderPopupIconClear &&
                refreshFolderPopupIconClear <
                    refreshFolderPopupContainer &&
                clearPopupEntriesBegin != std::string::npos &&
                clearPopupEntriesD2D != std::string::npos &&
                clearPopupEntriesModel != std::string::npos &&
                clearPopupEntriesD2D < clearPopupEntriesModel &&
                closePopupIconLifecycleBegin != std::string::npos &&
                closePopupClosingGuard != std::string::npos &&
                closePopupLoadCancel != std::string::npos &&
                closePopupAnimation != std::string::npos &&
                closePopupClosingGuard < closePopupLoadCancel &&
                closePopupLoadCancel < closePopupAnimation,
            "popup replacement must cancel stale icon work, detach drag targets, and erase D2D keys before destroying entry bitmaps");
        const std::size_t cancelPopupLoadsBegin =
            iconLoaderSource.find(
                "void DesktopApp::CancelDockFolderPopupIconLoads()");
        const std::size_t cancelPopupLoadsGeneration =
            iconLoaderSource.find(
                "popup_icon_load_rules::NextGeneration(",
                cancelPopupLoadsBegin);
        const std::size_t cancelPopupLoadsQueue =
            iconLoaderSource.find(
                "popup_icon_load_rules::CancelQueuedTasks(",
                cancelPopupLoadsGeneration);
        const std::size_t workerPopupGenerationCopy =
            iconLoaderSource.find(
                "result->popupGeneration = task.popupGeneration;");
        const std::size_t workerPostResult =
            iconLoaderSource.find(
                "PostMessageW(hwnd_, kIconLoadedMessage",
                workerPopupGenerationCopy);
        const std::size_t enqueueIconBegin =
            desktopReloadSource.find(
                "void DesktopApp::EnqueueIconLoad(");
        const std::size_t enqueuePopupGeneration =
            desktopReloadSource.find(
                "task.popupGeneration =",
                enqueueIconBegin);
        const std::size_t enqueueGenerationKey =
            desktopReloadSource.find(
                "std::to_wstring(task.popupGeneration)",
                enqueuePopupGeneration);
        const std::size_t enqueuePendingKey =
            desktopReloadSource.find(
                "iconLoaderPendingKeys_.insert(task.requestKey)",
                enqueueGenerationKey);
        const std::size_t enqueueQueuePush =
            desktopReloadSource.find(
                "iconLoaderQueue_.push_back(std::move(task));",
                enqueuePendingKey);
        const std::size_t onIconLoadedBegin =
            desktopReloadSource.find(
                "void DesktopApp::OnIconLoaded(");
        const std::size_t onIconLoadedPendingErase =
            desktopReloadSource.find(
                "iconLoaderPendingKeys_.erase(result->requestKey);",
                onIconLoadedBegin);
        const std::size_t onIconLoadedGenerationGate =
            desktopReloadSource.find(
                "popup_icon_load_rules::ShouldRejectResult(",
                onIconLoadedPendingErase);
        const std::size_t onIconLoadedModelScan =
            desktopReloadSource.find(
                "bool matched = false;",
                onIconLoadedGenerationGate);
        const std::size_t firstPopupGenerationField =
            appHeaderSource.find("uint64_t popupGeneration = 0;");
        const std::size_t secondPopupGenerationField =
            appHeaderSource.find(
                "uint64_t popupGeneration = 0;",
                firstPopupGenerationField + 1);
        const std::size_t popupGenerationOwner =
            appHeaderSource.find(
                "uint64_t dockFolderPopupIconGeneration_ = 1;");
        Check(cancelPopupLoadsBegin != std::string::npos &&
                cancelPopupLoadsGeneration != std::string::npos &&
                cancelPopupLoadsQueue != std::string::npos &&
                cancelPopupLoadsGeneration < cancelPopupLoadsQueue &&
                workerPopupGenerationCopy != std::string::npos &&
                workerPostResult != std::string::npos &&
                workerPopupGenerationCopy < workerPostResult &&
                enqueueIconBegin != std::string::npos &&
                enqueuePopupGeneration != std::string::npos &&
                enqueueGenerationKey != std::string::npos &&
                enqueuePendingKey != std::string::npos &&
                enqueueQueuePush != std::string::npos &&
                enqueuePopupGeneration < enqueueGenerationKey &&
                enqueueGenerationKey < enqueuePendingKey &&
                enqueuePendingKey < enqueueQueuePush &&
                onIconLoadedBegin != std::string::npos &&
                onIconLoadedPendingErase != std::string::npos &&
                onIconLoadedGenerationGate != std::string::npos &&
                onIconLoadedModelScan != std::string::npos &&
                onIconLoadedPendingErase <
                    onIconLoadedGenerationGate &&
                onIconLoadedGenerationGate <
                    onIconLoadedModelScan &&
                firstPopupGenerationField != std::string::npos &&
                secondPopupGenerationField != std::string::npos &&
                popupGenerationOwner != std::string::npos,
            "popup icon work must carry a generation through queue, worker, and stale-result rejection before model access");
        const std::size_t reloadItemsBegin =
            desktopReloadSource.find(
                "void DesktopApp::ReloadItems(");
        const std::size_t reloadItemsClear =
            desktopReloadSource.find(
                "ClearPopupDragTarget();",
                reloadItemsBegin);
        const std::size_t reloadItemsDragDeferral =
            desktopReloadSource.find(
                "ShouldDeferModelReload(",
                reloadItemsBegin);
        const std::size_t reloadItemsRetainedContext =
            desktopReloadSource.find(
                "dragSession_.HasContext()",
                reloadItemsDragDeferral);
        const std::size_t reloadItemsPending =
            desktopReloadSource.find(
                "shellReloadPending_ = true;",
                reloadItemsDragDeferral);
        const std::size_t reloadItemsRetryTimer =
            desktopReloadSource.find(
                "SetTimer(hwnd_, kShellChangeTimerId,",
                reloadItemsPending);
        const std::size_t reloadLayoutSlots =
            desktopReloadSource.find(
                "LoadLayoutSlots();",
                reloadItemsBegin);
        const std::size_t reloadFolderEntries =
            desktopReloadSource.find(
                "EnumerateFolderMappingEntries(widget);",
                reloadItemsBegin);
        const std::size_t reloadDesktopItems =
            desktopReloadSource.find(
                "LoadDesktopItems();",
                reloadItemsBegin);
        Check(reloadItemsBegin != std::string::npos &&
                reloadItemsDragDeferral != std::string::npos &&
                reloadItemsRetainedContext != std::string::npos &&
                reloadItemsPending != std::string::npos &&
                reloadItemsRetryTimer != std::string::npos &&
                reloadItemsClear != std::string::npos &&
                reloadLayoutSlots != std::string::npos &&
                reloadFolderEntries != std::string::npos &&
                reloadDesktopItems != std::string::npos &&
                reloadItemsDragDeferral < reloadItemsPending &&
                reloadItemsRetainedContext < reloadItemsPending &&
                reloadItemsPending < reloadItemsRetryTimer &&
                reloadItemsRetryTimer < reloadItemsClear &&
                reloadItemsClear < reloadLayoutSlots &&
                reloadItemsClear < reloadFolderEntries &&
                reloadItemsClear < reloadDesktopItems,
            "desktop reload must defer retained drag bindings, then detach popup handoff wrappers before replacing model storage");
        const std::size_t dockRefreshBegin =
            dockWindowTrackingSource.find(
                "void DesktopApp::RefreshDockRunningWindows(");
        const std::size_t dockRefreshDragDeferral =
            dockWindowTrackingSource.find(
                "ShouldDeferModelReload(",
                dockRefreshBegin);
        const std::size_t dockRefreshRetainedContext =
            dockWindowTrackingSource.find(
                "dragSession_.HasContext()",
                dockRefreshDragDeferral);
        const std::size_t dockRefreshModelRead =
            dockWindowTrackingSource.find(
                "PruneDockPendingCloseWindows();",
                dockRefreshBegin);
        const std::size_t dockRefreshModelReplace =
            dockWindowTrackingSource.find(
                "dockUnpinnedRunningApps_ = std::move(runningApps);",
                dockRefreshBegin);
        Check(dockRefreshBegin != std::string::npos &&
                dockRefreshDragDeferral != std::string::npos &&
                dockRefreshRetainedContext != std::string::npos &&
                dockRefreshModelRead != std::string::npos &&
                dockRefreshModelReplace != std::string::npos &&
                dockRefreshDragDeferral < dockRefreshModelRead &&
                dockRefreshRetainedContext < dockRefreshModelRead &&
                dockRefreshModelRead < dockRefreshModelReplace,
            "Dock running-item refresh must retain drag-bound item wrappers until native and OLE ownership end");
        const std::size_t shellReloadTimer =
            timerDispatchSource.find(
                "timerId == kShellChangeTimerId");
        const std::size_t shellReloadDragDeferral =
            timerDispatchSource.find(
                "ShouldDeferModelReload(",
                shellReloadTimer);
        const std::size_t shellReloadRetainedContext =
            timerDispatchSource.find(
                "dragSession_.HasContext()",
                shellReloadDragDeferral);
        const std::size_t shellReloadRetry =
            timerDispatchSource.find(
                "SetTimer(hwnd_, kShellChangeTimerId,",
                shellReloadDragDeferral);
        const std::size_t shellReloadExecute =
            timerDispatchSource.find(
                "ReloadItems(reloadLayoutFromDisk);",
                shellReloadRetry);
        Check(shellReloadTimer != std::string::npos &&
                shellReloadDragDeferral != std::string::npos &&
                shellReloadRetainedContext != std::string::npos &&
                shellReloadRetry != std::string::npos &&
                shellReloadExecute != std::string::npos &&
                shellReloadTimer < shellReloadDragDeferral &&
                shellReloadRetainedContext < shellReloadRetry &&
                shellReloadDragDeferral < shellReloadRetry &&
                shellReloadRetry < shellReloadExecute,
            "Shell debounce must keep reload pending while native or OLE drag ownership is active");
        const std::size_t selfOleEnter =
            oleDropSessionSource.find(
                "HRESULT DesktopApp::HandleOleDragEnter(");
        const std::size_t selfOleOver =
            oleDropSessionSource.find(
                "HRESULT DesktopApp::HandleOleDragOver(",
                selfOleEnter);
        const std::size_t selfOleLeave =
            oleDropSessionSource.find(
                "HRESULT DesktopApp::HandleOleDragLeave(",
                selfOleOver);
        const std::string selfOleEnterHandler =
            selfOleEnter == std::string::npos ||
                selfOleOver == std::string::npos
            ? std::string{}
            : oleDropSessionSource.substr(
                selfOleEnter, selfOleOver - selfOleEnter);
        const std::string selfOleOverHandler =
            selfOleOver == std::string::npos ||
                selfOleLeave == std::string::npos
            ? std::string{}
            : oleDropSessionSource.substr(
                selfOleOver, selfOleLeave - selfOleOver);
        const std::size_t selfOleEnterBranchEnd =
            selfOleEnterHandler.find("ExternalDragSummary externalSummary;");
        const std::string selfOleEnterBranch =
            selfOleEnterBranchEnd == std::string::npos
                ? std::string{}
                : selfOleEnterHandler.substr(0, selfOleEnterBranchEnd);
        const std::size_t selfOleOverBranchEnd =
            selfOleOverHandler.find(
                "dragDropController_.ContinueExternalDrag();");
        const std::string selfOleOverBranch =
            selfOleOverBranchEnd == std::string::npos
                ? std::string{}
                : selfOleOverHandler.substr(0, selfOleOverBranchEnd);
        Check(selfOleEnterHandler.find(
                  "MarkSelfDragReturned();") !=
                    std::string::npos &&
                selfOleEnterHandler.find(
                  "dragSession_.SetVisualVisible(false);") !=
                    std::string::npos &&
                selfOleEnterHandler.find(
                  "*effect = DROPEFFECT_NONE;") !=
                    std::string::npos &&
                selfOleEnterHandler.find(
                  "dragSession_.SetVisualVisible(true);") ==
                    std::string::npos &&
                selfOleOverHandler.find(
                  "dragSession_.SetVisualVisible(false);") !=
                    std::string::npos &&
                selfOleOverHandler.find(
                  "dragSession_.SetVisualVisible(true);") ==
                    std::string::npos &&
                selfOleEnterBranch.find(
                  "UpdateCollectionPopupDwell(client);") !=
                    std::string::npos &&
                selfOleEnterBranch.find(
                  "CancelCollectionPopupDwell();") ==
                    std::string::npos &&
                selfOleOverBranch.find(
                  "UpdateCollectionPopupDwell(client);") !=
                    std::string::npos,
            "self OLE callbacks must keep custom feedback hidden while requesting a native hand-back");

        const std::size_t selfOleDrop =
            oleDropSessionSource.find(
                "HRESULT DesktopApp::HandleOleDrop(",
                selfOleLeave);
        const std::string selfOleLeaveHandler =
            selfOleLeave == std::string::npos ||
                selfOleDrop == std::string::npos
                ? std::string{}
                : oleDropSessionSource.substr(
                    selfOleLeave,
                    selfOleDrop - selfOleLeave);
        Check(selfOleLeaveHandler.find(
                  "SelfDragNativeResumeRequested() &&") !=
                    std::string::npos &&
                selfOleLeaveHandler.find(
                  "TryGetDesktopHoverPointFromCursor(hoverPoint)") !=
                    std::string::npos &&
                selfOleLeaveHandler.find(
                  "kExternalOleDragLeaveGraceTimerId") !=
                    std::string::npos &&
                selfOleLeaveHandler.find(
                  "FinalizePendingExternalOleDragLeave();") !=
                    std::string::npos &&
                timerDispatchSource.find(
                  "timerId == kExternalOleDragLeaveGraceTimerId") !=
                    std::string::npos &&
                timerDispatchSource.find(
                  "FinalizePendingExternalOleDragLeave();") !=
                    std::string::npos &&
                lifecycleSource.find(
                  "CancelPendingExternalOleDragLeave();") !=
                    std::string::npos,
            "OLE surface handoffs must preserve dwell briefly and still finalize abandoned external drags");

        const std::size_t externalEnterBegin =
            selfOleEnterHandler.find("BeginExternalDrag(");
        const std::size_t externalEnterPopupDwell =
            selfOleEnterHandler.find(
                "UpdateCollectionPopupDwell(client);",
                externalEnterBegin);
        const std::size_t externalEnterHitTest =
            selfOleEnterHandler.find(
                "HitTestPopupForDrag(",
                externalEnterPopupDwell);
        const std::size_t externalOverBegin =
            selfOleOverHandler.find("ContinueExternalDrag();");
        const std::size_t externalOverPopupDwell =
            selfOleOverHandler.find(
                "UpdateCollectionPopupDwell(client);",
                externalOverBegin);
        const std::size_t externalOverHitTest =
            selfOleOverHandler.find(
                "HitTestPopupForDrag(",
                externalOverPopupDwell);
        Check(externalEnterBegin != std::string::npos &&
                externalEnterPopupDwell != std::string::npos &&
                externalEnterHitTest != std::string::npos &&
                externalEnterBegin < externalEnterPopupDwell &&
                externalEnterPopupDwell < externalEnterHitTest &&
                externalOverBegin != std::string::npos &&
                externalOverPopupDwell != std::string::npos &&
                externalOverHitTest != std::string::npos &&
                externalOverBegin < externalOverPopupDwell &&
                externalOverPopupDwell < externalOverHitTest,
            "external OLE enter and over must arm collection dwell before resolving popup targets");

        const std::size_t doDragDrop =
            pointerMoveSource.find(
                "DoDragDrop(dataObj.Get()");
        const std::size_t resumeOutcome =
            pointerMoveSource.find(
                "SelfDragNativeResumeRequested()",
                doDragDrop);
        const std::size_t releaseOleData =
            pointerMoveSource.find(
                "dataObj.Reset();", resumeOutcome);
        const std::size_t resetOleCursor =
            pointerMoveSource.find(
                "SetCursor(LoadCursorW(nullptr, IDC_ARROW));",
                releaseOleData);
        const std::size_t restoreCapture =
            pointerMoveSource.find(
                "SetCapture(restoreCapture);",
                resetOleCursor);
        const std::size_t restoreNativeVisual =
            pointerMoveSource.find(
                "dragSession_.SetVisualVisible(true);",
                restoreCapture);
        Check(doDragDrop != std::string::npos &&
                resumeOutcome != std::string::npos &&
                releaseOleData != std::string::npos &&
                resetOleCursor != std::string::npos &&
                restoreCapture != std::string::npos &&
                restoreNativeVisual != std::string::npos &&
                doDragDrop < resumeOutcome &&
                resumeOutcome < releaseOleData &&
                releaseOleData < resetOleCursor &&
                resetOleCursor < restoreCapture &&
                restoreCapture < restoreNativeVisual,
            "native drag feedback must resume only after OLE ownership and its effect cursor have ended");
        const std::size_t nativeResumeReturn =
            pointerMoveSource.find(
                "if (nativeDragResumed)",
                restoreNativeVisual);
        const std::size_t finishOleDataRelease =
            pointerMoveSource.find(
                "dataObj.Reset();",
                releaseOleData + 1);
        const std::size_t finishOleSelfReturn =
            pointerMoveSource.find(
                "dragDropController_.SelfDragReturned()",
                finishOleDataRelease);
        const std::size_t finishOleSurfaceCheck =
            pointerMoveSource.find(
                "TryGetNativeDragResumePointFromCursor(",
                finishOleSelfReturn);
        const std::size_t finishOleCursorReset =
            pointerMoveSource.find(
                "SetCursor(LoadCursorW(nullptr, IDC_ARROW));",
                finishOleSurfaceCheck);
        const std::size_t finishOlePostProcessing =
            pointerMoveSource.find(
                "if (hr == DRAGDROP_S_DROP",
                finishOleCursorReset);
        Check(nativeResumeReturn != std::string::npos &&
                finishOleDataRelease != std::string::npos &&
                finishOleSelfReturn != std::string::npos &&
                finishOleSurfaceCheck != std::string::npos &&
                finishOleCursorReset != std::string::npos &&
                finishOlePostProcessing != std::string::npos &&
                nativeResumeReturn < finishOleDataRelease &&
                finishOleDataRelease < finishOleSelfReturn &&
                finishOleSelfReturn < finishOleSurfaceCheck &&
                finishOleSurfaceCheck < finishOleCursorReset &&
                finishOleCursorReset < finishOlePostProcessing,
            "every terminal OLE path must release Shell drag data before post-processing and reset its cursor after self-return or on a native resume surface");
        const std::size_t cancelPressBegin =
            dragLifecycleSource.find(
                "void DesktopApp::CancelPointerPressWithoutCaptureRelease()");
        const std::size_t cancelPressEnd =
            dragLifecycleSource.find(
                "void DesktopApp::ReleaseCapturePreservingPointerState()",
                cancelPressBegin);
        const std::string cancelPressHandler =
            cancelPressBegin == std::string::npos ||
                cancelPressEnd == std::string::npos
            ? std::string{}
            : dragLifecycleSource.substr(
                cancelPressBegin,
                cancelPressEnd - cancelPressBegin);
        const std::size_t cancelDragBegin =
            dragLifecycleSource.find(
                "void DesktopApp::CancelActiveItemDrag()");
        const std::size_t cancelDragEnd =
            dragLifecycleSource.find(
                "void DesktopApp::CommitDragVisualEndBeforeShellOperation()",
                cancelDragBegin);
        const std::string cancelDragHandler =
            cancelDragBegin == std::string::npos ||
                cancelDragEnd == std::string::npos
            ? std::string{}
            : dragLifecycleSource.substr(
                cancelDragBegin,
                cancelDragEnd - cancelDragBegin);
        const std::size_t clearDockPressBegin =
            dragLifecycleSource.find(
                "void DesktopApp::ClearDockPressedState()");
        const std::size_t clearDockPressEnd =
            dragLifecycleSource.find(
                "bool DesktopApp::HasCancelablePointerPressState()",
                clearDockPressBegin);
        const std::string clearDockPressHandler =
            clearDockPressBegin == std::string::npos ||
                clearDockPressEnd == std::string::npos
            ? std::string{}
            : dragLifecycleSource.substr(
                clearDockPressBegin,
                clearDockPressEnd - clearDockPressBegin);
        const std::size_t cancelEndSession =
            cancelPressHandler.find("EndDragSession();");
        const std::size_t cancelClearPopupItem =
            cancelPressHandler.find("ClearPopupMouseDownItem();");
        const std::size_t endDragBegin =
            dragLifecycleSource.find(
                "void DesktopApp::EndDragSession()");
        const std::size_t endDragEnd =
            dragLifecycleSource.find(
                "void DesktopApp::ClearDockPressedState()",
                endDragBegin);
        const std::string endDragHandler =
            endDragBegin == std::string::npos ||
                endDragEnd == std::string::npos
            ? std::string{}
            : dragLifecycleSource.substr(
                endDragBegin, endDragEnd - endDragBegin);
        const std::size_t endDragSessionState =
            endDragHandler.find("dragSession_.End();");
        const std::size_t endDragPopupTarget =
            endDragHandler.find("ClearPopupDragTarget();");
        Check(endDragHandler.find(
                  "CancelCollectionPopupDwell();") !=
                    std::string::npos &&
                endDragHandler.find(
                  "CancelCollectionGroupTabDwell();") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "ClearPopupMouseDownItem();") !=
                    std::string::npos &&
                endDragHandler.find(
                  "ClearPopupDragTarget();") !=
                    std::string::npos &&
                endDragSessionState != std::string::npos &&
                endDragPopupTarget != std::string::npos &&
                endDragSessionState < endDragPopupTarget &&
                cancelPressHandler.find(
                  "mouseDown_ = false;") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "ClearDockPressedState();") !=
                    std::string::npos &&
                clearDockPressHandler.find(
                  "dockPressedEntry_ = static_cast<size_t>(-1);") !=
                    std::string::npos &&
                clearDockPressHandler.find(
                  "dockPressedContainer_ = nullptr;") !=
                    std::string::npos &&
                clearDockPressHandler.find(
                  "dockPressedClosedCollectionPopup_ = false;") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "marqueeActive_ = false;") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "EndDragSession();") !=
                    std::string::npos &&
                cancelEndSession < cancelClearPopupItem &&
                cancelDragHandler.find(
                  "CancelPointerPressWithoutCaptureRelease();") !=
                    std::string::npos &&
                cancelDragHandler.find(
                  "ReleaseCapture();") !=
                    std::string::npos &&
                keyboardInputSource.find(
                  "if (dragSession_.IsActive())\n        {\n            CancelActiveItemDrag();") !=
                    std::string::npos &&
                pointerMoveSource.find(
                  "ClearSelection();\n                    CancelActiveItemDrag();\n                    ReloadItems();") !=
                    std::string::npos &&
                pointerMoveSource.find(
                  "ClearDockPressedState();\n                ReleaseCapturePreservingPointerState();") !=
                    std::string::npos,
            "Escape and terminal OLE exits must clear every pressed item-drag state before a later button-up");
        Check(cancelPressHandler.find(
                  "widgetAction_ = WidgetAction::None;") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "widgetScrollbarDragging_ = false;") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "luaWidgetPanelMouseDown_ = false;") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "searchable->EndSearchPointerSelection();") !=
                    std::string::npos &&
                cancelPressHandler.find(
                  "pressedDockItem->SetSelected(false);") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                  "expectedCaptureReleaseDepth_ == 0") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                  "!dragDropController_.IsTransportActive()") !=
                    std::string::npos &&
                dragLifecycleSource.find(
                  "return window &&") !=
                    std::string::npos &&
                messageDispatchSource.find(
                  "!IsOwnedPointerCaptureWindow(") !=
                    std::string::npos &&
                messageDispatchSource.find(
                  "CanCancelPointerPressAfterCaptureLoss()") !=
                    std::string::npos &&
                floatingDockRenderSource.find(
                  "CanCancelPointerPressAfterCaptureLoss()") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "CanCancelPointerPressAfterCaptureLoss()") !=
                    std::string::npos &&
                CountOccurrences(
                  pointerMoveSource,
                  "ReleaseCapturePreservingPointerState();") >= 1,
            "unexpected capture loss must cancel the complete local press while preserving OLE-owned handoffs");
        Check(pointerDownSource.find(
                  "dockPressedClosedCollectionPopup_ =\n                        collectionPopupClosedByPointerDown;") !=
                    std::string::npos &&
                pointerReleaseSource.find(
                  "const bool pressedClosedCollectionPopup =") !=
                    std::string::npos &&
                pointerReleaseSource.find(
                  "widgetIndex, point, L\"\",\n                    pressedClosedCollectionPopup") !=
                    std::string::npos &&
                popupTransitionSource.find(
                  "popupAnimation_.IsClosing()))") !=
                    std::string::npos,
            "Dock collection release must preserve a down-phase close and queue the latest switch target");
        const std::size_t floatingDockLeftDownBegin =
            floatingDockRenderSource.find(
                "case WM_LBUTTONDOWN:");
        const std::size_t floatingDockLeftUpBegin =
            floatingDockRenderSource.find(
                "case WM_LBUTTONUP:",
                floatingDockLeftDownBegin);
        const std::string floatingDockLeftDownHandler =
            floatingDockLeftDownBegin == std::string::npos ||
                    floatingDockLeftUpBegin == std::string::npos
                ? std::string{}
                : floatingDockRenderSource.substr(
                    floatingDockLeftDownBegin,
                    floatingDockLeftUpBegin -
                        floatingDockLeftDownBegin);
        const std::size_t dismissDockContextMenu =
            floatingDockLeftDownHandler.find(
                "DismissActiveContextMenuForPopupTransition();");
        const std::size_t dispatchDockPointerDown =
            floatingDockLeftDownHandler.find(
                "OnLeftButtonDown(wp, desktopLParam());");
        Check(!floatingDockLeftDownHandler.empty() &&
                dismissDockContextMenu != std::string::npos &&
                dispatchDockPointerDown != std::string::npos &&
                dismissDockContextMenu < dispatchDockPointerDown,
            "a no-activate Dock press must dismiss the active context menu before routing the underlying click");
        const std::size_t floatingDockDoubleClickBegin =
            floatingDockRenderSource.find(
                "case WM_LBUTTONDBLCLK:");
        const std::size_t floatingDockMiddleClickBegin =
            floatingDockRenderSource.find(
                "case WM_MBUTTONDOWN:",
                floatingDockDoubleClickBegin);
        const std::string floatingDockDoubleClickHandler =
            floatingDockDoubleClickBegin == std::string::npos ||
                    floatingDockMiddleClickBegin == std::string::npos
                ? std::string{}
                : floatingDockRenderSource.substr(
                    floatingDockDoubleClickBegin,
                    floatingDockMiddleClickBegin -
                        floatingDockDoubleClickBegin);
        Check(CountOccurrences(
                  floatingDockDoubleClickHandler,
                  "handlingFloatingDockInput_") >= 2,
            "floating Dock double-click replay must retain the floating input capture context");

        const std::size_t resolverBegin =
            dragTargetUpdateSource.find(
                "void DesktopApp::ResolveCurrentDragTargetAt(");
        const std::size_t resolverEnd =
            dragTargetUpdateSource.find(
                "void DesktopApp::RefreshDragTargetAt(",
                resolverBegin);
        const std::string finalTargetResolver =
            resolverBegin == std::string::npos ||
                resolverEnd == std::string::npos
            ? std::string{}
            : dragTargetUpdateSource.substr(
                resolverBegin, resolverEnd - resolverBegin);
        Check(finalTargetResolver.find(
                  "dragSession_.UpdatePoint(clientPoint);") !=
                    std::string::npos &&
                finalTargetResolver.find(
                  "ResolveInternalTarget(") !=
                    std::string::npos &&
                finalTargetResolver.find(
                  "ResolveExternalTarget(") !=
                    std::string::npos &&
                finalTargetResolver.find(
                  "dragSession_.UpdateTarget(") !=
                    std::string::npos,
            "final-point resolution must update both point and target for native and external transports");
        Check(finalTargetResolver.find("UpdateDragPageNavigation(") ==
                    std::string::npos &&
                finalTargetResolver.find("Dwell") ==
                    std::string::npos &&
                finalTargetResolver.find("ShowDragHintWindow") ==
                    std::string::npos &&
                finalTargetResolver.find("Present") ==
                    std::string::npos &&
                finalTargetResolver.find("DoDragDrop") ==
                    std::string::npos,
            "final-point resolution must not trigger navigation, dwell, hint, presentation, or a new OLE loop");

        const std::size_t releaseBegin =
            pointerReleaseSource.find(
                "void DesktopApp::OnLeftButtonUpAt(");
        const std::size_t releaseEnd =
            pointerReleaseSource.find("\n}", releaseBegin);
        const std::string releaseHandler =
            releaseBegin == std::string::npos ||
                releaseEnd == std::string::npos
            ? std::string{}
            : pointerReleaseSource.substr(
                releaseBegin, releaseEnd - releaseBegin);
        const std::size_t clickRelease = releaseHandler.find(
            "HandleDockClickRelease(upPoint)");
        const std::size_t externalRelease = releaseHandler.find(
            "IsExternalDropWindowAt(upPoint)");
        const std::size_t finalReleaseResolve = releaseHandler.find(
            "ResolveCurrentDragTargetAt(upPoint);");
        const std::size_t dockRemoval = releaseHandler.find(
            "GetDockDragOutRemovalHint(upPoint)");
        const std::size_t deactivateDrop = releaseHandler.find(
            "dragSession_.DeactivateForDrop();");
        Check(clickRelease != std::string::npos &&
                externalRelease != std::string::npos &&
                finalReleaseResolve != std::string::npos &&
                dockRemoval != std::string::npos &&
                deactivateDrop != std::string::npos &&
                clickRelease < externalRelease &&
                externalRelease < finalReleaseResolve &&
                finalReleaseResolve < dockRemoval &&
                dockRemoval < deactivateDrop,
            "native release must reject an external endpoint and re-hit the final point before removal or commit");
        Check(releaseHandler.find("GET_X_LPARAM") ==
                    std::string::npos &&
                messageDispatchSource.find(
                  "OnLeftButtonUpAt(wp, pt);") !=
                    std::string::npos &&
                floatingDockRenderSource.find(
                  "wp, desktopPoint());") !=
                    std::string::npos &&
                floatingPopupSource.find(
                  "OnLeftButtonUpAt(wp, desktopPoint());") !=
                    std::string::npos,
            "button-up endpoints must preserve full POINT coordinates across every desktop surface");

        const std::size_t oleDropBegin =
            oleDropSessionSource.find(
                "HRESULT DesktopApp::HandleOleDrop(");
        const std::size_t oleDropEnd =
            oleDropSessionSource.find(
                "HRESULT DesktopApp::HandleOleQueryContinueDrag(",
                oleDropBegin);
        const std::string oleDropHandler =
            oleDropBegin == std::string::npos ||
                oleDropEnd == std::string::npos
            ? std::string{}
            : oleDropSessionSource.substr(
                oleDropBegin, oleDropEnd - oleDropBegin);
        const std::size_t oleClientPoint = oleDropHandler.find(
            "ScreenPointToClient(point)");
        const std::size_t oleFinalResolve = oleDropHandler.find(
            "ResolveCurrentDragTargetAt(clientPoint);");
        const std::size_t oleBlocked = oleDropHandler.find(
            "if (dragSession_.TargetRegion() == HitRegion::Blocked)");
        const std::size_t oleEndExternal = oleDropHandler.find(
            "dragDropController_.EndExternalDrag();");
        Check(oleClientPoint != std::string::npos &&
                oleFinalResolve != std::string::npos &&
                oleBlocked != std::string::npos &&
                oleEndExternal != std::string::npos &&
                oleClientPoint < oleFinalResolve &&
                oleFinalResolve < oleBlocked &&
                oleBlocked < oleEndExternal,
            "OLE Drop must re-hit its authoritative POINTL before blocked handling or ending transport state");
        const std::size_t outerEndSelf =
            pointerMoveSource.find(
                "dragDropController_.EndSelfDrag();",
                doDragDrop);
        Check(oleDropHandler.find(
                  "dragDropController_.MarkSelfDragReturned();") !=
                    std::string::npos &&
                oleDropHandler.find(
                  "dragDropController_.EndSelfDrag();") ==
                    std::string::npos &&
                outerEndSelf != std::string::npos &&
                doDragDrop < outerEndSelf,
            "self Drop callbacks must retain OLE transport ownership until the outer DoDragDrop call unwinds");
    }

    if (failures == 0)
        std::cout << "All Dock and window rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
