#include "slot.h"
#include "container.h"
#include "item.h"
#include <algorithm>

Slot::Slot(Container* parent, RECT bounds, size_t index)
    : parent_(parent), bounds_(bounds), index_(index) {}

Container* Slot::GetParent() const { return parent_; }
RECT Slot::GetBounds() const { return bounds_; }
Item* Slot::GetItem() const { return item_; }
void Slot::SetItem(Item* item) { item_ = item; }
size_t Slot::GetIndex() const { return index_; }
bool Slot::IsEmpty() const { return item_ == nullptr; }

RECT Slot::GetIconRect() const
{
    const int cellW = bounds_.right - bounds_.left;
    const int cellH = bounds_.bottom - bounds_.top;
    if (cellH < 50)
    {
        const int iconSz = std::min(32, cellH - 4);
        return RECT{
            bounds_.left + 4,
            bounds_.top + (cellH - iconSz) / 2,
            bounds_.left + 4 + iconSz,
            bounds_.top + (cellH + iconSz) / 2
        };
    }
    const int maxIconW = std::max(16, cellW - 8);
    const int maxIconH = std::max(16, cellH - 136 - 8);
    const int iconSz = std::min(maxIconW, maxIconH);
    const int iconX = bounds_.left + (cellW - iconSz) / 2;
    const int iconY = bounds_.top + 2;
    return RECT{ iconX, iconY, iconX + iconSz, iconY + iconSz };
}

HitRegion Slot::HitTest(POINT pt) const
{
    if (!PtInRect(&bounds_, pt)) return HitRegion::None;

    if (IsEmpty()) return HitRegion::Empty;

    // Handoff: hit on the icon area
    RECT iconRect = GetIconRect();
    RECT handoffRect = { iconRect.left - 4, iconRect.top - 2,
                         iconRect.right + 4, iconRect.bottom + 4 };
    if (PtInRect(&handoffRect, pt)) return HitRegion::Handoff;

    // Sort: which half of the slot?
    const int cellW = bounds_.right - bounds_.left;
    const int cellH = bounds_.bottom - bounds_.top;

    // ListContainer → HBar (top/bottom), GridContainer → VBar (left/right)
    // For now, use a simple heuristic: if height < 50, use left/right split; else top/bottom
    if (cellH < 50)
        return (pt.x < bounds_.left + cellW / 2) ? HitRegion::SortBefore : HitRegion::SortAfter;
    else
        return (pt.y < bounds_.top + cellH / 2) ? HitRegion::SortBefore : HitRegion::SortAfter;
}

std::wstring Slot::GetDropHint(HitRegion region, const std::vector<Item*>& sourceItems) const
{
    switch (region)
    {
    case HitRegion::Empty:
        return L"移动到此空位";
    case HitRegion::SortBefore:
    case HitRegion::SortAfter:
        return L"重新排序";
    case HitRegion::Handoff:
        if (item_) return L"交给「" + item_->GetTitle() + L"」处理";
        return L"交给此项目处理";
    default:
        return L"";
    }
    (void)sourceItems;
}

void Slot::ExecuteDrop(HitRegion region, const std::vector<Item*>& sourceItems, Container* origin, int mods)
{
    if (parent_)
        parent_->OnItemsDropped(sourceItems, origin, this, region, mods);
}

void Slot::DrawDropIndicator(ID2D1DeviceContext* ctx, HitRegion region) const
{
    // TODO: implement D2D indicator drawing
    // Handoff → nothing
    // Empty → placeholder (blue dashed rect for Grid, HBar for List)
    // SortBefore/SortAfter → VBar (Grid) or HBar (List)
    (void)ctx;
    (void)region;
}
