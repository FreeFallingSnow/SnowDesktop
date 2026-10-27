#include "dock/dock_work_area_reservation.h"
#include "layout/grid_spacing_rules.h"
#include "layout/item_visual_metrics.h"
#include "system/status_bar/status_bar_appbar.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
namespace reservation = snowdesktop::dock_work_area_reservation;
using reservation::Inputs;
int assertions = 0;
bool uncachedControl = false;
void Check(bool condition, const char* message)
{
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

bool Vertical(DockPosition position)
{
    return position == DockPosition::Left || position == DockPosition::Right;
}
UINT Edge(DockPosition position)
{
    return position == DockPosition::Left ? ABE_LEFT : position == DockPosition::Right ? ABE_RIGHT :
        position == DockPosition::Top ? ABE_TOP : ABE_BOTTOM;
}

// The oracle below retains the old candidate copy, best-page copy and complete
// ascending scan. Only the Shell boundary is supplied as geometry; use the real
// visual sizing, spacing, icon, component-margin and AppBar geometry rules.
RECT Reserve(GridPage& page, RECT original, DockPosition position, int reserved)
{
    page.workArea = original;
    RECT area = original;
    switch (position)
    {
    case DockPosition::Left: area.right = original.left + reserved; page.workArea.left = area.right; break;
    case DockPosition::Right: area.left = original.right - reserved; page.workArea.right = area.left; break;
    case DockPosition::Top: area.bottom = original.top + reserved; page.workArea.top = area.bottom; break;
    default: area.top = original.bottom - reserved; page.workArea.bottom = area.top; break;
    }
    return area;
}
void Spacing(GridPage& page, const Inputs& inputs)
{
    page.columns = std::max(1, page.columns); page.rows = std::max(1, page.rows);
    const RECT visual = IsRectEmpty(&page.visualWorkArea) ? page.workArea : page.visualWorkArea;
    const auto sizing = snowdesktop::ResolvePageVisualSizing(
        std::max(1L, visual.right - visual.left), std::max(1L, visual.bottom - visual.top),
        page.columns, page.rows);
    page.itemPitchWidth = sizing.pitchWidth; page.itemPitchHeight = sizing.pitchHeight;
    const auto metrics = snowdesktop::ResolvePageItemVisualMetrics(page.itemPitchWidth, page.itemPitchHeight,
        inputs.itemFontSizeCu, inputs.itemIconSizeScale);
    const auto horizontal = snowdesktop::grid_spacing_rules::ResolveAxis(
        std::max(1L, page.workArea.right - page.workArea.left), page.columns,
        sizing.marginX, kGapPercentX, metrics.minimumGridWidth, inputs.iconSpacingScale);
    const auto vertical = snowdesktop::grid_spacing_rules::ResolveAxis(
        std::max(1L, page.workArea.bottom - page.workArea.top), page.rows,
        sizing.marginY, kGapPercentY, metrics.minimumGridHeight, inputs.iconSpacingScale);
    page.marginX = horizontal.margin; page.cellWidth = horizontal.cell; page.gapX = horizontal.gap;
    page.marginY = vertical.margin; page.cellHeight = vertical.cell; page.gapY = vertical.gap;
}
int Desired(const GridPage& page, const Inputs& inputs)
{
    GridPage sizing = page;
    sizing.id = inputs.sizingPage.id;
    sizing.columns = inputs.sizingPage.columns; sizing.rows = inputs.sizingPage.rows;
    Spacing(sizing, inputs);
    const auto metrics = snowdesktop::ResolvePageItemVisualMetrics(sizing.itemPitchWidth, sizing.itemPitchHeight,
        inputs.itemFontSizeCu, inputs.itemIconSizeScale);
    const bool vertical = Vertical(inputs.settings.position);
    const int margin = ScaleDockEdgeDistance(snowdesktop::layout_spacing_rules::ComponentEdgeMargin(
        vertical ? sizing.marginX : sizing.marginY, vertical ? sizing.gapX : sizing.gapY,
        metrics.layoutScale, inputs.iconSpacingScale), inputs.settings.edgeDistanceScale);
    const float scale = ClampDockScale(inputs.settings.thicknessScale);
    const int icon = std::max(1, static_cast<int>(std::round(metrics.iconSize * scale)));
    const int gap = std::max(1, static_cast<int>(std::round(kDockSpacing * scale)));
    return icon + gap * 2 + (inputs.settings.edgeAttached ? 0 : margin);
}
struct Result { GridPage page; RECT area; int reserved; int evaluations; };
Result Original(const Inputs& inputs)
{
    GridPage bestPage = inputs.page;
    int bestReserved = 1, bestError = INT_MAX, evaluations = 0;
    for (int reserved = 1; reserved < inputs.edgeExtent; ++reserved)
    {
        GridPage candidate = inputs.page;
        Reserve(candidate, inputs.page.workArea, inputs.settings.position, reserved);
        Spacing(candidate, inputs);
        const int error = std::abs(Desired(candidate, inputs) - reserved);
        ++evaluations;
        if (error < bestError)
        {
            bestError = error; bestReserved = reserved; bestPage = candidate;
            if (error == 0) break;
        }
    }
    const RECT area = Reserve(bestPage, inputs.page.workArea, inputs.settings.position, bestReserved);
    Spacing(bestPage, inputs);
    return {bestPage, area, bestReserved, evaluations};
}
Result Cached(reservation::Cache& cache, const Inputs& inputs)
{
    int evaluations = 0;
    const auto desired = [&](int trial) {
        GridPage candidate = inputs.page;
        Reserve(candidate, inputs.page.workArea, inputs.settings.position, trial);
        Spacing(candidate, inputs); ++evaluations;
        return Desired(candidate, inputs);
    };
    const int reserved = uncachedControl ? reservation::Solve(inputs.edgeExtent, desired) :
        cache.Resolve(inputs, desired);
    GridPage page = inputs.page;
    const RECT area = Reserve(page, inputs.page.workArea, inputs.settings.position, reserved);
    Spacing(page, inputs);
    return {page, area, reserved, evaluations};
}
void Same(const Result& left, const Result& right)
{
    Check(left.reserved == right.reserved && EqualRect(&left.area, &right.area) &&
        reservation::SamePage(left.page, right.page), "cached output must equal original exhaustive page and Dock geometry");
}
Inputs Make(int width = 1920, int height = 1080, DockPosition position = DockPosition::Bottom)
{
    Inputs inputs;
    auto& page = inputs.page;
    page.id = L"page-2"; page.monitorId = L"monitor-secondary";
    page.bounds = {-width, -40, 0, height - 40}; page.workArea = page.bounds;
    page.workArea.top += 31; page.workArea.bottom -= 43;
    page.visualWorkArea = page.workArea; page.columns = 18; page.rows = 8;
    page.dpiX = 144; page.dpiY = 192;
    inputs.sizingPage = page;
    inputs.settings.position = position;
    inputs.edgeExtent = static_cast<int>(std::max(1L, Vertical(position) ?
        page.workArea.right - page.workArea.left : page.workArea.bottom - page.workArea.top));
    return inputs;
}
void Selection()
{
    int calls = 0;
    Check(reservation::Solve(8, [&](int pixel) { ++calls; return pixel < 3 ? 7 : pixel + 1; }) == 3 && calls == 7,
        "without an exact solution the first equally good width wins");
    calls = 0;
    Check(reservation::Solve(8, [&](int) { ++calls; return 4; }) == 4 && calls == 4,
        "the first exact width stops the scan");
    calls = 0;
    Check(reservation::Solve(1, [&](int) { ++calls; return 9; }) == 1 && calls == 0,
        "a one-pixel edge preserves the original empty-range fallback");
    Check(reservation::Solve(8, [](int) { return 100; }) == 7,
        "an oversized Dock chooses the original end-of-range minimum");
}
void Equivalence()
{
    int noExact = 0;
    for (const auto size : {SIZE{1920, 1080}, SIZE{2560, 1600}, SIZE{3840, 2160}, SIZE{97, 153}, SIZE{1, 1}})
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
            for (const float spacing : {.1f, 1.f, 2.f})
                for (const UINT dpi : {96u, 192u})
                for (int mode = 0; mode < 4; ++mode)
                {
                    Inputs inputs = Make(size.cx, size.cy, position);
                    inputs.page.dpiX = inputs.sizingPage.dpiX = dpi;
                    inputs.page.dpiY = inputs.sizingPage.dpiY = dpi == 96 ? 120u : 240u;
                    inputs.iconSpacingScale = spacing;
                    inputs.itemFontSizeCu = mode % 2 ? 24.f : 10.f;
                    inputs.itemIconSizeScale = mode % 2 ? 1.2f : .5f;
                    inputs.settings.thicknessScale = mode % 2 ? 2.f : .5f;
                    inputs.settings.edgeDistanceScale = mode % 2 ? 5.f : .1f;
                    inputs.settings.edgeAttached = mode >= 2;
                    if (mode == 1) inputs.page.visualWorkArea = inputs.sizingPage.visualWorkArea = {};
                    if (mode == 2) { inputs.sizingPage.id = L"home"; inputs.sizingPage.columns = 27; inputs.sizingPage.rows = 12; }
                    if (mode == 3) { inputs.page.columns = 0; inputs.page.rows = 0; inputs.sizingPage = inputs.page; }
                    reservation::Cache cache;
                    const auto expected = Original(inputs);
                    const auto cold = Cached(cache, inputs), warm = Cached(cache, inputs);
                    Same(expected, cold); Same(expected, warm);
                    Check(cold.evaluations == expected.evaluations && warm.evaluations == 0,
                        "a cold solve retains old evaluation order and a warm solve performs no trial evaluation");
                    noExact += Desired(expected.page, inputs) != expected.reserved;

                    // An approved Shell strip is consumed after scalar lookup;
                    // changing approval must never be cached with the solution.
                    RECT approved = expected.area;
                    if (Vertical(position)) OffsetRect(&approved, 3, 0); else OffsetRect(&approved, 0, 3);
                    auto approvedCold = cold.page, approvedWarm = warm.page;
                    approvedCold.workArea = snowdesktop::ConstrainStatusBarWorkArea(inputs.page.workArea, approved, Edge(position));
                    approvedWarm.workArea = snowdesktop::ConstrainStatusBarWorkArea(inputs.page.workArea, approved, Edge(position));
                    Spacing(approvedCold, inputs); Spacing(approvedWarm, inputs);
                    Check(reservation::SamePage(approvedCold, approvedWarm), "live approved AppBar geometry is reapplied equally after cache lookup");
                }
    Check(noExact > 0, "equivalence matrix includes scans with no exact solution or a minimal edge");
}
void Invalidation()
{
    const Inputs baseline = Make();
    const auto changed = [&](auto mutate) {
        reservation::Cache cache;
        const auto expected = Original(baseline);
        Same(expected, Cached(cache, baseline));
        Inputs next = baseline; mutate(next);
        const auto actual = Cached(cache, next);
        Check(actual.evaluations > 0, "changed geometry/settings/home input must force a fresh solve");
        Same(Original(next), actual);
        Same(expected, Cached(cache, baseline));
    };
    changed([](auto& i) { i.page.monitorId = L"replacement-monitor"; });
    changed([](auto& i) { i.page.id = L"new-page"; });
    changed([](auto& i) { i.page.bounds.left -= 100; });
    changed([](auto& i) { i.page.workArea.top += 53; });
    changed([](auto& i) { i.page.visualWorkArea = {}; });
    changed([](auto& i) { i.page.dpiX = 192; });
    changed([](auto& i) { i.page.dpiY = 120; });
    changed([](auto& i) { i.page.columns = 7; i.sizingPage.columns = 7; });
    changed([](auto& i) { i.page.rows = 16; i.sizingPage.rows = 16; });
    changed([](auto& i) { i.sizingPage.id = L"other-home"; });
    changed([](auto& i) { i.sizingPage.columns = 29; });
    changed([](auto& i) { i.sizingPage.rows = 3; });
    changed([](auto& i) { i.settings.position = DockPosition::Top; });
    changed([](auto& i) { i.settings.edgeAttached = true; });
    changed([](auto& i) { i.settings.thicknessScale = 2.f; });
    changed([](auto& i) { i.settings.edgeDistanceScale = 5.f; });
    changed([](auto& i) { i.settings.monitorScope = DockMonitorScope::All; });
    changed([](auto& i) { i.settings.lastMonitorUseHomeSize = false; });
    changed([](auto& i) { i.settings.reserveScreenSpace = true; });
    changed([](auto& i) { i.settings.allowDesktopContentOverlap = true; });
    changed([](auto& i) { i.settings.showOnlyWhenSummoned = true; });
    changed([](auto& i) { i.iconSpacingScale = .1f; });
    changed([](auto& i) { i.itemFontSizeCu = 24.f; });
    changed([](auto& i) { i.itemIconSizeScale = 1.2f; });
    changed([](auto& i) { --i.edgeExtent; });

    reservation::Cache cache;
    Same(Original(baseline), Cached(cache, baseline));
    for (std::size_t index = 0; index < reservation::Cache::kCapacity; ++index)
    {
        Inputs another = baseline; another.page.monitorId += std::to_wstring(index);
        Same(Original(another), Cached(cache, another));
    }
    Check(Cached(cache, baseline).evaluations > 0, "capacity eviction recomputes instead of retaining unbounded monitor/page history");

    Inputs invalid = baseline;
    invalid.iconSpacingScale = std::numeric_limits<float>::quiet_NaN();
    Same(Original(invalid), Cached(cache, invalid));
    Check(Cached(cache, invalid).evaluations > 0, "non-equal NaN settings remain uncached and use existing spacing fallback");
}
}

int main(int argc, char** argv)
{
    try
    {
        uncachedControl = argc == 2 && std::string(argv[1]) == "--uncached-control";
        Selection(); Equivalence(); Invalidation();
        std::cout << "Dock reservation: " << assertions << " assertions passed; cold/warm geometry equivalence and invalidation verified\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
