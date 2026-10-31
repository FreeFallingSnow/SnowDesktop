#pragma once
#include "shell_extension_menu.h"
#include <algorithm>
#include <iterator>
#include <tuple>

namespace snowdesktop::shell_extensions::presentation_detail
{
template<class Item>
inline void RemoveEmptySeparators(std::vector<Item>& items)
{
    auto output = items.begin();
    for (auto input = items.begin(); input != items.end(); ++input)
    {
        if (input->separator && (output == items.begin() || std::prev(output)->separator)) continue;
        if (output != input) *output = std::move(*input);
        ++output;
    }
    if (output != items.begin() && std::prev(output)->separator) --output;
    items.erase(output, items.end());
}
// Display reconciliation may accept ownership metadata arriving later. This
// comparison must never replace ResolveCommand's exact execution reference.
template<class Left, class Right>
inline bool SameDisplayReference(const Left& left, const Right& right)
{
    return std::get<0>(left) == std::get<0>(right) &&
        std::get<1>(left) == std::get<1>(right) &&
        std::get<2>(left) == std::get<2>(right) &&
        std::get<3>(left) == std::get<3>(right) &&
        (std::get<4>(left).empty() || std::get<4>(right).empty() ||
         std::get<4>(left) == std::get<4>(right));
}
inline bool SameDisplayEntry(const Entry& left, const Entry& right)
{
    return !left.separator && !right.separator && SameDisplayReference(
        std::tie(left.provider, left.key, left.label, left.native, left.registration),
        std::tie(right.provider, right.key, right.label, right.native, right.registration));
}
inline bool SameEntry(const Entry& left, const Entry& right)
{
    return std::tie(left.provider, left.key, left.registration, left.label, left.enabled,
               left.checked, left.separator, left.native, left.width, left.height,
               left.pixels, left.accessKey) ==
        std::tie(right.provider, right.key, right.registration, right.label, right.enabled,
               right.checked, right.separator, right.native, right.width, right.height,
               right.pixels, right.accessKey) &&
        left.children.size() == right.children.size() &&
        std::equal(left.children.begin(), left.children.end(), right.children.begin(), SameEntry);
}
inline Entry MergeEntry(const Entry& previous, Entry next, bool wasPending)
{
    if (next.pixels.empty())
    {
        next.width = previous.width;
        next.height = previous.height;
        next.pixels = previous.pixels;
    }
    if (wasPending || next.native) return next;

    // Preserve already displayed children and their order while supplementing
    // the aggregate/source tree. Multiplicity is counted within each payload.
    auto children = previous.children;
    std::vector<bool> used(children.size(), false);
    bool divider = false;
    for (auto& child : next.children)
    {
        if (child.separator) { divider = true; continue; }
        size_t match = 0;
        while (match < children.size() &&
               (used[match] || !SameDisplayEntry(children[match], child))) ++match;
        if (match < children.size())
        {
            children[match] = MergeEntry(children[match], std::move(child), false);
            used[match] = true;
        }
        else
        {
            if (divider && !children.empty() && !children.back().separator)
            {
                Entry separator; separator.separator = true;
                children.push_back(std::move(separator)); used.push_back(true);
            }
            children.push_back(std::move(child)); used.push_back(true);
        }
        divider = false;
    }
    next.children = std::move(children);
    return next;
}
} // namespace snowdesktop::shell_extensions::presentation_detail
