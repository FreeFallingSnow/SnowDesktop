#include "app.h"
#include <algorithm>
#include <numeric>

// Dock container lookup, invalidation and work-area reservation.

DockContainer* DesktopApp::GetDockContainer() const
{
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get())) return dock;
    return nullptr;
}

DockContainer* DesktopApp::GetDockContainerAtPoint(POINT point) const
{
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        if (desktopIconsHidden_ &&
            !dockSettings_.keepWhenDesktopHidden &&
            !(floatingDockVisible_ &&
                dock == floatingDockContainer_))
            continue;
        if (dock->ContainsInteractivePoint(point)) return dock;
    }
    return nullptr;
}

void DesktopApp::InvalidateDockContainers()
{
    for (const auto& container : containers_)
    {
        if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
            dock->InvalidateSlots();
    }
}

bool DesktopApp::SynchronizeDockContainerAreas()
{
    const size_t dockContainerCount = static_cast<size_t>(std::count_if(
        containers_.begin(), containers_.end(), [](const auto& container) {
            return dynamic_cast<DockContainer*>(container.get()) != nullptr;
        }));
    if (dockContainerCount != dockAreas_.size())
        return false;

    size_t areaIndex = 0;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        dock->SetReservedArea(dockAreas_[areaIndex++]);
    }
    return true;
}

void DesktopApp::InvalidateDockRects(BOOL erase)
{
    if (floatingDockHostActive_)
        InvalidatePersistentDockHosts(true);
    if (!hwnd_) return;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock ||
            IsDockHostedByPersistentHost(dock))
            continue;
        const RECT bounds = dock->GetInteractiveBounds();
        InvalidateRect(hwnd_, &bounds, erase);
    }
}

void DesktopApp::PrepareDockBackdropForDragTransition()
{
    // RemovePanel changes the helper HWND region and submits WinComp
    // immediately. Doing that before the first drag paint creates a frame in
    // which the Dock content still exists but its glass panel is already gone.
    // Reconcile every panel in the next paint instead: BeginFrame/EndFrame can
    // then replace the old hover geometry and new drag geometry atomically.
    desktopBackdropFullCollectionPending_ = true;
}

int DesktopApp::GetGridPageItemIconSize(const GridPage& page) const
{
    return GetPageItemVisualMetrics(page).iconSize;
}

