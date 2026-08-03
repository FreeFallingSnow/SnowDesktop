#include "navigation_settings.h"
#include "quick_navigation_animation_rules.h"
#include "quick_navigation_rules.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace rules =
    snowdesktop::quick_navigation_rules;

std::wstring GetDataFilePath(
    const wchar_t* filename)
{
    return filename ? filename : L"";
}

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

bool NearlyEqual(
    float left, float right,
    float tolerance = 0.0001f)
{
    return std::fabs(left - right) <= tolerance;
}

void TestViewModePersistenceValues()
{
    QuickNavigationDesktopViewMode mode =
        QuickNavigationDesktopViewMode::Tile;
    Check(
        QuickNavigationDesktopViewModeFromJson(
            "source", mode) &&
            mode ==
                QuickNavigationDesktopViewMode::Source,
        "source mode must parse");
    Check(
        std::string(
            QuickNavigationDesktopViewModeToJson(
                QuickNavigationDesktopViewMode::Initial)) ==
            "initial",
        "initial mode must serialize");
    Check(
        !QuickNavigationDesktopViewModeFromJson(
            "invalid", mode),
        "invalid mode must be rejected");

    Check(
        rules::NextQuickNavigationDesktopViewMode(
            QuickNavigationDesktopViewMode::Tile) ==
                QuickNavigationDesktopViewMode::Source &&
            rules::NextQuickNavigationDesktopViewMode(
                QuickNavigationDesktopViewMode::Source) ==
                QuickNavigationDesktopViewMode::Initial &&
            rules::NextQuickNavigationDesktopViewMode(
                QuickNavigationDesktopViewMode::Initial) ==
                QuickNavigationDesktopViewMode::Tile,
        "view mode button must cycle through all modes");
    Check(
        rules::QuickNavigationDesktopViewModeGlyph(
            QuickNavigationDesktopViewMode::Tile) ==
                L"\uF462" &&
            rules::QuickNavigationDesktopViewModeGlyph(
                QuickNavigationDesktopViewMode::Source) ==
                L"\uE6CA" &&
            rules::QuickNavigationDesktopViewModeGlyph(
                QuickNavigationDesktopViewMode::Initial) ==
                L"\uF802",
        "each view mode must map to its Fluent System Icons glyph");
}

