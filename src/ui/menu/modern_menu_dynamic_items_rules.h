#pragma once
#include "modern_menu.h"
#include <algorithm>

namespace snowdesktop::modern_menu
{
// Compare the complete physical row so adding a neighbouring button cannot
// resize or move the pointer's target even when that target's Item is unchanged.
inline bool PreservesHoveredRow(const std::vector<Item> &current,
                                const std::vector<Item> &updated, int hovered)
{
    if (hovered < 0 || static_cast<size_t>(hovered) >= current.size())
        return false;
    const auto &item = current[hovered];
    if (item.textInput)
        return false;
    size_t prefix = static_cast<size_t>(hovered) + 1;
    if (item.quickAction)
        while (prefix < current.size() && current[prefix].quickAction)
            ++prefix;
    if (item.inlineAction && item.inlineGroup)
        while (prefix < current.size() && current[prefix].inlineAction &&
               current[prefix].inlineGroup == item.inlineGroup)
            ++prefix;
    if (updated.size() < prefix ||
        !std::equal(current.begin(), current.begin() + prefix, updated.begin()))
        return false;
    if (prefix < updated.size() &&
        ((item.quickAction && updated[prefix].quickAction) ||
         (item.inlineAction && item.inlineGroup && updated[prefix].inlineAction &&
          updated[prefix].inlineGroup == item.inlineGroup)))
        return false;
    return true;
}
}