void DesktopApp::ApplyDockWorkAreaReservation()
{
    if (dockWorkAreaReservationApplied_)
    {
        for (const RECT& dockArea : dockAreas_)
        {
            for (auto& page : gridPages_)
            {
                RECT intersect;
                if (!IntersectRect(
                        &intersect, &dockArea,
                        &page.bounds))
                    continue;
                int reserved;
                switch (dockWorkAreaReservationPosition_)
                {
                case DockPosition::Top:
                    reserved = dockArea.bottom -
                        dockArea.top;
                    page.workArea.top = std::max(
                        page.bounds.top,
                        page.workArea.top - reserved);
                    break;
                case DockPosition::Bottom:
                    reserved = dockArea.bottom -
                        dockArea.top;
                    page.workArea.bottom = std::min(
                        page.bounds.bottom,
                        page.workArea.bottom + reserved);
                    break;
                case DockPosition::Left:
                    reserved = dockArea.right -
                        dockArea.left;
                    page.workArea.left = std::max(
                        page.bounds.left,
                        page.workArea.left - reserved);
                    break;
                case DockPosition::Right:
                    reserved = dockArea.right -
                        dockArea.left;
                    page.workArea.right = std::min(
                        page.bounds.right,
                        page.workArea.right + reserved);
                    break;
                }
                break;
            }
        }
    }

    dockAreas_.clear();
    dockWorkAreaReservationApplied_ = false;
    if (!generalSettings_.dockEnabled || gridPages_.empty()) return;

    std::vector<size_t> targetPages = BuildMonitorRenderOrder();
    if (targetPages.empty())
    {
        targetPages.resize(gridPages_.size());
        std::iota(targetPages.begin(), targetPages.end(), size_t{ 0 });
    }
    if (targetPages.size() > 1)
    {
        switch (dockSettings_.monitorScope)
        {
        case DockMonitorScope::Last:
            targetPages.erase(targetPages.begin(), targetPages.end() - 1);
            break;
        case DockMonitorScope::First:
            targetPages.erase(targetPages.begin() + 1, targetPages.end());
            break;
        case DockMonitorScope::All:
        default:
            break;
        }
    }

    const bool vertical = dockSettings_.position == DockPosition::Left ||
        dockSettings_.position == DockPosition::Right;

    for (size_t pageIndex : targetPages)
    {
        if (pageIndex >= gridPages_.size()) continue;
        GridPage& targetPage = gridPages_[pageIndex];
        const RECT originalWorkArea = targetPage.workArea;
        const int width = std::max(1, static_cast<int>(
            originalWorkArea.right - originalWorkArea.left));
        const int height = std::max(1, static_cast<int>(
            originalWorkArea.bottom - originalWorkArea.top));
        const int edgeExtent = vertical ? width : height;
        auto reserveEdge = [&](GridPage& page, int reserved, RECT* dockArea) {
            page.workArea = originalWorkArea;
            RECT area{};
            switch (dockSettings_.position)
            {
            case DockPosition::Top:
                area = RECT{ originalWorkArea.left, originalWorkArea.top,
                    originalWorkArea.right, originalWorkArea.top + reserved };
                page.workArea.top = area.bottom;
                break;
            case DockPosition::Left:
                area = RECT{ originalWorkArea.left, originalWorkArea.top,
                    originalWorkArea.left + reserved, originalWorkArea.bottom };
                page.workArea.left = area.right;
                break;
            case DockPosition::Right:
                area = RECT{ originalWorkArea.right - reserved, originalWorkArea.top,
                    originalWorkArea.right, originalWorkArea.bottom };
                page.workArea.right = area.left;
                break;
            case DockPosition::Bottom:
            default:
                area = RECT{ originalWorkArea.left, originalWorkArea.bottom - reserved,
                    originalWorkArea.right, originalWorkArea.bottom };
                page.workArea.bottom = area.top;
                break;
            }
            if (dockArea) *dockArea = area;
        };

        // Match each Dock copy to the icon grid of its own display. This also
        // keeps mixed-resolution monitors from inheriting another screen's size.
        GridPage bestPage = targetPage;
        int bestReserved = 1;
        int bestError = INT_MAX;
        for (int reserved = 1; reserved < edgeExtent; ++reserved)
        {
            GridPage candidate = targetPage;
            reserveEdge(candidate, reserved, nullptr);
            ApplyIconSpacingToPage(candidate);
            const int componentMargin = GetComponentEdgeMargin(
                candidate, vertical);
            const float dockScale = ClampDockScale(dockSettings_.thicknessScale);
            const int scaledIconSize = std::max(1, static_cast<int>(std::round(
                GetGridPageItemIconSize(candidate) * dockScale)));
            const int scaledSpacing = std::max(1, static_cast<int>(std::round(
                kDockSpacing * dockScale)));
            const int edgeDistance = componentMargin;
            const int innerGap = 0;
            const int panelThickness = scaledIconSize + scaledSpacing * 2;
            const int desiredReservation = dockSettings_.edgeAttached
                ? panelThickness + innerGap
                : panelThickness + edgeDistance + innerGap;
            const int error = std::abs(desiredReservation - reserved);
            if (error < bestError)
            {
                bestError = error;
                bestReserved = reserved;
                bestPage = candidate;
                if (error == 0) break;
            }
        }

        targetPage = bestPage;
        RECT dockArea{};
        reserveEdge(targetPage, bestReserved, &dockArea);
        ApplyIconSpacingToPage(targetPage);
        if (!IsRectEmptyRect(dockArea)) dockAreas_.push_back(dockArea);
    }
    dockWorkAreaReservationApplied_ =
        !dockAreas_.empty();
    if (dockWorkAreaReservationApplied_)
        dockWorkAreaReservationPosition_ =
            dockSettings_.position;
}
