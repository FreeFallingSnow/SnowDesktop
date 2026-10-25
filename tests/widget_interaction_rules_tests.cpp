#include "../src/widgets/collection_group_rules.h"
#include "../src/app/widgets/widget_group_transition.h"
#include "../src/widgets/widget_pair_drop.h"
#include "app/widgets/widget_pair_drag_source.h"
#include "app/widgets/widget_right_drag_actions.h"
#include "widgets/widget_group_drop.h"
#include "desktop/desktop_hover_rules.h"
#include "dock/dock_magnification.h"
#include "drag_drop/drag_input_rules.h"
#include "drag_drop/drag_hint_rules.h"
#include "widget/view/widget_scroll_rules.h"
#include "widget/runtime/widget_visibility_rules.h"
#include "../src/widgets/widget_chrome_rules.h"
#include "../src/widgets/storage_title_bar_layout.h"
#include "../src/widgets/guide_widget_rules.h"
#include "drag_drop/pending_drop_rules.h"
#include "layout/list_detail_rules.h"
#include "layout/popup_icon_load_rules.h"
#include "layout/collection_popup_layout.h"
#include "layout/item_layout_rules.h"
#include "test_source_boundary.h"
#include "widget/runtime/widget_menu_catalogue.h"

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

void TestCollectionTitleSettingScope()
{
    using snowdesktop::item_layout_rules::ResolveTitleLines;
    // Exercise opposite settings, so accidentally applying the large-folder
    // preference to its popup cannot pass with the default two-line values.
    for (const int largeFolderLines : {1, 2})
    {
        const int scrollingLines = 3 - largeFolderLines;
        Check(ResolveTitleLines(true, true, false, 2, largeFolderLines, scrollingLines) == largeFolderLines,
            "desktop large-folder preview follows its own label-line preference");
        Check(ResolveTitleLines(true, true, true, 2, largeFolderLines, scrollingLines) == scrollingLines,
            "expanding a large folder uses scrolling labels instead of its desktop-preview preference");
        Check(ResolveTitleLines(true, false, true, 2, largeFolderLines, scrollingLines) == scrollingLines &&
            ResolveTitleLines(true, false, false, 2, largeFolderLines, scrollingLines) == scrollingLines,
            "scrolling collections and folder mappings retain their label preference on both surfaces");
        Check(ResolveTitleLines(false, false, false, 1, largeFolderLines, scrollingLines) == 1,
            "desktop icon labels stay independent of both container preferences");
    }
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

    // The entire original footprint remains an activation target when folded.
    // Leaving, including native mouse-leave,
    // returns to the saved fold unless an interaction still owns the content.
    Check(titleBar::ExpandOnHover(true, false, false, true, false, false, false),
        "hovering anywhere in the full frame opens a collapsed widget");
    Check(titleBar::ExpandOnHover(true, true, false, true, false, false, false),
        "moving within the full frame keeps the widget expanded");
    Check(!titleBar::ExpandOnHover(true, true, false, false, false, false, false) &&
            titleBar::ExpandOnHover(true, true, false, false, false, false, true),
        "leaving folds the widget unless a menu or editing interaction retains it");
    Check(titleBar::ExpandOnHover(true, true, false, false, true, false, false),
        "widget selection retains hover expansion after pointer leave");
    Check(titleBar::ExpandOnHover(true, true, false, false, false, true, false),
        "selected inner files retain hover expansion after pointer leave");
    Check(!titleBar::ExpandOnHover(true, false, false, false, false, false, true),
        "interaction retention alone must not open an unhovered widget");
    Check(!titleBar::ExpandOnHover(true, false, false, false, true, false, false),
        "selection left by manual collapse must not immediately reopen the widget");
    Check(!titleBar::ExpandOnHover(true, false, true, true, true, false, false),
        "manual collapse suppresses expansion throughout the full frame");
    Check(!titleBar::ExpandOnHover(false, true, false, true, true, true, true),
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
        rules::ShouldShowFileGroupSourceTabs(
            true, false) &&
        !rules::ShouldShowInnerCategoryTabs(
            true, true, false),
        "file-group search results retain source tabs and hide inner categories");
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
            8, 4) == 146,
        "search already includes the preceding source row and must not reserve it twice");
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
        hoverRules::ShouldHoldHoverDuringContextMenu(true, true) &&
            !hoverRules::ShouldHoldHoverDuringContextMenu(true, false) &&
            !hoverRules::ShouldHoldHoverDuringContextMenu(false, true) &&
            !hoverRules::ShouldHoldHoverDuringContextMenu(false, false),
        "context-menu capture retains Dock hover only while the live pointer remains on the Dock");
    Check(
        hoverRules::ShouldRetainHoverAcrossMouseLeave(
            true, false) &&
            hoverRules::ShouldRetainHoverAcrossMouseLeave(
                false, true) &&
            !hoverRules::ShouldRetainHoverAcrossMouseLeave(
                false, false),
        "content and paired backdrop windows must form one logical hover surface");
    Check(!hoverRules::ShouldClearRejectedDesktopHover(true, true, true) &&
            hoverRules::ShouldClearRejectedDesktopHover(true, false, true) &&
            hoverRules::ShouldClearRejectedDesktopHover(true, true, false) &&
            !hoverRules::ShouldClearRejectedDesktopHover(false, false, false),
        "stale desktop moves must retain hover owned by the live Dock while external and visual-only targets still clear");
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
    Check(hoverRules::CanCoalescePassiveMouseMoves(true, false, false, false) &&
            !hoverRules::CanCoalescePassiveMouseMoves(false, false, false, false) &&
            !hoverRules::CanCoalescePassiveMouseMoves(true, true, false, false) &&
            !hoverRules::CanCoalescePassiveMouseMoves(true, false, true, false) &&
            !hoverRules::CanCoalescePassiveMouseMoves(true, false, false, true),
        "passive queue coalescing must exclude pressed, captured and OLE input");
    Check(!hoverRules::ShouldProbeMouseMoveQueue(false, true, false) &&
            hoverRules::ShouldProbeMouseMoveQueue(false, true, true) &&
            hoverRules::ShouldProbeMouseMoveQueue(true, false, false) &&
            !hoverRules::ShouldProbeMouseMoveQueue(false, false, true),
        "passive physical input must avoid redundant retrieval while posted moves and active gestures retain compaction");
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
        "an active hover may follow a changed desktop or Dock sample after activation is allowed");
    Check(
        hoverRules::IsActiveHoverSampleSurface(true, false, false) &&
            hoverRules::IsActiveHoverSampleSurface(false, true, true) &&
            !hoverRules::IsActiveHoverSampleSurface(false, true, false) &&
            !hoverRules::IsActiveHoverSampleSurface(false, false, true),
        "active hover sampling includes actual Dock input but excludes visual overdraw and unrelated popup bridges");
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

