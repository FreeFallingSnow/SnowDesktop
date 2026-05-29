#include "desktop.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include "constants.h"
#include "utils.h"

DesktopGrid::DesktopGrid(std::vector<GridPage>* pages, std::vector<DesktopItem>* items, SnowDesktopApp* app)
    : pages_(pages), items_(items), app_(app) {}

std::wstring DesktopGrid::GetTitle() const { return L"Desktop"; }
RECT DesktopGrid::GetBounds() const { return bounds_; }

std::vector<std::unique_ptr<Slot>> DesktopGrid::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    if (!pages_ || !items_) return slots;

    for (const auto& page : *pages_)
    {
        for (int row = 0; row < page.rows; ++row)
        {
            for (int col = 0; col < page.columns; ++col)
            {
                RECT cellBounds = {
                    page.bounds.left + page.marginX + col * (page.cellWidth + page.gapX),
                    page.bounds.top + page.marginY + row * (page.cellHeight + page.gapY),
                    page.bounds.left + page.marginX + col * (page.cellWidth + page.gapX) + page.cellWidth,
                    page.bounds.top + page.marginY + row * (page.cellHeight + page.gapY) + page.cellHeight
                };
                slots.push_back(std::make_unique<Slot>(this, cellBounds, slots.size()));
            }
        }
    }
    return slots;
}

void DesktopGrid::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    // TODO: implement using existing MoveToCell / FindBestDropCell / PlaceWidgetWithDisplacement
    // If sourceItems contain Widget → PlaceWidgetWithDisplacement
    // Otherwise → MoveToCell / FindBestDropCell
    (void)sourceItems;
    (void)origin;
    (void)targetSlot;
    (void)region;
    (void)mods;
}

void DesktopGrid::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    // TODO: page navigation buttons
    (void)context;
    (void)mousePt;
}