std::wstring MakeTemporarySettingsPath()
{
    wchar_t directory[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    if (GetTempPathW(
            static_cast<DWORD>(
                std::size(directory)),
            directory) == 0)
        return {};
    if (GetTempFileNameW(
            directory, L"SDN", 0, path) == 0)
        return {};
    return path;
}

void TestViewModeFilePersistence()
{
    const std::wstring path =
        MakeTemporarySettingsPath();
    Check(
        !path.empty(),
        "temporary settings path must be created");
    if (path.empty()) return;

    constexpr QuickNavigationDesktopViewMode modes[] = {
        QuickNavigationDesktopViewMode::Tile,
        QuickNavigationDesktopViewMode::Source,
        QuickNavigationDesktopViewMode::Initial,
    };
    for (const auto mode : modes)
    {
        NavigationSettings saved;
        saved.desktopViewMode = mode;
        Check(
            SaveNavigationSettings(
                path.c_str(), saved),
            "navigation settings must save");
        NavigationSettings loaded;
        Check(
            LoadNavigationSettings(
                path.c_str(), loaded) &&
                loaded.desktopViewMode == mode,
            "navigation view mode must round trip");
    }

    {
        std::ofstream legacy(
            path, std::ios::binary |
                std::ios::trunc);
        legacy <<
            "{\"enabled\":true,"
            "\"modifiers\":3,"
            "\"virtualKey\":32}";
    }
    NavigationSettings legacyLoaded;
    legacyLoaded.desktopViewMode =
        QuickNavigationDesktopViewMode::Source;
    Check(
        LoadNavigationSettings(
            path.c_str(), legacyLoaded) &&
            legacyLoaded.desktopViewMode ==
                QuickNavigationDesktopViewMode::Tile,
        "legacy settings must default to tile");

    {
        std::ofstream invalid(
            path, std::ios::binary |
                std::ios::trunc);
        invalid <<
            "{\"desktopViewMode\":"
            "\"unsupported\"}";
    }
    NavigationSettings invalidLoaded;
    invalidLoaded.desktopViewMode =
        QuickNavigationDesktopViewMode::Initial;
    Check(
        LoadNavigationSettings(
            path.c_str(), invalidLoaded) &&
            invalidLoaded.desktopViewMode ==
                QuickNavigationDesktopViewMode::Tile,
        "invalid settings must default to tile");
    DeleteFileW(path.c_str());
}

void TestPinyinInitials()
{
    Check(
        rules::InitialBucket(L"微信") == L'W',
        "Chinese names must use pinyin initials");
    Check(
        rules::InitialBucket(L"alpha") == L'A',
        "Latin initials must be upper-case");
    Check(
        rules::InitialBucket(L"123") == L'#',
        "digits must use the fallback bucket");
    Check(
        rules::InitialBucket(L"-alpha") == L'#',
        "leading symbols must use the fallback bucket");
    Check(
        rules::InitialBucket(L"") == L'#',
        "empty names must use the fallback bucket");
    Check(
        rules::InitialSortKey(L"微信") <
            rules::InitialSortKey(L"文档"),
        "full pinyin keys must provide deterministic ordering");

    std::vector<std::wstring> names = {
        L"文档", L"微信", L"alpha", L"Alpha"};
    std::stable_sort(
        names.begin(), names.end(),
        rules::InitialNameLess);
    Check(
        names[0] == L"alpha" &&
            names[1] == L"Alpha" &&
            names[2] == L"微信" &&
            names[3] == L"文档",
        "initial sort must use pinyin and preserve stable ties");
}

void TestSourceOwnership()
{
    const std::vector<std::wstring> items = {
        L"DOCK", L"SHARED", L"GROUPED",
        L"FILES", L"LOOSE"};
    const std::vector<std::vector<
        std::wstring>> sources = {
            {L"DOCK", L"SHARED"},
            {L"SHARED", L"GROUPED"},
            {},
            {L"FILES"},
        };
    const auto owners =
        rules::AssignSourceOwners(
            items, sources);
    Check(
        owners ==
            std::vector<int>(
                {0, 0, 1, 3, -1}),
        "source priority must keep Dock first and leave unowned items loose");
}

void TestMappingSectionsFollowTabOrder()
{
    const std::vector<std::wstring> ids{
        L"collection-a",
        L"mapping-a",
        L"mapping-grouped",
        L"files-a",
        L"mapping-b",
    };
    const std::vector<std::wstring> tabOrder{
        L"mapping-b",
        L"mapping-grouped",
        L"files-a",
        L"collection-a",
        L"mapping-a",
    };
    const std::vector<bool> topTabEligible{
        true, true, true, true, true,
    };
    const std::vector<bool> mappingEligible{
        false, true, true, false, true,
    };

    const auto topOrder =
        rules::OrderIndicesByTabIds(
            tabOrder, ids, topTabEligible);
    const auto mappingOrder =
        rules::OrderIndicesByTabIds(
            tabOrder, ids, mappingEligible);

    std::vector<size_t> visibleMappingsFromTop;
    for (const size_t index : topOrder)
        if (mappingEligible[index])
            visibleMappingsFromTop.push_back(index);

    Check(
        mappingOrder == visibleMappingsFromTop,
        "mapping sections must be the mapping-only projection of the top-tab order");
    Check(
        std::find(
            mappingOrder.begin(),
            mappingOrder.end(), 2) ==
            mappingOrder.begin() + 1,
        "file-group mappings must retain their own top tab and mapping section");

    const std::vector<std::wstring> incompleteOrder{
        L"mapping-b"
    };
    const auto withFallback =
        rules::OrderIndicesByTabIds(
            incompleteOrder, ids,
            mappingEligible);
    Check(
        !withFallback.empty() &&
            withFallback.front() == 4 &&
            withFallback ==
                std::vector<size_t>({4, 1, 2}),
        "unregistered mappings must append stably after ordered tabs");
}

void TestSectionLayout()
{
    const auto layouts =
        rules::BuildSectionLayouts(
            {3, 0, 5},
            2, 100, 10,
            28, 8, 12);
    Check(
        layouts.size() == 2,
        "empty sections must be omitted");
    Check(
        layouts[0].firstItem == 0 &&
            layouts[0].gridTop == 36 &&
            layouts[0].bottom == 246,
        "first section geometry must include its header");
    Check(
        layouts[1].firstItem == 3 &&
            layouts[1].headerTop == 258 &&
            layouts[1].gridTop == 294 &&
            layouts[1].bottom == 614,
        "later sections must start after the section gap");
    Check(
        rules::FindItemSection(
            layouts, 4) == &layouts[1],
        "item lookup must skip headers");
    rules::SectionItemCell cell;
    Check(
        rules::TryGetSectionItemCell(
            layouts, 4, 2, 100, 10,
            cell) &&
            cell.column == 1 &&
            cell.row == 0 &&
            cell.top == 294,
        "item rectangles must use section-local columns below the header");
    Check(
        rules::SectionedContentHeight(
            layouts, 12) == 614,
        "content height must exclude the trailing gap");

    Check(
        rules::TabStripMaxScrollOffset(
            400, 100, 500) == 0 &&
            rules::TabStripMaxScrollOffset(
                400, 100, 350) == 150,
        "reserved button space must reduce the tab viewport");
    Check(
        rules::TabStripLabelStart(
            100, true, 28, 8) == 136 &&
            rules::TabStripLabelStart(
                100, false, 28, 8) == 100,
        "mode button must reserve space before the tabs only while visible");
    Check(
        rules::InitialJumpBucketAt(0) == L'A' &&
            rules::InitialJumpBucketAt(26) == L'#' &&
            rules::InitialJumpBucketIndex(L'W') == 22 &&
            rules::InitialJumpBucketIndex(L'#') == 26,
        "initial jump buckets must map A-Z and fallback consistently");
}

void TestAnimationRules()
{
    using namespace
        snowdesktop::quick_navigation_animation_rules;

    State state;
    Check(state.IsHidden(),
        "quick navigation starts hidden");

    state.Open(1000);
    Check(state.IsAnimating(),
        "open starts an animation");
    Check(state.IsInteractive(),
        "opening is immediately interactive");
    Check(state.GetVisual().visible,
        "opening window exists immediately so its search field can focus");
    state.Advance(1000 + kOpenDurationMs / 2);
    const Visual halfOpen = state.GetVisual();
    Check(halfOpen.visible,
        "half-open panel remains visible");
    Check(NearlyEqual(halfOpen.progress, 0.5f),
        "opening progress is time based");
    Check(NearlyEqual(halfOpen.opacity, 0.5f),
        "half-open opacity follows eased progress");
    Check(halfOpen.scale > kMinimumScale &&
            halfOpen.scale < 1.0f,
        "opening grows from the Dock icon scale");
    Check(NearlyEqual(
            ScaleCoordinate(
                42.0f, 42.0f,
                halfOpen.scale),
            42.0f),
        "the Dock search icon anchor remains stationary");
    Check(ScaleCoordinate(
            442.0f, 42.0f,
            kMinimumScale) < 80.0f,
        "the panel edge contracts toward the Dock search icon");
    Check(ShouldRefreshCloseAnchor(
            AnchorMode::Pointer),
        "shortcut close follows the current pointer");
    Check(!ShouldRefreshCloseAnchor(
            AnchorMode::DockSearch),
        "Dock close keeps the search icon anchor");
    Check(NearlyEqual(
            ScaleCoordinate(
                640.0f, 1200.0f, 1.0f),
            640.0f),
        "changing the close pointer at full scale cannot move or flash the panel");

    const Visual beforeClose = state.GetVisual();
    state.Close(1000 + kOpenDurationMs / 2);
    Check(state.IsClosing(),
        "close can interrupt opening");
    Check(NearlyEqual(
            beforeClose.opacity,
            state.GetVisual().opacity),
        "interrupting open keeps the current visual frame");
    state.Advance(
        1000 + kOpenDurationMs / 2 + 20);
    const Visual closing = state.GetVisual();
    Check(closing.progress <
            beforeClose.progress,
        "interrupted close moves toward hidden");

    state.Open(
        1000 + kOpenDurationMs / 2 + 20);
    Check(state.IsInteractive(),
        "open can interrupt closing");
    Check(NearlyEqual(
            closing.scale,
            state.GetVisual().scale),
        "reopening keeps scale continuous");
    state.Advance(2000);
    Check(!state.IsAnimating(),
        "reopened animation completes");
    Check(NearlyEqual(
            state.GetVisual().opacity, 1.0f),
        "completed open is fully opaque");

    state.Close(3000);
    state.Advance(
        3000 + kCloseDurationMs);
    Check(state.IsHidden(),
        "close completes at hidden");
    Check(!state.IsAnimating(),
        "hidden animation stops");

    state.ShowImmediately();
    Check(NearlyEqual(
            state.GetVisual().scale, 1.0f),
        "disabled animations show at full scale");
    state.ResetHidden();
    Check(state.IsHidden(),
        "reset returns to hidden");
}

void TestDeactivateRules()
{
    Check(!rules::ShouldCloseOnDeactivate(true),
        "an owned context menu must keep quick navigation open");
    Check(rules::ShouldCloseOnDeactivate(false),
        "activation outside quick navigation must close it");
}
}

int main()
{
    TestViewModePersistenceValues();
    TestViewModeFilePersistence();
    TestPinyinInitials();
    TestSourceOwnership();
    TestMappingSectionsFollowTabOrder();
    TestSectionLayout();
    TestAnimationRules();
    TestDeactivateRules();
    if (failures == 0)
    {
        std::cout
            << "quick navigation rules tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