void TestDockDesktopHoverHandoff()
{
    // Replace only native hit sampling/tracking. Both variants drive the real
    // magnification timeline; the old content/backdrop-only route is a negative
    // control for the stationary desktop hit recorded after minimize.
    const auto preservesHandoff = [](bool acceptDesktopHandoff) {
        snowdesktop::dock_magnification::HoverEntryAnimation entry;
        int contentTracking = 0;
        int desktopTracking = 0;
        const auto routeLeave = [&](bool content, bool backdrop, bool desktop, double now) {
            const auto rearm = [&](bool trackContent) {
                if (trackContent) ++contentTracking;
                else ++desktopTracking;
            };
            const bool retained = acceptDesktopHandoff
                ? hoverRules::RetainDockSurfaceMouseLeave(content, backdrop, desktop, rearm)
                : hoverRules::RetainPairedSurfaceMouseLeave(content, backdrop,
                    [&] { rearm(true); });
            if (!retained) entry.SetHovered(false, now, 1.0);
            return retained;
        };
        entry.SetHovered(true, 1000.0, 1.0);
        entry.Advance(1080.0);
        const bool retained = routeLeave(false, false, true, 1080.0);
        entry.SetHovered(true, 1080.0, 1.0);
        entry.Advance(1160.0);
        const bool uninterrupted = retained && desktopTracking == 1 &&
            contentTracking == 0 && entry.FocusScale(2.0f) == 2.0f && !entry.IsAnimating();
        const bool backdropRetained = routeLeave(false, true, false, 1160.0) &&
            contentTracking == 0 && desktopTracking == 1;
        const bool contentRetained = routeLeave(true, false, false, 1160.0) &&
            contentTracking == 1 && desktopTracking == 1;
        const bool exited = !routeLeave(false, false, false, 1200.0) &&
            entry.FocusScale(2.0f) == 1.0f && !entry.IsAnimating();
        return uninterrupted && backdropRetained && contentRetained && exited;
    };
    Check(preservesHandoff(true),
        "minimize desktop handoff must preserve the wave, transfer leave tracking, and clear on a real exit");
    Check(!preservesHandoff(false),
        "negative control must reject the old route that restarts hover and leaves desktop tracking unarmed");
}

