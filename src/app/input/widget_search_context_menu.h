#pragma once

#include "core/widget.h"
#include "core/slot.h"
#include "common/utils.h"
#include <optional>
#include <unordered_set>

namespace snowdesktop::widget_search_context_menu
{
template<class Containers>
bool IsSearchResultAtPoint(const Containers& containers, POINT point)
{
    for (const auto& container : containers)
    {
        auto* view = dynamic_cast<ScrollingItemWidget*>(container.get());
        if (!view || !view->IsSearchActive()) continue;
        const RECT viewport = view->GetContentViewportRect();
        if (!PtInRect(&viewport, point)) continue;
        for (const auto& slot : view->GetSlots())
        {
            const RECT bounds = slot->GetBounds();
            if (slot->GetItem() && PtInRect(&bounds, point)) return true;
        }
    }
    return false;
}

inline bool NeedsPathMenu(ScrollingItemWidget& view, const Item& hit)
{
    if (hit.IsSearchResult()) return true;
    if (!hit.IsSelected()) return false;
    for (const auto* item : view.GetSelectedItems())
        if (item && item->IsSearchResult()) return true;
    return false;
}
// Search rows have presentation indices, not persistent folder-member indices.
// Own the selected paths before any native menu pumps messages/rebuilds views.
template<class Clear, class Preserve, class Eligible>
std::optional<std::vector<std::wstring>> Prepare(ScrollingItemWidget& view,
    POINT point, Clear clearSelection, Preserve preserveWidgetSelection, Eligible eligible)
{
    if (!view.UsesFolderSearch()) return std::nullopt;
    const RECT viewport = view.GetContentViewportRect();
    if (!PtInRect(&viewport, point)) return std::nullopt;
    size_t index = static_cast<size_t>(-1);
    std::wstring path;
    bool selected = false;
    for (const auto& slot : view.GetSlots())
    {
        const RECT bounds = slot->GetBounds();
        auto* item = slot->GetItem();
        if (!PtInRect(&bounds, point) || !item || !eligible(item)) continue;
        if (!NeedsPathMenu(view, *item)) return std::nullopt;
        index = slot->GetIndex(); path = item->GetPath(); selected = item->IsSelected(); break;
    }
    if (path.empty()) return std::nullopt;
    if (!selected)
    {
        clearSelection();
        // Selection clearing can observe a completed search. Never substitute
        // another file that has since occupied the same presentation index.
        auto* item = view.GetMemberItem(index);
        if (!item || item->GetPath() != path)
        {
            item = nullptr;
            for (size_t i = 0; i < view.GetSlotCount(); ++i)
            {
                auto* candidate = view.GetMemberItem(i);
                if (candidate && candidate->GetPath() == path) { item = candidate; break; }
            }
        }
        if (!item || !eligible(item)) return std::nullopt;
        item->SetSelected(true);
    }
    preserveWidgetSelection();
    std::vector<std::wstring> paths;
    std::unordered_set<std::wstring> seen;
    for (auto* item : view.GetSelectedItems())
    {
        if (!item || !eligible(item)) continue;
        auto selectedPath = item->GetPath();
        if (!selectedPath.empty() && seen.insert(ToUpperInvariant(selectedPath)).second)
            paths.push_back(std::move(selectedPath));
    }
    return paths.empty() ? std::nullopt : std::make_optional(std::move(paths));
}
}
