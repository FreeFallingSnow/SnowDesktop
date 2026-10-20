#pragma once

#include "selected_desktop_cases.h"
#include "pointer_geometry.h"

namespace desktop_request_cases
{
using namespace selected_desktop_cases;

// Only the source-surface wrapper is supplied. It cannot route, hit-test,
// render or commit; all tested request/planning/commit behavior stays real.
class Origin final : public Container
{
public:
    std::wstring GetTitle() const override { return L"coordinate source fixture"; }
    slot_contract::SlotSurfaceKind GetSlotSurfaceKind() const override { return slot_contract::SlotSurfaceKind::Desktop; }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override { throw std::runtime_error("coordinate fixture cannot build slots"); }
    void OnItemsDropped(const std::vector<Item*>&, Container*, Slot*, HitRegion, int) override { throw std::runtime_error("coordinate fixture cannot commit"); }
    void DrawContents(ID2D1DeviceContext*) override { throw std::runtime_error("coordinate fixture cannot render"); }
    RECT GetBounds() const override { return {}; }
    BarStyle GetInsertionStyle() const override { return BarStyle::VBar; }
    HitRegion HitTestDrag(POINT, Slot*&) override { throw std::runtime_error("coordinate fixture cannot hit-test"); }
};

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    for (size_t monitors : {size_t{1}, size_t{2}})
    for (bool reverse : {false, true})
    for (bool mixedDpi : {false, true})
    for (size_t count : {size_t{1}, size_t{2}})
    for (bool large : {false, true})
    for (bool pointerAnchor : {false, true})
    for (bool deactivated : {false, true})
    {
        if (monitors == 1 && (reverse || mixedDpi)) continue;
        World world(root / std::to_wstring(serial++), monitors, reverse);
        internal_desktop_cases::Seed(world, count, 0, large);
        for (size_t index = 0; index < world.items.size(); ++index)
        {
            auto& item = world.items[index]; item.selected = index < count;
            if (index < count) item.gridCell.pageId = world.pages[world.order.front()].id;
            world.records.at(item.layoutKey) = {item.gridCell, item.gridSpan, item.largeIcon, true, item.slot};
        }
        world.Apply(); world.Legal(); pointer_geometry::Configure(world.pages, mixedDpi);
        const auto& sourcePage = world.pages[world.order.front()];
        const auto& targetPage = world.pages[world.order.back()];
        const auto sourceFrame = pointer_geometry::Frame(sourcePage, {sourcePage.id, 0, 0}, {1, 1});
        const POINT groupOrigin{sourceFrame.left, sourceFrame.top};
        const int grabX = count == 2 ? 340 : large ? 140 : 20;
        const POINT grab{grabX * static_cast<int>(sourcePage.dpiX) / 96, 40 * static_cast<int>(sourcePage.dpiY) / 96};
        const POINT mouseDown{groupOrigin.x + grab.x, groupOrigin.y + grab.y};
        const GridCell expected{targetPage.id, count == 2 ? 0 : 2, 1};
        const auto frame = pointer_geometry::Frame(targetPage, expected, {1, 1});
        const POINT current = pointerAnchor ? pointer_geometry::Center(targetPage, expected.column, expected.row)
            : POINT{frame.left + grab.x, frame.top + grab.y};
        Origin origin; auto sources = internal_desktop_cases::Sources(world, count); sources.BindRuntimeOrigin(&origin);
        DragSession session; session.Begin(&origin, {}, sources, mouseDown, mouseDown);
        if (pointerAnchor) session.AnchorToPointer(mouseDown);
        session.UpdatePoint(current);
        if (deactivated) session.DeactivateForDrop();
        const auto before = world.Snapshot();
        const auto requested = desktop_request::Resolve(world.pages, session.SourceList(), session, groupOrigin,
            {10000, -10000}, current, &sourcePage);
        Require(desktop_drop_cache::SameCell(requested, expected),
            "desktop coordinate ingress keeps the captured source anchor and pointer-selected monitor through drop deactivation");
        desktop_drop_cache::BestCellEntry cache;
        const auto preview = Preview(world, session.SourceList(), requested, &cache,
            {true, session.IsActive(), session.CurrentPoint(), session.MouseDownPoint(), session.StaticSceneRevision()});
        Require(world.Snapshot() == before && preview.landings.size() == count,
            "free coordinate-driven desktop selection previews every captured source without model mutation");
        for (const auto& landing : preview.landings)
        {
            const auto actual = GetGridRect(world.pages, landing.cell, landing.span);
            const auto rectangle = pointer_geometry::Frame(targetPage, landing.cell, landing.span);
            Require(actual.left == rectangle.left && actual.top == rectangle.top && actual.right == rectangle.right && actual.bottom == rectangle.bottom,
                "desktop landing geometry represents the complete target-grid footprint in physical pixels");
        }
        CheckCommit(world, session.SourceList(), preview);
    }
    Require(serial != 0, "coordinate ingress executes a nonempty scene matrix");
    std::cout << serial << " coordinate ingress, real session anchor, target-grid geometry, search, commit and storage scenes passed\n";
}
}
