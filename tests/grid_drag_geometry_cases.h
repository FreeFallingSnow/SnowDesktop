// Production geometry used by ordinary icon and whole-widget placement.
// These checks cover coordinate selection, not desktop interaction/rendering.
void TestGridDragGeometry()
{
    GridPage source;
    source.id = L"source";
    source.bounds = source.workArea = {-600, -400, 0, 0};
    source.columns = 6;
    source.rows = 4;
    source.cellWidth = source.cellHeight = 80;
    source.marginX = source.marginY = 10;
    source.gapX = source.gapY = 20;
    GridPage target = source;
    target.id = L"target";
    target.bounds = target.workArea = {0, 0, 600, 400};
    std::vector<GridPage> pages{source, target};

    // At the monitor seam the grabbed widget extends into the old display.
    // Its landing belongs to the pointer's display even with an off-page origin.
    auto cell = ResolveGridDragCell(pages, {20, 20}, {-140, -60}, &pages.front());
    Check(cell.pageId == L"target" && cell.column == 0 && cell.row == 0,
        "cross-display widget origin must not choose the source page behind the pointer");
    cell = ResolveGridDragCell(pages, {-20, -20}, {40, 30}, &pages.back());
    Check(cell.pageId == L"source" && cell.column == 5 && cell.row == 3,
        "reverse drag into negative monitor coordinates must use the pointer page");

    // Cell starts are 10, 110, 210, ... . An origin of 180 is closer to 210,
    // despite being contained in the preceding 110..190 cell.
    cell = ResolveGridDragCell(pages, {220, 250}, {180, 170});
    Check(cell.pageId == L"target" && cell.column == 2 && cell.row == 2,
        "icon and widget preview must snap to the nearest origin instead of lagging almost a cell");
    const RECT preview = GetGridRect(pages, cell, {2, 1});
    Check(preview.left == 210 && preview.top == 210 && preview.right == 390 && preview.bottom == 290,
        "snapped multi-cell preview must keep the complete grid footprint");
    const auto sameCell = ResolveGridDragCell(pages, {240, 250}, {190, 190});
    Check(sameCell.column == 2 && sameCell.row == 2,
        "moving an origin across a cell gap must not introduce a one-cell jump");

    // A replacement page on the same display has a denser, non-square grid.
    // The pixel grab offset survives; snapping uses the newly displayed grid.
    pages.back().id = L"replacement";
    pages.back().columns = 12;
    pages.back().rows = 8;
    pages.back().cellWidth = 40;
    pages.back().cellHeight = 30;
    pages.back().marginX = 5;
    pages.back().marginY = 10;
    pages.back().gapX = 10;
    pages.back().gapY = 20;
    cell = ResolveGridDragCell(pages, {220, 250}, {180, 170});
    Check(cell.pageId == L"replacement" && cell.column == 3 && cell.row == 3,
        "page replacement must resolve the unchanged drag origin on the destination grid");
    pages.back() = target;
    cell = ResolveGridDragCell(pages, {220, 250}, {180, 170});
    Check(cell.pageId == L"target" && cell.column == 2 && cell.row == 2,
        "turning back must not accumulate a grab-offset adjustment");

    // Page-edge fitting is a separate widget rule: keep its span unchanged.
    const auto edge = ResolveGridDragCell(pages, {590, 390}, {560, 360});
    const auto fitted = ClampGridCellToFitPage(target, edge, {3, 2});
    Check(fitted.column == 3 && fitted.row == 2 && GridAreaFitsPage(target, fitted, {3, 2}),
        "widget landing at an edge must retain its span after pointer-page selection");
    Check(ResolveGridDragCell({}, {0, 0}, {0, 0}).pageId.empty(),
        "missing grid geometry must reject placement");
}
