#include "container.h"
#include "slot.h"
#include "item.h"

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
