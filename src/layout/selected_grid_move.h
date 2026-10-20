#pragma once

#include "common/types.h"
#include <algorithm>
#include <climits>
#include <vector>
#include <utility>

namespace snowdesktop::selected_grid_move
{
struct Move { size_t index; GridCell cell; };

// Plan the complete selection before callers mutate or persist any cell.
// Occupied excludes the selected source items, but includes other items/widgets.
template<class Occupied>
std::vector<Move> Build(const std::vector<DesktopItem>& items,
    const std::vector<GridPage>& pages, GridCell target, Occupied occupied)
{
    std::vector<size_t> selected;
    int left = INT_MAX, top = INT_MAX, right = 0, bottom = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].selected)
        {
            if (items[i].gridSpan.columns < 1 || items[i].gridSpan.rows < 1) return {};
            selected.push_back(i);
            left = std::min(left, items[i].gridCell.column);
            top = std::min(top, items[i].gridCell.row);
            right = std::max(right, items[i].gridCell.column + items[i].gridSpan.columns);
            bottom = std::max(bottom, items[i].gridCell.row + items[i].gridSpan.rows);
        }
    if (selected.empty()) return {};
    const auto page = std::find_if(pages.begin(), pages.end(),
        [&](const auto& value) { return value.id == target.pageId; });
    if (page == pages.end() || page->columns < 1 || page->rows < 1) return {};
    const int width = std::max(1, right - left), height = std::max(1, bottom - top);
    const bool stacked = width == 1 && height == 1 && selected.size() > 1;
    const int spread = stacked ? std::min(static_cast<int>(selected.size()), page->columns) : width;
    const bool large = std::any_of(selected.begin(), selected.end(),
        [&](size_t index) { return items[index].largeIcon.has_value(); });
    if (large && (target.column < 0 || target.row < 0 ||
        target.column + spread > page->columns || target.row + height > page->rows)) return {};
    target.column = std::clamp(target.column, 0, std::max(0, page->columns - spread));
    target.row = std::clamp(target.row, 0, std::max(0, page->rows - height));
    std::vector<Move> moves;
    const auto fits = [&](const GridCell& cell, GridSpan span) {
        if (cell.column < 0 || cell.row < 0 || span.columns > page->columns - cell.column ||
            span.rows > page->rows - cell.row || occupied(cell, span)) return false;
        return std::none_of(moves.begin(), moves.end(), [&](const auto& move) {
            const auto other = items[move.index].gridSpan;
            return cell.column < move.cell.column + other.columns &&
                cell.column + span.columns > move.cell.column &&
                cell.row < move.cell.row + other.rows && cell.row + span.rows > move.cell.row;
        });
    };
    int sequence = 0;
    for (const auto index : selected)
    {
        GridCell cell = target;
        if (stacked)
        {
            bool found = false;
            while (sequence < page->columns * page->rows)
            {
                GridCell probe = target;
                probe.column += sequence / page->rows;
                probe.row += sequence % page->rows;
                ++sequence;
                if (!fits(probe, items[index].gridSpan)) continue;
                cell = std::move(probe);
                found = true;
                break;
            }
            if (!found) return {};
        }
        else
        {
            cell.column += items[index].gridCell.column - left;
            cell.row += items[index].gridCell.row - top;
            if (!fits(cell, items[index].gridSpan)) return {};
        }
        moves.push_back({index, std::move(cell)});
    }
    return moves;
}
}
