#pragma once

#include "container.h"
#include "item.h"
#include "transient_drag_slot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace snowdesktop
{
/**
 * @brief Owns a stable transient Slot and the Item wrappers used by handoff hits.
 *
 * Pointer-move hit testing must not append a fresh Item wrapper for every sample.
 * A two-entry cache reuses common boundary hits while keeping retained wrappers
 * strictly bounded. Only the Item currently bound to the Slot is guaranteed to
 * remain alive; inactive cached wrappers may be evicted before Reset().
 */
class OwnedTransientDragTarget
{
public:
    ~OwnedTransientDragTarget() { Reset(); }

    Slot* BindPlacement(Container* parent, RECT bounds,
        std::size_t index, SlotFeedbackRole feedbackRole)
    {
        Slot* bound = BindTransientDragSlot(
            slot_, parent, bounds, index, feedbackRole);
        SelectEpochAfterSlotRebind(
            parent, parent ? parent->GetSlotGeneration() : 0);
        return bound;
    }

    template <typename Factory>
    Slot* BindHandoff(Container* parent, RECT bounds,
        std::size_t index, SlotFeedbackRole feedbackRole,
        const void* memberIdentity,
        Factory&& createItem)
    {
        const std::uint64_t generation =
            parent ? parent->GetSlotGeneration() : 0;
        const bool sameEpoch =
            epochParent_ == parent &&
            epochGeneration_ == generation;
        if (sameEpoch)
        {
            for (std::size_t cachedIndex = 0;
                 cachedIndex < itemCount_; ++cachedIndex)
            {
                const auto& cached = items_[cachedIndex];
                if (cached.index == index &&
                    cached.memberIdentity == memberIdentity)
                {
                    return BindTransientDragSlot(
                        slot_, parent, bounds, index,
                        feedbackRole, cached.item.get());
                }
            }
        }

        std::unique_ptr<Item> item =
            std::forward<Factory>(createItem)();
        if (!item)
            return BindPlacement(
                parent, bounds, index, feedbackRole);

        Item* raw = item.get();
        Slot* bound = BindTransientDragSlot(
            slot_, parent, bounds, index,
            feedbackRole, raw);
        SelectEpochAfterSlotRebind(parent, generation);
        const std::size_t cacheIndex = itemCount_ < items_.size()
            ? itemCount_++ : nextVictim_;
        if (itemCount_ == items_.size())
            nextVictim_ = (cacheIndex + 1) % items_.size();
        items_[cacheIndex] = {
            index, memberIdentity, std::move(item) };
        return bound;
    }

    void Reset()
    {
        // Destroy the non-owning Slot before releasing Items it can reference.
        slot_.reset();
        ResetItems();
        epochParent_ = nullptr;
        epochGeneration_ = 0;
    }

    Slot* Get() const { return slot_.get(); }
    std::size_t OwnedItemCount() const { return itemCount_; }

private:
    struct OwnedItem
    {
        std::size_t index = 0;
        const void* memberIdentity = nullptr;
        std::unique_ptr<Item> item;
    };

    void SelectEpochAfterSlotRebind(
        Container* parent, std::uint64_t generation)
    {
        if (epochParent_ == parent &&
            epochGeneration_ == generation)
            return;
        // The Slot no longer points at the previous epoch, so its wrappers
        // can now be released without exposing a dangling handoff target.
        ResetItems();
        epochParent_ = parent;
        epochGeneration_ = generation;
    }

    void ResetItems()
    {
        for (auto& cached : items_)
            cached = {};
        itemCount_ = 0;
        nextVictim_ = 0;
    }

    // Two entries cover the common boundary oscillation without allowing
    // pointer travel across a large popup to grow retained wrappers.
    std::array<OwnedItem, 2> items_{};
    std::size_t itemCount_ = 0;
    std::size_t nextVictim_ = 0;
    std::unique_ptr<Slot> slot_;
    Container* epochParent_ = nullptr;
    std::uint64_t epochGeneration_ = 0;
};
}
