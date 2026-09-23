#include "widgets/collection_group_rules.h"
#include "app/widget_group_transition.h"
#include "widgets/widget_pair_drop.h"
#include "desktop_hover_rules.h"
#include "drag_input_rules.h"
#include "drag_hint_rules.h"
#include "widget_scroll_rules.h"
#include "widget_visibility_rules.h"
#include "widgets/widget_chrome_rules.h"
#include "widgets/storage_title_bar_layout.h"
#include "widgets/guide_widget_rules.h"
#include "pending_drop_rules.h"
#include "list_detail_rules.h"
#include "popup_icon_load_rules.h"
#include "widget_menu_catalogue.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rules =
    snowdesktop::collection_group_rules;
namespace visibilityRules =
    snowdesktop::widget_visibility_rules;
namespace hoverRules =
    snowdesktop::desktop_hover_rules;
namespace dragInputRules =
    snowdesktop::drag_input_rules;
namespace dragHintRules =
    snowdesktop::drag_hint_rules;
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

void TestWidgetMenuMetadataReuse()
{
    namespace menu = snowdesktop::widget_menu;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        (L"SnowDesktop-menu-metadata-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"builtin");
    fs::create_directories(root / L"development");
    struct Cleanup
    {
        fs::path root;
        ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); }
    } cleanup{root};
    const auto write = [](const fs::path& directory, const char* text) {
        std::ofstream(directory / L"widget.json", std::ios::binary) << text;
    };
    write(root / L"builtin", "alpha");
    write(root / L"development", "dev");
    std::vector<snowdesktop::widget::InstalledPackage> packages(1);
    auto& package = packages.front();
    package.manifest.id = "package-a";
    package.manifest.version = "1.0.0";
    package.root = root / L"builtin";
    package.builtin = true;
    int reads = 0;
    std::string language = "en-US";
    // Substitute only the engine's manifest parser/version check. The actual
    // menu catalogue probes real files, filters states and controls all reads.
    const auto load = [&](const auto& input) {
        ++reads;
        std::ifstream file(input.root / L"widget.json", std::ios::binary);
        std::string name;
        std::getline(file, name);
        menu::Metadata result;
        result.packageId.assign(input.manifest.id.begin(), input.manifest.id.end());
        result.name.assign(name.begin(), name.end());
        if (language == "zh-CN") result.name = L"中文组件";
        result.description = L"Searchable description";
        result.publisher = L"Publisher";
        result.compatible = name != "future";
        result.valid = !name.empty();
        return result;
    };
    menu::Catalogue catalogue;
    auto entries = catalogue.Build(packages, language, load);
    Check(reads == 1 && entries.size() == 1 &&
            entries[0].displayName == L"alpha" &&
            entries[0].searchText ==
                L"alpha\npackage-a\nSearchable description\nPublisher" &&
            entries[0].source == menu::Source::Builtin,
        "menu loads name/search metadata once from the active package");
    for (int opening = 0; opening < 10; ++opening)
        entries = catalogue.Build(packages, language, load);
    Check(reads == 1 && entries.size() == 1,
        "reopening an unchanged menu must not read component manifests again");

    const auto originalTime = fs::last_write_time(package.root / L"widget.json");
    write(package.root, "bravo"); // Same size: modification time must invalidate.
    fs::last_write_time(package.root / L"widget.json",
        originalTime + std::chrono::seconds(2));
    entries = catalogue.Build(packages, language, load);
    Check(reads == 2 && entries[0].displayName == L"bravo",
        "same-size development manifest edits refresh the menu");
    const auto editedTime = fs::last_write_time(package.root / L"widget.json");
    write(package.root, "longer title");
    fs::last_write_time(package.root / L"widget.json", editedTime);
    entries = catalogue.Build(packages, language, load);
    Check(reads == 3 && entries[0].displayName == L"longer title",
        "size changes refresh metadata even with a preserved timestamp");

    language = "zh-CN";
    entries = catalogue.Build(packages, language, load);
    Check(reads == 4 && entries[0].displayName == L"中文组件",
        "language changes cannot reuse text from the previous language");
    package.root = root / L"development";
    package.builtin = false;
    package.development = true;
    language = "en-US";
    entries = catalogue.Build(packages, language, load);
    Check(reads == 5 && entries[0].displayName == L"dev" &&
            entries[0].source == menu::Source::Development,
        "source overrides replace both metadata and source classification");
    package.development = false;
    entries = catalogue.Build(packages, language, load);
    Check(reads == 5 && entries[0].source == menu::Source::Installed,
        "source classification reflects the current package state on cache hits");
    package.sha256 = "replacement-content";
    entries = catalogue.Build(packages, language, load);
    Check(reads == 6, "package replacement invalidates identical file metadata");

    package.enabled = false;
    Check(catalogue.Build(packages, language, load).empty() && reads == 6,
        "disabled packages disappear without reading their manifests");
    package.enabled = true;
    package.active = false;
    Check(catalogue.Build(packages, language, load).empty() && reads == 6,
        "shadowed packages cannot reappear through cached entries");
    package.active = true;
    entries = catalogue.Build(packages, language, load);
    Check(reads == 7 && entries.size() == 1,
        "reenabling a package reloads metadata discarded while disabled");

    write(package.root, "future");
    Check(catalogue.Build(packages, language, load).empty(),
        "cached metadata retains the host-version compatibility filter");
    fs::remove(package.root / L"widget.json");
    entries = catalogue.Build(packages, language, load);
    Check(entries.size() == 1 && entries[0].displayName == L"package-a",
        "a missing manifest does not retain stale names or compatibility state");
    write(package.root, "restored");
    entries = catalogue.Build(packages, language, load);
    Check(entries.size() == 1 && entries[0].displayName == L"restored",
        "a recovered manifest replaces a failed read immediately");
    packages.clear();
    Check(catalogue.Build(packages, language, load).empty(),
        "uninstalled packages leave no menu entries");
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
            partial->left == 0 && partial->top == 4 &&
            partial->right == 60 && partial->bottom == 28,
            "clipped hit bounds must equal the full visible intersection");
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