void TestRetainedDockLeaveDoesNotFeedBack()
{
    hoverRules::RetainedDockLeaveTracking tracking;
    snowdesktop::dock_magnification::HoverEntryAnimation entry;
    entry.SetHovered(true, 1000.0, 1.0);
    entry.Advance(1160.0);
    int contentSubscriptions = 0, desktopSubscriptions = 0;
    const auto leave = [&](bool content, bool backdrop, bool desktop, int x, int y) {
        const bool retained = hoverRules::RetainDockSurfaceMouseLeave(
            content, backdrop, desktop, [&](bool trackContent) {
                if (!hoverRules::TryRearmRetainedDockLeave(tracking, trackContent, x, y)) return;
                if (trackContent) ++contentSubscriptions;
                else ++desktopSubscriptions;
            });
        if (!retained) entry.SetHovered(false, 1200.0, 1.0);
        return retained;
    };
    // Model User32 immediately posting another WM_MOUSELEAVE after each
    // subscription at the recorded stationary bottom-edge position.
    for (int i = 0; i < 256; ++i)
        Check(leave(true, false, false, 1402, 1439),
            "repeated native leaves must retain the logical Dock hover");
    Check(contentSubscriptions == 1 && entry.FocusScale(2.0f) == 2.0f,
        "stationary retained leaves must not continuously resubscribe or restart magnification");
    hoverRules::ObserveRetainedDockLeavePointer(tracking, 1402, 1439);
    Check(leave(true, false, false, 1402, 1439) && contentSubscriptions == 1,
        "a synthetic move at the same position must not reopen the leave loop");
    Check(leave(false, true, false, 1402, 1439) && contentSubscriptions == 1 &&
        desktopSubscriptions == 0, "backdrop handoff retains hover without subscribing content");
    for (int i = 0; i < 256; ++i)
        Check(leave(false, false, true, 1402, 1439), "desktop handoff must retain hover");
    Check(desktopSubscriptions == 1,
        "the desktop surface gets one independent subscription at a stationary handoff");
    hoverRules::ObserveRetainedDockLeavePointer(tracking, 1403, 1439);
    Check(leave(true, false, false, 1403, 1439) && contentSubscriptions == 2,
        "real pointer movement restores the next native leave subscription");
    Check(!leave(false, false, false, 1403, 1439) && entry.FocusScale(2.0f) == 1.0f,
        "a real exit still clears Dock magnification");
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
    Check(dragInputRules::IsPointerGestureButtonDown(false, false, false, true, true) &&
        !dragInputRules::IsPointerGestureButtonDown(false, true, false, true, false) &&
        dragInputRules::IsPointerGestureButtonDown(true, false, true, true, false),
        "right drags must follow right-button release without borrowing the left button, while middle gestures retain precedence");
    Check(!dragInputRules::IsItemDragRelease(true, false),
        "left-up must retain a right drag even when its wParam reports the still-held right button");
    Check(dragInputRules::IsItemDragRelease(true, true) &&
        dragInputRules::IsItemDragRelease(false, false) &&
        !dragInputRules::IsItemDragRelease(false, true),
        "only release of the initiating item-drag button may finish the gesture, independent of other held buttons");
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

void TestDragHintPlacementRules()
{
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

void TestGroupedLabelPairDrops()
{
    namespace pair = snowdesktop::widget_pair_drop;
    namespace drag = snowdesktop::widget_pair_drag;
    using Type = DesktopWidgetType;
    // The host resolves a captured label, then calls this ownership transaction.
    // No Item pointer, active-tab lookup or filesystem operation is substituted.
    const auto normalize = [](const std::wstring& key) { return key; };
    struct Case { Type source; Type target; };
    const Case cases[] = {
        {Type::Collection, Type::Collection},
        {Type::FileCategories, Type::FileCategories},
        {Type::FileCategories, Type::FolderMapping},
        {Type::FolderMapping, Type::FileCategories},
        {Type::FolderMapping, Type::FolderMapping},
    };
    for (const auto& test : cases)
        for (const bool targetInDock : {false, true})
            for (const auto action : {pair::Action::Merge,
                     pair::GetOptions(test.source, test.target).group})
            {
                std::vector<DesktopWidget> widgets(5);
                widgets[0].id = L"old-group";
                widgets[0].type = test.source == Type::Collection
                    ? Type::CollectionGroup : Type::FileGroup;
                widgets[0].childWidgetIds = {L"before", L"source", L"after"};
                widgets[0].activeCategoryId = L"source";
                widgets[0].customTitle = L"Keep original group";
                widgets[1].id = L"source";
                widgets[1].type = test.source;
                widgets[1].itemKeys = {L"shared", L"source-only"};
                widgets[1].sourceFolderPath = L"C:\\mapped-source";
                widgets[1].customTitle = L"Keep child settings";
                widgets[1].showSearchBox = true;
                widgets[2].id = L"before";
                widgets[2].type = test.source;
                widgets[3].id = L"target";
                widgets[3].type = test.target;
                widgets[3].itemKeys = {L"target-first", L"shared"};
                widgets[3].sourceFolderPath = L"C:\\mapped-target";
                widgets[3].gridCell = {targetInDock ? kDockPageId : L"page-a", 2, 3};
                widgets[4].id = L"after";
                widgets[4].type = test.source;
                std::vector<DockEntry> dock{{DockEntryTypeForWidget(widgets[0].type), L"old-group"}};
                if (targetInDock)
                    dock.push_back({DockEntryTypeForWidget(test.target), L"target"});
                DragSourceList list;
                list.hasOriginWidget = true;
                list.originWidgetId = L"old-group";
                list.originWidgetType = widgets[0].type;
                list.hasCollectionGroupEntries = test.source == Type::Collection;
                list.hasFileGroupSourceLabels = test.source != Type::Collection;
                list.hasFileGroupEntries = test.source == Type::FileCategories;
                list.hasWidgets = test.source == Type::FolderMapping;
                DragSourceEntry entry;
                entry.widgetId = L"source";
                entry.kind = test.source == Type::Collection ? DropSourceKind::CollectionGroupEntry
                    : test.source == Type::FileCategories ? DropSourceKind::FileGroupEntry
                    : DropSourceKind::Widget;
                entry.dockEntryType = DockEntryTypeForWidget(test.source);
                list.entries.push_back(entry);
                // Dwell may switch the visible tab while the child is in flight.
                widgets[0].activeCategoryId = L"before";
                const auto source = drag::ResolveSource(list, widgets, dock);
                Check(source.widgetIndex == 1 && source.groupId == L"old-group",
                    "collection, desktop-files and folder-mapping labels must resolve the captured child after tab switching");
                widgets[0].activeCategoryId = L"source";
                Check(!pair::CanPair(widgets, 1, 3) &&
                        pair::CanPair(widgets, 1, 3, source.groupId),
                    "grouped children can pair only through their explicitly captured source owner");
                DesktopWidget group;
                group.id = L"new-group";
                group.gridCell = widgets[3].gridCell;
                Check(!pair::Apply(widgets, dock, 1, 3, pair::Action::None, group, normalize, source.groupId) &&
                        widgets[0].childWidgetIds.size() == 3 && widgets[0].activeCategoryId == L"source",
                    "unarmed label drops must retain source membership and active tab");
                const bool expected = action != pair::Action::Merge ||
                    (test.source == test.target && test.source != Type::FolderMapping);
                Check(pair::Apply(widgets, dock, source.widgetIndex, 3, action, group, normalize, source.groupId) == expected,
                    "labels must support the same Shift merge and Ctrl group matrix as standalone components");
                if (!expected)
                {
                    Check(widgets.size() == 5 && widgets[0].childWidgetIds.size() == 3 &&
                            widgets[1].itemKeys.size() == 2 && widgets[0].activeCategoryId == L"source",
                        "unsupported mapping merges must keep the original group, source and target intact");
                    continue;
                }
                Check(widgets[0].childWidgetIds == std::vector<std::wstring>{L"before", L"after"} &&
                        widgets[0].activeCategoryId == L"after" && widgets[0].customTitle == L"Keep original group" &&
                        dock.front().reference == L"old-group",
                    "successful label drops must detach only the moved child and select its next neighbour in the original group");
                const auto target = std::find_if(widgets.begin(), widgets.end(),
                    [](const auto& widget) { return widget.id == L"target"; });
                if (action == pair::Action::Merge)
                    Check(widgets.size() == 4 && target != widgets.end() &&
                            target->itemKeys == std::vector<std::wstring>{L"target-first", L"shared", L"source-only"} &&
                            dock.size() == (targetInDock ? 2u : 1u),
                        "label merge must transfer and deduplicate contents before removing the child, preserving the target Dock reference");
                else
                    Check(widgets.size() == 6 && widgets.back().childWidgetIds ==
                            std::vector<std::wstring>{L"target", L"source"} &&
                            widgets.back().activeCategoryId == L"target" &&
                            widgets[1].sourceFolderPath == L"C:\\mapped-source" &&
                            widgets[1].customTitle == L"Keep child settings" && widgets[1].showSearchBox &&
                            target != widgets.end() && target->sourceFolderPath == L"C:\\mapped-target" && dock.size() == 1,
                        "label grouping must preserve both children, their mapping paths and settings without leaving duplicate Dock ownership");
                Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
                    "a stale label snapshot cannot consume a merged or reparented child again");
            }

    std::vector<DesktopWidget> widgets(3);
    widgets[0].id = L"group";
    widgets[0].type = Type::CollectionGroup;
    widgets[0].childWidgetIds = {L"child"};
    widgets[1].id = L"child";
    widgets[2].id = L"target";
    DragSourceList list;
    list.hasOriginWidget = list.hasCollectionGroupEntries = true;
    list.originWidgetType = Type::CollectionGroup;
    list.originWidgetId = L"group";
    DragSourceEntry entry;
    entry.kind = DropSourceKind::CollectionGroupEntry;
    entry.widgetId = L"child";
    list.entries.push_back(entry);
    const std::vector<DockEntry> dock;
    list.originWidgetId = L"wrong-owner";
    Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
        "labels with stale or mismatched source groups must be rejected");
    list.originWidgetId = L"group";
    list.entries.push_back(entry);
    Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
        "multiple labels must not arm a two-component transaction");
    list.entries.resize(1);
    list.entries[0].kind = DropSourceKind::DesktopIcon;
    Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
        "files inside a group must not be mistaken for the group label");
    widgets[0].childWidgetIds.push_back(L"target");
    Check(!pair::CanPair(widgets, 1, 2, L"group"),
        "a grouped target must not be merged or nested into a new group by label release");
    widgets[0].childWidgetIds = {L"child", L"child"};
    Check(!pair::CanUseSource(widgets, 1, L"group"),
        "duplicate source ownership must reject before detaching any data");

    widgets[0].childWidgetIds.clear();
    DragSourceList dockList;
    dockList.hasWidgets = true;
    entry.kind = DropSourceKind::Widget;
    entry.fromDock = true;
    entry.dockEntryType = DockEntryType::Collection;
    entry.dockReference = L"child";
    dockList.entries.push_back(entry);
    std::vector<DockEntry> dockEntries{{DockEntryType::Collection, L"child"}};
    Check(drag::ResolveSource(dockList, widgets, dockEntries).widgetIndex == 1 &&
            drag::ResolveSource(dockList, widgets, dockEntries).groupId.empty(),
        "standalone Dock widget drags must retain their existing pair route");
    dockEntries.clear();
    Check(drag::ResolveSource(dockList, widgets, dockEntries).widgetIndex >= widgets.size(),
        "stale Dock widget entries must not arm a pair drop");
}

