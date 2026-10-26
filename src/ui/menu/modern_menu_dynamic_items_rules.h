#pragma once
#include "modern_menu.h"
#include <algorithm>
#include <tuple>

namespace snowdesktop::modern_menu
{
inline bool SameRootRow(const Item &left, const Item &right)
{
    // Closed cascades do not affect the parent's hit geometry. Keep the
    // submenu affordance, but allow its contents/tokens to finish loading.
    const auto row = [](const Item &i) {
        return std::tie(i.command, i.label, i.glyph, i.enabled, i.checked, i.separator,
            i.iconFont, i.quickAction, i.quickIcon, i.inlineAction, i.inlineGroup,
            i.compactInlineAction, i.horizontalScrollAction, i.textInput, i.inputText,
            i.image, i.accessKey, i.measureInlineAction, i.builtinIcon, i.fitInlineActionToContent);
    };
    return left.children.empty() == right.children.empty() && row(left) == row(right);
}
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
        !std::equal(current.begin(), current.begin() + prefix, updated.begin(), SameRootRow))
        return false;
    if (prefix < updated.size() &&
        ((item.quickAction && updated[prefix].quickAction) ||
         (item.inlineAction && item.inlineGroup && updated[prefix].inlineAction &&
          updated[prefix].inlineGroup == item.inlineGroup)))
        return false;
    return true;
}
}
