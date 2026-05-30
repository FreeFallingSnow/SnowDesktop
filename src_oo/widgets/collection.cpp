#include "widget.h"
#include "slot.h"
#include "types.h"

std::vector<std::unique_ptr<Slot>> Collection::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || data_->type != DesktopWidgetType::Collection) return slots;

    const RECT& bounds = data_->bounds;
    const int cellW = kCellWidth;
    const int cellH = kMinCellHeight;
    const int cols = std::max(1, static_cast<int>((bounds.right - bounds.left) / cellW));

    size_t idx = 0;
    for (const auto& key : data_->itemKeys)
    {
        int col = static_cast<int>(idx) % cols;
        int row = static_cast<int>(idx) / cols;
        RECT cell = {
            bounds.left + col * cellW,
            bounds.top + row * cellH,
            bounds.left + (col + 1) * cellW,
            bounds.top + (row + 1) * cellH
        };
        slots.push_back(std::make_unique<Slot>(this, cell, idx));
        ++idx;
    }
    return slots;
}

void Collection::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_) return;
    // TODO: app_->MoveSelectedItemsToCollection(data_->id, targetSlot ? targetSlot->GetIndex() : 0);
    (void)sourceItems; (void)origin; (void)targetSlot; (void)region; (void)mods;
}

void Collection::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    // TODO: list/icon toggle button, title bar, gradient bottom bar
    (void)context; (void)mousePt;
}