void TestDesktopIconSourceVacatesItsHitBlock()
{
    namespace drag = snowdesktop::widget_pair_drag;
    for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
            DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    {
        DesktopWidget source;
        source.id = L"dragged-entrance";
        source.type = type;
        source.gridCell = {L"desktop-page", 2, 3};
        source.gridSpan = {1, 1};
        source.singleCellIconMode = true;
        source.selected = true;
        DragSourceList list;
        list.hasWidgets = true;
        list.hasOriginSurface = true;
        list.originSurface = snowdesktop::slot_contract::SlotSurfaceKind::Desktop;
        DragSourceEntry entry;
        entry.kind = DropSourceKind::Widget;
        entry.desktopWidgetIcon = true;
        entry.widgetId = source.id;
        entry.widgetType = type;
        list.entries.push_back(entry);
        // The host's native container uses this exact predicate before its
        // geometry hit, so a rejected widget target cannot block its old cell.
        Check(drag::IsCapturedDesktopIcon(list, source),
            "all five captured icon entrances expose the desktop landing underneath their original position");
        auto other = source;
        other.id = L"selected-but-not-dragged";
        Check(!drag::IsCapturedDesktopIcon(list, other),
            "selection alone never removes another component's target hit block");
        DragSourceEntry file;
        file.kind = DropSourceKind::DesktopIcon;
        list.entries.push_back(file);
        list.hasDesktopIcons = true;
        Check(drag::IsCapturedDesktopIcon(list, source),
            "mixed selection also vacates each captured native entrance");
        source.selected = false;
        Check(drag::IsCapturedDesktopIcon(list, source),
            "captured identity remains authoritative across selection changes during a drag");
        source.singleCellIconMode = false;
        Check(!drag::IsCapturedDesktopIcon(list, source),
            "component-mode storage retains its independent container semantics");
        source.singleCellIconMode = true;
        list.entries.front().fromDock = true;
        Check(!drag::IsCapturedDesktopIcon(list, source),
            "Dock captures must not vacate an independent desktop component");
        list.entries.front().fromDock = false;
        list.hasOriginWidget = true;
        Check(!drag::IsCapturedDesktopIcon(list, source),
            "popup members do not vacate their parent's desktop footprint");
    }
}

