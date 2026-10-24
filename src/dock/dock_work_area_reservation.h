#pragma once

#include "common/types.h"
#include "dock_layout_settings.h"

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace snowdesktop::dock_work_area_reservation
{
inline bool SamePage(const GridPage& left, const GridPage& right)
{
    const auto fields = [](const GridPage& page) {
        return std::tie(page.id, page.monitorId,
            page.bounds.left, page.bounds.top, page.bounds.right, page.bounds.bottom,
            page.workArea.left, page.workArea.top, page.workArea.right, page.workArea.bottom,
            page.visualWorkArea.left, page.visualWorkArea.top,
            page.visualWorkArea.right, page.visualWorkArea.bottom,
            page.isPrimary, page.dpiX, page.dpiY, page.columns, page.rows,
            page.cellWidth, page.cellHeight, page.itemPitchWidth, page.itemPitchHeight,
            page.gapX, page.gapY, page.marginX, page.marginY);
    };
    return fields(left) == fields(right);
}

// sizingPage is ResolveDockSizingPage(page) before any trial reservation. It
// captures the effective home-page ID/counts as well as the display geometry;
// monitor order or saved dimensions must never reuse another page's solution.
struct Inputs
{
    GridPage page;
    GridPage sizingPage;
    DockLayoutSettings settings;
    float iconSpacingScale = 1.0f;
    float itemFontSizeCu = kDefaultItemFontSizeCu;
    float itemIconSizeScale = kDefaultItemIconSizeScale;
    int edgeExtent = 1;

    bool operator==(const Inputs& other) const
    {
        return SamePage(page, other.page) && SamePage(sizingPage, other.sizingPage) &&
            settings == other.settings && iconSpacingScale == other.iconSpacingScale &&
            itemFontSizeCu == other.itemFontSizeCu && itemIconSizeScale == other.itemIconSizeScale &&
            edgeExtent == other.edgeExtent;
    }
};

// Keep the ascending exhaustive selection: strictly smaller error wins, the
// first exact match stops the scan, and an empty range still reserves one pixel.
template<class DesiredReservation>
int Solve(int edgeExtent, DesiredReservation&& desiredReservation)
{
    int bestReserved = 1;
    int bestError = std::numeric_limits<int>::max();
    for (int reserved = 1; reserved < edgeExtent; ++reserved)
    {
        const int error = std::abs(desiredReservation(reserved) - reserved);
        if (error < bestError)
        {
            bestError = error;
            bestReserved = reserved;
            if (error == 0) break;
        }
    }
    return bestReserved;
}

// Cache only the scalar solution, never a GridPage to restore over current
// state. The caller still reapplies spacing and negotiates live AppBar bounds.
// Thread-local ownership at the call site requires no cross-thread sharing or
// host lifetime hooks. Fixed capacity also bounds retained page IDs and geometry.
class Cache
{
public:
    static constexpr std::size_t kCapacity = 32;

    template<class DesiredReservation>
    int Resolve(const Inputs& inputs, DesiredReservation&& desiredReservation)
    {
        for (const auto& entry : entries_)
            if (entry && entry->inputs == inputs) return entry->reserved;
        const int reserved = Solve(inputs.edgeExtent,
            std::forward<DesiredReservation>(desiredReservation));
        entries_[next_] = Entry{inputs, reserved};
        next_ = (next_ + 1) % kCapacity;
        return reserved;
    }

private:
    struct Entry { Inputs inputs; int reserved; };
    std::array<std::optional<Entry>, kCapacity> entries_;
    std::size_t next_ = 0;
};
}
