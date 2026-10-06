#pragma once

#include "page_layout_settings.h"

#include <optional>
#include <string_view>
#include <unordered_map>

namespace snowdesktop::page_management
{
inline constexpr std::size_t kMaximumNameCharacters = 64;

inline bool RetainsPage(std::wstring_view name, bool hasContent) noexcept
{
    return !name.empty() || hasContent;
}

// Used by host mutations and failure-injection tests. Persistence must use
// the layout store's atomic replacement; failure restores all candidate state.
template<class Change, class Persist, class Restore>
bool Commit(Change&& change, Persist&& persist, Restore&& restore)
{
    try
    {
        change();
        if (persist()) return true;
    }
    catch (...)
    {
    }
    restore();
    return false;
}

inline bool NameSpace(wchar_t ch) noexcept
{
    return ch == L' ' || (ch >= 9 && ch <= 13) || ch == 0x85 ||
        ch == 0xa0 || ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200a) ||
        ch == 0x2028 || ch == 0x2029 || ch == 0x202f || ch == 0x205f ||
        ch == 0x3000;
}

// UTF-16 scalar counting keeps emoji intact and applies the same limit to
// menu input, WinUI input and persisted names. Empty means the default label.
inline std::optional<std::wstring> NormalizeName(std::wstring_view name)
{
    while (!name.empty() && NameSpace(name.front())) name.remove_prefix(1);
    while (!name.empty() && NameSpace(name.back())) name.remove_suffix(1);
    std::size_t count = 0;
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        const auto ch = static_cast<unsigned int>(name[i]);
        if (ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) ||
            ch == 0x2028 || ch == 0x2029 || (ch >= 0xdc00 && ch <= 0xdfff))
            return std::nullopt;
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            if (++i >= name.size() || name[i] < 0xdc00 || name[i] > 0xdfff)
                return std::nullopt;
        }
        if (++count > kMaximumNameCharacters) return std::nullopt;
    }
    return std::wstring(name);
}

inline std::wstring DisplayName(std::wstring defaultLabel, std::wstring_view name)
{
    if (!name.empty()) defaultLabel += L" · " + std::wstring(name);
    return defaultLabel;
}

inline std::wstring MenuLabel(std::wstring_view label)
{
    std::wstring result;
    for (const auto ch : label)
    {
        result += ch;
        if (ch == L'&') result += ch;
    }
    return result;
}

inline void RemapNames(std::unordered_map<std::wstring, std::wstring>& names,
    const std::unordered_map<std::wstring, std::wstring>& mapping)
{
    std::unordered_map<std::wstring, std::wstring> result;
    for (const auto& [id, name] : names)
    {
        const auto found = mapping.find(id);
        result.emplace(found == mapping.end() ? id : found->second, name);
    }
    names = std::move(result);
}

struct Placement
{
    std::size_t index = 0; // index into the host item/widget vector
    bool widget = false;
    bool guide = false;
    std::wstring pageId;
    int column = 0;
    int row = 0;
    int columns = 1;
    int rows = 1;
};

struct RemovalPlan
{
    PageRemovalImpact impact;
    std::vector<PageLayoutEntry> pages;
    std::vector<Placement> moved;
    std::vector<std::size_t> removedGuides;
    std::wstring destinationId;
};

// Pure planning: existing placements are reserved before moving anything.
// The caller commits this complete plan atomically or retains its old layout.
inline RemovalPlan PlanRemoval(const PageLayoutSnapshot& snapshot,
    const std::wstring& pageId, const std::vector<Placement>& content)
{
    RemovalPlan plan;
    const auto source = std::ranges::find(snapshot.pages, pageId, &PageLayoutEntry::id);
    if (!snapshot.editable || source == snapshot.pages.end() ||
        snapshot.pages.size() <= std::max<std::size_t>(1, snapshot.monitorCount))
        return plan;
    const auto sourceIndex = static_cast<std::size_t>(source - snapshot.pages.begin());
    plan.pages = snapshot.pages;
    plan.pages.erase(plan.pages.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
    plan.destinationId = plan.pages[sourceIndex == 0 ? 0 : sourceIndex - 1].id;

    std::vector<Placement> occupied;
    std::vector<Placement> moving;
    for (const auto& entry : content)
    {
        if (entry.pageId != pageId) occupied.push_back(entry);
        else if (entry.guide) plan.removedGuides.push_back(entry.index);
        else
        {
            if (entry.columns < 1 || entry.rows < 1 ||
                entry.columns > 50 || entry.rows > 50) return {};
            moving.push_back(entry);
            if (entry.widget) ++plan.impact.widgetCount;
            else ++plan.impact.itemCount;
        }
    }
    // Place larger shapes first to avoid fragmenting the free space.
    std::stable_sort(moving.begin(), moving.end(), [](const auto& a, const auto& b) {
        return a.columns * a.rows > b.columns * b.rows;
    });
    auto place = [&](const PageLayoutEntry& page, Placement& entry) {
        for (int column = 0; column + entry.columns <= page.columns; ++column)
            for (int row = 0; row + entry.rows <= page.rows; ++row)
            {
                const bool collision = std::ranges::any_of(occupied, [&](const auto& other) {
                    return other.pageId == page.id && column < other.column + other.columns &&
                        column + entry.columns > other.column && row < other.row + other.rows &&
                        row + entry.rows > other.row;
                });
                if (collision) continue;
                entry.pageId = page.id;
                entry.column = column;
                entry.row = row;
                return true;
            }
        return false;
    };
    std::unordered_set<std::wstring> reservedIds;
    for (const auto& page : snapshot.pages) reservedIds.insert(page.id);
    for (auto entry : moving)
    {
        const auto preferred = std::ranges::find(plan.pages, plan.destinationId, &PageLayoutEntry::id);
        bool placed = place(*preferred, entry);
        if (!placed)
            for (const auto& page : plan.pages)
                if (page.id != plan.destinationId && place(page, entry))
                { placed = true; break; }
        if (!placed)
        {
            if (plan.pages.size() >= 9999) return {};
            PageLayoutEntry added;
            for (unsigned int n = 1; n <= 10000; ++n)
            {
                added.id = L"__page:" + std::to_wstring(n);
                if (reservedIds.insert(added.id).second) break;
                added.id.clear();
            }
            if (added.id.empty()) return {};
            added.columns = std::max(source->columns, entry.columns);
            added.rows = std::max(source->rows, entry.rows);
            if (!place(added, entry)) return {};
            plan.pages.push_back(std::move(added));
            ++plan.impact.addedPageCount;
        }
        occupied.push_back(entry);
        plan.moved.push_back(std::move(entry));
    }
    plan.impact.valid = true;
    return plan;
}
} // namespace snowdesktop::page_management
