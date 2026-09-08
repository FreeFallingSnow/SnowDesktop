#include "navigation_settings.h"
#include "quick_navigation_animation_rules.h"
#include "quick_navigation_genie_rules.h"
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

void TestExtendedNavigationKeyNames()
{
    const auto formatKey = [](UINT virtualKey) {
        NavigationSettings settings;
        settings.modifiers = 0;
        settings.virtualKey = virtualKey;
        return FormatNavigationHotkey(settings);
    };

    const std::wstring pageUp = formatKey(VK_PRIOR);
    const std::wstring pageDown = formatKey(VK_NEXT);
    Check(!pageUp.empty() && pageUp != formatKey(VK_NUMPAD9),
        "Page Up must not be displayed as numeric keypad 9");
    Check(!pageDown.empty() && pageDown != formatKey(VK_NUMPAD3),
        "Page Down must not be displayed as numeric keypad 3");
    Check(formatKey(VK_LEFT) != formatKey(VK_NUMPAD4) &&
            formatKey(VK_RIGHT) != formatKey(VK_NUMPAD6) &&
            formatKey(VK_UP) != formatKey(VK_NUMPAD8) &&
            formatKey(VK_DOWN) != formatKey(VK_NUMPAD2),
        "arrow keys must not be displayed as numeric keypad keys");
}

