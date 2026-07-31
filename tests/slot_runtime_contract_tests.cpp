#include "core/container.h"
#include "core/drag_session.h"
#include "core/slot.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
int failures = 0;

void Check(bool condition, const std::string& message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

class ContractContainer final : public Container
{
public:
    explicit ContractContainer(
        BarStyle style,
        snowdesktop::slot_contract::
            SlotSurfaceKind surface =
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Collection)
        : style_(style),
          surface_(surface)
    {
    }

    std::wstring GetTitle() const override
    {
        return L"contract fixture";
    }

    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return surface_;
    }

    std::vector<std::unique_ptr<Slot>>
        BuildSlots() override
    {
        ++buildCount;
        std::vector<std::unique_ptr<Slot>> result;
        result.push_back(std::make_unique<Slot>(
            this, RECT{0, 0, 100, 100}, 0));
        result.push_back(std::make_unique<Slot>(
            this, RECT{100, 0, 200, 100}, 1));
        return result;
    }

    void OnItemsDropped(
        const std::vector<Item*>& sourceItems,
        Container* origin,
        Slot* targetSlot,
        HitRegion region,
        int mods) override
    {
        ++dropCount;
        lastItems = sourceItems;
        lastOrigin = origin;
        lastTarget = targetSlot;
        lastRegion = region;
        lastMods = mods;
    }

    RECT GetBounds() const override
    {
        return RECT{0, 0, 200, 100};
    }

    BarStyle GetInsertionStyle() const override
    {
        return style_;
    }

    HitRegion HitTestDrag(
        POINT point, Slot*& outSlot) override
    {
        for (const auto& slot : GetSlots())
        {
            const HitRegion region =
                slot->HitTest(point);
            if (region != HitRegion::None)
            {
                outSlot = slot.get();
                return region;
            }
        }
        outSlot = nullptr;
        return HitRegion::None;
    }

    int buildCount = 0;
    int dropCount = 0;
    std::vector<Item*> lastItems;
    Container* lastOrigin = nullptr;
    Slot* lastTarget = nullptr;
    HitRegion lastRegion = HitRegion::None;
    int lastMods = 0;

private:
    BarStyle style_;
    snowdesktop::slot_contract::
        SlotSurfaceKind surface_;
};

Item* NonOwningItemToken()
{
    return reinterpret_cast<Item*>(
        static_cast<std::uintptr_t>(1));
}

POINT CenterOf(const RECT& rect)
{
    return POINT{
        rect.left +
            (rect.right - rect.left) / 2,
        rect.top +
            (rect.bottom - rect.top) / 2,
    };
}

void TestSlotCacheAndIdentity()
{
    ContractContainer container(BarStyle::VBar);
    const auto& first = container.GetSlots();
    Check(container.buildCount == 1,
        "GetSlots must build the cache lazily");
    Check(first.size() == 2,
        "fixture must expose its complete slot sequence");
    for (std::size_t index = 0;
        index < first.size(); ++index)
    {
        Check(first[index]->GetParent() == &container,
            "every built Slot must retain its real parent");
        Check(first[index]->GetIndex() == index,
            "slot indices must be contiguous and stable");
    }

    Slot* firstSlot = first.front().get();
    const auto generation =
        container.GetSlotGeneration();
    const auto& cached = container.GetSlots();
    Check(container.buildCount == 1 &&
            cached.front().get() == firstSlot,
        "slot references must stay stable before invalidation");

    container.InvalidateSlots();
    Check(container.GetSlotGeneration() != generation,
        "invalidating slots must advance the cache generation");
    const auto& rebuilt = container.GetSlots();
    Check(container.buildCount == 2,
        "the next GetSlots call must rebuild an invalid cache");
    Check(rebuilt.front()->GetParent() == &container,
        "rebuilt slots must remain bound to the container");
}