void TestScrollableStorageTitleBar()
{
    namespace titleBar = snowdesktop::storage_title_bar;
    DesktopWidget widget;
    widget.gridSpan.columns = 3;
    widget.gridSpan.rows = 4;
    for (const auto type : {DesktopWidgetType::Collection,
            DesktopWidgetType::FileCategories, DesktopWidgetType::FolderMapping,
            DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    {
        widget.type = type;
        widget.scrollContainerMode = true;
        Check(titleBar::UsesTop(widget, true) && !titleBar::UsesTop(widget, false),
            "the global position switch applies to every scrollable storage type");
    }
    widget.type = DesktopWidgetType::Collection;
    widget.scrollContainerMode = false;
    Check(!titleBar::UsesTop(widget, true), "large-folder collections keep their bottom bar");
    widget.scrollContainerMode = true;
    widget.gridSpan.columns = widget.gridSpan.rows = 1;
    Check(!titleBar::UsesTop(widget, true), "compact collections keep their bottom bar");
    for (const auto type : {DesktopWidgetType::LuaScript, DesktopWidgetType::Guide})
    {
        widget.type = type;
        Check(!titleBar::UsesTop(widget, true), "Lua and guide chrome is unaffected");
    }

    const RECT frame{100, 200, 400, 600};
    const auto bottom = titleBar::Resolve(frame, false, 24, 24, 22, 12, 4, 2);
    Check(bottom.body.top == 200 && bottom.body.bottom == 578 &&
            bottom.titleBar.top == 574 && bottom.titleBar.bottom == 598 &&
            bottom.resize.left == 372 && bottom.resize.right == 396 &&
            bottom.resize.top == 574 && bottom.contentBottom == 574,
        "default bottom position preserves existing content, move and resize geometry");
    // A former footer clamp at titleBar.top would erase this content viewport.
    for (const int height : {24, 34, 48})
    {
        const auto top = titleBar::Resolve(frame, true, height, 24, 22, 12, 4, 2);
        Check(top.titleBar.top == 202 &&
                top.titleBar.bottom - top.titleBar.top == height &&
                top.body.top == top.titleBar.bottom && top.body.bottom == 600 &&
                top.contentBottom == 598 && top.contentBottom > top.body.top,
            "top title uses tab height, shifts content once and releases the footer row");
        Check(EqualRect(&top.resize, &bottom.resize) &&
                top.resize.top > top.titleBar.bottom,
            "top title height never moves or enlarges the bottom-right resize target");
    }
    const auto scaled = titleBar::Resolve({200, 400, 800, 1200},
        true, 68, 48, 44, 56, 8, 4);
    Check(scaled.titleBar.bottom == 472 && scaled.body.bottom == 1200 &&
            scaled.resize.top == 1148 && scaled.resize.bottom == 1196 &&
            scaled.resize.right < 800,
        "scaled title and resize targets remain inside rounded corners");

    widget.type = DesktopWidgetType::FolderMapping;
    widget.gridSpan.columns = 3;
    widget.gridSpan.rows = 4;
    widget.bounds = frame;
    widget.scrollOffset = 137;
    Check(!titleBar::IsCollapsed(widget, true, false, false, false),
        "new and legacy widgets start expanded");
    widget.titleBarCollapsed = true;
    Check(titleBar::IsCollapsed(widget, true, false, false, false) &&
            !titleBar::IsCollapsed(widget, false, false, false, false),
        "only top title bars apply the stored collapse preference");
    Check(!titleBar::IsCollapsed(widget, true, true, false, false),
        "internal drag and retained drop context temporarily expand a collapsed target");
    Check(!titleBar::IsCollapsed(widget, true, false, true, false),
        "external file drag temporarily expands a collapsed target");
    Check(!titleBar::IsCollapsed(widget, true, false, false, true),
        "moving a widget temporarily expands collapsed targets");
    const RECT collapsed = titleBar::VisibleFrame(frame,
        titleBar::IsCollapsed(widget, true, false, false, false), 34, 2);
    const RECT expanded = titleBar::VisibleFrame(frame, false, 34, 2);
    Check(collapsed.left == 100 && collapsed.top == 200 &&
            collapsed.right == 400 && collapsed.bottom == 238 &&
            EqualRect(&expanded, &frame),
        "ending or cancelling a drag restores just the title row; expanding restores full bounds");
    Check(widget.titleBarCollapsed && widget.gridSpan.columns == 3 &&
            widget.gridSpan.rows == 4 && widget.scrollOffset == 137 &&
            EqualRect(&widget.bounds, &frame),
        "temporary expansion never changes saved preference, occupancy, size or scroll position");
    widget.type = DesktopWidgetType::Collection;
    widget.scrollContainerMode = false;
    Check(!titleBar::IsCollapsed(widget, true, false, false, false),
        "switching to large-folder mode reveals content despite a retained collapse preference");

    const RECT centered = titleBar::CenteredTitleRect({100, 202, 400, 236}, 26, 60, 3);
    Check(centered.left == 160 && centered.right == 340 &&
            centered.top == 205 && centered.bottom == 233,
        "title is centered on the complete widget while avoiding both toolbar sides");
    const RECT narrow = titleBar::CenteredTitleRect({100, 202, 150, 236}, 26, 60, 3);
    Check(narrow.left == 125 && narrow.right == 125,
        "narrow widgets hide title text instead of overlapping controls or inverting its rectangle");

    // Hover must enter the visible title first; the hidden original footprint
    // is not an activation target. Leaving, including native mouse-leave,
    // returns to the saved fold unless an interaction still owns the content.
    Check(!titleBar::ExpandOnHover(true, false, false, false, true, false),
        "hovering the hidden body must not open a collapsed widget");
    Check(titleBar::ExpandOnHover(true, false, false, true, true, false) &&
            titleBar::ExpandOnHover(true, true, false, false, true, false),
        "entering the title expands and moving into content keeps it open");
    Check(!titleBar::ExpandOnHover(true, true, false, false, false, false) &&
            titleBar::ExpandOnHover(true, true, false, false, false, true),
        "leaving folds the widget unless a menu or editing interaction retains it");
    Check(!titleBar::ExpandOnHover(true, false, true, true, true, false),
        "manual collapse suppresses automatic expansion until pointer leaves the title");
    Check(!titleBar::ExpandOnHover(false, true, false, true, true, true),
        "disabling hover expansion or top mode clears transient expansion");
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
    Check(
        chromeRules::BottomBarTitleTrailingReserve(
            0, 14, 4, 4, 20, 2) == 26 &&
        chromeRules::BottomBarTitleTrailingReserve(
            3, 14, 4, 4, 20, 2) == 76,
        "bottom-bar titles must reserve only the controls that are actually visible");
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
            false, false, false, false, false, false, false,
            false),
        "regular widget remains visible while idle");
    Check(
        !visibilityRules::ShouldRenderWidget(
            true, false, false, false, false, false, false,
            false),
        "hover-only widget remains hidden while idle");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, true, false, false, false, false, false,
            false),
        "item drag reveals hover-only widget");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, true, false, false, false,
            false),
        "widget move reveals hover-only widget");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, false, true, false, false,
            false),
        "selected hover-only widget remains visible");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, false, false, true, false,
            false),
        "hover-only widget with selected files remains visible");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, false, false, false, true,
            false),
        "retained inner rename reveals hover-only widget");
    Check(
        visibilityRules::ShouldRenderWidget(
            true, false, false, false, false, false, false,
            true),
        "pointer hover reveals hover-only widget");
    Check(
        visibilityRules::ShouldRetainForKeyboardNavigation(
            true, true, 3, 3) &&
            !visibilityRules::ShouldRetainForKeyboardNavigation(
                false, true, 3, 3) &&
            !visibilityRules::ShouldRetainForKeyboardNavigation(
                true, false, 3, 3) &&
            !visibilityRules::ShouldRetainForKeyboardNavigation(
                true, true, 3, 4),
        "only keyboard-driven inner navigation retains its owning widget");
}

