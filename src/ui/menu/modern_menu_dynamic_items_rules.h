#pragma once
#include "modern_menu.h"
#include <algorithm>
#include <tuple>

namespace snowdesktop::modern_menu
{
template<class Before, class After>
inline bool ApplyPrefetchedChildren(std::vector<Item> &current, const std::vector<Item> &updated,
                                    bool safe, Before before, After after)
{
    if (!safe) return false;
    bool changed = false;
    for (auto &item : current)
    {
        const auto next = std::find_if(updated.begin(), updated.end(), [&](const auto &candidate) {
            return item.command && candidate.command == item.command && candidate.label == item.label;
        });
        if (next == updated.end()) continue;
        // Image content does not move the row or change its command target.
        // Correct interim artwork even while its loaded cascade stays open.
        if (next->image && item.image != next->image)
        {
            item.image = next->image;
            changed = true;
        }
        if (item.childrenPending && !next->childrenPending)
        {
            before(item.children);
            item.children = next->children;
            item.childrenPending = false;
            item.enabled = next->enabled;
            item.checked = next->checked;
            after(item.children);
            changed = true;
        }
        else if (!item.childrenPending)
            changed |= ApplyPrefetchedChildren(item.children, next->children, true, before, after);
    }
    return changed;
}
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
