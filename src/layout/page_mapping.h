#pragma once

#include "common/constants.h"
#include "common/types.h"
#include "page_management_rules.h"
#include "page_navigation_rules.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace snowdesktop::page_mapping
{
struct State
{
    std::vector<std::wstring>& ids;
    std::unordered_map<std::wstring, int>& columns;
    std::unordered_map<std::wstring, int>& rows;
    std::unordered_map<std::wstring, std::wstring>& names;
    std::unordered_map<std::wstring, LayoutRecord>& records;
    std::vector<DesktopWidget>& widgets;
    std::vector<DesktopItem>& items;
    std::vector<GridPage>& pages;
    int& offset;
    std::wstring& lastPage;
    std::uint64_t& revision;
};
struct Services
{
    std::function<bool()> ready;
    std::function<bool(const std::wstring&)> hasContent;
    std::function<std::vector<size_t>()> monitorOrder;
    std::function<const GridPage*()> firstPage;
    std::function<void()> cancelHover, refreshOwnership, reserveDock, refreshIcons;
    std::function<void(GridPage&)> spacing;
    std::function<void(int)> notify;
};

inline std::wstring GenerateId(const std::vector<std::wstring>& ids)
{
    int maxNumber = 0;
    for (const auto& id : ids)
        if (id.starts_with(L"__page:")) maxNumber = std::max(maxNumber, std::stoi(id.substr(7)));
    return L"__page:" + std::to_wstring(maxNumber + 1);
}

inline bool Navigable(State state, const std::wstring& id, const Services& services)
{
    const auto name = state.names.find(id);
    return page_management::RetainsPage(name == state.names.end() ? std::wstring_view{} : name->second, services.hasContent(id));
}

inline void Normalize(State state)
{
    std::erase(state.ids, std::wstring(kDockPageId));
    state.columns.erase(kDockPageId); state.rows.erase(kDockPageId); state.names.erase(kDockPageId);
    if (state.ids.empty() || state.ids.size() > 9999) return;
    std::unordered_map<std::wstring, std::wstring> mapping;
    for (size_t index = 0; index < state.ids.size(); ++index)
    {
        const auto id = L"__page:" + std::to_wstring(index + 1);
        if (state.ids[index] != id) mapping[state.ids[index]] = id;
    }
    if (mapping.empty()) return;
    for (auto& id : state.ids) if (mapping.contains(id)) id = mapping[id];
    std::unordered_map<std::wstring, LayoutRecord> records;
    for (auto& [key, record] : state.records)
    {
        if (mapping.contains(record.cell.pageId)) record.cell.pageId = mapping[record.cell.pageId];
        records.try_emplace(mapping.contains(key) ? mapping[key] : key, std::move(record));
    }
    state.records = std::move(records);
    for (auto& widget : state.widgets)
        if (mapping.contains(widget.gridCell.pageId)) widget.gridCell.pageId = mapping[widget.gridCell.pageId];
    for (auto& item : state.items)
        if (mapping.contains(item.gridCell.pageId)) item.gridCell.pageId = mapping[item.gridCell.pageId];
    std::unordered_map<std::wstring, int> columns, rows;
    for (const auto& [id, value] : state.columns) columns.try_emplace(mapping.contains(id) ? mapping[id] : id, value);
    for (const auto& [id, value] : state.rows) rows.try_emplace(mapping.contains(id) ? mapping[id] : id, value);
    state.columns = std::move(columns); state.rows = std::move(rows);
    page_management::RemapNames(state.names, mapping);
    ++state.revision;
}

inline void Prune(State state, const Services& services)
{
    services.refreshOwnership();
    state.offset = page_navigation_rules::PruneEmptyPages(state.ids, state.columns, state.rows, state.pages.size(), services.ready(),
        [&](const auto& id) { return Navigable(state, id, services); }, state.offset);
    std::erase_if(state.names, [&](const auto& entry) { return std::ranges::find(state.ids, entry.first) == state.ids.end(); });
}

inline void Pad(State state, const Services& services)
{
    const size_t target = state.pages.size();
    if (target == 0) return;
    const auto order = services.monitorOrder();
    while (state.ids.size() < target)
    {
        const auto id = GenerateId(state.ids);
        const auto index = state.ids.size();
        const auto* reference = index < order.size() && order[index] < state.pages.size() ? &state.pages[order[index]] : services.firstPage();
        if (!reference) return;
        state.ids.push_back(id);
        state.columns[id] = std::max(1, reference->columns); state.rows[id] = std::max(1, reference->rows);
    }
}

inline void Compact(State state)
{
    for (size_t index = 0; index < state.ids.size(); ++index)
        if (state.ids[index] != L"__page:" + std::to_wstring(index + 1)) { Normalize(state); return; }
}

inline void Map(State state, const Services& services)
{
    const auto previous = state.lastPage;
    state.lastPage.clear();
    if (state.pages.empty()) return;
    const int maximum = page_navigation_rules::MaximumOffset(state.ids.size(), state.pages.size(),
        [&](size_t index) { return Navigable(state, state.ids[index], services); });
    state.offset = std::clamp(state.offset, 0, maximum);
    const auto order = services.monitorOrder();
    for (size_t index = 0; index < order.size(); ++index)
    {
        auto& page = state.pages[order[index]];
        const bool last = index == order.size() - 1;
        const auto pageIndex = index + (last ? static_cast<size_t>(state.offset) : 0);
        page.id = pageIndex < state.ids.size() ? state.ids[pageIndex] : L"";
        if (last) state.lastPage = page.id;
    }
    if (!state.lastPage.empty() && state.ids.size() <= state.pages.size()) state.lastPage.clear();
    if (!previous.empty() && !state.lastPage.empty() && state.lastPage != previous)
    {
        const auto current = std::ranges::find(state.ids, state.lastPage);
        if (current != state.ids.end()) services.notify(static_cast<int>(current - state.ids.begin()));
    }
}

inline void Dimensions(State state, const Services& services)
{
    for (auto& page : state.pages)
    {
        const auto column = state.columns.find(page.id), row = state.rows.find(page.id);
        if (column != state.columns.end() && row != state.rows.end() && column->second >= 1 && row->second >= 1)
        {
            page.columns = column->second; page.rows = row->second;
            services.spacing(page);
        }
    }
}

// This is the host's actual cleanup/padding/remap orchestration. Pixel spacing,
// monitor discovery, ownership caches, notifications and rendering stay services.
inline void Apply(State state, const Services& services)
{
    services.cancelHover();
    if (state.pages.empty()) { state.lastPage.clear(); return; }
    if (state.ids.empty())
    {
        const auto id = GenerateId(state.ids);
        state.ids.push_back(id);
        const auto* reference = services.firstPage();
        if (!reference) return;
        state.columns[id] = std::max(1, reference->columns); state.rows[id] = std::max(1, reference->rows);
    }
    Prune(state, services);
    Pad(state, services);
    Compact(state);
    Map(state, services);
    Dimensions(state, services);
    services.reserveDock();
    services.refreshIcons();
}
}
