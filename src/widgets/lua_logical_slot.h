#pragma once

#include "../core/container.h"
#include "../core/slot.h"
#include "../widget_engine.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * Host-owned drag target projected from one committed Lua slotSurface.
 *
 * The proxy never exposes Container, Slot, IDataObject, paths, or pointers to
 * Lua. It only reads an immutable geometry/policy snapshot and asks the host
 * callback to persist a reference after a native drop has committed.
 */
class LuaLogicalSlotContainer final : public Container
{
public:
    struct ItemHit
    {
        std::string itemId;
        snowdesktop::widget_runtime::LogicalSlotKind kind =
            snowdesktop::widget_runtime::LogicalSlotKind::Binding;
        std::size_t index = 0;
        std::size_t itemCount = 0;
        bool canRemove = false;
    };

    using SurfaceProvider =
        std::function<std::optional<LogicalSlotHostSurface>()>;
    using DropCommitter = std::function<bool(
        const std::vector<Item*>&, std::size_t)>;

    LuaLogicalSlotContainer(std::wstring widgetId, std::string slotId,
        SurfaceProvider provider, DropCommitter committer);

    const std::wstring& WidgetId() const noexcept { return widgetId_; }
    const std::string& SlotId() const noexcept { return slotId_; }

    std::wstring GetTitle() const override;
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SlotSurfaceKind::LuaLogicalSlot;
    }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems,
        Container* origin, Slot* targetSlot, HitRegion region,
        int mods) override;
    RECT GetBounds() const override;
    BarStyle GetInsertionStyle() const override;
    HitRegion HitTestDrag(POINT point, Slot*& outSlot) override;
    bool AcceptsDragPayload(
        snowdesktop::slot_contract::DragPayloadKind payload,
        std::size_t count) const override;
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin,
        int mods) const override;
    void DrawDropPreview(ID2D1DeviceContext* context, Slot* slot,
        HitRegion region) override;

    std::size_t GetDropInsertIndex(Slot* slot,
        HitRegion region) const;
    bool CommitItems(const std::vector<Item*>& sourceItems,
        Slot* targetSlot, HitRegion region);
    std::optional<ItemHit> ItemAtPoint(POINT point) const;

private:
    std::optional<LogicalSlotHostSurface> Surface() const;
    static bool AcceptsKind(const LogicalSlotHostSurface& surface,
        std::string_view kind);

    std::wstring widgetId_;
    std::string slotId_;
    SurfaceProvider provider_;
    DropCommitter committer_;
};