void TestWidgetRightDragMenuActions()
{
    namespace menu = snowdesktop::widget_right_drag;
    namespace pair = snowdesktop::widget_pair_drop;
    using Action = pair::Action;
    Check(menu::MoveModifiers(true) == 0 && menu::MoveModifiers(false) == MK_SHIFT,
        "right-menu Move never accidentally becomes Shift-merge for widgets, while files keep their Shell move verb");
    const auto collections = menu::PairChoices(DesktopWidgetType::Collection,
        pair::GetOptions(DesktopWidgetType::Collection, DesktopWidgetType::Collection), true);
    Check(collections.size() == 2 && collections[0].action == Action::Merge &&
            collections[0].modifiers == MK_SHIFT && collections[1].action == Action::CreateCollectionGroup &&
            collections[1].modifiers == MK_CONTROL && collections[0].command != collections[1].command,
        "collection right-drop offers independent merge and group commands with the existing modifier semantics");
    const auto files = menu::PairChoices(DesktopWidgetType::FileCategories,
        pair::GetOptions(DesktopWidgetType::FileCategories, DesktopWidgetType::FileCategories), true);
    Check(files.size() == 2 && files[0].action == Action::Merge && files[1].action == Action::CreateFileGroup,
        "desktop files offer their own merge and file-group actions");
    const auto mapping = menu::PairChoices(DesktopWidgetType::FolderMapping,
        pair::GetOptions(DesktopWidgetType::FolderMapping, DesktopWidgetType::FileCategories), true);
    Check(mapping.size() == 1 && mapping[0].action == Action::CreateFileGroup,
        "folder mappings offer grouping without pretending their filesystem contents can be merged");
    Check(menu::PairChoices(DesktopWidgetType::Collection,
            pair::GetOptions(DesktopWidgetType::Collection, DesktopWidgetType::FileCategories), true).empty(),
        "incompatible pairs expose no destructive component verbs");
    const auto noGroupSpace = menu::PairChoices(DesktopWidgetType::Collection,
        pair::GetOptions(DesktopWidgetType::Collection, DesktopWidgetType::Collection), false);
    Check(noGroupSpace.size() == 1 && noGroupSpace[0].action == Action::Merge &&
            menu::PairChoices(DesktopWidgetType::FolderMapping,
                pair::GetOptions(DesktopWidgetType::FolderMapping, DesktopWidgetType::FolderMapping), false).empty(),
        "insufficient group space removes only unavailable actions");

    for (const auto& choice : collections)
    {
        std::vector<DesktopWidget> widgets(2);
        for (size_t i = 0; i < widgets.size(); ++i)
        {
            widgets[i].id = i == 0 ? L"source" : L"target";
            widgets[i].type = DesktopWidgetType::Collection;
            widgets[i].itemKeys = {i == 0 ? L"source-item" : L"target-item"};
        }
        const auto action = pair::ResolveAction(pair::GetOptions(widgets[0].type, widgets[1].type),
            (choice.modifiers & MK_CONTROL) != 0, (choice.modifiers & MK_SHIFT) != 0, false);
        DesktopWidget group;
        group.id = L"created-by-right-menu";
        std::vector<DockEntry> dock;
        Check(action == choice.action && pair::Apply(widgets, dock, 0, 1, action, std::move(group),
                [](const std::wstring& key) { return key; }),
            "the returned menu choice reaches the real model transaction");
        if (choice.action == Action::Merge)
            Check(widgets.size() == 1 && widgets[0].id == L"target" && widgets[0].itemKeys.size() == 2,
                "menu merge consumes its source exactly once and retains both contents");
        else
            Check(widgets.size() == 3 && widgets.back().childWidgetIds == std::vector<std::wstring>{L"target", L"source"},
                "menu grouping preserves both children in target-then-source order");
    }
}

void TestRightDragIconSourcesAfterMenu()
{
    namespace menu = snowdesktop::widget_right_drag;
    using Type = DesktopWidgetType;
    for (const auto type : {Type::Collection, Type::FileCategories, Type::FolderMapping, Type::CollectionGroup, Type::FileGroup})
    {
        DesktopWidget widget;
        widget.id = L"captured-icon";
        widget.type = type;
        widget.gridCell = {L"desktop", 2, 3};
        widget.gridSpan = {1, 1};
        widget.singleCellIconMode = true;
        DragSourceList source;
        source.hasWidgets = source.hasOriginSurface = true;
        source.originSurface = snowdesktop::slot_contract::SlotSurfaceKind::Desktop;
        DragSourceEntry entry;
        entry.desktopWidgetIcon = true;
        entry.kind = DropSourceKind::Widget;
        entry.widgetId = widget.id;
        entry.widgetType = type;
        entry.originalCell = widget.gridCell;
        entry.originalSpan = widget.gridSpan;
        source.entries.push_back(entry);
        std::vector<DesktopWidget> widgets = {widget};
        Check(menu::CapturedIconsCurrent(source, widgets),
            "right-menu continuation uses captured identities even without any surviving runtime Item");
        widgets.insert(widgets.begin(), DesktopWidget{});
        Check(menu::CapturedIconsCurrent(source, widgets),
            "runtime rebuilds and model reordering preserve the captured native source");
        auto changed = widgets;
        changed.back().gridCell.column++;
        Check(!menu::CapturedIconsCurrent(source, changed), "menu cannot move an entrance whose position changed while open");
        changed = widgets;
        changed.back().singleCellIconMode = false;
        Check(!menu::CapturedIconsCurrent(source, changed), "menu cannot resume after an entrance changes presentation");
        changed = widgets;
        changed.front().childWidgetIds = {widget.id};
        Check(!menu::CapturedIconsCurrent(source, changed), "menu cannot steal a source acquired by another group");
        changed = widgets;
        changed.push_back(widget);
        Check(!menu::CapturedIconsCurrent(source, changed), "duplicate source identities cancel rather than picking an arbitrary component");
        changed.pop_back();
        changed.pop_back();
        Check(!menu::CapturedIconsCurrent(source, changed), "removed sources cancel menu continuation");
        DragSourceEntry file;
        file.kind = DropSourceKind::DesktopIcon;
        source.entries.push_back(file);
        source.hasDesktopIcons = true;
        Check(menu::CapturedIconsCurrent(source, widgets), "mixed file and component selections retain native source validation");
    }
}

