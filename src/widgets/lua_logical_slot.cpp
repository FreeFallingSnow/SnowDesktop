#include "lua_logical_slot.h"

#include "../core/item.h"

#include <algorithm>
#include <cmath>
#include <wrl/client.h>

namespace
{
using Microsoft::WRL::ComPtr;

float DropOpacity(
    const snowdesktop::widget_runtime::ViewStyle& style) noexcept
{
    return std::clamp(style.opacity.value_or(1.0f), 0.0f, 1.0f);
}

ComPtr<ID2D1SolidColorBrush> MakeDropBrush(
    ID2D1DeviceContext* context, std::uint32_t color, float opacity)
{
    ComPtr<ID2D1SolidColorBrush> brush;
    if (context)
        context->CreateSolidColorBrush(
            D2D1::ColorF(color, opacity), &brush);
    return brush;
}

bool DrawDropSurface(ID2D1DeviceContext* context,
    const LogicalSlotHostSurface& surface)
{
    const auto& style = surface.dropStyle;
    if (!context || (!style.background && !style.borderColor))
        return false;
    const D2D1_RECT_F bounds = D2D1::RectF(
        static_cast<float>(surface.bounds.left),
        static_cast<float>(surface.bounds.top),
        static_cast<float>(surface.bounds.right),
        static_cast<float>(surface.bounds.bottom));
    const float radius = std::max(
        0.0f, style.cornerRadius.value_or(0.0f));
    const D2D1_ROUNDED_RECT rounded =
        D2D1::RoundedRect(bounds, radius, radius);
    const float opacity = DropOpacity(style);
    if (style.background)
    {
        const auto fill = MakeDropBrush(
            context, *style.background, opacity);
        if (fill) context->FillRoundedRectangle(rounded, fill.Get());
    }
    const float borderWidth = std::max(
        0.0f, style.borderWidth.value_or(1.0f));
    if (style.borderColor && borderWidth > 0.0f)
    {
        const auto border = MakeDropBrush(
            context, *style.borderColor, opacity);
        if (border)
            context->DrawRoundedRectangle(
                rounded, border.Get(), borderWidth);
    }
    return true;
}

bool DrawDropIndicator(ID2D1DeviceContext* context,
    const LogicalSlotHostSurface& surface, Slot* slot,
    HitRegion region, BarStyle insertionStyle)
{
    if (!context || !slot || !surface.dropStyle.foreground ||
        region == HitRegion::None || region == HitRegion::Handoff ||
        region == HitRegion::Blocked)
        return false;
    const auto brush = MakeDropBrush(context,
        *surface.dropStyle.foreground,
        DropOpacity(surface.dropStyle));
    if (!brush) return true;
    const float lineWidth = 3.0f;
    if (region == HitRegion::Empty)
    {
        RECT bounds = surface.bounds;
        InflateRect(&bounds, -4, -4);
        const D2D1_RECT_F rectangle = D2D1::RectF(
            static_cast<float>(bounds.left),
            static_cast<float>(bounds.top),
            static_cast<float>(bounds.right),
            static_cast<float>(bounds.bottom));
        const float radius = std::max(0.0f,
            surface.dropStyle.cornerRadius.value_or(0.0f) - 4.0f);
        context->DrawRoundedRectangle(
            D2D1::RoundedRect(rectangle, radius, radius), brush.Get(),
            std::max(0.5f,
                surface.dropStyle.borderWidth.value_or(2.0f)));
        return true;
    }

    const RECT bounds = slot->GetBounds();
    if (insertionStyle == BarStyle::VBar)
    {
        const float x = region == HitRegion::SortBefore
            ? static_cast<float>(bounds.left) - lineWidth / 2.0f
            : static_cast<float>(bounds.right) - lineWidth / 2.0f;
        context->FillRectangle(D2D1::RectF(
            x, static_cast<float>(bounds.top) + 2.0f,
            x + lineWidth, static_cast<float>(bounds.bottom) - 2.0f),
            brush.Get());
    }
    else
    {
        const float y = region == HitRegion::SortBefore
            ? static_cast<float>(bounds.top) - lineWidth / 2.0f
            : static_cast<float>(bounds.bottom) - lineWidth / 2.0f;
        context->FillRectangle(D2D1::RectF(
            static_cast<float>(bounds.left) + 4.0f, y,
            static_cast<float>(bounds.right) - 4.0f, y + lineWidth),
            brush.Get());
    }
    return true;
}
}

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
    const SlotFeedbackIdentity feedbackIdentity{
        SlotFeedbackRole::LuaLogical,
        surface->bounds,
        surface->revision,
        static_cast<std::uint32_t>(surface->kind),
    };

    if (surface->kind ==
        snowdesktop::widget_runtime::LogicalSlotKind::Collection)
    {
        slots.reserve(surface->items.size() + 1);
        for (std::size_t index = 0;
            index < surface->items.size(); ++index)
        {
            slots.push_back(std::make_unique<Slot>(
                this, surface->items[index].bounds, index,
                SlotLifetime::ContainerCache, feedbackIdentity));
        }
        if (surface->itemCount < surface->capacity)
        {
            slots.push_back(std::make_unique<Slot>(
                this, surface->bounds, surface->items.size(),
                SlotLifetime::ContainerCache, feedbackIdentity));
        }
    }
    else
    {
        slots.push_back(std::make_unique<Slot>(
            this, surface->bounds, 0,
            SlotLifetime::ContainerCache, feedbackIdentity));
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
    if (!slot) return;
    const auto surface = Surface();
    if (!surface)
    {
        slot->DrawDropIndicator(context, region);
        return;
    }
    const bool styledSurface = DrawDropSurface(context, *surface);
    const bool styledIndicator = DrawDropIndicator(
        context, *surface, slot, region, GetInsertionStyle());
    if (!styledIndicator &&
        !(region == HitRegion::Empty && styledSurface))
        slot->DrawDropIndicator(context, region);
}