void TestWidgetDesktopSurfaceVisibility()
{
    Check(
        visibilityRules::IsDesktopSurfaceVisible(
            false, false, true, true),
        "visible desktop widget remains runtime-visible independently of repaint frequency");
    Check(
        !visibilityRules::IsDesktopSurfaceVisible(
            true, false, true, true),
        "hidden desktop pauses an ordinary widget");
    Check(
        visibilityRules::IsDesktopSurfaceVisible(
            true, true, true, true),
        "keep-when-hidden preserves widget runtime visibility");
    Check(
        !visibilityRules::IsDesktopSurfaceVisible(
            false, false, false, true),
        "dock-exclusive widget has no visible desktop surface");
    Check(
        !visibilityRules::IsDesktopSurfaceVisible(
            false, false, true, false),
        "interaction-hidden widget pauses its desktop surface");
    Check(
        visibilityRules::ShouldKeepTopologyHiddenPageRuntimeActive(
            false, false, true, true),
        "a page removed by the display topology keeps its runtime active while its surface is hidden");
    Check(
        !visibilityRules::ShouldKeepTopologyHiddenPageRuntimeActive(
            true, false, true, true) &&
            !visibilityRules::ShouldKeepTopologyHiddenPageRuntimeActive(
                false, true, true, true) &&
            !visibilityRules::ShouldKeepTopologyHiddenPageRuntimeActive(
                false, false, false, true) &&
            !visibilityRules::ShouldKeepTopologyHiddenPageRuntimeActive(
                false, false, true, false),
        "desktop hiding, visible surfaces, virtual pages, and ordinary hidden surfaces do not keep runtimes active");
}

void TestWidgetPreviewSourceVisibility()
{
    Check(
        visibilityRules::ShouldHideWidgetPreviewSource(
            true, false, true),
        "moving a widget must hide independent content owned by the preview source");
    Check(
        visibilityRules::ShouldHideWidgetPreviewSource(
            false, true, true),
        "resizing a widget must hide independent content owned by the preview source");
    Check(
        !visibilityRules::ShouldHideWidgetPreviewSource(
            false, false, true) &&
            !visibilityRules::ShouldHideWidgetPreviewSource(
                true, false, false) &&
            !visibilityRules::ShouldHideWidgetPreviewSource(
                false, true, false),
        "idle widgets and independent content from other widgets must remain visible");
}

void TestDesktopHoverDeactivation()
{
    using hoverRules::ReconcileMode;
    Check(
        hoverRules::ShellPopupCloseReconcileMode() ==
            ReconcileMode::DeactivateOnly,
        "closing a Shell popup must not reactivate hover from the menu's last cursor position");
    Check(
        hoverRules::ShouldHoldHoverDuringNativeShellPopup(true) &&
            !hoverRules::ShouldHoldHoverDuringNativeShellPopup(false),
        "native Shell popup capture must hold the complete hover frame until the menu closes");
    Check(
        hoverRules::ShouldRetainHoverAcrossMouseLeave(
            true, false) &&
            hoverRules::ShouldRetainHoverAcrossMouseLeave(
                false, true) &&
            !hoverRules::ShouldRetainHoverAcrossMouseLeave(
                false, false),
        "content and paired backdrop windows must form one logical hover surface");
    Check(
        hoverRules::ShouldResamplePassiveMouseMove(
            false, false, false) &&
            !hoverRules::ShouldResamplePassiveMouseMove(
                true, false, false) &&
            !hoverRules::ShouldResamplePassiveMouseMove(
                false, true, false) &&
            !hoverRules::ShouldResamplePassiveMouseMove(
                false, false, true),
        "only passive mouse moves may replace queued message coordinates with the live cursor");
    Check(
        hoverRules::ShouldPresentRetainedMouseLeave(
            true, true) &&
            !hoverRules::ShouldPresentRetainedMouseLeave(
                true, false) &&
            !hoverRules::ShouldPresentRetainedMouseLeave(
                false, true),
        "retained leaves must present exactly one changed passive pointer sample");
    Check(
        hoverRules::ShouldReconcileFromSurfaceSample(
            false, false, false) &&
            hoverRules::ShouldReconcileFromSurfaceSample(
                false, true, true) &&
            !hoverRules::ShouldReconcileFromSurfaceSample(
                true, true, true) &&
            !hoverRules::ShouldReconcileFromSurfaceSample(
                false, true, false),
        "native Shell popup layers and disabled dialog owners must suspend sampled hover reconciliation");
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
    Check(
        hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
            true, false, true, true,
            ReconcileMode::AllowImmediateActivation, false) &&
            hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
                true, false, true, true,
                ReconcileMode::AllowActivationAfterForegroundSettle, true),
        "an active hover may follow a changed base-desktop sample after activation is allowed");
    Check(
        !hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
            false, false, true, true,
            ReconcileMode::AllowImmediateActivation, true) &&
            !hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
                true, true, true, true,
                ReconcileMode::AllowImmediateActivation, true) &&
            !hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
                true, false, false, true,
                ReconcileMode::AllowImmediateActivation, true) &&
            !hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
                true, false, true, false,
                ReconcileMode::AllowImmediateActivation, true) &&
            !hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
                true, false, true, true,
                ReconcileMode::DeactivateOnly, true) &&
            !hoverRules::ShouldRefreshActiveHoverFromSurfaceSample(
                true, false, true, true,
                ReconcileMode::AllowActivationAfterForegroundSettle, false),
        "active-hover fallback must reject bridges, cleared state, unchanged points, gestures, and unsettled samples");
}