void TestWidgetGroupMembershipMatrix()
{
    namespace groups = snowdesktop::widget_group_drop;
    using Type = DesktopWidgetType;
    for (const auto groupType : {Type::CollectionGroup, Type::FileGroup})
        for (const auto sourceType : {Type::Collection, Type::FileCategories, Type::FolderMapping, Type::CollectionGroup, Type::FileGroup})
            for (const int presentation : {0, 1, 2})
            {
                std::vector<DesktopWidget> widgets(3);
                widgets[0].id = L"target-group";
                widgets[0].type = groupType;
                widgets[0].gridSpan = presentation == 2 ? GridSpan{3, 3} : GridSpan{1, 1};
                widgets[0].singleCellIconMode = presentation == 1;
                widgets[0].childWidgetIds = {L"existing-child"};
                widgets[0].activeCategoryId = L"existing-child";
                widgets[1].id = L"existing-child";
                widgets[1].type = groupType == Type::CollectionGroup ? Type::Collection : Type::FileCategories;
                widgets[2].id = L"moving-icon";
                widgets[2].type = sourceType;
                widgets[2].gridCell = {L"desktop", 4, 5};
                widgets[2].gridSpan = {1, 1};
                widgets[2].singleCellIconMode = true;
                widgets[2].sourceFolderPath = L"C:\\mapping-target";
                widgets[2].itemKeys = {L"retained-source-file"};
                std::vector<DockEntry> dock;
                const bool accepted = groupType == Type::CollectionGroup ? sourceType == Type::Collection
                    : sourceType == Type::FileCategories || sourceType == Type::FolderMapping;
                Check(groups::Apply(widgets, dock, 0, {L"moving-icon"}, 1) == accepted,
                    "all five entrance types use the same existing-group membership rules across all three target presentations");
                Check(widgets.size() == 3 && widgets[2].itemKeys == std::vector<std::wstring>{L"retained-source-file"} &&
                        widgets[2].sourceFolderPath == L"C:\\mapping-target" && widgets[2].singleCellIconMode &&
                        widgets[2].gridCell.column == 4 && widgets[0].singleCellIconMode == (presentation == 1) &&
                        widgets[0].gridSpan.columns == (presentation == 2 ? 3 : 1),
                    "grouping retains source settings, contents and return cell as well as the target presentation");
                Check(widgets[0].childWidgetIds == (accepted
                        ? std::vector<std::wstring>{L"existing-child", L"moving-icon"}
                        : std::vector<std::wstring>{L"existing-child"}) && widgets[0].activeCategoryId == L"existing-child",
                    "membership changes exactly once on accepted moves and rejected nesting leaves ownership intact");
            }
    std::vector<DesktopWidget> widgets(4);
    widgets[0].id = L"file-group";
    widgets[0].type = Type::FileGroup;
    widgets[1].id = L"desktop-files";
    widgets[1].type = Type::FileCategories;
    widgets[2].id = L"mapping";
    widgets[2].type = Type::FolderMapping;
    widgets[3].id = L"collection";
    widgets[3].type = Type::Collection;
    std::vector<DockEntry> dock{{DockEntryType::FolderMapping, L"mapping", false}};
    Check(!groups::Apply(widgets, dock, 0, {L"desktop-files", L"collection"}, 0) &&
            widgets[0].childWidgetIds.empty() && dock.size() == 1,
        "a rejected mixed membership transaction never detaches a valid source first");
    Check(groups::Apply(widgets, dock, 0, {L"desktop-files", L"mapping", L"mapping"}, 0) && dock.empty() &&
            widgets[0].childWidgetIds == std::vector<std::wstring>{L"desktop-files", L"mapping"},
        "compatible source kinds enter once in selection order and retire their Dock ownership");
    Check(groups::Apply(widgets, dock, 0, {L"desktop-files"}, 2) &&
            widgets[0].childWidgetIds == std::vector<std::wstring>{L"mapping", L"desktop-files"},
        "existing children reorder with the same insertion-index adjustment");
    Check(!groups::Apply(widgets, dock, 0, {}, 0) && !groups::Apply(widgets, dock, 0, {L"missing-source"}, 0) &&
            widgets[0].childWidgetIds == std::vector<std::wstring>{L"mapping", L"desktop-files"},
        "empty/cancelled and stale-ID decisions preserve membership");
}

