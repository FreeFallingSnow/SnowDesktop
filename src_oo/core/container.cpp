#include "container.h"
#include "slot.h"
#include "item.h"
#include <algorithm>

const std::vector<std::unique_ptr<Slot>>& Container::GetSlots()
{
    if (!slotsValid_)
    {
        cachedSlots_ = BuildSlots();
        slotsValid_ = true;
    }
    return cachedSlots_;
}

void Container::DrawContents(ID2D1DeviceContext* context)
{
    auto& slots = GetSlots();
    for (auto& slot : slots)
    {
        if (Item* item = slot->GetItem())
        {
            bool selected = item->IsSelected();
            item->Draw(context, slot->GetBounds(), selected ? 2 : 0);
        }
    }
}

std::vector<std::unique_ptr<Slot>> ListContainer::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    size_t count = GetSlotCount();
    if (count == 0 && !IncludeTrailingEmptySlot()) return slots;

    RECT bounds = GetBounds();
    // Content area = bounds minus 24px bottom chrome handle
    RECT body = {
        bounds.left + 4,
        bounds.top + 4,
        bounds.right - 4,
        std::max<LONG>(bounds.top + 28, bounds.bottom - 26)
    };
    int bodyW = std::max(1L, static_cast<LONG>(body.right - body.left));

    int itemW = GetItemWidth();
    int itemH = GetItemHeight();
    int cols = SingleColumn() ? 1 : std::max(1, bodyW / itemW);

    size_t total = IncludeTrailingEmptySlot() ? count + 1 : count;
    for (size_t idx = 0; idx < total; ++idx)
    {
        int col = static_cast<int>(idx) % cols;
        int row = static_cast<int>(idx) / cols;
        RECT cell = {
            body.left + col * itemW,
            body.top  + row * itemH,
            body.left + std::min<LONG>(col * itemW + itemW, body.right),
            body.top  + row * itemH + itemH
        };
        slots.push_back(std::make_unique<Slot>(this, cell, idx));
    }
    return slots;
}
