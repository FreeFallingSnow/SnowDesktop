#include "desktop_drop_search.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

GridCell Cell(const wchar_t* page, int column, int row)
{
    return { page, column, row };
}
}

int main()
{
    using namespace snowdesktop::desktop_drop_cache;

    const SearchDirection right =
        ResolveSearchDirection(20, 5);
    Check(right == ResolveSearchDirection(200, 50) &&
            right == ResolveSearchDirection(10, 10) &&
            right == ResolveSearchDirection(0, 0) &&
            right == SearchDirection{1, 0},
        "same horizontal direction, ties, and a stationary pointer resolve right");
    Check(ResolveSearchDirection(-20, 5) ==
            SearchDirection{-1, 0} &&
            ResolveSearchDirection(5, 20) ==
            SearchDirection{0, 1} &&
            ResolveSearchDirection(5, -20) ==
            SearchDirection{0, -1},
        "left, down, and up resolve to distinct search directions");

    BestCellEntry entry;
    const BestCellKey base{
        Cell(L"page-a", 3, 4), right, 17 };
    const GridCell best = Cell(L"page-a", 5, 4);
    GridCell result;
    Check(!entry.TryGet(true, base, result),
        "an empty cache misses");
    entry.Store(true, base, best);
    Check(entry.TryGet(true, base, result) &&
            SameCell(result, best),
        "an identical active-drag key reuses its best cell");
    Check(!entry.TryGet(false, base, result),
        "an inactive drag never reuses an interaction cache entry");

    BestCellKey changed = base;
    changed.direction = {0, 1};
    Check(!entry.TryGet(true, changed, result),
        "changing the primary search direction invalidates the entry");
    changed = base;
    changed.requested.pageId = L"page-b";
    Check(!entry.TryGet(true, changed, result),
        "changing the requested page invalidates the entry");
    changed = base;
    ++changed.requested.column;
    Check(!entry.TryGet(true, changed, result),
        "changing the requested column invalidates the entry");
    changed = base;
    ++changed.requested.row;
    Check(!entry.TryGet(true, changed, result),
        "changing the requested row invalidates the entry");
    changed = base;
    ++changed.staticSceneRevision;
    Check(!entry.TryGet(true, changed, result),
        "changing the drag static-scene revision invalidates the entry");

    entry.Store(true, base, base.requested);
    Check(entry.TryGet(true, base, result) &&
            SameCell(result, base.requested),
        "a no-alternative fallback result is cached like a successful search");
    entry.Clear();
    Check(!entry.TryGet(true, base, result),
        "clearing the entry forces the next search to run");

    entry.Store(false, base, best);
    Check(!entry.TryGet(true, base, result),
        "inactive callers cannot populate the interaction cache");

    // Exercise the production search against an occupied grid. A rejected
    // large-icon landing must remain rejected even when an adjacent area fits.
    BestCellEntry searchCache;
    const BestCellKey occupied{ Cell(L"desktop", 3, 3), right, 20 };
    int probes = 0;
    const auto fits = [&](const GridCell& cell, GridSpan span) {
        ++probes;
        if (cell.pageId != L"desktop" || cell.column < 0 || cell.row < 0 ||
            cell.column + span.columns > 6 || cell.row + span.rows > 6) return false;
        return !(cell.column <= 3 && cell.column + span.columns > 3 &&
            cell.row <= 3 && cell.row + span.rows > 3);
    };
    const auto ordinaryFits = [&](const GridCell& cell) { return fits(cell, { 1, 1 }); };
    const auto largeFits = [&](const GridCell& cell) { return fits(cell, { 2, 2 }); };
    result = FindBestCell(searchCache, true, occupied, false, 6, 6, ordinaryFits);
    Check(SameCell(result, Cell(L"desktop", 4, 3)) && ordinaryFits(result),
        "ordinary icons still search forward for a free neighbouring slot");
    probes = 0;
    result = FindBestCell(searchCache, true, occupied, false, 6, 6, ordinaryFits);
    Check(SameCell(result, Cell(L"desktop", 4, 3)) && probes == 0,
        "ordinary placement still reuses its cached neighbour");
    result = FindBestCell(searchCache, true, occupied, true, 6, 6, largeFits);
    Check(SameCell(result, occupied.requested) && !largeFits(result) &&
            largeFits(Cell(L"desktop", 4, 3)),
        "an occupied large-icon request cannot reuse a cached ordinary neighbour");
    result = FindBestCell(searchCache, false, occupied, true, 6, 6, ordinaryFits);
    Check(SameCell(result, occupied.requested) && !ordinaryFits(result),
        "a 1x1 large icon also rejects occupancy after interactive drag ends");

    const BestCellKey edge{ Cell(L"desktop", 5, 0), right, 21 };
    result = FindBestCell(searchCache, false, edge, true, 6, 6, largeFits);
    Check(SameCell(result, edge.requested) && !largeFits(result) &&
            largeFits(Cell(L"desktop", 4, 0)),
        "a large span crossing the page edge is not shifted into a nearby fitting area");
    const BestCellKey legal{ Cell(L"desktop", 1, 0), right, 22 };
    result = FindBestCell(searchCache, false, legal, true, 6, 6, largeFits);
    Check(SameCell(result, legal.requested) && largeFits(result),
        "a legal large-icon landing keeps the requested anchor and full span");
    const auto mixedGroupFits = [&](const GridCell& cell) {
        return fits(cell, { 4, 2 });
    };
    const BestCellKey mixedEdge{ Cell(L"desktop", 3, 0), right, 23 };
    result = FindBestCell(searchCache, false, mixedEdge, true, 6, 6, mixedGroupFits);
    Check(SameCell(result, mixedEdge.requested) && !mixedGroupFits(result) &&
            mixedGroupFits(Cell(L"desktop", 2, 0)),
        "a mixed group containing a large icon is not shifted to fit its full extent");
    const BestCellKey otherPage{ Cell(L"small-page", 1, 0), right, 24 };
    const auto fitsSmallPage = [&](const GridCell& cell) {
        return cell.pageId == L"small-page" && cell.column == 0 && cell.row == 0;
    };
    result = FindBestCell(searchCache, false, otherPage, true, 2, 2, fitsSmallPage);
    Check(SameCell(result, otherPage.requested) && !fitsSmallPage(result) &&
            fitsSmallPage(Cell(L"small-page", 0, 0)),
        "cross-page large-icon placement does not relocate an invalid request");

    // Preserve the ordinary opposite-direction and ring fallbacks.
    result = FindBestCell(searchCache, false, edge, false, 6, 6,
        [&](const GridCell& cell) { return SameCell(cell, Cell(L"desktop", 4, 0)); });
    Check(SameCell(result, Cell(L"desktop", 4, 0)),
        "ordinary search checks the opposite direction at a page edge");
    result = FindBestCell(searchCache, false, occupied, false, 6, 6,
        [&](const GridCell& cell) { return SameCell(cell, Cell(L"desktop", 3, 2)); });
    Check(SameCell(result, Cell(L"desktop", 3, 2)),
        "ordinary search reaches a ring fallback when both primary directions are blocked");

    std::cout << "All desktop drop cache tests passed.\n";
    return 0;
}