void TestDragInputSampling()
{
    Check(
        dragInputRules::IsNativeDragActive(true, false) &&
            !dragInputRules::IsNativeDragActive(false, false) &&
            !dragInputRules::IsNativeDragActive(true, true),
        "only drag sessions outside OLE transport may use native pointer routing");
    Check(
        dragInputRules::IsLatencySensitivePointerGesture(
            true, false, false, false) &&
            dragInputRules::IsLatencySensitivePointerGesture(
                false, true, false, false) &&
            dragInputRules::IsLatencySensitivePointerGesture(
                false, false, true, true) &&
            !dragInputRules::IsLatencySensitivePointerGesture(
                false, false, true, false) &&
            !dragInputRules::IsLatencySensitivePointerGesture(
                false, false, false, true),
        "native drags, marquee selection, and valid widget move or resize targets must share latency-sensitive pointer routing");
    Check(
        !dragInputRules::ShouldDeferModelReload(false, false) &&
            dragInputRules::ShouldDeferModelReload(true, false) &&
            dragInputRules::ShouldDeferModelReload(false, true) &&
            dragInputRules::ShouldDeferModelReload(true, true),
        "desktop model reloads must wait for retained drop context and OLE transport to end");
    Check(
        dragInputRules::ShouldSampleLivePointer(true, true) &&
            !dragInputRules::ShouldSampleLivePointer(false, true) &&
            !dragInputRules::ShouldSampleLivePointer(true, false),
        "live pointer sampling must stop after the active gesture button is released");
    Check(
        dragInputRules::IsPointerGestureButtonDown(
            false, true, false) &&
            !dragInputRules::IsPointerGestureButtonDown(
                false, false, true) &&
            dragInputRules::IsPointerGestureButtonDown(
                true, false, true) &&
            !dragInputRules::IsPointerGestureButtonDown(
                true, true, false),
        "primary gestures must follow the left button while middle-button widget moves preserve their own release barrier");
    Check(
        dragInputRules::IsMarqueePointerGesture(
            true, false, true, true, true,
            true, true, true, true, true, false) &&
        dragInputRules::IsMarqueePointerGesture(
            false, true, false, false, false,
            false, false, false, false, false, true) &&
        !dragInputRules::IsMarqueePointerGesture(
            false, true, true, false, false,
            false, false, false, false, false, true) &&
        !dragInputRules::IsMarqueePointerGesture(
            false, true, false, false, true,
            false, false, false, false, false, true) &&
        !dragInputRules::IsMarqueePointerGesture(
            false, true, false, false, false,
            false, false, false, false, true, true) &&
        !dragInputRules::IsMarqueePointerGesture(
            false, true, false, false, false,
            false, false, false, false, false, false),
        "only an active marquee or an unclaimed pressed marquee target may use latency-sensitive pointer routing");
    Check(
        dragInputRules::ShouldSampleFloatingWindowPointer(true, true) &&
            !dragInputRules::ShouldSampleFloatingWindowPointer(true, false) &&
            dragInputRules::ShouldSampleFloatingWindowPointer(false, true) &&
            dragInputRules::ShouldSampleFloatingWindowPointer(false, false),
        "floating windows must keep ordinary live hover sampling but preserve a queued native-drag release point");
    Check(
        dragInputRules::IsLatencySensitivePointerMessageSurface(
            true, false, false) &&
        dragInputRules::IsLatencySensitivePointerMessageSurface(
            false, true, false) &&
        dragInputRules::IsLatencySensitivePointerMessageSurface(
            false, false, true) &&
        !dragInputRules::IsLatencySensitivePointerMessageSurface(
            false, false, false),
        "latency-sensitive pointer coalescing must cover the desktop, floating Dock, and floating popup windows");
    Check(
        dragInputRules::ShouldStartQueuedMouseMoveCoalescing(
            true, true, true) &&
        !dragInputRules::ShouldStartQueuedMouseMoveCoalescing(
            false, true, true) &&
        !dragInputRules::ShouldStartQueuedMouseMoveCoalescing(
            true, false, true) &&
        !dragInputRules::ShouldStartQueuedMouseMoveCoalescing(
            true, true, false),
        "latency-sensitive pointer coalescing must start only for a move on an eligible input surface");
    Check(
        dragInputRules::ShouldCoalesceQueuedMouseMove(
            true, true, true) &&
            !dragInputRules::ShouldCoalesceQueuedMouseMove(
                false, true, true) &&
            !dragInputRules::ShouldCoalesceQueuedMouseMove(
                true, false, true) &&
            !dragInputRules::ShouldCoalesceQueuedMouseMove(
                true, true, false),
        "latency-sensitive pointer coalescing must stop at another window or message kind");
}

void TestPopupIconLoadCancellationRules()
{
    namespace popupRules =
        snowdesktop::popup_icon_load_rules;
    const std::uint64_t currentGeneration = 43;
    Check(popupRules::NextGeneration(42) == currentGeneration &&
            popupRules::NextGeneration(
                std::numeric_limits<std::uint64_t>::max()) == 1,
        "popup generations must advance and skip the reserved zero epoch after wraparound");
    Check(popupRules::ShouldRejectResult(
                true, 42, currentGeneration) &&
            !popupRules::ShouldRejectResult(
                true, currentGeneration, currentGeneration) &&
            !popupRules::ShouldRejectResult(
                false, 42, currentGeneration),
        "only stale popup results must be rejected by the popup epoch gate");
}

void TestDragHintRasterRules()
{
    Check(!dragHintRules::ShouldReuseRaster(true, true, 96, 96, false),
        "arming or releasing a modifier must repaint a cached hint with identical text");
    Check(dragHintRules::ShouldReuseRaster(true, true, 96, 96),
        "valid cached text at the same DPI is eligible for raster reuse");
    Check(
        !dragHintRules::ShouldReuseRaster(true, false, 96, 96) &&
            !dragHintRules::ShouldReuseRaster(true, true, 96, 144) &&
            !dragHintRules::ShouldReuseRaster(false, true, 96, 96),
        "text, DPI, and raster validity changes must invalidate the drag hint cache");

    const auto bottomRight = dragHintRules::ResolveWindowPosition(
        {1910, 1070}, {200, 40}, {0, 0, 1920, 1080},
        48, 22, 8);
    Check(bottomRight.x == 1712 && bottomRight.y == 1032,
        "drag hints must remain inside the monitor work-area margins");
    const auto negativeMonitor = dragHintRules::ResolveWindowPosition(
        {-1910, -1070}, {200, 40}, {-1920, -1080, 0, 0},
        48, 22, 8);
    Check(negativeMonitor.x >= -1912 && negativeMonitor.y >= -1072,
        "drag hint placement must preserve negative virtual-screen coordinates");
    const auto tinyWorkArea = dragHintRules::ResolveWindowPosition(
        {90, 10}, {200, 40}, {0, 0, 100, 20},
        48, 22, 8);
    Check(tinyWorkArea.x == -50 && tinyWorkArea.y == -10,
        "undersized work areas must center overflow without invalid clamp bounds");
}

