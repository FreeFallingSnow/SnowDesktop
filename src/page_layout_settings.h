#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace snowdesktop
{

enum class PageLayoutRole : std::uint8_t
{
    FixedMonitor,
    LastMonitorDefault,
    Overflow,
};

struct PageLayoutEntry
{
    std::wstring id;
    int columns = 1;
    int rows = 1;
    std::size_t itemCount = 0;
    std::size_t widgetCount = 0;
    PageLayoutRole role = PageLayoutRole::Overflow;
    std::size_t monitorOrdinal = 0;
    bool visible = false;
    bool activeOnLastMonitor = false;

    friend bool operator==(const PageLayoutEntry&, const PageLayoutEntry&) =
        default;
};

struct PageLayoutSnapshot
{
    std::uint64_t revision = 0;
    std::size_t monitorCount = 0;
    std::vector<PageLayoutEntry> pages;

    friend bool operator==(const PageLayoutSnapshot&, const PageLayoutSnapshot&) =
        default;
};

struct PageGridChangeImpact
{
    bool valid = false;
    std::wstring pageId;
    int previousColumns = 1;
    int previousRows = 1;
    int columns = 1;
    int rows = 1;
    std::size_t displacedItemCount = 0;
    std::size_t displacedWidgetCount = 0;
    std::size_t resizedWidgetCount = 0;

    [[nodiscard]] bool RequiresConfirmation() const noexcept
    {
        return displacedItemCount != 0 || displacedWidgetCount != 0 ||
            resizedWidgetCount != 0;
    }
};

enum class PageLayoutOperationStatus : std::uint8_t
{
    Succeeded,
    Stale,
    Invalid,
    Failed,
};

struct PageLayoutOperationResult
{
    PageLayoutOperationStatus status = PageLayoutOperationStatus::Failed;
    std::wstring message;
    PageLayoutSnapshot snapshot;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == PageLayoutOperationStatus::Succeeded;
    }
};

[[nodiscard]] inline bool IsValidPageOrder(
    const std::vector<std::wstring>& current,
    const std::vector<std::wstring>& requested)
{
    if (current.size() != requested.size())
        return false;

    std::unordered_set<std::wstring> expected;
    expected.reserve(current.size());
    for (const auto& id : current)
    {
        if (id.empty() || !expected.insert(id).second)
            return false;
    }

    std::unordered_set<std::wstring> actual;
    actual.reserve(requested.size());
    for (const auto& id : requested)
    {
        if (id.empty() || !actual.insert(id).second ||
            !expected.contains(id))
        {
            return false;
        }
    }
    return actual.size() == expected.size();
}

} // namespace snowdesktop
