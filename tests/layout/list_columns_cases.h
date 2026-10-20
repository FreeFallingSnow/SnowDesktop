#pragma once

#include "common/types.h"
#include "layout/list_detail_rules.h"
#include "widget/runtime/widget_item_layout.h"

namespace snowdesktop::tests::list_columns
{
template<class Check>
void Run(Check check)
{
    namespace detail = snowdesktop::list_detail_rules;
    namespace items = snowdesktop::widget_item_layout;
    DesktopWidget view;
    DockEntry dock;
    check(detail::ListColumns(view) == 1 && detail::ListColumns(dock) == 1,
        "native widgets and Dock folders retain the single-column default");
    for (int columns = 1; columns <= 4; ++columns)
    {
        detail::SetListColumns(view, columns);
        detail::SetListColumns(dock, columns);
        check(detail::ListColumns(view) == columns && detail::ListColumns(dock) == columns,
            "both display owners accept all four list column choices");
        const RECT viewport{17, 23, 537, 183};
        const auto layout = items::ResolveList(viewport, 38, {1.0f, 1.0f}, columns);
        const auto first = items::ItemRect(layout, 0);
        const auto last = items::ItemRect(layout, columns - 1);
        const auto next = items::ItemRect(layout, columns);
        check(layout.horizontal.count == columns && first.left >= viewport.left &&
                last.right <= viewport.right && last.top == first.top &&
                next.left == first.left && next.top > first.bottom,
            "multi-column list cells fill rows, stay in the viewport and then wrap");
        for (int column = 1; column < columns; ++column)
            check(items::ItemRect(layout, column - 1).right <= items::ItemRect(layout, column).left,
                "adjacent list cells and their hover targets never overlap");
        constexpr size_t count = 29;
        const int maxScroll = std::max(0, items::ContentHeight(layout, count) - 160);
        const auto final = items::ItemRect(layout, count - 1, maxScroll);
        const auto range = items::VisibleRange(layout, count, maxScroll, 160);
        check(final.bottom <= viewport.bottom && range.first <= count - 1 && range.second == count,
            "list scroll height and visible range include the final partial row");
        const auto spacious = items::ResolveList(viewport, 38, {1.0f, 5.0f}, columns);
        check(spacious.horizontal.cell == layout.horizontal.cell &&
                spacious.horizontal.count == columns,
            "list row spacing preserves column widths and capacity");
    }
    const RECT groupedRows[] = {{0, 0, 100, 38}, {110, 0, 210, 38},
        {0, 80, 100, 118}, {110, 80, 210, 118}, {220, 80, 320, 118},
        {0, 125, 100, 163}};
    const auto groupBounds = [&](size_t index) { return groupedRows[index]; };
    check(items::VerticalNeighbor(6, 1, true, groupBounds) == 3 &&
            items::VerticalNeighbor(6, 2, false, groupBounds) == 0 &&
            items::VerticalNeighbor(6, 4, true, groupBounds) == 5 &&
            items::VerticalNeighbor(6, 5, true, groupBounds) == 5,
        "arrow navigation follows visible rows across date headers and partial groups");
    view.detailShowModified = view.detailShowType = view.detailShowSize = true;
    view.listColumns = 4;
    check(detail::ListColumns(view) == 1,
        "restored metadata takes priority over an incompatible multi-column preference");
    detail::SetListColumns(view, 3);
    check(detail::ListColumns(view) == 3 && !view.detailShowModified &&
            !view.detailShowType && !view.detailShowSize,
        "selecting multiple list columns disables every metadata column");
    detail::SetListColumns(view, -9);
    detail::SetListColumns(dock, 99);
    check(view.listColumns == 1 && dock.listColumns == 4,
        "invalid stored or requested list column counts are bounded");
    check(detail::HoverTitleOffset(2000, 0, 1) == 0 &&
            detail::HoverTitleOffset(799, 64, 1) == 0 &&
            std::abs(detail::HoverTitleOffset(1800, 64, 1) - 32) < .01f &&
            detail::HoverTitleOffset(3000, 64, 1) == 64 &&
            std::abs(detail::HoverTitleOffset(4600, 64, 1) - 32) < .01f &&
            detail::HoverTitleOffset(5600, 64, 1) == 0,
        "overflow titles pause at both ends, reveal the suffix and return without jumping");
    check(std::abs(detail::HoverTitleOffset(1800, 128, 2) - 64) < .01f,
        "hover filename scrolling scales with the owning page");
}
}