void TestWidgetPairDrops()
{
    namespace pair = snowdesktop::widget_pair_drop;
    using Type = DesktopWidgetType;
    using Action = pair::Action;
    // Independent supported pairs. The production entry is the same Apply
    // transaction used by pointer release; only key normalization is supplied.
    const auto normalize = [](std::wstring key) {
        for (auto& c : key) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
        return key;
    };
    struct Case { Type source; Type target; bool merge; Action group; };
    const Case cases[] = {
        {Type::Collection, Type::Collection, true, Action::CreateCollectionGroup},
        {Type::FileCategories, Type::FileCategories, true, Action::CreateFileGroup},
        {Type::FileCategories, Type::FolderMapping, false, Action::CreateFileGroup},
        {Type::FolderMapping, Type::FileCategories, false, Action::CreateFileGroup},
        {Type::FolderMapping, Type::FolderMapping, false, Action::CreateFileGroup},
        {Type::Collection, Type::FileCategories, false, Action::None},
        {Type::FileCategories, Type::Collection, false, Action::None},
        {Type::LuaScript, Type::Collection, false, Action::None},
        {Type::Collection, Type::CollectionGroup, false, Action::None},
        {Type::FileCategories, Type::FileGroup, false, Action::None},
    };
    for (const auto& entry : cases)
    {
        const auto options = pair::GetOptions(entry.source, entry.target);
        for (unsigned mods = 0; mods < 8; ++mods)
        {
            const Action expected = mods == 1 ? entry.group
                : (mods == 2 && entry.merge ? Action::Merge : Action::None);
            Check(pair::ResolveAction(options, mods & 1, mods & 2, mods & 4) == expected,
                "only a lone Ctrl or supported lone Shift arms the requested pair operation");
        }
        const auto makeWidgets = [&] {
            std::vector<DesktopWidget> widgets(3);
            widgets[0].id = L"source";
            widgets[0].type = entry.source;
            widgets[0].itemKeys = {L"shared", L"source-only", L"missing-on-disk"};
            widgets[0].sourceFolderPath = L"C:\\mapped-source";
            widgets[0].gridSpan = {3, 4};
            widgets[0].showSearchBox = true;
            widgets[1].id = L"target";
            widgets[1].type = entry.target;
            widgets[1].sourceFolderPath = L"C:\\mapped-target";
            widgets[1].itemKeys = {L"target-first", L"SHARED"};
            widgets[1].customTitle = L"Keep target title";
            widgets[1].showFileCategories = true;
            widgets[1].gridCell = {L"page-a", 2, 3};
            widgets[1].gridSpan = {2, 2};
            widgets[2].id = L"untouched";
            widgets[2].itemKeys = {L"unrelated"};
            return widgets;
        };
        DesktopWidget group;
        group.id = L"new-group";
        group.gridCell = {L"page-a", 2, 3};
        group.gridSpan = {2, 2};
        std::vector<DockEntry> dock;
        auto widgets = makeWidgets();
        Check(!pair::Apply(widgets, dock, 0, 1, Action::None, {}, normalize) &&
                widgets.size() == 3 && widgets[0].itemKeys.size() == 3,
            "cancelled or unarmed drops must retain the complete source");
        Check(pair::Apply(widgets, dock, 0, 1, Action::Merge, {}, normalize) == entry.merge,
            "merge must reject mismatched component types");
        if (entry.merge)
        {
            Check(widgets.size() == 2 && widgets[0].id == L"target" &&
                    widgets[0].itemKeys == std::vector<std::wstring>{
                        L"target-first", L"SHARED", L"source-only", L"missing-on-disk"} &&
                    widgets[0].customTitle == L"Keep target title" &&
                    widgets[0].showFileCategories && widgets[1].id == L"untouched" &&
                    widgets[1].itemKeys == std::vector<std::wstring>{L"unrelated"},
                "merge must append all source contents before deleting the source, deduplicate keys and preserve target/unrelated data");
        }
        else
            Check(widgets.size() == 3 && widgets[0].itemKeys.size() == 3 &&
                    widgets[1].itemKeys.size() == 2,
                "rejected merges must leave both components intact");
        widgets = makeWidgets();
        const bool grouped = pair::Apply(widgets, dock, 0, 1, entry.group, group, normalize);
        Check(grouped == (entry.group != Action::None), "group creation must enforce the supported pair matrix");
        if (grouped)
        {
            Check(widgets.size() == 4 && widgets[3].id == L"new-group" &&
                    widgets[3].childWidgetIds == std::vector<std::wstring>{L"target", L"source"} &&
                    widgets[3].activeCategoryId == L"target" &&
                    widgets[3].type == (entry.group == Action::CreateCollectionGroup
                        ? Type::CollectionGroup : Type::FileGroup) &&
                    widgets[3].gridCell.pageId == L"page-a" && widgets[3].gridCell.column == 2 &&
                    widgets[0].id == L"source" && widgets[0].itemKeys.size() == 3 &&
                    widgets[0].sourceFolderPath == L"C:\\mapped-source" &&
                    widgets[0].gridSpan.columns == 3 && widgets[0].gridSpan.rows == 4 &&
                    widgets[0].showSearchBox && widgets[1].customTitle == L"Keep target title" &&
                    widgets[1].sourceFolderPath == L"C:\\mapped-target",
                "group creation must retain both children and their settings, put target first and activate it");
            Check(!pair::Apply(widgets, dock, 0, 1, entry.group, group, normalize) && widgets.size() == 4,
                "a repeated release must not create another group or reparent hidden children");
        }
        widgets = makeWidgets();
        Check(!pair::Apply(widgets, dock, 0, 0, Action::Merge, {}, normalize) &&
                !pair::Apply(widgets, dock, 0, 90, Action::Merge, {}, normalize) && widgets.size() == 3,
            "self drops and stale targets must never delete a component");
        group.id = L"target";
        Check(!pair::Apply(widgets, dock, 0, 1, entry.group, group, normalize) && widgets.size() == 3,
            "group IDs must not overwrite an existing component");
    }

    std::vector<DesktopWidget> widgets(2);
    widgets[0].id = L"target";
    widgets[0].itemKeys = {L"kept"};
    widgets[1].id = L"source";
    std::vector<DockEntry> dock(2);
    dock[0].type = dock[1].type = DockEntryType::Collection;
    dock[0].reference = L"source";
    dock[1].reference = L"target";
    Check(pair::Apply(widgets, dock, 1, 0, Action::Merge, {}, normalize) &&
            widgets.size() == 1 && widgets[0].id == L"target" &&
            widgets[0].itemKeys == std::vector<std::wstring>{L"kept"} &&
            dock.size() == 1 && dock[0].reference == L"target",
        "merging an empty later source must preserve the target and remove only the stale source Dock reference");
}

