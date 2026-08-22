#include "desktop_drop_cache.h"

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

    std::cout << "All desktop drop cache tests passed.\n";
    return 0;
}
