#include "container.h"
#include "slot.h"
#include "item.h"

void Container::DrawContents(ID2D1DeviceContext* context)
{
    auto slots = BuildSlots();
    for (auto& slot : slots)
    {
        if (Item* item = slot->GetItem())
        {
            bool selected = item->IsSelected();
            item->Draw(context, slot->GetBounds(), selected ? 2 : 0);
        }
    }
}