void TestDesktopIconModePairDrops()
{
    namespace pair = snowdesktop::widget_pair_drop;
    namespace drag = snowdesktop::widget_pair_drag;
    using Type = DesktopWidgetType;
    using Action = pair::Action;
    // The real desktop drag payload enters the same source resolver and model
    // transaction as Dock/label drags. Runtime Item wrappers are not required.
    struct Case { Type source, target; Action shift, control; };
    const Case cases[] = {
        {Type::Collection, Type::Collection, Action::Merge, Action::CreateCollectionGroup},
        {Type::FileCategories, Type::FileCategories, Action::Merge, Action::CreateFileGroup},
        {Type::FileCategories, Type::FolderMapping, Action::None, Action::CreateFileGroup},
        {Type::FolderMapping, Type::FileCategories, Action::None, Action::CreateFileGroup},
        {Type::FolderMapping, Type::FolderMapping, Action::None, Action::CreateFileGroup},
        {Type::CollectionGroup, Type::Collection, Action::None, Action::None},
        {Type::FileGroup, Type::FileCategories, Action::None, Action::None},
    };
    for (const auto& test : cases)
        for (const bool control : {false, true})
        {
            std::vector<DesktopWidget> widgets(2);
            widgets[0].id = L"source";
            widgets[0].type = test.source;
            widgets[0].gridCell = {L"page", 1, 2};
            widgets[0].gridSpan = {1, 1};
            widgets[0].singleCellIconMode = true;
            widgets[0].itemKeys = {L"shared", L"source-only"};
            widgets[0].sourceFolderPath = L"C:\\mapped-source";
            widgets[1].id = L"target";
            widgets[1].type = test.target;
            widgets[1].gridCell = {L"page", 3, 2};
            widgets[1].itemKeys = {L"target-first", L"shared"};
            DragSourceList list;
            list.hasWidgets = list.hasOriginSurface = true;
            list.originSurface = snowdesktop::slot_contract::SlotSurfaceKind::Desktop;
            DragSourceEntry entry;
            entry.kind = DropSourceKind::Widget;
            entry.widgetId = L"source";
            entry.widgetType = test.source;
            entry.desktopWidgetIcon = true;
            list.entries.push_back(entry);
            std::vector<DockEntry> dock;
            const auto source = drag::ResolveSource(list, widgets, dock);
            Check(source.widgetIndex == 0 && source.groupId.empty(),
                "a single desktop icon-mode component resolves by captured identity");
            std::swap(widgets[0], widgets[1]);
            Check(drag::ResolveSource(list, widgets, dock).widgetIndex == 1,
                "container rebuilds must resolve the captured entrance ID rather than its old index");
            std::swap(widgets[0], widgets[1]);
            widgets[0].singleCellIconMode = false;
            Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
                "a mode change during drag must not consume a stale icon-mode entrance");
            widgets[0].singleCellIconMode = true;

            // Check mixed-selection rejection while the source is still valid,
            // so this cannot pass merely because a prior transaction removed it.
            list.entries.push_back(entry);
            Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
                "multiple selected entrances cannot arm a two-component pair transaction");
            list.entries.back().kind = DropSourceKind::DesktopIcon;
            list.entries.back().desktopWidgetIcon = false;
            Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
                "a mixed file/entrance selection cannot arm a partial merge or group");
            list.entries.resize(1);
            const auto options = pair::GetOptions(test.source, test.target);
            const auto action = pair::ResolveAction(options, control, !control, false);
            Check(action == (control ? test.control : test.shift) &&
                    pair::ResolveAction(options, false, false, false) == Action::None &&
                    pair::ResolveAction(options, true, true, false) == Action::None &&
                    pair::ResolveAction(options, control, !control, true) == Action::None,
                "icon-mode sources use existing Shift merge and Ctrl group rules, including cancellation keys");
            DesktopWidget group;
            group.id = L"new-group";
            Check(pair::Apply(widgets, dock, source.widgetIndex, 1, action, group,
                    [](const std::wstring& key) { return key; }) == (action != Action::None),
                "only a compatible armed icon-mode pair commits an ownership transaction");
            if (action == Action::None)
                Check(widgets.size() == 2 && widgets[0].itemKeys.size() == 2 &&
                        widgets[0].gridCell.column == 1 && widgets[0].sourceFolderPath == L"C:\\mapped-source",
                    "unsupported or unarmed pairs retain both source content and placement");
            else if (action == Action::Merge)
                Check(widgets.size() == 1 && widgets.front().id == L"target" &&
                        widgets.front().itemKeys == std::vector<std::wstring>{L"target-first", L"shared", L"source-only"},
                    "Shift transfers and deduplicates source contents once before removing its entrance");
            else
                Check(widgets.size() == 3 && widgets.back().childWidgetIds ==
                        std::vector<std::wstring>{L"target", L"source"} &&
                        widgets.back().type == (control && test.control == Action::CreateCollectionGroup
                            ? Type::CollectionGroup : Type::FileGroup) &&
                        widgets[0].sourceFolderPath == L"C:\\mapped-source",
                    "Ctrl creates the matching group and retains each child and its mapping settings");
            if (action != Action::None)
                Check(drag::ResolveSource(list, widgets, dock).widgetIndex >= widgets.size(),
                    "a consumed or newly grouped desktop entrance cannot commit the captured pair twice");

        }
}

