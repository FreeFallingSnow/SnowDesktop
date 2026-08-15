#include "lua_logical_slot.h"

#include "../core/item.h"

#include <algorithm>
#include <cmath>

LuaLogicalSlotContainer::LuaLogicalSlotContainer(
    std::wstring widgetId, std::string slotId,
    SurfaceProvider provider, DropCommitter committer)
    : widgetId_(std::move(widgetId)),
      slotId_(std::move(slotId)),
      provider_(std::move(provider)),
      committer_(std::move(committer))
{
}

std::optional<LogicalSlotHostSurface>
LuaLogicalSlotContainer::Surface() const
{
    return provider_ ? provider_() : std::nullopt;
}

std::wstring LuaLogicalSlotContainer::GetTitle() const
{
    return std::wstring(slotId_.begin(), slotId_.end());
}

std::optional<LuaLogicalSlotContainer::ItemHit>
LuaLogicalSlotContainer::ItemAtPoint(POINT point) const
{
    const auto surface = Surface();
    if (!surface) return std::nullopt;
    for (std::size_t index = 0; index < surface->items.size(); ++index)
    {
        if (!PtInRect(&surface->items[index].bounds, point)) continue;
        return ItemHit{
            surface->items[index].itemId,
            surface->kind,
            index,
            surface->itemCount,
            surface->kind ==
                    snowdesktop::widget_runtime::LogicalSlotKind::Collection ||
                surface->allowClear,
        };
    }
    return std::nullopt;
}

std::vector<std::unique_ptr<Slot>>
LuaLogicalSlotContainer::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    const auto surface = Surface();
    if (!surface) return slots;

    if (surface->kind ==
        snowdesktop::widget_runtime::LogicalSlotKind::Collection)
    {
        slots.reserve(surface->items.size() + 1);
        for (std::size_t index = 0;
            index < surface->items.size(); ++index)
        {
            slots.push_back(std::make_unique<Slot>(
                this, surface->items[index].bounds, index));
        }
        if (surface->itemCount < surface->capacity)
        {
            slots.push_back(std::make_unique<Slot>(
                this, surface->bounds, surface->items.size()));
        }
    }
    else
    {
        slots.push_back(std::make_unique<Slot>(
            this, surface->bounds, 0));
    }
    return slots;
}

RECT LuaLogicalSlotContainer::GetBounds() const
{
    const auto surface = Surface();
    return surface ? surface->bounds : RECT{};
}

BarStyle LuaLogicalSlotContainer::GetInsertionStyle() const
{
    const auto surface = Surface();
    if (!surface || surface->items.size() < 2)
        return BarStyle::HBar;
    const RECT& first = surface->items[0].bounds;
    const RECT& second = surface->items[1].bounds;
    const LONG deltaX = std::abs(
        (second.left + second.right) - (first.left + first.right));
    const LONG deltaY = std::abs(
        (second.top + second.bottom) - (first.top + first.bottom));
    return deltaX > deltaY ? BarStyle::VBar : BarStyle::HBar;
}

bool LuaLogicalSlotContainer::AcceptsKind(
    const LogicalSlotHostSurface& surface, std::string_view kind)
{
    return std::find(surface.accepts.begin(), surface.accepts.end(), kind) !=
        surface.accepts.end();
}

bool LuaLogicalSlotContainer::AcceptsDragPayload(
    snowdesktop::slot_contract::DragPayloadKind payload,
    std::size_t count) const
{
    if (count != 1) return false;
    const auto surface = Surface();
    if (!surface) return false;
    if (surface->kind ==
            snowdesktop::widget_runtime::LogicalSlotKind::Collection &&
        surface->itemCount >= surface->capacity)
        return false;
    if (surface->kind ==
            snowdesktop::widget_runtime::LogicalSlotKind::Binding &&
        surface->itemCount != 0 && surface->replacePolicy == "reject")
        return false;

    using Payload = snowdesktop::slot_contract::DragPayloadKind;
    if (payload == Payload::DesktopItem)
    {
        return AcceptsKind(*surface, "desktop.item") ||
            AcceptsKind(*surface, "app.reference") ||
            AcceptsKind(*surface, "filesystem.reference");
    }
    if (payload == Payload::FolderEntry ||
        payload == Payload::ExternalFile)
        return AcceptsKind(*surface, "filesystem.reference");
    return false;
}

HitRegion LuaLogicalSlotContainer::HitTestDrag(
    POINT point, Slot*& outSlot)
{
    outSlot = nullptr;
    const auto surface = Surface();
    if (!surface || !PtInRect(&surface->bounds, point))
        return HitRegion::None;

    InvalidateSlots();
    const auto& slots = GetSlots();
    if (slots.empty()) return HitRegion::Blocked;
    if (surface->kind ==
        snowdesktop::widget_runtime::LogicalSlotKind::Binding)
    {
        outSlot = slots.front().get();
        return HitRegion::Empty;
    }

    const BarStyle style = GetInsertionStyle();
    for (std::size_t index = 0;
        index < surface->items.size() && index < slots.size(); ++index)
    {
        const RECT& bounds = surface->items[index].bounds;
        if (!PtInRect(&bounds, point)) continue;
        outSlot = slots[index].get();
        if (style == BarStyle::VBar)
            return point.x < bounds.left +
                    (bounds.right - bounds.left) / 2
                ? HitRegion::SortBefore : HitRegion::SortAfter;
        return point.y < bounds.top +
                (bounds.bottom - bounds.top) / 2
            ? HitRegion::SortBefore : HitRegion::SortAfter;
    }

    if (surface->itemCount >= surface->capacity)
        return HitRegion::Blocked;
    outSlot = slots.back().get();
    return HitRegion::Empty;
}

std::size_t LuaLogicalSlotContainer::GetDropInsertIndex(
    Slot* slot, HitRegion region) const
{
    const auto surface = Surface();
    if (!surface || surface->kind ==
        snowdesktop::widget_runtime::LogicalSlotKind::Binding)
        return 0;
    if (!slot) return surface->itemCount;
    const std::size_t index = std::min(
        slot->GetIndex(), surface->itemCount);
    if (region == HitRegion::SortAfter && index < surface->itemCount)
        return index + 1;
    return index;
}

bool LuaLogicalSlotContainer::CommitItems(
    const std::vector<Item*>& sourceItems, Slot* targetSlot,
    HitRegion region)
{
    if (!committer_ || sourceItems.size() != 1 || !sourceItems.front())
        return false;
    return committer_(sourceItems,
        GetDropInsertIndex(targetSlot, region));
}

void LuaLogicalSlotContainer::OnItemsDropped(
    const std::vector<Item*>& sourceItems, Container*, Slot* targetSlot,
    HitRegion region, int)
{
    (void)CommitItems(sourceItems, targetSlot, region);
}

std::wstring LuaLogicalSlotContainer::GetDragHint(
    Slot* slot, HitRegion region, const std::vector<Item*>& sourceItems,
    Container*, int) const
{
    return slot ? slot->GetDropHint(region, sourceItems) : L"";
}

void LuaLogicalSlotContainer::DrawDropPreview(
    ID2D1DeviceContext* context, Slot* slot, HitRegion region)
{
    if (slot) slot->DrawDropIndicator(context, region);
}
