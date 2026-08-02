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
#include "dock_window_rules.h"
#include "dock_window_preview.h"
#include "dock_window_transition.h"
#include "dock_settings_rules.h"
#include "desktop_item_reference_migration.h"
#include "app/desktop_backdrop_update_rules.h"
#include "floating_dock_rules.h"
#include "display_topology_refresh.h"
#include "widget_spacing_rules.h"

#include <dwrite.h>
#include <wrl/client.h>

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

    bool showRunningApps = false;
    bool showWindowPreviews = false;
    snowdesktop::dock_settings_rules::
        NormalizeAlwaysEnabledFeatures(
            showRunningApps, showWindowPreviews);
    Check(showRunningApps,
        "the Dock running area must remain enabled after settings normalization");
    Check(showWindowPreviews,
        "Dock window previews must remain enabled after settings normalization");

    namespace widgetSpacing = snowdesktop::widget_spacing_rules;
    const float compactComponentLimit =
        widgetSpacing::MaximumComponentScaleForPage(40, 40, 2, 2, 1.0f);
    const float roomyComponentLimit =
        widgetSpacing::MaximumComponentScaleForPage(40, 40, 20, 20, 1.0f);
    Check(roomyComponentLimit > compactComponentLimit,
        "larger actual grid gaps must expand the component range");
    Check(widgetSpacing::MaximumComponentScaleForCollectionRows(
            2, 16, 1.0f) == 3.0f,
        "collection spacing must stop after row gaps are exhausted so the complete last-row title remains visible");
    Check(widgetSpacing::MaximumComponentScaleForCollectionRows(
            3, 16, 1.0f) >
            widgetSpacing::MaximumComponentScaleForCollectionRows(
                2, 16, 1.0f),
        "additional collection rows must contribute their real shrinkable gaps to the component range");
    Check(widgetSpacing::CollectionRowOffsetForComponentSpacing(
            1, 2, 16, 1.0f, 0.5f) == 4,
        "smaller component spacing must expand the large collection's vertical row gap symmetrically");
    Check(widgetSpacing::CollectionRowOffsetForComponentSpacing(
            1, 2, 16, 1.0f, 2.0f) == -8,
        "larger component spacing must consume the large collection's vertical row gap before its content cells");
    Check(widgetSpacing::ClampComponentScale(3.0f, 2.0f) == 2.0f,
        "component spacing must respect the actual geometry limit");
    Check(widgetSpacing::ClampComponentScale(
            4.0f, roomyComponentLimit) == roomyComponentLimit,
        "component spacing must accept the expanded geometry limit");
    Check(widgetSpacing::ClampComponentScale(0.5f, roomyComponentLimit) == 0.5f,
        "component spacing must retain the shared lower bound");
    Check(widgetSpacing::EffectiveComponentEdgeGap(
            14, 8, 1.0f, 1.0f) == 14,
        "component edge gap must include the current visible component inset");
    Check(widgetSpacing::EffectiveComponentEdgeGap(
            14, 8, 1.0f, 1.5f) >
            widgetSpacing::EffectiveComponentEdgeGap(
                14, 8, 1.0f, 1.0f),
        "larger component spacing must increase the visible edge gap");

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
        bool isDirectory = false;
        FILETIME lastWriteTime{};
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
        folderSort::kModified, false);
    Check(
        sortEntries[0].name == L"folder-b" &&
            sortEntries[1].name ==
                L"folder-a" &&
            sortEntries[2].name ==
                L"alpha.png" &&
            sortEntries[3].name ==
                L"zeta.txt",
        "folder popup date sort must use cached times and preserve directory grouping");
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
        "folder popup type sort must compare extensions within the file group");
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
        "the floating Dock must never use permanent topmost style");
    Check((floatingDock::kWindowExStyle & WS_EX_NOACTIVATE) != 0,
        "the floating Dock must not steal foreground activation");
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
    Check(!floatingDock::ShouldDismissForPointerDown(
            false, POINT{ 150, 930 },
            floatingDockRect, floatingPopupRect),
        "a click in the Dock must keep the floating host open");
    Check(!floatingDock::ShouldDismissForPointerDown(
            false, POINT{ 300, 600 },
            floatingDockRect, floatingPopupRect),
        "a click in the collection popup must keep the host open");
    Check(floatingDock::ShouldDismissForPointerDown(
            false, POINT{ 20, 20 },
            floatingDockRect, floatingPopupRect),
        "an external click must dismiss the floating host");
    Check(!floatingDock::ShouldDismissForPointerDown(
            true, POINT{ 20, 20 },
            floatingDockRect, floatingPopupRect),
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
    Check(floatingDock::ShouldPresentPointerFrame(
            100, 0, false) &&
            !floatingDock::ShouldPresentPointerFrame(
                104, 100, false) &&
            floatingDock::ShouldPresentPointerFrame(
                108, 100, false) &&
            floatingDock::ShouldPresentPointerFrame(
                104, 100, true) &&
            floatingDock::ShouldPresentPointerFrame(
                2, 100, false),
        "passive Dock hover frames are throttled while drag frames and clock resets present immediately");
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
    Check(rules::NeedsDockMinimizeSystemCommandFallback(false) &&
            !rules::NeedsDockMinimizeSystemCommandFallback(true),
        "a rejected asynchronous minimize must use the system-command fallback");
    Check(rules::NeedsDockCloseSystemCommandFallback(false) &&
            !rules::NeedsDockCloseSystemCommandFallback(true),
        "a rejected graceful close must use the system-command fallback");
    Check(rules::NeedsDockWindowSwitchFallback(false, true),
        "a visible background window must always switch to the foreground");
    Check(rules::NeedsDockWindowSwitchFallback(true, false),
        "a minimized elevated window must switch when its restore request is rejected");
    Check(!rules::NeedsDockWindowSwitchFallback(true, true),
        "an accepted minimized-window restore must not be issued twice");
    Check(rules::MustCloseFloatingDockBeforeWindowCommand(
            true, rules::DockClickAction::Minimize) &&
            rules::MustCloseFloatingDockBeforeWindowCommand(
                true, rules::DockClickAction::Restore) &&
            rules::MustCloseFloatingDockBeforeWindowCommand(
                true, rules::DockClickAction::Activate),
        "the top-level Dock must leave composition before any running-window command");
    Check(!rules::MustCloseFloatingDockBeforeWindowCommand(
            false, rules::DockClickAction::Minimize) &&
            !rules::MustCloseFloatingDockBeforeWindowCommand(
                true, rules::DockClickAction::Launch),
        "desktop-layer Docks and application launches must not add a capture synchronization");
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
    Check(kDockWindowSnapshotPresentOptions ==
            D2D1_PRESENT_OPTIONS_NONE,
        "snapshot frames must synchronize presentation to the display refresh");
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
                WS_EX_TRANSPARENT) != 0 &&
            (kDockWindowTransitionExStyle &
                WS_EX_NOACTIVATE) != 0,
        "the transition host must use a non-activating layered surface without a DWM shadow");
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

    if (failures == 0)
        std::cout << "All Dock and window rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