void TestHitRegionsUseContainerOrientation()
{
    ContractContainer vertical(BarStyle::VBar);
    Slot* verticalSlot =
        vertical.GetSlots().front().get();
    const RECT verticalBounds =
        verticalSlot->GetBounds();

    Check(verticalSlot->HitTest(
            CenterOf(verticalBounds)) ==
            HitRegion::Empty,
        "an empty slot must accept its full bounds");
    verticalSlot->SetItem(NonOwningItemToken());
    Check(verticalSlot->HitTest(
            CenterOf(verticalSlot->
                GetIconRect())) ==
            HitRegion::Handoff,
        "the item icon must remain a distinct handoff region");
    const LONG lowerY =
        verticalBounds.bottom - 1;
    Check(verticalSlot->HitTest({
            verticalBounds.left +
                (verticalBounds.right -
                    verticalBounds.left) / 4,
            lowerY}) ==
            HitRegion::SortBefore,
        "a vertical grid must sort before on the leading half");
    Check(verticalSlot->HitTest({
            verticalBounds.left +
                3 * (verticalBounds.right -
                    verticalBounds.left) / 4,
            lowerY}) ==
            HitRegion::SortAfter,
        "a vertical grid must sort after on the trailing half");
    Check(verticalSlot->HitTest({
            verticalBounds.right,
            verticalBounds.bottom}) ==
            HitRegion::None,
        "points outside a slot must not leak into it");

    ContractContainer horizontal(BarStyle::HBar);
    Slot* horizontalSlot =
        horizontal.GetSlots().front().get();
    horizontalSlot->SetItem(NonOwningItemToken());
    const RECT horizontalBounds =
        horizontalSlot->GetBounds();
    const LONG trailingX =
        horizontalBounds.right - 1;
    Check(horizontalSlot->HitTest({
            trailingX,
            horizontalBounds.top +
                (horizontalBounds.bottom -
                    horizontalBounds.top) / 4}) ==
            HitRegion::SortBefore,
        "a horizontal list must sort before on the leading half");
    Check(horizontalSlot->HitTest({
            trailingX,
            horizontalBounds.top +
                3 * (horizontalBounds.bottom -
                    horizontalBounds.top) / 4}) ==
            HitRegion::SortAfter,
        "a horizontal list must sort after on the trailing half");
}

void TestExecuteDropDelegatesOnce()
{
    ContractContainer source(BarStyle::VBar);
    ContractContainer target(BarStyle::HBar);
    Slot* targetSlot =
        target.GetSlots().front().get();
    std::vector<Item*> items{
        NonOwningItemToken()
    };

    targetSlot->ExecuteDrop(
        HitRegion::SortAfter,
        items, &source, MK_CONTROL);

    Check(target.dropCount == 1,
        "Slot::ExecuteDrop must delegate exactly once");
    Check(target.lastItems == items &&
            target.lastOrigin == &source &&
            target.lastTarget == targetSlot &&
            target.lastRegion ==
                HitRegion::SortAfter &&
            target.lastMods == MK_CONTROL,
        "Slot::ExecuteDrop must preserve the full drop context");
}

void TestDragSessionRejectsInvalidatedSlots()
{
    ContractContainer container(BarStyle::VBar);
    Slot* slot = container.GetSlots().front().get();
    DragSourceList sourceList;
    sourceList.origin = &container;
    sourceList.originSurface =
        container.GetSlotSurfaceKind();
    sourceList.hasOriginSurface = true;
    DragSourceEntry sourceEntry;
    sourceEntry.item = NonOwningItemToken();
    sourceList.entries.push_back(sourceEntry);
    DragSession session;
    session.Begin(
        &container, {NonOwningItemToken()},
        std::move(sourceList),
        POINT{}, POINT{});
    session.UpdateTarget(
        &container, slot,
        HitRegion::SortBefore);

    Check(session.TargetSlot() == slot &&
            session.TargetRegion() ==
                HitRegion::SortBefore,
        "a current slot must remain available to the drag session");

    container.InvalidateSlots();
    Check(session.TargetSlot() == nullptr &&
            session.TargetRegion() ==
                HitRegion::None,
        "an invalidated slot must be rejected before dereference");

    Slot* rebuilt =
        container.GetSlots().front().get();
    session.UpdateTarget(
        &container, rebuilt,
        HitRegion::SortAfter);
    Check(session.TargetSlot() == rebuilt,
        "a rebuilt slot can be rebound explicitly");

    session.DetachRuntimeBindings();
    Check(session.Source() == nullptr &&
            session.Items().empty() &&
            session.SourceList().origin == nullptr &&
            session.SourceList().entries.size() == 1 &&
            session.SourceList().entries[0].item == nullptr,
        "container tree rebuilds must detach every source binding");
    Check(session.SourceList().
            SourceSurfaceKind() ==
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Collection,
        "detaching runtime pointers must preserve stable source-surface metadata");
    Check(session.TargetContainer() == nullptr &&
            session.TargetSlot() == nullptr &&
            session.TargetRegion() ==
                HitRegion::None,
        "container tree rebuilds must detach every target binding");
}

void TestEverySurfaceRetainsStableDragMetadata()
{
    using Surface =
        snowdesktop::slot_contract::
            SlotSurfaceKind;
    for (const auto& descriptor :
         snowdesktop::slot_contract::
            kSurfaceDescriptors)
    {
        if (!descriptor.buildsSlots) continue;
        ContractContainer source(
            BarStyle::VBar,
            descriptor.kind);
        DragSourceList sourceList;
        sourceList.BindRuntimeOrigin(&source);
        DragSourceEntry entry;
        entry.item = NonOwningItemToken();
        sourceList.entries.push_back(entry);

        DragSession session;
        session.Begin(
            &source, {NonOwningItemToken()},
            std::move(sourceList),
            POINT{}, POINT{});
        session.DetachRuntimeBindings();

        Check(
            session.SourceList().
                SourceSurfaceKind() ==
                    descriptor.kind,
            "every registered container surface must survive runtime-source detachment");
    }

    DragSourceList external;
    external.hasExternalFiles = true;
    Check(
        external.SourceSurfaceKind() ==
            Surface::External,
        "external drags without a runtime container must retain the external surface fallback");
}