void TestPairGroupsDissolveAndDockOwnership()
{
    namespace pair = snowdesktop::widget_pair_drop;
    using Type = DesktopWidgetType;
    const auto normalize = [](const std::wstring& key) { return key; };
    for (const auto type : {Type::Collection, Type::FileCategories, Type::FolderMapping})
    {
        std::vector<DesktopWidget> widgets(2);
        widgets[0].id = L"source";
        widgets[0].type = type;
        widgets[0].itemKeys = {L"original-a", L"original-b"};
        widgets[0].sourceFolderPath = L"C:\\original-mapping";
        widgets[0].gridSpan = {3, 4};
        widgets[0].customTitle = L"Original title";
        widgets[0].showSearchBox = true;
        widgets[0].gridCell = {L"__dock", 0, 0};
        widgets[1].id = L"target";
        widgets[1].type = type;
        const auto dockType = type == Type::Collection ? DockEntryType::Collection
            : type == Type::FileCategories ? DockEntryType::DesktopFiles : DockEntryType::FolderMapping;
        std::vector<DockEntry> dock{{dockType, L"source"}, {DockEntryType::DesktopItem, L"unrelated"}};
        DesktopWidget group;
        group.id = L"group";
        group.gridCell = {L"page", 2, 3};
        group.gridSpan = {2, 2};
        const auto action = type == Type::Collection
            ? pair::Action::CreateCollectionGroup : pair::Action::CreateFileGroup;
        Check(!pair::Apply(widgets, dock, 0, 0, action, group, normalize) &&
                dock.size() == 2 && widgets.size() == 2,
            "a rejected Dock pair must keep its widget and Dock reference");
        Check(pair::Apply(widgets, dock, 0, 1, action, group, normalize) &&
                dock.size() == 1 && dock[0].reference == L"unrelated" &&
                widgets.size() == 3 && widgets[2].dissolveWhenSingle,
            "a committed Dock pair must transfer sole ownership and persist automatic dissolution");
        Check(!pair::Dissolve(widgets, 2, {L"page", 2, 3}) && widgets.size() == 3,
            "a pair group with two children must remain a group");
        widgets[2].childWidgetIds = {L"source"}; // The target was moved out through a group label.
        widgets[1].gridCell = {L"elsewhere", 0, 0};
        widgets[2].dissolveWhenSingle = false;
        Check(!pair::Dissolve(widgets, 2, {L"page", 2, 3}),
            "manual and legacy groups must not dissolve automatically");
        widgets[2].dissolveWhenSingle = true;
        widgets[2].gridSpan = {4, 3}; // The user resized the group after creation.
        Check(pair::Dissolve(widgets, 2, {L"page", 2, 3}) && widgets.size() == 2 &&
                widgets[0].id == L"source" && widgets[0].type == type &&
                widgets[0].itemKeys == std::vector<std::wstring>{L"original-a", L"original-b"} &&
                widgets[0].sourceFolderPath == L"C:\\original-mapping" &&
                widgets[0].gridSpan.columns == 4 && widgets[0].gridSpan.rows == 3 &&
                widgets[0].customTitle == L"Original title" && widgets[0].showSearchBox &&
                widgets[0].gridCell.pageId == L"page" && widgets[0].gridCell.column == 2 &&
                widgets[0].gridCell.row == 3 && widgets[1].gridCell.pageId == L"elsewhere",
            "single-child dissolution must inherit the current group size, retain source settings and leave the sibling untouched");
        group.type = type == Type::Collection ? Type::CollectionGroup : Type::FileGroup;
        group.dissolveWhenSingle = true;
        group.childWidgetIds = {L"missing"};
        widgets.push_back(group);
        Check(!pair::Dissolve(widgets, 2, {}) && widgets.size() == 3,
            "an unresolved child must not be discarded with its group");
        widgets[2].childWidgetIds.clear();
        Check(pair::Dissolve(widgets, 2, {}) && widgets.size() == 2,
            "an empty auto group must remove only the wrapper");
    }
}

void TestGroupRestorationPublishesOnlyTheFinalFrame()
{
    namespace pair = snowdesktop::widget_pair_drop;
    for (const auto type : {DesktopWidgetType::Collection,
            DesktopWidgetType::FileCategories, DesktopWidgetType::FolderMapping})
    {
        snowdesktop::WidgetGroupTransition transition;
        std::vector<DesktopWidget> widgets(2);
        widgets[0].id = L"group";
        widgets[0].type = type == DesktopWidgetType::Collection
            ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileGroup;
        widgets[0].gridCell = {L"page", 1, 2};
        widgets[0].gridSpan = {4, 3};
        widgets[0].dissolveWhenSingle = true;
        widgets[0].childWidgetIds = {L"child", L"outgoing"};
        widgets[1].id = L"child";
        widgets[1].type = type;
        widgets[1].gridSpan = {2, 2};
        struct Frame { DesktopWidgetType type; size_t members; GridSpan span; };
        std::vector<Frame> frames;
        auto paint = [&] {
            if (!transition.ShouldDeferPaint())
                frames.push_back({widgets[0].type, widgets[0].childWidgetIds.size(), widgets[0].gridSpan});
        };
        paint(); // Previously presented group, before the drop removes a member.
        widgets[0].childWidgetIds = {L"child"};
        transition.Request(); // The production runtime rebuild requests restoration.
        transition.Request(); // Repeated layout passes in the same input dispatch.
        paint(); // A nested WM_PAINT must retain the previous complete frame.
        int restores = 0;
        Check(transition.FinishDispatch(true, [&] {
                ++restores;
                Check(pair::Dissolve(widgets, 0, {L"page", 1, 2}), "restoration commits at the dispatch boundary");
                transition.Request(); // A rebuild during restoration cannot schedule a loop.
                paint(); // Nor can a nested paint publish a partially rebuilt final model.
            }), "settled restoration requests one complete final repaint");
        paint();
        Check(restores == 1 && frames.size() == 2 && frames[0].members == 2 &&
                frames.back().type == type && frames.back().span.columns == 4 &&
                frames.back().span.rows == 3,
            "presentation moves directly from the full group to its resized child without a single-member group frame");
        Check(!transition.FinishDispatch(true, [&] { ++restores; }) && restores == 1,
            "one dispatch coalesces repeated rebuild requests into a single restoration");
    }

    snowdesktop::WidgetGroupTransition blocked;
    int restores = 0;
    blocked.Request();
    Check(blocked.FinishDispatch(false, [&] { ++restores; }) && restores == 0 &&
            !blocked.ShouldDeferPaint(),
        "an active drag/menu/rename keeps its references and releases the short paint hold at the outer boundary");
    Check(!blocked.FinishDispatch(false, [&] { ++restores; }),
        "a long interaction cannot force repeated repaint or freeze unrelated desktop updates");
    Check(blocked.FinishDispatch(true, [&] {
            ++restores;
            Check(!blocked.FinishDispatch(true, [&] { ++restores; }),
                "nested dispatch cannot re-enter an in-progress restoration");
        }) && restores == 1,
        "the first safe dispatch consumes a previously blocked restoration without a timer delay");
    blocked.Request();
    Check(blocked.FinishDispatch(true, [] {}) && !blocked.ShouldDeferPaint() &&
            !blocked.FinishDispatch(true, [] {}),
        "an invalid or no-longer-single group releases its frame without an unbounded retry loop");
}

