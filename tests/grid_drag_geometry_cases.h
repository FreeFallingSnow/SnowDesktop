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
    // Pixel-anchored icon placement retains its origin on the new grid.
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

void TestWidgetDragAnchorAcrossGrids()
{
    GridPage source;
    source.id = L"large";
    source.bounds = source.workArea = {-1200, -800, 0, 0};
    source.columns = 6;
    source.rows = 4;
    source.cellWidth = source.cellHeight = 160;
    source.marginX = source.marginY = 20;
    source.gapX = source.gapY = 40;
    GridPage target = source;
    target.id = L"small";
    target.bounds = target.workArea = {0, 0, 600, 400};
    target.cellWidth = target.cellHeight = 80;
    target.marginX = target.marginY = 10;
    target.gapX = target.gapY = 20;
    std::vector<GridPage> pages{source, target};

    // The real 2x2 source frame is 360x360 at (-980,-580). Grab at
    // (75%,25%): keeping its old (270,90) pixel offset on the small screen
    // would choose column 1 instead of 2. Expectations are measured frame
    // positions, independent of the capture/resolution implementation.
    const GridSpan span{2, 2};
    const auto anchor = CaptureGridDragAnchor({-980, -580, -620, -220}, {-710, -490});
    auto cell = ResolveGridSpanDragCell(pages, {-710, -490}, anchor, span);
    Check(cell.pageId == L"large" && cell.column == 1 && cell.row == 1,
        "widget anchor must preserve its source placement before crossing monitors");
    cell = ResolveGridSpanDragCell(pages, {345, 155}, anchor, span);
    Check(cell.pageId == L"small" && cell.column == 2 && cell.row == 1,
        "cross-screen widget must scale its grab offset to the destination frame");
    const RECT preview = GetGridRect(pages, cell, span);
    Check(preview.left == 210 && preview.top == 110 && preview.right == 390 && preview.bottom == 290,
        "resolved widget landing must render the target-sized frame beneath the same relative grab point");

    // Horizontal size is 75% of the source; vertical size is 25%.
    // A single CU/DPI scalar cannot preserve both grab coordinates.
    pages.back().bounds = pages.back().workArea = {0, 0, 900, 200};
    pages.back().cellWidth = 120;
    pages.back().cellHeight = 40;
    pages.back().marginX = 15;
    pages.back().marginY = 5;
    pages.back().gapX = 30;
    pages.back().gapY = 10;
    cell = ResolveGridSpanDragCell(pages, {518, 78}, anchor, span);
    Check(cell.pageId == L"small" && cell.column == 2 && cell.row == 1,
        "widget grab coordinates must follow horizontal and vertical scaling independently");

    // Paging can hide the source and replace the target grid during one drag.
    pages.erase(pages.begin());
    pages.back() = target;
    pages.back().id = L"dense";
    pages.back().columns = 12;
    pages.back().rows = 8;
    pages.back().cellWidth = 40;
    pages.back().cellHeight = 30;
    pages.back().marginX = 5;
    pages.back().marginY = 10;
    pages.back().gapX = 10;
    pages.back().gapY = 20;
    cell = ResolveGridSpanDragCell(pages, {273, 130}, anchor, span);
    Check(cell.pageId == L"dense" && cell.column == 4 && cell.row == 2,
        "widget paging must use the new grid without reading hidden source bounds");
    pages = {source, target};
    cell = ResolveGridSpanDragCell(pages, {-710, -490}, anchor, span);
    Check(cell.pageId == L"large" && cell.column == 1 && cell.row == 1,
        "cross-screen round trips must not accumulate grab-offset drift");

    const auto reverseAnchor = CaptureGridDragAnchor({210, 110, 390, 290}, {345, 155});
    cell = ResolveGridSpanDragCell(pages, {-710, -490}, reverseAnchor, span);
    Check(cell.pageId == L"large" && cell.column == 1 && cell.row == 1,
        "small-to-large widget drags must also scale correctly into negative monitor coordinates");
    const auto handleAnchor = CaptureGridDragAnchor({-980, -580, -620, -220}, {-800, -616});
    cell = ResolveGridSpanDragCell(pages, {300, 50}, handleAnchor, span);
    Check(cell.pageId == L"small" && cell.column == 2 && cell.row == 1,
        "a move handle above the widget must retain its signed relative offset");

    cell = ResolveGridSpanDragCell(pages, {599, 399}, anchor, span);
    Check(cell.column == 4 && cell.row == 2 && GridAreaFitsPage(target, cell, span),
        "widget anchor snapping must fit the complete span at a page edge");
    cell = ResolveGridSpanDragCell(pages, {599, 399}, anchor, {8, 6});
    const RECT clipped = GetGridRect(pages, cell, {8, 6});
    Check(cell.column == 0 && cell.row == 0 && clipped.left == 10 && clipped.top == 10 &&
        clipped.right == 590 && clipped.bottom == 390,
        "a widget larger than the destination must retain the existing page-fit preview rule");
    cell = ResolveGridSpanDragCell(pages, {900, 700}, anchor, span, &pages.back());
    Check(cell.pageId == L"small" && cell.column == 4 && cell.row == 2,
        "widget drags outside monitor bounds must retain the explicit fallback page");
    Check(ResolveGridSpanDragCell({}, {0, 0}, anchor, span).pageId.empty(),
        "widget dragging without a grid must reject placement");
}
