#pragma once

#include "core/drop_model.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace snowdesktop::desktop_icon_drop_order
{
inline bool AppliesTo(const DragSourceList& sources)
{
    return sources.entries.size() > 1 && sources.hasOriginSurface &&
        sources.SourceSurfaceKind() == slot_contract::SlotSurfaceKind::Desktop &&
        std::all_of(sources.entries.begin(), sources.entries.end(), [](const DragSourceEntry& entry) {
            return entry.kind == DropSourceKind::DesktopIcon && !entry.fromDock &&
                !entry.originalCell.pageId.empty() &&
                entry.originalCell.column >= 0 && entry.originalCell.row >= 0;
        });
}

inline std::vector<const DragSourceEntry*> WidgetInsertionSources(
    const DragSourceList& sources, const std::vector<std::wstring>& pageOrder)
{
    std::vector<const DragSourceEntry*> ordered;
    ordered.reserve(sources.entries.size());
    if (!AppliesTo(sources))
    {
        for (const auto& entry : sources.entries) ordered.push_back(&entry);
        return ordered;
    }

    struct Page
    {
        std::wstring_view id;
        size_t rank;
        std::vector<const DragSourceEntry*> entries;
    };
    std::vector<Page> pages;
    for (const auto& entry : sources.entries)
    {
        const auto& id = entry.originalCell.pageId;
        auto page = std::ranges::find(pages, std::wstring_view{id}, &Page::id);
        if (page == pages.end())
        {
            const auto position = std::find(pageOrder.begin(), pageOrder.end(), id);
            pages.push_back({id, static_cast<size_t>(position - pageOrder.begin()), {}});
            page = std::prev(pages.end());
        }
        page->entries.push_back(&entry);
    }
    // Unlisted pages share the last rank and keep their captured page order.
    std::ranges::stable_sort(pages, {}, &Page::rank);
    for (auto& page : pages)
    {
        const auto columns = std::ranges::minmax_element(page.entries, {}, [](const auto* entry) {
            return entry->originalCell.column;
        });
        const auto rows = std::ranges::minmax_element(page.entries, {}, [](const auto* entry) {
            return entry->originalCell.row;
        });
        const bool horizontal =
            (*columns.max)->originalCell.column - (*columns.min)->originalCell.column >
            (*rows.max)->originalCell.row - (*rows.min)->originalCell.row;
        std::ranges::stable_sort(page.entries, [horizontal](const auto* a, const auto* b) {
            const auto& first = a->originalCell;
            const auto& second = b->originalCell;
            return horizontal
                ? std::tie(first.row, first.column) < std::tie(second.row, second.column)
                : std::tie(first.column, first.row) < std::tie(second.column, second.row);
        });
        ordered.insert(ordered.end(), page.entries.begin(), page.entries.end());
    }
    return ordered;
}

// Folder refresh uses the same planned order as keyed-widget insertion.
inline std::vector<const DragSourceEntry*> FolderInsertionSources(
    const DragSourceList& sources, const DropPreviewList& preview)
{
    if (!AppliesTo(sources) || sources.entries.size() != preview.landings.size()) return {};
    std::unordered_map<size_t, const DragSourceEntry*> byIndex;
    byIndex.reserve(sources.entries.size());
    for (const auto& entry : sources.entries) byIndex.emplace(entry.sourceIndex, &entry);

    std::vector<const DragSourceEntry*> ordered;
    ordered.reserve(preview.landings.size());
    for (const auto& landing : preview.landings)
    {
        auto match = byIndex.extract(landing.sourceIndex);
        if (match.empty()) return {};
        ordered.push_back(match.mapped());
    }
    return ordered;
}
}