void TestApplicationIconCacheIdentity()
{
    const std::wstring first =
        rules::ApplicationIconCacheIdentity(
            L"shell:AppsFolder\\Vendor.First_app!Main",
            L"Shared name");
    const std::wstring second =
        rules::ApplicationIconCacheIdentity(
            L"shell:AppsFolder\\Vendor.Second_app!Main",
            L"Shared name");
    Check(
        first != second,
        "app icon cache identities must not collapse distinct AppsFolder items");
    Check(
        first == rules::ApplicationIconCacheIdentity(
            L"SHELL:APPSFOLDER\\VENDOR.FIRST_APP!MAIN",
            L"Renamed display value"),
        "app icon cache identities must be case-insensitive and independent of display names");
    Check(
        rules::ApplicationIconCacheIdentity(
            L"", L"Fallback App") ==
            L"FALLBACK APP",
        "display names must provide a stable fallback cache identity");
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
    Check(state.IsOpening(),
        "opening state is reported while the panel targets visible");
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
    Check(NearlyEqual(
            SegmentNormalizedStartSlope(0.0f, true), 0.0f) &&
            NearlyEqual(
                SegmentNormalizedStartSlope(1.0f, false), 0.0f),
        "terminal smoothstep segments start at rest");
    Check(NearlyEqual(
            SegmentNormalizedStartSlope(0.5f, true), 1.5f) &&
            NearlyEqual(
                SegmentNormalizedStartSlope(0.5f, false), 1.5f),
        "open and close compositor segments preserve midpoint velocity");
    Check(NearlyEqual(
            ScaleCoordinate(
                640.0f, 1200.0f, 1.0f),
            640.0f),
        "changing the close pointer at full scale cannot move or flash the panel");

    const Visual beforeClose = state.GetVisual();
    state.Close(1000 + kOpenDurationMs / 2);
    Check(state.IsClosing(),
        "close can interrupt opening");
    Check(!state.IsOpening(),
        "closing state is not reported as opening");
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
    Check(SegmentNormalizedStartSlope(
            closing.progress, true) > 0.0f,
        "a reversed compositor segment carries forward its current velocity");
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

void TestAnimationEffects()
{
    using namespace snowdesktop::quick_navigation_animation_rules;
    struct EffectCase
    {
        bool dockSource;
        int windowEffect;
        int popupEffect;
        Effect expected;
    };
    const EffectCase cases[] = {
        {true, 1, 0, Effect::Scale},
        {true, 2, 2, Effect::Fade},
        {true, 3, 1, Effect::Genie},
        {true, 0, 0, Effect::None},
        {true, 0, 1, Effect::Fade},
        {true, 0, 2, Effect::Scale},
        {false, 3, 0, Effect::None},
        {false, 3, 1, Effect::Fade},
        {false, 3, 2, Effect::Scale},
    };
    for (const auto& item : cases)
    {
        Check(ResolveEffect(true, item.dockSource, item.windowEffect,
                item.popupEffect) == item.expected,
            "only a Dock source with a valid anchor may override popup animation preferences");
        Check(ResolveEffect(false, item.dockSource, item.windowEffect,
                item.popupEffect) == Effect::None,
            "global animation disable must override both Dock and popup effects");
    }

    State genie;
    genie.Configure(Effect::Genie, 0.7, true);
    Check(genie.GetEffect() == Effect::Genie &&
            NearlyEqual(static_cast<float>(genie.DurationMilliseconds(true)), 252.0f) &&
            NearlyEqual(static_cast<float>(genie.DurationMilliseconds(false)), 252.0f),
        "Dock Genie uses the window transition's 360ms duration with the selected speed in both directions");
    genie.Configure(Effect::Fade, 1.0, true);
    Check(genie.DurationMilliseconds(true) == 180.0 &&
            genie.DurationMilliseconds(false) == 180.0,
        "Dock fade matches the window transition duration");
    genie.Configure(Effect::Scale, 1.0, true);
    Check(genie.DurationMilliseconds(true) == 240.0 &&
            genie.DurationMilliseconds(false) == 240.0,
        "Dock scale matches the window transition duration");
    genie.Configure(true, 1.0);
    Check(genie.GetEffect() == Effect::Fade && genie.HiddenScale() == 1.0f &&
            genie.DurationMilliseconds(true) == kOpenDurationMs &&
            genie.DurationMilliseconds(false) == kCloseDurationMs,
        "legacy fade configuration must restore the original popup timings");
    genie.Configure(false, 1.0);
    Check(genie.GetEffect() == Effect::Scale && genie.HiddenScale() == kMinimumScale,
        "legacy scale configuration keeps its original hidden scale");

    genie.Configure(Effect::Genie, 1.0, true);
    genie.Open(1000);
    genie.Advance(1090);
    const Visual quarterOpen = genie.GetVisual();
    Check(NearlyEqual(quarterOpen.progress, 0.25f) &&
            quarterOpen.scale == 1.0f && quarterOpen.opacity == 1.0f,
        "Genie leaves geometric deformation to the renderer and becomes opaque early in opening");
    genie.Close(1090);
    Check(genie.IsClosing() &&
            NearlyEqual(genie.GetVisual().progress, quarterOpen.progress),
        "reversing Genie into close must preserve its current deformation progress");
    genie.Advance(1135);
    const Visual closing = genie.GetVisual();
    Check(NearlyEqual(closing.progress, 0.125f) &&
            closing.opacity > 0.0f && closing.opacity < 1.0f && closing.scale == 1.0f,
        "closing Genie fades only near the collapsed end");
    genie.Open(1135);
    Check(genie.IsOpening() && NearlyEqual(genie.GetVisual().progress, closing.progress) &&
            NearlyEqual(genie.GetVisual().opacity, closing.opacity),
        "reopening Genie must preserve both deformation and opacity");
    genie.Advance(1450);
    Check(!genie.IsAnimating() && genie.GetVisual().opacity == 1.0f,
        "reversed Genie completes using only its remaining duration");
    genie.Close(2000);
    genie.Advance(2360);
    Check(genie.IsHidden() && !genie.IsAnimating() && genie.GetVisual().opacity == 0.0f,
        "completed Genie close must release the visible animation state");

    genie.Open(3000);
    genie.Advance(3090);
    genie.Configure(Effect::None, 1.0);
    Check(!genie.IsAnimating() && genie.GetVisual().progress == 1.0f,
        "disabling an in-flight animation must settle its requested visible state");
    genie.Close(3100);
    Check(genie.IsHidden() && !genie.IsAnimating(),
        "None must close immediately without scheduling animation work");
    genie.Open(3200);
    Check(genie.GetVisual().opacity == 1.0f && !genie.IsAnimating(),
        "None must open immediately at full opacity");
}

void TestGenieTranslucentContentCoverage()
{
    namespace genie = snowdesktop::dock_genie;
    namespace navigation = snowdesktop::quick_navigation_animation_rules;
    const double sizes[][2] = {{481.375, 613.625}, {96.125, 47.875},
        {1281.2, 721.6}, {3840.5, 2160.25}};
    const genie::Edge edges[] = {genie::Edge::Bottom, genie::Edge::Top,
        genie::Edge::Left, genie::Edge::Right};
    for (const auto& size : sizes)
    {
        const double width = static_cast<float>(size[0]);
        const double height = static_cast<float>(size[1]);
        const genie::Rect window{-2840.25, -1200.75, -2840.25 + width, -1200.75 + height};
        const genie::Rect docks[] = {
            {-725.5, window.bottom + 200.125, -674.25, window.bottom + 242.625},
            {-725.5, window.top - 242.625, -674.25, window.top - 200.125},
            {window.left - 242.625, -325.5, window.left - 200.125, -274.25},
            {window.right + 200.125, -325.5, window.right + 242.625, -274.25}};
        for (std::size_t direction = 0; direction < std::size(edges); ++direction)
        {
            const auto edge = edges[direction];
            const bool vertical = genie::Vertical(edge);
            // Also cover a Dock on another monitor, physically opposite to
            // its configured edge. Geometry must not fold during transit.
            for (const auto& dock : {docks[direction], docks[direction ^ 1]})
            for (double collapsed : {0.0, 0.08, 0.12, 0.20, 0.33, 0.45,
                    0.55, 0.70, 0.81, 0.94, 1.0})
            {
                struct Interval { double begin, end; };
                std::vector<Interval> coverage;
                navigation::GenieStripProjection previous;
                // DComp consumes float matrices. Compare well below a pixel,
                // allowing the rounding of homogeneous coordinates.
                constexpr double tolerance = 0.002;
                for (std::size_t index = 0; index < genie::StripCount; ++index)
                {
                    const auto matrix = navigation::GenieProjection(window, dock, edge,
                        collapsed, width, height, index, -3200.25, -1500.75);
                    const auto band = navigation::GenieBandClip(matrix, index, edge);
                    // Intersect the parent clip with the child bitmap, matching
                    // the actual renderer, including its float clip coordinates.
                    const double left = std::max(0.0,
                        matrix.sourceX + static_cast<double>(static_cast<float>(band.left)));
                    const double top = std::max(0.0,
                        matrix.sourceY + static_cast<double>(static_cast<float>(band.top)));
                    const double right = std::min(width,
                        matrix.sourceX + static_cast<double>(static_cast<float>(band.right)));
                    const double bottom = std::min(height,
                        matrix.sourceY + static_cast<double>(static_cast<float>(band.bottom)));
                    const auto mapAxis = [vertical, &matrix](double x, double y) {
                        const auto point = matrix.Map(x, y);
                        return vertical ? point.y : point.x;
                    };
                    const Interval strip{mapAxis(left, top), mapAxis(right, bottom)};
                    Check(strip.end > strip.begin,
                        "each transformed Genie strip must retain positive axial coverage");
                    if (!coverage.empty())
                    {
                        Check(std::fabs(strip.begin - coverage.back().end) < tolerance,
                            "adjacent translucent Genie strips must meet without overlap or a gap");
                        for (double across : {0.0, 0.25, 0.5, 0.75, 1.0})
                        {
                            const double x = vertical ? width * across : matrix.sourceX;
                            const double y = vertical ? matrix.sourceY : height * across;
                            const auto before = previous.Map(x, y);
                            const auto after = matrix.Map(x, y);
                            Check(std::hypot(before.x - after.x, before.y - after.y) < tolerance,
                                "the entire shared edge must meet, preventing staircase sides and displaced text");
                        }
                    }
                    coverage.push_back(strip);
                    previous = matrix;
                    for (const double x : {left, right})
                    for (const double y : {top, bottom})
                    {
                        const double w = (x - matrix.sourceX) * matrix.m14 +
                            (y - matrix.sourceY) * matrix.m24 + matrix.m44;
                        const auto point = matrix.Map(x, y);
                        Check(w > 0.0 && std::isfinite(point.x) && std::isfinite(point.y),
                            "projected Genie content must stay finite and in front of the camera");
                        if (collapsed == 0.0 || collapsed == 1.0)
                        {
                            const auto& extent = collapsed == 0.0 ? window : dock;
                            const double expectedX = extent.left + 3200.25 +
                                (extent.right - extent.left) * x / width;
                            const double expectedY = extent.top + 1500.75 +
                                (extent.bottom - extent.top) * y / height;
                            Check(std::hypot(point.x - expectedX, point.y - expectedY) < tolerance,
                                "Genie endpoints must preserve the complete panel and Dock geometry");
                        }
                    }
                }
                for (std::size_t index = 1; index < coverage.size(); ++index)
                {
                    const double seam = (coverage[index - 1].end + coverage[index].begin) * 0.5;
                    // Probe either side outside float rounding uncertainty;
                    // even a small overlap of translucent strips must fail.
                    for (double offset : {-0.01, 0.01})
                    {
                        double alpha = 0.0;
                        for (const auto& strip : coverage)
                            if (seam + offset >= strip.begin && seam + offset < strip.end)
                                alpha += 0.4 * (1.0 - alpha);
                        Check(std::fabs(alpha - 0.4) < 0.00001,
                            "source-over beside a warped seam must not darken content or reveal a gap");
                    }
                }
            }
        }
    }
}

void TestDeactivateRules()
{
    Check(!rules::ShouldCloseOnDeactivate(
            true),
        "an owned context menu must keep quick navigation open");
    Check(rules::ShouldCloseOnDeactivate(
            false),
        "activation outside quick navigation must close it");

    Check(!rules::ShouldOpenFromDockSearchPress(true),
        "the Dock search press that dismissed Quick Navigation must not reopen it");
    Check(rules::ShouldOpenFromDockSearchPress(false),
        "a fresh Dock search press opens Quick Navigation");

}

void TestSearchEditKeyboardRouting()
{
    Check(
        !rules::ShouldRouteSearchEditKeyToResults(
            VK_LEFT) &&
            !rules::ShouldRouteSearchEditKeyToResults(
                VK_RIGHT) &&
            !rules::ShouldRouteSearchEditKeyToResults(
                VK_UP) &&
            !rules::ShouldRouteSearchEditKeyToResults(
                VK_DOWN),
        "arrow keys in the focused search edit must remain available for caret movement");
    Check(
        rules::ShouldRouteSearchEditKeyToResults(
            VK_RETURN),
        "Enter in the focused search edit must still activate a search result");
}

void TestAnimatedPointerHitRules()
{
    const RECT panel{ 400, 200, 1000, 800 };
    const RECT openingVisual{ 200, 600, 700, 950 };

    Check(!rules::ShouldAcceptPointerHit(
            true, POINT{ 250, 900 },
            panel, openingVisual),
        "the animated path outside the final panel must pass Dock and desktop input through");
    Check(rules::ShouldAcceptPointerHit(
            true, POINT{ 500, 700 },
            panel, openingVisual),
        "the visible intersection inside the final panel must remain interactive");
    Check(!rules::ShouldAcceptPointerHit(
            true, POINT{ 900, 300 },
            panel, openingVisual),
        "the not-yet-visible part of the final panel must not accept input");
    Check(!rules::ShouldAcceptPointerHit(
            false, POINT{ 500, 700 },
            panel, openingVisual),
        "a closed navigation surface must remain transparent");
}
}

int main()
{
    TestViewModePersistenceValues();
    TestExtendedNavigationKeyNames();
    TestViewModeFilePersistence();
    TestPinyinInitials();
    TestApplicationIconCacheIdentity();
    TestSourceOwnership();
    TestMappingSectionsFollowTabOrder();
    TestSectionLayout();
    TestAnimationRules();
    TestAnimationEffects();
    TestGenieTranslucentContentCoverage();
    TestDeactivateRules();
    TestSearchEditKeyboardRouting();
    TestAnimatedPointerHitRules();
    if (failures == 0)
    {
        std::cout
            << "quick navigation rules tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