void TestBottomBarContentReservation()
{
    Check(
        chromeRules::ReservedBottomBarHeight(true, false, 36) == 36,
        "a persistent titled bottom bar must reserve content height");
    Check(
        chromeRules::ReservedBottomBarHeight(false, false, 36) == 0,
        "a titleless widget must not reserve a nonexistent bottom bar");
    Check(
        chromeRules::ReservedBottomBarHeight(true, true, 36) == 0,
        "a hover bottom bar must not permanently shrink content");
    Check(
        chromeRules::ReservedBottomBarHeight(false, true, -1) == 0,
        "the reserved bottom bar height must remain non-negative");
    Check(
        !chromeRules::HasBottomBar(false) &&
            chromeRules::HasBottomBar(true),
        "only titled Lua widgets expose the host move bar");
    Check(
        !chromeRules::ShowsBottomBar(false, false, true) &&
            !chromeRules::ShowsBottomBar(true, true, false) &&
            chromeRules::ShowsBottomBar(true, true, true) &&
            chromeRules::ShowsBottomBar(true, false, false),
        "bottom-bar drawing must respect both ownership and hover mode");
    Check(
        !chromeRules::ShowsResizeHandle(false, false, false) &&
            chromeRules::ShowsResizeHandle(false, false, true) &&
            chromeRules::ShowsResizeHandle(true, true, true) &&
            chromeRules::ShowsResizeHandle(true, false, false),
        "resize-handle drawing must preserve titleless hover access");
    Check(
        !chromeRules::ShowsCompactMoveHandle(false, false) &&
            chromeRules::ShowsCompactMoveHandle(false, true) &&
            !chromeRules::ShowsCompactMoveHandle(true, true),
        "only hovered titleless widgets expose a compact move handle");
    const auto lightForegroundChrome =
        chromeRules::ResolveWidgetChromeForegroundStyle(0);
    const auto darkForegroundChrome =
        chromeRules::ResolveWidgetChromeForegroundStyle(1);
    Check(
        !lightForegroundChrome.darkForeground &&
            lightForegroundChrome.fontWeightAdjustment == 0 &&
            darkForegroundChrome.darkForeground &&
            darkForegroundChrome.fontWeightAdjustment == -200,
        "widget titles and handles must resolve their foreground from the widget content theme");
    Check(
        chromeRules::CompactEdgeHandleWidth(120, 24) == 24 &&
            chromeRules::CompactEdgeHandleWidth(30, 24) == 15 &&
            chromeRules::CompactEdgeHandleWidth(-1, 24) == 0,
        "compact edge handles must remain disjoint on narrow widgets");
    Check(
        chromeRules::HostActionContentBottom(
            false, 0, 110, 80, 5) == 110,
        "a compact titleless host placeholder may use the full frame height");
    Check(
        chromeRules::HostActionContentBottom(
            true, 0, 110, 80, 5) == 75,
        "a titled host placeholder must stay clear of its move bar");
    Check(
        chromeRules::UsesCategorizedControlAccentOutline(true, true) &&
            !chromeRules::UsesCategorizedControlAccentOutline(false, true) &&
            !chromeRules::UsesCategorizedControlAccentOutline(true, false) &&
            chromeRules::CategorizedControlOutlineWidth(true) == 2.0f &&
            chromeRules::CategorizedControlOutlineWidth(false) == 1.0f,
        "only keyboard-selected search boxes and tabs use the emphasized accent outline");
}

void TestNestedWidgetScrolling()
{
    using snowdesktop::widget_scroll_rules::
        ApplyWheelDelta;
    const auto innerBoundary =
        ApplyWheelDelta(0, 0, 120);
    Check(!innerBoundary.moved &&
            !innerBoundary.reachedEnd && innerBoundary.offset == 0,
        "wheel at a nested scroll boundary can bubble");
    const auto outerScroll =
        ApplyWheelDelta(48, 240, 120);
    Check(outerScroll.moved &&
            !outerScroll.reachedEnd && outerScroll.offset == 0,
        "wheel moves the first enclosing scroll area that can move");
    const auto lowerBoundary =
        ApplyWheelDelta(240, 240, -120);
    Check(!lowerBoundary.moved &&
            !lowerBoundary.reachedEnd && lowerBoundary.offset == 240,
        "wheel at the lower boundary can bubble");
    const auto precisionWheel =
        ApplyWheelDelta(20, 240, 15);
    Check(precisionWheel.moved &&
            precisionWheel.offset < 20,
        "precision touchpad wheel deltas still scroll");
    const auto reachesEnd = ApplyWheelDelta(220, 240, -120);
    Check(reachesEnd.moved && reachesEnd.offset == 240 &&
            reachesEnd.reachedEnd,
        "a wheel movement reports the transition that first reaches the end");
    Check(!snowdesktop::widget_scroll_rules::ReachedScrollEnd(
            240, 240, 240) &&
            snowdesktop::widget_scroll_rules::ReachedScrollEnd(
                120, 240, 240),
        "scroll-end transitions do not repeat while already at the boundary");
}

void TestScrollbarThumbDragging()
{
    namespace scroll = snowdesktop::widget_scroll_rules;
    const auto geometry = scroll::ResolveScrollbarAxisGeometry(
        100, 300, 800, 200, 300, 1.0f);
    Check(geometry.maximum == 600 &&
            geometry.trackStart == 104 && geometry.trackEnd == 296 &&
            geometry.thumbStart > geometry.trackStart &&
            geometry.thumbEnd < geometry.trackEnd,
        "scrollbar geometry stays inside the actual content viewport");
    Check(scroll::ScrollbarThumbHit(
            geometry, geometry.thumbStart, 397, 400, 1.0f) &&
            !scroll::ScrollbarThumbHit(
                geometry, geometry.thumbStart, 380, 400, 1.0f),
        "scrollbar dragging uses a forgiving target only at the viewport edge");
    Check(scroll::ApplyScrollbarThumbDrag(
            300, geometry.ThumbTravel(), geometry) == 600 &&
            scroll::ApplyScrollbarThumbDrag(
                300, -geometry.ThumbTravel(), geometry) == 0,
        "thumb movement maps to the complete scroll range and clamps at both ends");

    const auto compact = scroll::ResolveScrollbarAxisGeometry(
        10, 24, 1000, 14, 0, 2.0f);
    Check(compact.TrackExtent() > 0 &&
            compact.ThumbExtent() <= compact.TrackExtent(),
        "compact scrollbars keep their thumb inside the available track");
}