void TestEveryRegisteredSurfaceOriginLifecycle()
{
    namespace contract = snowdesktop::slot_contract;
    for (const auto& initialDescriptor :
         contract::kSurfaceDescriptors)
    {
        if (!initialDescriptor.buildsSlots) continue;
        for (const auto& reboundDescriptor :
             contract::kSurfaceDescriptors)
        {
            if (!reboundDescriptor.buildsSlots) continue;
            auto oldContainer =
                std::make_unique<ContractContainer>(
                    BarStyle::VBar,
                    initialDescriptor.kind);
            DragSourceList initial;
            initial.BindRuntimeOrigin(
                oldContainer.get());
            DragSourceEntry initialEntry;
            initialEntry.item = NonOwningItemToken();
            initial.entries.push_back(initialEntry);

            DragSession session;
            session.Begin(
                oldContainer.get(),
                {NonOwningItemToken()},
                std::move(initial),
                POINT{}, POINT{});

            auto stableContainer =
                std::make_unique<ContractContainer>(
                    BarStyle::VBar,
                    reboundDescriptor.kind);
            DragSourceList rebound;
            rebound.BindRuntimeOrigin(
                stableContainer.get());
            DragSourceEntry reboundEntry;
            reboundEntry.item =
                NonOwningItemToken();
            rebound.entries.push_back(reboundEntry);
            session.RebindSource(
                stableContainer.get(),
                {NonOwningItemToken()},
                std::move(rebound));

            oldContainer.reset();
            const std::string pair =
                std::string(initialDescriptor.name) +
                " -> " +
                std::string(reboundDescriptor.name);
            Check(
                session.Source() ==
                    stableContainer.get() &&
                    session.SourceList().origin ==
                        stableContainer.get() &&
                    session.SourceList().
                        SourceSurfaceKind() ==
                            reboundDescriptor.kind,
                pair +
                    ": every transient source replacement must rebind all runtime pointers before destruction");
            Check(
                session.Source() &&
                    session.Source()->
                        GetSlotSurfaceKind() ==
                            reboundDescriptor.kind,
                pair +
                    ": the next hit-test must use the registered replacement surface");
            const auto relation =
                contract::ClassifyRelation(
                    session.SourceList().
                        SourceSurfaceKind(),
                    session.Source()->
                        GetSlotSurfaceKind(),
                    session.SourceList().origin ==
                        session.Source());
            Check(
                relation ==
                    contract::DragRelation::SameInstance &&
                    contract::RelationMatches(
                        session.SourceList().
                            SourceSurfaceKind(),
                        session.Source()->
                            GetSlotSurfaceKind(),
                        relation),
                pair +
                    ": replacement metadata must form a valid same-instance relation");
        }
    }
}

void TestDropActionModifiers()
{
    DragSession session;
    session.Begin(nullptr, {}, {}, POINT{}, POINT{});
    Check(session.Action() == DropAction::Move,
        "an internal drag must begin as a move");

    Check(session.UpdateActionFromMods(
            MK_CONTROL) &&
            session.Action() == DropAction::Copy,
        "Ctrl must select copy");
    Check(session.UpdateActionFromMods(
            MK_ALT | MK_CONTROL) &&
            session.Action() == DropAction::Link,
        "Alt must take precedence and select link");
    Check(session.UpdateActionFromMods(
            MK_SHIFT) &&
            session.Action() == DropAction::Move,
        "Shift must select move");
    Check(session.UpdateActionFromMods(
            0, DropAction::Copy) &&
            session.Action() == DropAction::Copy,
        "external ingress can provide copy as its default action");
    Check(!session.UpdateActionFromMods(
            0, DropAction::Copy),
        "reapplying the same action must not invalidate state");
}
}

int main()
{
    TestSlotCacheAndIdentity();
    TestHitRegionsUseContainerOrientation();
    TestExecuteDropDelegatesOnce();
    TestDragSessionRejectsInvalidatedSlots();
    TestEverySurfaceRetainsStableDragMetadata();
    TestEveryRegisteredSurfaceOriginLifecycle();
    TestDropActionModifiers();
    if (failures != 0)
    {
        std::cerr << failures
            << " slot runtime contract test(s) failed\n";
        return 1;
    }
    std::cout
        << "All slot runtime contract tests passed\n";
    return 0;
}
