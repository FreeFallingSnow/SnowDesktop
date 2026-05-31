#pragma once
#include "container.h"
#include "slot.h"
#include <vector>
#include <memory>

struct GridPage;
struct DesktopItem;
struct GridCell;
class DesktopApp;
class DesktopIcon;

class DesktopGrid : public GridContainer
{
public:
    DesktopGrid(std::vector<GridPage>* pages, std::vector<DesktopItem>* items, DesktopApp* app);
    ~DesktopGrid();
    std::wstring GetTitle() const override;
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;
    RECT GetBounds() const override;
    const std::vector<DesktopItem>& GetItems() const { return *items_; }
    std::vector<DesktopItem>& GetItems() { return *items_; }
    const std::vector<GridPage>& GetPages() const { return *pages_; }

    // Direct point test — O(1), no slot allocation
    HitRegion HitTestAtPoint(POINT pt, Slot*& outSlot);

    // ── Container drag virtuals ──────────────────────────
    std::vector<Item*> GetSelectedItems() const override;
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const override;
    void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region) override;

private:
    std::vector<GridPage>* pages_;
    std::vector<DesktopItem>* items_;
    DesktopApp* app_;
    RECT bounds_{};
    mutable std::vector<std::unique_ptr<DesktopIcon>> dragSourceCache_;
};