void TestListDetailRules()
{
    namespace details = snowdesktop::list_detail_rules;
    Check(details::ResolveFontSize(std::nullopt, 18.0f) == 18.0f,
        "legacy layouts inherit the saved icon title font size");
    Check(details::ResolveFontSize(15.0f, 18.0f) == 15.0f,
        "new layouts retain their independent list font size");
    Check(details::RowHeight(36, 38, 10.0f, 15.0f) == 36,
        "10 cu list text preserves the minimum compatible row height");
    Check(details::RowHeight(36, 38, 15.0f, 15.0f) == 38,
        "15 cu list text preserves the legacy row height");
    Check(details::RowHeight(36, 38, 24.0f, 15.0f) == 49,
        "24 cu list text expands rows by the scaled line-height delta");
    Check(details::RowHeight(54, 57, 36.0f,
            22.5f) == 73,
        "row height applies the same formula at component scale");

    constexpr auto defaultControls =
        rules::ResolveCategorizedControlMetrics(34.0f);
    constexpr auto largeControls =
        rules::ResolveCategorizedControlMetrics(48.0f);
    Check(defaultControls.fontSizeCu == 15.0f &&
            defaultControls.searchBoxHeightCu == 30.0f &&
            defaultControls.detailsHeaderHeightCu ==
                defaultControls.searchBoxHeightCu,
        "detail headers share the default categorized font and search-box height");
    Check(largeControls.fontSizeCu >
                defaultControls.fontSizeCu &&
            largeControls.detailsHeaderHeightCu == 44.0f &&
            largeControls.detailsHeaderHeightCu ==
                largeControls.searchBoxHeightCu,
        "detail header font and height scale with categorized tabs and search boxes");

    const auto nameOnly = details::BuildColumns(
        299, false, false, false,
        details::kDefaultModifiedPosition,
        details::kDefaultTypePosition,
        details::kDefaultSizePosition);
    Check(nameOnly.nameWidth == 299 &&
            !nameOnly.showModified && !nameOnly.showType &&
            !nameOnly.showSize &&
            !details::HasMetadataColumns(false, false, false),
        "name-only list mode uses the full width without a detail header");
    const auto modified = details::BuildColumns(
        300, true, false, false,
        details::kDefaultModifiedPosition,
        details::kDefaultTypePosition,
        details::kDefaultSizePosition);
    Check(modified.nameWidth == 82 &&
            modified.modifiedWidth == 218 &&
            modified.showModified && !modified.showType &&
            !modified.showSize,
        "a lone detail divider keeps its percentage position");
    const auto sizeOnly = details::BuildColumns(
        230, false, false, true,
        details::kDefaultModifiedPosition,
        details::kDefaultTypePosition,
        details::kDefaultSizePosition);
    Check(sizeOnly.nameWidth == 189 && sizeOnly.sizeWidth == 41 &&
            !sizeOnly.showModified && !sizeOnly.showType &&
            sizeOnly.showSize,
        "individually selected columns use their own divider percentage");
    const auto all = details::BuildColumns(
        510, true, true, true,
        details::kDefaultModifiedPosition,
        details::kDefaultTypePosition,
        details::kDefaultSizePosition);
    Check(all.nameWidth == 140 && all.showModified &&
            all.showType && all.showSize,
        "all selected detail columns fit at their baseline widths");
    Check(details::HitColumn(all, 20) == details::Column::Name &&
            details::HitColumn(all, 200) == details::Column::Modified &&
            details::HitColumn(all, 330) == details::Column::Type &&
            details::HitColumn(all, 460) == details::Column::Size,
        "fixed detail headers route clicks to the visible column");
    Check(details::HitDivider(all, 140, 3) ==
                details::Column::Modified &&
            details::HitDivider(all, 300, 3) ==
                details::Column::Type &&
            details::HitDivider(all, 420, 3) ==
                details::Column::Size &&
            details::HitDivider(all, 250, 3) ==
                details::Column::None,
        "detail header dividers take priority within their resize tolerance");
    const auto custom = details::BuildColumns(
        600, true, true, true, 0.25f, 0.55f, 0.80f);
    Check(custom.nameWidth == 150 && custom.modifiedWidth == 180 &&
            custom.typeWidth == 150 && custom.sizeWidth == 120,
        "custom divider percentages scale directly with component width");
    const auto constrained = details::BuildColumns(
        300, true, true, true,
        details::kDefaultModifiedPosition,
        details::kDefaultTypePosition,
        details::kDefaultSizePosition);
    Check(constrained.nameWidth == 82 &&
            constrained.modifiedWidth == 94 &&
            constrained.typeWidth == 71 &&
            constrained.sizeWidth == 53,
        "narrow components preserve divider percentages without auto-sizing");
    const details::DividerPositions defaults;
    const float movedType = details::ClampDraggedPosition(
        details::Column::Type, 0.70f,
        true, true, true, defaults);
    const auto moved = details::BuildColumns(
        1000, true, true, true,
        defaults.modified, movedType, defaults.size);
    Check(moved.nameWidth == 275 && moved.modifiedWidth == 425 &&
            moved.typeWidth == 124 && moved.sizeWidth == 176,
        "dragging one divider leaves every other divider percentage fixed");
    Check(details::ClampDraggedPosition(
                details::Column::Modified, 0.90f,
                true, true, true, defaults) ==
            defaults.type - details::kMinimumDividerGap &&
            details::ClampDraggedPosition(
                details::Column::Type, 0.10f,
                true, true, true, defaults) ==
            defaults.modified + details::kMinimumDividerGap &&
            details::ClampDraggedPosition(
                details::Column::Size, 0.10f,
                true, true, true, defaults) ==
            defaults.type + details::kMinimumDividerGap,
        "dragged dividers stop at adjacent visible dividers without moving them");
    const auto legacyPositions = details::LegacyWidthsToPositions(
        160.0f, 120.0f, 90.0f);
    Check(std::abs(legacyPositions.modified -
                details::kDefaultModifiedPosition) < 0.0001f &&
            std::abs(legacyPositions.type -
                details::kDefaultTypePosition) < 0.0001f &&
            std::abs(legacyPositions.size -
                details::kDefaultSizePosition) < 0.0001f,
        "legacy saved widths migrate to the equivalent baseline percentages");
    Check(details::DefaultAscending(details::Column::Name) &&
            details::DefaultAscending(details::Column::Type) &&
            !details::DefaultAscending(details::Column::Modified) &&
            !details::DefaultAscending(details::Column::Size),
        "detail columns use Explorer-style initial directions");
    Check(details::FromLegacyFolderSortMode(2) ==
            details::Column::Modified &&
            details::FromLegacyFolderSortMode(3) == details::Column::Size,
        "legacy folder sorting migrates to detail column state");
}
}

int main()
{
    TestWidgetMenuMetadataReuse();
    TestMarqueeUsesContentCoordinates();
    TestViewportClipping();
    TestActiveItemFallback();
    TestTabWidthDistribution();
    TestBottomBarWidthFollowsCornerAndHeight();
    TestScrollableStorageTitleBar();
    TestBottomBarContentReservation();
    TestGuidePlaceholderLifecycle();
    TestStableReorder();
    TestPendingFilePlacementReconciliation();
    TestFileGroupRules();
    TestGridPlacementInvariants();
    TestHoverOnlyWidgetVisibility();
    TestWidgetDesktopSurfaceVisibility();
    TestWidgetPreviewSourceVisibility();
    TestDesktopHoverDeactivation();
    TestDragInputSampling();
    TestPopupIconLoadCancellationRules();
    TestDragHintRasterRules();
    TestWidgetPairDrops();
    TestPairGroupsDissolveAndDockOwnership();
    TestGroupRestorationPublishesOnlyTheFinalFrame();
    TestNestedWidgetScrolling();
    TestScrollbarThumbDragging();
    TestListDetailRules();
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
