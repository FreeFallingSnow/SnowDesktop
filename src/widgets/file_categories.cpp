#include "../widget.h"
#include "../slot.h"
#include "../types.h"

std::vector<std::unique_ptr<Slot>> FileCategories::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || data_->type != DesktopWidgetType::FileCategories) return slots;

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

void FileCategories::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_) return;
    // TODO: app_->AddSelectedItemsToFileCategoryWidget(data_->id, targetSlot ? targetSlot->GetIndex() : 0);
    (void)sourceItems; (void)origin; (void)targetSlot; (void)region; (void)mods;
}

void FileCategories::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    // TODO: category tab bar, list/icon toggle button
    (void)context; (void)mousePt;
}