void TestDockComponentInsertionHints()
{
    for (const bool control : {false, true})
    {
        Check(std::string_view(dragHintRules::DockInsertionHintKey(false, false, true, control)) ==
                "core.drag.move_widget_dock",
            "component and grouped-label Dock insertion must never advertise Ctrl file mapping");
        Check(std::string_view(dragHintRules::DockInsertionHintKey(false, false, false, control)) ==
                (control ? "core.dock.release_dock_map_full" : "core.dock.release_move_dock_ctrl"),
            "ordinary file Dock insertion must retain its Ctrl mapping hint");
    }
    Check(std::string_view(dragHintRules::DockInsertionHintKey(true, false, true, true)) ==
            "core.drag.release_adjust_order" &&
            std::string_view(dragHintRules::DockInsertionHintKey(false, true, false, false)) ==
            "core.dock.release_dock_map_full",
        "same-Dock reorder and external file mapping must keep their existing hints");
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
            darkForegroundChrome.darkForeground,
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

namespace
{
void TestPopupChromeScaling()
{
    namespace popup = snowdesktop::collection_popup_layout;
    // Reproduce the reported 2x page: controls must be measured on the popup
    // page, and nondefault tab heights must reserve the complete painted header.
    const auto doubled = popup::ResolveMetrics(184, 232, 184, 232, 2.0f);
    Check(popup::ResolveTitleFontSize(doubled.scale) == 32.0f &&
            popup::ResolveControlFontSize(doubled.scale) == 28.0f,
        "popup titles and sort labels use their own readable 2x fonts");
    Check(popup::ResolveDetailsHeaderHeight(doubled, 24.0f) == 40 &&
            popup::ResolveDetailsHeaderHeight(doubled, 34.0f) == 60 &&
            popup::ResolveDetailsHeaderHeight(doubled, 48.0f) == 88,
        "popup headers reserve the actual configured height instead of fixed 60 pixels");
    const auto fractional = popup::ResolveMetrics(138, 174, 138, 174, 1.5f);
    Check(popup::ResolveDetailsHeaderHeight(fractional, 35.5f) == 47,
        "popup header geometry preserves fractional stored heights before pixel rounding");
    const auto dense = popup::ResolveMetrics(9, 12, 9, 12, 0.1f);
    const auto bounds = popup::ResolveHeaderVerticalBounds(dense.scale);
    Check(dense.scale == 0.75f && dense.headerHeight == 41 &&
            popup::ResolveTitleFontSize(dense.scale) == 12.0f &&
            bounds.titleBottom - bounds.titleTop >= 18 &&
            popup::ResolveDetailsHeaderHeight(dense, 24.0f) == 15,
        "dense desktop grids retain legible popup titles and control heights");
    Check(popup::ResolveChromeScale(std::numeric_limits<float>::quiet_NaN()) == 1.0f &&
            popup::ResolveChromeScale(std::numeric_limits<float>::infinity()) == 1.0f,
        "invalid popup scaling falls back to readable finite geometry");
    // Negative architecture boundary: this user-requested independent title
    // must never silently pick up icon title size again. Not rendering proof.
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    Check(snowdesktop::test::CheckSourceBoundaries(root, {
        {"src/app/popup/app_popup_render.cpp", "", "",
            {"itemTextFormat_", "itemFontSizeCu_"}},
        {"src/app/input/app_rename_begin.cpp", "if (popupTitleRename)",
            "else if (IsGroupedCollection", {"itemTextFormat_", "itemFontSizeCu_"}},
        {"src/app/popup/app_popup_render.cpp", "if (dockFolderPopupOpen_)",
            "RECT content =", {"OffsetRect(&sortTextRect"}}
    }), "popup title independence and sort centering retain their rendering boundaries");
}

void TestBalancedPopupColumns()
{
    namespace popup = snowdesktop::collection_popup_layout;
    const auto metrics = popup::ResolveMetrics(92, 116, 84, 108, 1.0f);
    const auto measure = [&](std::size_t count, int columns, int controlsHeight) {
        const int rows = popup::RequiredRowCount(count, columns);
        return std::pair{
            popup::ResolveFixedGridFrameWidth(metrics, columns),
            metrics.headerHeight + controlsHeight + rows * metrics.cellHeight +
                (rows - 1) * metrics.gapY + metrics.bottomPadding};
    };
    const auto nineItems = [&](int columns) { return measure(9, columns, 80); };
    Check(popup::ResolveBalancedColumnCount(9, 5, 640, nineItems) == 4,
        "nine-item popups add a third row and reduce columns when the full frame fits");
    Check(popup::ResolveBalancedColumnCount(9, 5, 410, nineItems) == 5,
        "short work areas preserve a fitting wider popup instead of adding scrolling");
    Check(popup::ResolveBalancedColumnCount(9, 2, 640, nineItems) == 2,
        "narrow work areas never request unavailable grid columns");
    Check(popup::ResolveBalancedColumnCount(0, 5, 640, nineItems) == 3 &&
            popup::ResolveBalancedColumnCount(2, 5, 640,
                [&](int columns) { return measure(2, columns, 80); }) == 3,
        "empty and tiny collections preserve their compact baseline footprint");
    Check(popup::ResolveBalancedColumnCount(25, 5, 640,
            [&](int columns) { return measure(25, columns, 80); }) == 5,
        "large collections minimize required scrolling before optimizing aspect ratio");
    Check(popup::ResolveBalancedColumnCount(9, 5, 1280,
            [&](int columns) {
                const auto size = nineItems(columns);
                return std::pair{size.first * 2, size.second * 2};
            }) == 4,
        "uniform high-DPI scaling preserves the chosen popup proportions");
}

void TestWholeColumnPopupWidth()
{
    namespace popup = snowdesktop::collection_popup_layout;
    // The reported page needs a 296 cu three-column frame, not a 320 cu frame
    // with centered tracks. Search and category controls must follow that width.
    for (const float scale : {0.75f, 1.0f, 1.25f, 2.0f})
    {
        const auto metrics = popup::ResolveMetrics(
            popup::ScaleDimension(80, scale), popup::ScaleDimension(106, scale),
            popup::ScaleDimension(74, scale), popup::ScaleDimension(100, scale), scale);
        for (const int columns : {3, 4})
        {
            const int frameWidth = popup::ResolveFixedGridFrameWidth(metrics, columns);
            const RECT viewport{-600 + metrics.paddingX, -200,
                -600 + frameWidth - metrics.paddingX, 200};
            const RECT first = popup::ResolveFixedGridItemRect(metrics, viewport, 0);
            const RECT last = popup::ResolveFixedGridItemRect(metrics, viewport, columns - 1);
            const RECT nextRow = popup::ResolveFixedGridItemRect(metrics, viewport, columns, 17);
            Check(popup::ResolveFixedGridColumnCount(metrics,
                        static_cast<int>(viewport.right - viewport.left)) == columns &&
                    first.left == viewport.left && last.right == viewport.right,
                "three- and four-column frames have exactly the normal popup padding at both edges");
            Check(nextRow.left == first.left && nextRow.right == first.right &&
                    nextRow.top == first.top + metrics.cellHeight + metrics.gapY - 17 &&
                    popup::RequiredRowCount(3, columns) == 2,
                "whole popup tracks align across rows while preserving scrolling and the two-row floor");
        }
    }
    const auto metrics = popup::ResolveMetrics(92, 116, 84, 108, 1.0f);
    const RECT exact{-600, -200, -304, 200};
    const RECT first = popup::ResolveFixedGridItemRect(metrics, exact, 0);
    const RECT last = popup::ResolveFixedGridItemRect(metrics, exact, 2);
    Check(first.left == exact.left && last.right == exact.right,
        "whole-track popup widths retain their existing symmetric baseline insets");
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    Check(snowdesktop::test::CheckSourceBoundaries(root, {
        {"src/app/popup/app_popup_geometry.cpp", "RECT DesktopApp::GetCollectionPopupRect(",
            "snowdesktop::collection_popup_layout::Metrics", {"ScaleDimension(320"}}
    }), "grid popup frame sizing must not regain a control-width floor between complete columns");
}
}

int main()
{
    TestCollectionTitleSettingScope();
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
    TestDockDesktopHoverHandoff();
    TestRetainedDockLeaveDoesNotFeedBack();
    TestDragInputSampling();
    TestPopupIconLoadCancellationRules();
    TestDragHintPlacementRules();
    TestWidgetPairDrops();
    TestGroupedLabelPairDrops();
    TestDesktopIconModePairDrops();
    TestDesktopIconSourceVacatesItsHitBlock();
    TestWidgetRightDragMenuActions();
    TestRightDragIconSourcesAfterMenu();
    TestWidgetGroupMembershipMatrix();
    TestDockComponentInsertionHints();
    TestPairGroupsDissolveAndDockOwnership();
    TestGroupRestorationPublishesOnlyTheFinalFrame();
    TestNestedWidgetScrolling();
    TestScrollbarThumbDragging();
    TestListDetailRules();
    TestPopupChromeScaling();
    TestBalancedPopupColumns();
    TestWholeColumnPopupWidth();
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
