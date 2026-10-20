#pragma once

#include "dock/dock_folder_rules.h"
#include "dock/dock_drop_rules.h"

#include <limits>

namespace snowdesktop::tests::dock_insertion
{
template <typename Check>
void Run(Check check)
{
    namespace rules = dock_folder_rules;
    const auto files = rules::GroupInsertRange(true, 3, 2);
    for (std::size_t fixed = 0; fixed < 3; ++fixed)
        for (const bool after : {false, true})
            check(rules::SlotInsertionIndex(files, fixed, after) == 3,
                "both halves of every fixed icon must place a file-section drag before the first file icon");

    check(rules::SlotInsertionIndex(files, 3, false) == 3 &&
            rules::SlotInsertionIndex(files, 3, true) == 4 &&
            rules::SlotInsertionIndex(files, 4, false) == 4 &&
            rules::SlotInsertionIndex(files, 4, true) == 5,
        "file-section hits must retain normal before/after reordering inside their own section");

    const auto fixed = rules::GroupInsertRange(false, 3, 2);
    for (const std::size_t trailing : {3u, 4u, 5u})
        for (const bool after : {false, true})
            check(rules::SlotInsertionIndex(fixed, trailing, after) == 3,
                "a fixed-section drag over trailing sections must stay at the end of the fixed section");

    for (const auto range : {rules::GroupInsertRange(true, 3, 0),
             rules::GroupInsertRange(false, 0, 2)})
        for (const std::size_t slot : {std::size_t{0}, std::size_t{3},
                 std::numeric_limits<std::size_t>::max()})
            for (const bool after : {false, true})
                check(rules::SlotInsertionIndex(range, slot, after) == range.begin,
                    "an empty section must keep its sole insertion boundary without index overflow");

    const RECT horizontal{100, 200, 500, 280};
    const RECT search{400, 200, 476, 280};
    const RECT hiddenSearch{476, 200, 476, 280};
    for (const auto anchor : {search, hiddenSearch})
    {
        const RECT indicator = dock_drop_rules::RecycleBinInsertionIndicatorBounds(
            horizontal, anchor, false);
        check(indicator.left == 474 && indicator.right == 478 &&
                indicator.top == 210 && indicator.bottom == 270,
            "the Recycle Bin's blue insertion bar must stay immediately after Search or its hidden boundary");
    }
    const RECT vertical{100, 200, 180, 600};
    const RECT searchVertical{100, 500, 180, 576};
    const RECT hiddenSearchVertical{100, 576, 180, 576};
    for (const auto anchor : {searchVertical, hiddenSearchVertical})
    {
        const RECT indicator = dock_drop_rules::RecycleBinInsertionIndicatorBounds(
            vertical, anchor, true);
        check(indicator.left == 110 && indicator.right == 170 &&
                indicator.top == 574 && indicator.bottom == 578,
            "a vertical Dock must place the Recycle Bin's blue bar below Search or its hidden boundary");
    }
}
} // namespace snowdesktop::tests::dock_insertion
