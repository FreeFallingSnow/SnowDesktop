#pragma once
#include "container.h"
#include "slot.h"
#include <vector>
#include <memory>

struct GridPage;
struct DesktopItem;
struct GridCell;

class DesktopGrid : public GridContainer
{
public:
    DesktopGrid(std::vector<GridPage>* pages, std::vector<DesktopItem>* items, void* app);
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

private:
    bool EnsureSlotCache();
    std::vector<GridPage>* pages_;
    std::vector<DesktopItem>* items_;
    void* app_;
    RECT bounds_{};
    bool slotsBuilt_ = false;
};
