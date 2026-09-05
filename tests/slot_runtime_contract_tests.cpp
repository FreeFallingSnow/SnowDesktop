#include "core/container.h"
#include "core/drag_source_rebind.h"
#include "core/drag_session.h"
#include "core/drag_target_resolver.h"
#include "core/item.h"
#include "core/owned_transient_drag_target.h"
#include "core/slot.h"
#include "app/drag_drop_controller.h"
#include "app/ole_drag_drop_adapter.h"
#include "app/popup_dwell_controller.h"
#include "app/rename_controller.h"
#include "app/selection_controller.h"
#include "app/tray_icon_controller.h"
#include "drag_input_rules.h"
#include "ole_drag_rules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
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

class ContractItem final : public Item
{
public:
    explicit ContractItem(RECT bounds) : bounds_(bounds) {}

    std::wstring GetTitle() const override { return L"item"; }
    std::wstring GetPath() const override { return L""; }
    HBITMAP GetIconBitmap() const override { return nullptr; }
    RECT GetBounds() const override { return bounds_; }
    void SetBounds(RECT bounds) override { bounds_ = bounds; }
    bool IsSelected() const override { return selected_; }
    void SetSelected(bool selected) override { selected_ = selected; }
    Container* GetContainer() const override { return nullptr; }
    void Draw(ID2D1DeviceContext*, RECT, int) override {}
    Microsoft::WRL::ComPtr<IDataObject> CreateDataObject() override
    {
        return {};
    }

private:
    RECT bounds_{};
    bool selected_ = false;
};

class FakeOleDragDropHandler final : public OleDragDropHandler
{
public:
    HRESULT HandleOleDragEnter(IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect) override
    {
        ++dragEnterCount;
        lastDataObject = dataObject;
        lastKeyState = keyState;
        lastPoint = point;
        if (effect) *effect = DROPEFFECT_LINK;
        return S_OK;
    }

    HRESULT HandleOleDragOver(DWORD keyState,
        POINTL point, DWORD* effect) override
    {
        ++dragOverCount;
        lastKeyState = keyState;
        lastPoint = point;
        if (effect) *effect = DROPEFFECT_MOVE;
        return S_FALSE;
    }

    HRESULT HandleOleDragLeave() override
    {
        ++dragLeaveCount;
        return S_OK;
    }

    HRESULT HandleOleDrop(IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect) override
    {
        ++dropCount;
        lastDataObject = dataObject;
        lastKeyState = keyState;
        lastPoint = point;
        if (effect) *effect = DROPEFFECT_COPY;
        return S_OK;
    }

    HRESULT HandleOleQueryContinueDrag(
        BOOL escapePressed, DWORD keyState) override
    {
        ++queryContinueCount;
        return escapePressed || keyState == 0
            ? DRAGDROP_S_CANCEL : S_OK;
    }

    HRESULT HandleOleGiveFeedback(DWORD effect) override
    {
        ++feedbackCount;
        lastEffect = effect;
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

    int dragEnterCount = 0;
    int dragOverCount = 0;
    int dragLeaveCount = 0;
    int dropCount = 0;
    int queryContinueCount = 0;
    int feedbackCount = 0;
    IDataObject* lastDataObject = nullptr;
    DWORD lastKeyState = 0;
    DWORD lastEffect = 0;
    POINTL lastPoint{};
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
    const std::uint64_t beginPresentationRevision =
        session.PresentationRevision();
    session.UpdatePoint({ 5, 7 });
    Check(session.PresentationRevision() ==
            beginPresentationRevision,
        "pure pointer motion must not invalidate drop feedback");
    Check(session.UpdateTarget(
        &container, slot,
        HitRegion::SortBefore),
        "binding a new target must invalidate drop feedback");
    const std::uint64_t targetPresentationRevision =
        session.PresentationRevision();
    Check(!session.UpdateTarget(
            &container, slot,
            HitRegion::SortBefore) &&
            session.PresentationRevision() ==
                targetPresentationRevision,
        "repeating the same target must preserve the drop-feedback revision");

    const std::uint64_t stableTargetRevision =
        session.PresentationRevision();
    Check(session.UpdatePresentationAnchor(
            GridCell{L"primary", 2, 3}) &&
            session.PresentationRevision() !=
                stableTargetRevision,
        "binding an effective desktop landing cell must invalidate drop feedback");
    const std::uint64_t anchorRevision =
        session.PresentationRevision();
    Check(!session.UpdatePresentationAnchor(
            GridCell{L"primary", 2, 3}) &&
            session.PresentationRevision() ==
                anchorRevision,
        "repeating the same effective landing cell must preserve drop feedback");
    Check(session.UpdatePresentationAnchor(
            GridCell{L"primary", 3, 3}) &&
            session.PresentationRevision() !=
                anchorRevision,
        "an effective landing cell change must refresh feedback even when the hit slot is unchanged");
    Check(session.ClearPresentationAnchor() &&
            !session.ClearPresentationAnchor(),
        "leaving the desktop target must clear its presentation anchor exactly once");

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
    Check(session.UpdateTarget(
            &container, rebuilt,
            HitRegion::SortBefore),
        "rebinding equal cached geometry from a new generation must refresh feedback");
    Check(session.TargetSlot() == rebuilt,
        "a rebuilt slot can be rebound explicitly");

    auto transient = std::make_unique<Slot>(
        &container, RECT{0, 0, 100, 100}, 0,
        SlotLifetime::TransientDragTarget,
        SlotFeedbackIdentity{
            SlotFeedbackRole::Popup,
            RECT{0, 0, 100, 100}, 0, 0});
    Check(session.UpdateTarget(
            &container, transient.get(),
            HitRegion::SortBefore),
        "switching from a cached slot to a transient target must refresh feedback");
    const std::uint64_t transientRevision =
        session.PresentationRevision();
    auto equivalentTransient = std::make_unique<Slot>(
        &container, RECT{0, 0, 100, 100}, 0,
        SlotLifetime::TransientDragTarget,
        SlotFeedbackIdentity{
            SlotFeedbackRole::Popup,
            RECT{0, 0, 100, 100}, 0, 0});
    Check(!session.UpdateTarget(
            &container, equivalentTransient.get(),
            HitRegion::SortBefore) &&
            session.PresentationRevision() == transientRevision &&
            session.TargetSlot() == equivalentTransient.get(),
        "reallocating the same logical transient target must not redraw feedback");
    equivalentTransient->RebindTransientDragTarget(
        &container, RECT{100, 0, 200, 100}, 1,
        SlotFeedbackRole::Popup);
    Check(session.UpdateTarget(
            &container, equivalentTransient.get(),
            HitRegion::SortBefore),
        "reusing a transient address for another logical target must refresh feedback");
    equivalentTransient->RebindTransientDragTarget(
        &container, RECT{0, 0, 100, 100}, 0,
        SlotFeedbackRole::FileGroupSourceTab);
    Check(session.UpdateTarget(
            &container, equivalentTransient.get(),
            HitRegion::SortBefore),
        "changing transient geometry back must refresh feedback");
    equivalentTransient->RebindTransientDragTarget(
        &container, RECT{0, 0, 100, 100}, 0,
        SlotFeedbackRole::FileGroupHosted);
    Check(session.UpdateTarget(
            &container, equivalentTransient.get(),
            HitRegion::SortBefore),
        "equal transient geometry with a different role must refresh feedback");
    container.InvalidateSlots();
    Check(session.TargetSlot() == equivalentTransient.get(),
        "container cache invalidation must not reject an independently owned transient target");

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

void TestDragSessionVisualVisibilityFollowsOleOwnership()
{
    DragSession session;
    session.Begin(
        nullptr, {NonOwningItemToken()}, {},
        POINT{10, 20}, POINT{10, 20});
    Check(session.IsVisualVisible(),
        "a newly started internal drag must show its custom visual");

    Check(session.SetVisualVisible(false) &&
            !session.IsVisualVisible(),
        "OLE handoff must be able to hide the custom visual without ending the session");
    Check(!session.SetVisualVisible(false),
        "repeating the same visual state must be a no-op");
    Check(session.IsActive() && session.HasContext() &&
            session.Items().size() == 1,
        "hiding the visual must retain the live drag payload and context");

    Check(session.SetVisualVisible(true) &&
            session.IsVisualVisible(),
        "re-entering SnowDesktop must restore the custom visual");
    const std::uint64_t activeRevision =
        session.StaticSceneRevision();
    session.DeactivateForDrop();
    Check(!session.IsActive() &&
            !session.IsVisualVisible() &&
            session.HasContext() &&
            session.StaticSceneRevision() != activeRevision,
        "drop execution must cross an inactive revision barrier while preserving commit context");
    session.End();
    Check(!session.IsVisualVisible() && !session.HasContext(),
        "ending a drag must clear both visual and commit state");
}

void TestListDragCanAnchorVisualAndLandingToPointer()
{
    ContractContainer source(BarStyle::HBar);
    ContractItem item(RECT{100, 200, 500, 238});
    DragSourceList sourceList;
    sourceList.BindRuntimeOrigin(&source);

    DragSession session;
    session.Begin(
        &source, {&item}, std::move(sourceList),
        POINT{360, 219}, POINT{365, 224});
    session.SetVisualItemBounds({item.GetBounds()});

    // The row is 400 pixels wide, but its compact icon is centered at x=118.
    // Anchoring that icon to a press in the text area must remove the permanent
    // row-origin offset from both the landing coordinate and the ghost.
    session.AnchorToPointer(POINT{118, 219});

    const POINT current{700, 400};
    const POINT target = session.ResolveTargetPoint(
        POINT{100, 200}, current);
    Check(target.x == current.x && target.y == current.y,
        "pointer-anchored list drag must hit the cell under the pointer");

    const RECT ghost = session.ResolveDraggedBounds(
        0, item.GetBounds(), current);
    const POINT ghostIconCenter{
        ghost.left + 18,
        ghost.top + 19
    };
    Check(ghostIconCenter.x == current.x &&
            ghostIconCenter.y == current.y,
        "pointer-anchored list drag must keep the icon ghost under the pointer");

    session.End();

    DragSourceList ordinarySourceList;
    ordinarySourceList.BindRuntimeOrigin(&source);
    session.Begin(
        &source, {&item}, std::move(ordinarySourceList),
        POINT{360, 219}, POINT{365, 224});
    const POINT ordinaryTarget = session.ResolveTargetPoint(
        POINT{100, 200}, current);
    Check(ordinaryTarget.x == 440 &&
            ordinaryTarget.y == 381,
        "a new ordinary drag must restore the original grab-offset policy");
}

void TestDesktopPlacementPolicyMatchesSourceSemantics()
{
    using Surface = snowdesktop::slot_contract::
        SlotSurfaceKind;
    ContractContainer desktop(
        BarStyle::HBar, Surface::Desktop);
    ContractContainer dock(
        BarStyle::HBar, Surface::Dock);

    DragSourceList dockWidget;
    dockWidget.BindRuntimeOrigin(&dock);
    dockWidget.entries.push_back({});
    dockWidget.hasWidgets = true;
    Check(dockWidget.UsesPointerDesktopPlacement(),
        "Dock sources must resolve desktop placement from the pointer cell");
    Check(!dockWidget.SupportsDesktopShellHandoff(),
        "widget payloads must not advertise a Shell handoff target");

    DragSourceList dockFile;
    dockFile.BindRuntimeOrigin(&dock);
    dockFile.entries.push_back({});
    Check(dockFile.UsesPointerDesktopPlacement() &&
            dockFile.SupportsDesktopShellHandoff(),
        "path-backed Dock entries must keep pointer placement and Shell handoff support");

    DragSourceList desktopItems;
    desktopItems.BindRuntimeOrigin(&desktop);
    desktopItems.entries.push_back({});
    Check(!desktopItems.UsesPointerDesktopPlacement(),
        "ordinary desktop drags must preserve their group-origin grab offset");
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

struct SelectionFixtureEntry
{
    bool selected = false;
};

struct SelectionFixtureWidget
{
    bool selected = false;
    std::vector<SelectionFixtureEntry> folderEntries;
};

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

void TestEveryDragSourceSurvivesPageTurnRebindMatrix()
{
    namespace contract = snowdesktop::slot_contract;
    struct PageTurnCase
    {
        const char* name;
        POINT nextGroupOrigin;
        RECT reboundBounds;
        bool sourcePageHidden;
    };
    constexpr POINT pointerDown{100, 80};
    constexpr POINT pointerCurrent{120, 90};
    constexpr POINT originalGroupOrigin{40, 30};
    constexpr RECT originalBounds{50, 40, 130, 120};
    constexpr std::array pageTurns{
        PageTurnCase{
            "same-display page replacement",
            originalGroupOrigin,
            RECT{},
            true,
        },
        PageTurnCase{
            "cross-display migration with page replacement",
            POINT{1960, 150},
            RECT{1970, 160, 2060, 250},
            false,
        },
    };

    for (const auto& descriptor :
         contract::kSurfaceDescriptors)
    {
        if (!descriptor.buildsSlots) continue;
        for (const PageTurnCase& pageTurn : pageTurns)
        {
            const std::string caseName =
                std::string(descriptor.name) + " / " +
                pageTurn.name;
            auto original =
                std::make_unique<ContractContainer>(
                    BarStyle::VBar, descriptor.kind);
            auto originalItem =
                std::make_unique<ContractItem>(
                    originalBounds);
            DragSourceList originalList;
            originalList.BindRuntimeOrigin(
                original.get());
            for (const auto& widgetDescriptor :
                 contract::kWidgetContractDescriptors)
            {
                if (widgetDescriptor.role !=
                        contract::WidgetContainerRole::
                            SlotContainer ||
                    widgetDescriptor.surface !=
                        descriptor.kind)
                {
                    continue;
                }
                originalList.hasOriginWidget = true;
                originalList.originWidgetId =
                    L"matrix-source";
                originalList.originWidgetType =
                    widgetDescriptor.type;
                break;
            }
            DragSourceEntry originalEntry;
            originalEntry.item = originalItem.get();
            originalEntry.memberIndex = 3;
            originalList.entries.push_back(originalEntry);

            DragSession session;
            session.Begin(
                original.get(), {originalItem.get()},
                std::move(originalList),
                pointerDown, pointerCurrent);
            session.SetVisualItemBounds(
                {originalItem->GetBounds()});
            const POINT targetBeforeTurn =
                session.ResolveTargetPoint(
                    originalGroupOrigin,
                    pointerCurrent);
            Check(targetBeforeTurn.x == 60 &&
                    targetBeforeTurn.y == 40,
                caseName +
                    ": the pre-turn landing point must use the drag-start origin");

            ContractContainer staleTarget(
                BarStyle::VBar,
                contract::SlotSurfaceKind::Desktop);
            session.UpdateTarget(
                &staleTarget,
                staleTarget.GetSlots().front().get(),
                HitRegion::Empty);

            // ApplyPageMapping + LayoutItems destroys the runtime tree before
            // a cross-display origin adjustment is known. Mirror that order:
            // detach, rebuild the source, then compensate the group origin.
            session.DetachRuntimeBindings();
            originalItem.reset();
            original.reset();
            Check(session.TargetContainer() == nullptr &&
                    session.TargetSlot() == nullptr &&
                    session.TargetRegion() == HitRegion::None,
                caseName +
                    ": a page rebuild must discard the stale pre-turn hit target");

            ContractContainer rebuilt(
                BarStyle::VBar, descriptor.kind);
            ContractItem reboundItem(pageTurn.reboundBounds);
            const bool needsRecordedMemberRestore =
                pageTurn.sourcePageHidden &&
                snowdesktop::drag_source_rebind::
                    CanRestoreRecordedWidgetMembers(
                        session.SourceList());
            std::vector<Item*> runtimeItems;
            if (!needsRecordedMemberRestore)
                runtimeItems.push_back(&reboundItem);
            int resolvedMemberCount = 0;
            std::vector<Item*> reboundItems =
                snowdesktop::drag_source_rebind::
                    ResolveItemsAfterRebuild(
                        std::move(runtimeItems),
                        session.SourceList(),
                        [&](size_t memberIndex) -> Item* {
                            ++resolvedMemberCount;
                            return memberIndex == 3
                                ? &reboundItem
                                : nullptr;
                        });
            Check(reboundItems.size() == 1 &&
                    reboundItems.front() ==
                        &reboundItem &&
                    resolvedMemberCount ==
                        (needsRecordedMemberRestore
                            ? 1 : 0),
                caseName +
                    ": runtime source recovery must use visible wrappers or the exact recorded member as appropriate");

            DragSourceList reboundList =
                session.SourceList();
            reboundList.BindRuntimeOrigin(&rebuilt);
            reboundList.entries.front().item =
                &reboundItem;
            session.RebindSource(
                &rebuilt, std::move(reboundItems),
                std::move(reboundList));
            session.AdjustForGroupOriginChange(
                originalGroupOrigin,
                pageTurn.nextGroupOrigin);

            const RECT actualBounds =
                reboundItem.GetBounds();
            const RECT expectedBounds =
                pageTurn.sourcePageHidden
                    ? originalBounds
                    : pageTurn.reboundBounds;
            Check(actualBounds.left == expectedBounds.left &&
                    actualBounds.top == expectedBounds.top &&
                    actualBounds.right == expectedBounds.right &&
                    actualBounds.bottom == expectedBounds.bottom,
                caseName +
                    ": source runtime bounds must survive the page replacement policy");
            Check(session.IsActive() &&
                    session.Items().size() == 1 &&
                    session.Items().front() == &reboundItem &&
                    session.Source() == &rebuilt &&
                    session.SourceList().SourceSurfaceKind() ==
                        descriptor.kind,
                caseName +
                    ": every registered drag source must rebind without ending the session");

            const POINT targetAfterTurn =
                session.ResolveTargetPoint(
                    pageTurn.nextGroupOrigin,
                    pointerCurrent);
            Check(targetAfterTurn.x == targetBeforeTurn.x &&
                    targetAfterTurn.y == targetBeforeTurn.y,
                caseName +
                    ": page replacement and monitor migration must preserve the landing coordinate");

            ContractContainer newTarget(
                BarStyle::VBar,
                contract::SlotSurfaceKind::Desktop);
            Slot* targetSlot = nullptr;
            const HitRegion targetRegion =
                newTarget.HitTestDrag(
                    targetAfterTurn, targetSlot);
            session.UpdateTarget(
                &newTarget, targetSlot, targetRegion);
            Check(targetSlot != nullptr &&
                    targetRegion != HitRegion::None &&
                    session.TargetSlot() == targetSlot &&
                    session.TargetRegion() == targetRegion,
                caseName +
                    ": the first post-turn hit-test must bind a slot at the preserved coordinate");

            const RECT ghost =
                session.ResolveDraggedBounds(
                    0, reboundItem.GetBounds(),
                    pointerCurrent);
            Check(ghost.left == 70 && ghost.top == 50 &&
                    ghost.right == 150 && ghost.bottom == 130,
                caseName +
                    ": the drag ghost must remain anchored to the start snapshot");

            session.DeactivateForDrop();
            const POINT commitPoint =
                session.ResolveTargetPoint(
                    pageTurn.nextGroupOrigin,
                    pointerCurrent);
            Check(!session.IsActive() &&
                    session.HasContext() &&
                    commitPoint.x == targetBeforeTurn.x &&
                    commitPoint.y == targetBeforeTurn.y,
                caseName +
                    ": drop commit must retain the same post-turn hit context");
            session.End();
        }
    }
}

void TestDragTargetResolutionUsesContractAndZOrder()
{
    namespace contract = snowdesktop::slot_contract;
    ContractContainer source(
        BarStyle::VBar,
        contract::SlotSurfaceKind::Desktop);
    DragSourceList sourceList;
    sourceList.BindRuntimeOrigin(&source);
    sourceList.hasDesktopIcons = true;

    std::vector<std::unique_ptr<Container>> containers;
    auto lower = std::make_unique<ContractContainer>(
        BarStyle::VBar,
        contract::SlotSurfaceKind::Desktop);
    Container* lowerPointer = lower.get();
    containers.push_back(std::move(lower));
    auto upper = std::make_unique<ContractContainer>(
        BarStyle::VBar,
        contract::SlotSurfaceKind::Collection);
    Container* upperPointer = upper.get();
    containers.push_back(std::move(upper));

    const POINT point{50, 50};
    DragTargetResolution resolved =
        DragTargetResolver::ResolveInternal(
            containers, point, sourceList);
    Check(resolved.container == upperPointer &&
            resolved.slot &&
            resolved.region == HitRegion::Empty,
        "the topmost contract-compatible slot surface must win hit testing");

    resolved = DragTargetResolver::ResolveInternal(
        containers, point, sourceList,
        [&](const Container& candidate) {
            return &candidate != upperPointer;
        });
    Check(resolved.container == lowerPointer,
        "application policy must be able to filter a top surface and fall through");

    auto guide = std::make_unique<ContractContainer>(
        BarStyle::VBar,
        contract::SlotSurfaceKind::Guide);
    Container* guidePointer = guide.get();
    containers.push_back(std::move(guide));
    resolved = DragTargetResolver::ResolveInternal(
        containers, point, sourceList);
    Check(resolved.container == upperPointer &&
            resolved.container != guidePointer,
        "a visually topmost surface rejected by the slot contract must be skipped");

    resolved = DragTargetResolver::ResolveExternal(
        containers, point);
    Check(resolved.container == upperPointer &&
            resolved.container != guidePointer,
        "external ingress must use the same contract-aware z-order resolution");

    DragSourceList mixed;
    mixed.BindRuntimeOrigin(&source);
    mixed.hasDesktopIcons = true;
    mixed.hasFolderEntries = true;
    Check(!DragTargetResolver::AcceptsInternal(
            *upperPointer, mixed),
        "ambiguous mixed payload families must not bypass centralized classification");
}

void TestDragDropControllerOwnsTransportTransitions()
{
    namespace contract = snowdesktop::slot_contract;
    ContractContainer source(
        BarStyle::VBar,
        contract::SlotSurfaceKind::Desktop);
    DragSourceList sourceList;
    sourceList.BindRuntimeOrigin(&source);
    sourceList.hasDesktopIcons = true;

    DragSession session;
    session.Begin(
        &source, {}, std::move(sourceList),
        POINT{}, POINT{});
    DragDropController controller(session);

    controller.BeginSelfDrag();
    Check(controller.IsSelfDragActive() &&
            controller.IsTransportActive() &&
            !controller.SelfDragReturned() &&
            !controller.SelfDragNativeResumeRequested(),
        "starting a self OLE drag must reset and own the return state");
    controller.MarkSelfDragReturned();
    controller.ClearSelfDragReturned();
    Check(!controller.SelfDragReturned(),
        "leaving a self target before cancellation must clear the transient return state");
    controller.MarkSelfDragReturned();
    controller.RequestSelfDragNativeResume();
    controller.ClearSelfDragReturned();
    controller.EndSelfDrag();
    Check(!controller.IsTransportActive() &&
            controller.SelfDragReturned() &&
            controller.SelfDragNativeResumeRequested(),
        "ending self transport must preserve its latched native-resume result for the caller");

    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        controller.BeginSelfDrag();
        controller.MarkSelfDragReturned();
        controller.RequestSelfDragNativeResume();
        controller.EndSelfDrag();
        Check(!controller.IsTransportActive() &&
                controller.SelfDragReturned() &&
                controller.SelfDragNativeResumeRequested(),
            "repeated self OLE hand-backs must not accumulate or lose transport state");
    }

    controller.BeginExternalDrag({3});
    controller.ContinueExternalDrag();
    Check(controller.IsExternalDragActive() &&
            controller.ExternalSummary().fileCount == 3,
        "drag-over must preserve metadata captured by external drag-enter");

    std::vector<std::unique_ptr<Container>> containers;
    auto target = std::make_unique<ContractContainer>(
        BarStyle::VBar,
        contract::SlotSurfaceKind::Collection);
    Container* targetPointer = target.get();
    containers.push_back(std::move(target));
    const DragTargetResolution resolved =
        controller.ResolveInternalTarget(
            containers, POINT{50, 50});
    Check(resolved.container == targetPointer &&
            session.TargetContainer() == targetPointer &&
            session.TargetSlot() == resolved.slot &&
            session.TargetRegion() == resolved.region,
        "the application controller must atomically resolve and update the core session target");

    controller.EndExternalDrag();
    Check(!controller.IsTransportActive() &&
            controller.ExternalSummary().fileCount == 0,
        "ending external transport must clear transient ingress metadata");
}

void TestModelReloadDeferralCoversRetainedDragLifecycle()
{
    ContractContainer source(
        BarStyle::VBar,
        snowdesktop::slot_contract::SlotSurfaceKind::Desktop);
    ContractItem item(RECT{0, 0, 40, 40});
    DragSession session;
    DragDropController controller(session);
    const auto shouldDeferReload = [&]()
    {
        return snowdesktop::drag_input_rules::ShouldDeferModelReload(
            session.HasContext(), controller.IsTransportActive());
    };

    session.Begin(
        &source, {&item}, {}, POINT{}, POINT{});
    controller.BeginSelfDrag();
    session.DeactivateForDrop();
    Check(!session.IsActive() && session.HasContext() &&
            shouldDeferReload(),
        "a synchronous drop must defer model reload after the active flag clears");

    controller.EndSelfDrag();
    Check(shouldDeferReload(),
        "ending OLE first must still defer reload while the drop context is retained");
    session.End();
    Check(!shouldDeferReload(),
        "model reload may resume only after both drag owners have released state");

    int completedReloads = 0;
    bool reloadPending = false;
    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        session.Begin(
            &source, {&item}, {}, POINT{}, POINT{});
        controller.BeginSelfDrag();

        for (int request = 0; request < 4; ++request)
        {
            if (shouldDeferReload())
                reloadPending = true;
            else
                ++completedReloads;
        }

        if ((iteration & 1) == 0)
        {
            session.DeactivateForDrop();
            session.End();
            Check(shouldDeferReload(),
                "transport ownership must defer reload after the session ends first");
            controller.EndSelfDrag();
        }
        else
        {
            controller.EndSelfDrag();
            session.DeactivateForDrop();
            Check(shouldDeferReload(),
                "retained session context must defer reload after OLE ends first");
            session.End();
        }

        if (reloadPending && !shouldDeferReload())
        {
            reloadPending = false;
            ++completedReloads;
        }
    }
    Check(!reloadPending && completedReloads == 10000,
        "10000 alternating unwind orders must coalesce repeated reload requests and release each one exactly once");
}

void TestOwnedTransientDragTargetBoundsMemberWrappers()
{
    ContractContainer container(
        BarStyle::VBar,
        snowdesktop::slot_contract::
            SlotSurfaceKind::Collection);
    ContractItem sourceItem(RECT{0, 0, 40, 40});
    DragSession session;
    session.Begin(
        &container, {&sourceItem}, {}, POINT{}, POINT{});
    snowdesktop::OwnedTransientDragTarget target;
    std::array<Item*, 2> itemByIndex{};
    Slot* stableSlot = nullptr;
    int factoryCalls = 0;
    std::array<int, 3> memberIdentities{};
    bool slotStayedStable = true;
    bool itemStayedStable = true;
    bool sessionStayedBound = true;

    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        const std::size_t index =
            static_cast<std::size_t>(iteration % 2);
        const RECT bounds{
            static_cast<LONG>(index * 100), 0,
            static_cast<LONG>((index + 1) * 100), 100 };
        Slot* slot = target.BindHandoff(
            &container, bounds, index,
            SlotFeedbackRole::Popup,
            &memberIdentities[index],
            [&]() -> std::unique_ptr<Item> {
                ++factoryCalls;
                return std::make_unique<ContractItem>(bounds);
            });
        session.UpdateTarget(
            &container, slot, HitRegion::Handoff);
        if (!stableSlot)
            stableSlot = slot;
        else if (slot != stableSlot)
            slotStayedStable = false;
        if (!itemByIndex[index])
            itemByIndex[index] = slot->GetItem();
        else if (slot->GetItem() != itemByIndex[index])
            itemStayedStable = false;
        sessionStayedBound = sessionStayedBound &&
            session.TargetSlot() == stableSlot &&
            session.TargetSlot()->GetItem() == slot->GetItem();
    }

    Check(slotStayedStable && itemStayedStable &&
            sessionStayedBound &&
            factoryCalls == 2 &&
            target.OwnedItemCount() == 2,
        "10000 alternating popup handoff hits must retain one stable wrapper per logical member");

    Slot* thirdMember = target.BindHandoff(
        &container, RECT{200, 0, 300, 100}, 2,
        SlotFeedbackRole::Popup,
        &memberIdentities[2],
        [&]() -> std::unique_ptr<Item> {
            ++factoryCalls;
            return std::make_unique<ContractItem>(
                RECT{200, 0, 300, 100});
        });
    session.UpdateTarget(
        &container, thirdMember, HitRegion::Handoff);
    Slot* cachedSecondMember = target.BindHandoff(
        &container, RECT{100, 0, 200, 100}, 1,
        SlotFeedbackRole::Popup,
        &memberIdentities[1],
        [&]() -> std::unique_ptr<Item> {
            ++factoryCalls;
            return std::make_unique<ContractItem>(
                RECT{100, 0, 200, 100});
        });
    session.UpdateTarget(
        &container, cachedSecondMember,
        HitRegion::Handoff);
    Slot* evictedFirstMember = target.BindHandoff(
        &container, RECT{0, 0, 100, 100}, 0,
        SlotFeedbackRole::Popup,
        &memberIdentities[0],
        [&]() -> std::unique_ptr<Item> {
            ++factoryCalls;
            return std::make_unique<ContractItem>(
                RECT{0, 0, 100, 100});
        });
    session.UpdateTarget(
        &container, evictedFirstMember,
        HitRegion::Handoff);
    Check(thirdMember == stableSlot &&
            cachedSecondMember == stableSlot &&
            evictedFirstMember == stableSlot &&
            session.TargetSlot() == stableSlot &&
            session.TargetSlot()->GetItem() != nullptr &&
            factoryCalls == 4 &&
            target.OwnedItemCount() == 2,
        "walking across three popup members must evict inactive wrappers while keeping the active session target valid");

    Slot* placement = target.BindPlacement(
        &container, RECT{0, 0, 100, 100}, 0,
        SlotFeedbackRole::Popup);
    session.UpdateTarget(
        &container, placement, HitRegion::SortBefore);
    Check(placement == stableSlot &&
            placement->GetItem() == nullptr &&
            session.TargetSlot() == placement &&
            target.OwnedItemCount() == 2,
        "leaving handoff must detach the Slot without invalidating bounded cached members");

    bool generationSlotStayedStable = true;
    bool generationCacheStayedBounded = true;
    for (int generation = 0; generation < 10000; ++generation)
    {
        container.InvalidateSlots();
        Slot* nextGeneration = target.BindHandoff(
            &container, RECT{0, 0, 100, 100}, 0,
            SlotFeedbackRole::Popup,
            &memberIdentities[0],
            [&]() -> std::unique_ptr<Item> {
                ++factoryCalls;
                return std::make_unique<ContractItem>(
                    RECT{0, 0, 100, 100});
            });
        session.UpdateTarget(
            &container, nextGeneration,
            HitRegion::Handoff);
        generationSlotStayedStable =
            generationSlotStayedStable &&
            nextGeneration == stableSlot &&
            nextGeneration->GetItem() != nullptr &&
            session.TargetSlot() == stableSlot &&
            session.TargetSlot()->GetItem() ==
                nextGeneration->GetItem();
        generationCacheStayedBounded =
            generationCacheStayedBounded &&
            target.OwnedItemCount() == 1;
    }
    Check(generationSlotStayedStable &&
            generationCacheStayedBounded &&
            factoryCalls == 10004,
        "10000 container generations must replace the previous epoch without accumulating handoff wrappers");

    Item* committedTargetItem =
        session.TargetSlot()->GetItem();
    session.DeactivateForDrop();
    Check(!session.IsActive() &&
            session.HasContext() &&
            session.TargetSlot() == stableSlot &&
            session.TargetSlot()->GetItem() ==
                committedTargetItem,
        "deactivating for a synchronous drop must retain the transient target until the commit context is detached");
    session.UpdateTarget(
        nullptr, nullptr, HitRegion::None);
    target.Reset();
    Check(session.TargetSlot() == nullptr &&
            target.Get() == nullptr &&
            target.OwnedItemCount() == 0,
        "popup teardown must detach an inactive drop context before destroying the transient target");
    session.End();
}

void TestQueuedNativeDragMovesCoalesceAtOrderingBarriers()
{
    ContractContainer source(
        BarStyle::VBar,
        snowdesktop::slot_contract::SlotSurfaceKind::Desktop);
    ContractItem item(RECT{0, 0, 40, 40});
    DragSourceList sourceList;
    sourceList.BindRuntimeOrigin(&source);
    sourceList.hasDesktopIcons = true;

    DragSession session;
    session.Begin(
        &source, {&item}, std::move(sourceList),
        POINT{}, POINT{});
    DragDropController controller(session);
    const bool nativeDragActive =
        snowdesktop::drag_input_rules::IsNativeDragActive(
            session.IsActive(), controller.IsTransportActive());

    const HWND mainWindow = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(1));
    const HWND floatingDock = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(2));
    const HWND floatingPopup = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(3));
    std::deque<MSG> queue;
    LPARAM moveOrdinal = 0;
    const auto appendMoves = [&](HWND window, int count) {
        for (int index = 0; index < count; ++index)
        {
            MSG message{};
            message.hwnd = window;
            message.message = WM_MOUSEMOVE;
            message.lParam = moveOrdinal++;
            queue.push_back(message);
        }
    };
    const auto appendBarrier = [&](HWND window, UINT kind, LPARAM token) {
        MSG message{};
        message.hwnd = window;
        message.message = kind;
        message.lParam = token;
        queue.push_back(message);
    };

    appendMoves(mainWindow, 4000);
    appendBarrier(mainWindow, WM_TIMER, -1);
    appendMoves(mainWindow, 3000);
    appendBarrier(mainWindow, WM_KEYDOWN, -2);
    queue.back().wParam = VK_ESCAPE;
    appendMoves(floatingDock, 2000);
    appendMoves(floatingPopup, 1000);
    appendBarrier(floatingPopup, WM_LBUTTONUP, -3);

    std::vector<MSG> dispatched;
    std::size_t coalesced = 0;
    while (!queue.empty())
    {
        MSG current = queue.front();
        queue.pop_front();
        const bool pointerSurface =
            snowdesktop::drag_input_rules::
                IsLatencySensitivePointerMessageSurface(
                    current.hwnd == mainWindow,
                    current.hwnd == floatingDock,
                    current.hwnd == floatingPopup);
        coalesced += snowdesktop::drag_input_rules::
            CoalesceQueuedMouseMoves(
                nativeDragActive,
                pointerSurface,
                current,
                [&](MSG& next) {
                    if (queue.empty()) return false;
                    next = queue.front();
                    return true;
                },
                [&](MSG& next) {
                    if (queue.empty()) return false;
                    next = queue.front();
                    queue.pop_front();
                    return true;
                },
                [](const MSG& left, const MSG& right) {
                    return left.hwnd == right.hwnd;
                },
                [](const MSG& message) {
                    return message.message == WM_MOUSEMOVE;
                });
        dispatched.push_back(current);
    }

    Check(nativeDragActive && moveOrdinal == 10000 &&
            coalesced == 9996 && dispatched.size() == 7,
        "a long native drag queue must collapse 10000 moves to one move per ordered segment");
    Check(dispatched.size() == 7 &&
            dispatched[0].message == WM_MOUSEMOVE &&
            dispatched[0].hwnd == mainWindow &&
            dispatched[0].lParam == 3999 &&
            dispatched[1].message == WM_TIMER &&
            dispatched[2].message == WM_MOUSEMOVE &&
            dispatched[2].lParam == 6999 &&
            dispatched[3].message == WM_KEYDOWN &&
            dispatched[3].wParam == VK_ESCAPE &&
            dispatched[4].message == WM_MOUSEMOVE &&
            dispatched[4].hwnd == floatingDock &&
            dispatched[4].lParam == 8999 &&
            dispatched[5].message == WM_MOUSEMOVE &&
            dispatched[5].hwnd == floatingPopup &&
            dispatched[5].lParam == 9999 &&
            dispatched[6].message == WM_LBUTTONUP,
        "move coalescing must preserve timer, Escape, cross-window, and button-up ordering barriers");
}

void TestSelfOleReturnCancelsTransportBeforeNativeResume()
{
    using snowdesktop::ole_drag_rules::SelfOleUnwindAction;
    using snowdesktop::ole_drag_rules::SelectSelfOleUnwindAction;

    ContractContainer source(
        BarStyle::VBar,
        snowdesktop::slot_contract::SlotSurfaceKind::Desktop);
    ContractItem item(RECT{0, 0, 40, 40});
    DragSourceList sourceList;
    sourceList.BindRuntimeOrigin(&source);
    sourceList.hasDesktopIcons = true;
    DragSession session;
    session.Begin(
        &source, {&item}, std::move(sourceList),
        POINT{}, POINT{});
    DragDropController controller(session);

    controller.BeginSelfDrag();
    controller.MarkSelfDragReturned();
    const HRESULT heldReturn = controller.QueryContinueSelfDrag(
        false, true, true);
    Check(heldReturn == DRAGDROP_S_CANCEL &&
            controller.SelfDragNativeResumeRequested() &&
            SelectSelfOleUnwindAction(
                true, true, true, true) ==
                SelfOleUnwindAction::ResumeNativeHeld,
        "an internal return with the button held must cancel OLE and latch native resume");
    controller.EndSelfDrag();
    Check(!controller.IsTransportActive() &&
            snowdesktop::drag_input_rules::IsNativeDragActive(
                session.IsActive(), controller.IsTransportActive()),
        "native input must become eligible only after self OLE transport has ended");

    controller.BeginSelfDrag();
    controller.MarkSelfDragReturned();
    const HRESULT releasedReturn = controller.QueryContinueSelfDrag(
        false, false, true);
    Check(releasedReturn == DRAGDROP_S_CANCEL &&
            controller.SelfDragNativeResumeRequested() &&
            SelectSelfOleUnwindAction(
                true, true, false, true) ==
                SelfOleUnwindAction::ReleaseNative,
        "a button release during internal return must cancel OLE before one native release");

    controller.BeginSelfDrag();
    controller.MarkSelfDragReturned();
    const HRESULT escaped = controller.QueryContinueSelfDrag(
        true, true, true);
    Check(escaped == DRAGDROP_S_CANCEL &&
            !controller.SelfDragNativeResumeRequested() &&
            SelectSelfOleUnwindAction(
                false, true, true, true) ==
                SelfOleUnwindAction::FinishOle,
        "Escape must cancel OLE without latching a native resume or release");

    controller.BeginSelfDrag();
    controller.MarkSelfDragReturned();
    controller.ClearSelfDragReturned();
    Check(controller.QueryContinueSelfDrag(
              false, true, true) == S_OK &&
            !controller.SelfDragNativeResumeRequested(),
        "a cleared transient return must continue OLE instead of resuming native input");
    Check(controller.QueryContinueSelfDrag(
              false, false, false) == DRAGDROP_S_DROP &&
            !controller.SelfDragNativeResumeRequested(),
        "a release outside SnowDesktop must remain an OLE drop");

    Check(SelectSelfOleUnwindAction(
              true, false, true, true) ==
                SelfOleUnwindAction::RestartOle &&
            SelectSelfOleUnwindAction(
              true, false, false, true) ==
                SelfOleUnwindAction::FinishOle &&
            SelectSelfOleUnwindAction(
              true, true, true, false) ==
                SelfOleUnwindAction::FinishOle,
        "OLE unwind must restart only for an active held gesture that crossed outside again");
}

void TestOleAdapterOwnsComBoundary()
{
    FakeOleDragDropHandler handler;
    auto* adapter = new OleDragDropAdapter(&handler);

    IDropTarget* target = nullptr;
    IDropSource* source = nullptr;
    void* unsupported = reinterpret_cast<void*>(1);
    Check(adapter->QueryInterface(IID_IDropTarget,
            reinterpret_cast<void**>(&target)) == S_OK && target,
        "OLE adapter must expose IDropTarget independently of DesktopApp");
    Check(adapter->QueryInterface(IID_IDropSource,
            reinterpret_cast<void**>(&source)) == S_OK && source,
        "OLE adapter must expose IDropSource independently of DesktopApp");
    Check(adapter->QueryInterface(IID_IStream, &unsupported) ==
            E_NOINTERFACE && unsupported == nullptr,
        "OLE adapter must reject unrelated COM interfaces");

    auto* dataObject = reinterpret_cast<IDataObject*>(
        static_cast<std::uintptr_t>(1));
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    const POINTL point{123, 456};
    Check(target->DragEnter(dataObject, MK_CONTROL, point, &effect) == S_OK &&
            handler.dragEnterCount == 1 &&
            handler.lastDataObject == dataObject &&
            handler.lastKeyState == MK_CONTROL &&
            handler.lastPoint.x == point.x &&
            handler.lastPoint.y == point.y &&
            effect == DROPEFFECT_LINK,
        "OLE target callback must forward arguments and effects once");
    effect = DROPEFFECT_COPY;
    Check(target->DragOver(MK_SHIFT, POINTL{7, 8}, &effect) == S_FALSE &&
            handler.dragOverCount == 1 && effect == DROPEFFECT_MOVE,
        "OLE drag-over result must come from the application handler");
    Check(target->DragLeave() == S_OK && handler.dragLeaveCount == 1,
        "OLE drag-leave must be forwarded once");
    Check(source->QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK &&
            handler.queryContinueCount == 1,
        "OLE source continuation must be delegated to the handler");
    Check(source->GiveFeedback(DROPEFFECT_COPY) ==
            DRAGDROP_S_USEDEFAULTCURSORS &&
            handler.feedbackCount == 1 &&
            handler.lastEffect == DROPEFFECT_COPY,
        "OLE feedback must be delegated without DesktopApp COM identity");

    adapter->Detach();
    Check(target->DragOver(0, POINTL{}, &effect) == E_UNEXPECTED &&
            source->QueryContinueDrag(FALSE, MK_LBUTTON) ==
                DRAGDROP_S_CANCEL,
        "detached adapters must fail safely after the application is gone");

    target->Release();
    source->Release();
    adapter->Release();
}

void TestTrayCallbackClassification()
{
    Check(TrayIconController::ClassifyCallback(
            MAKELPARAM(WM_CONTEXTMENU, 0)) ==
            TrayCallbackAction::ShowContextMenu &&
            TrayIconController::ClassifyCallback(
                MAKELPARAM(WM_RBUTTONUP, 0)) ==
            TrayCallbackAction::ShowContextMenu,
        "tray context-menu callbacks must share one application action");
    Check(TrayIconController::ClassifyCallback(
            MAKELPARAM(WM_LBUTTONDBLCLK, 0)) ==
            TrayCallbackAction::ReloadItems,
        "tray double-click must map to the reload action");
    Check(TrayIconController::ClassifyCallback(
            MAKELPARAM(WM_MOUSEMOVE, 0)) ==
            TrayCallbackAction::None,
        "unhandled tray notifications must not leak into application behavior");
}

void TestSelectionControllerCoversEveryRegisteredRange()
{
    SelectionController controller;
    std::vector<SelectionFixtureEntry> desktop{{true}, {false}};
    std::vector<SelectionFixtureEntry> dock{{true}};
    std::vector<SelectionFixtureEntry> running{{true}};
    std::vector<SelectionFixtureWidget> widgets{
        {true, {{true}, {false}}},
        {false, {{true}}},
    };

    Check(controller.ClearAll(
            desktop, dock, running, widgets),
        "clearing selection must report a mutation across registered ranges");
    const auto allClear = [](const auto& range) {
        return std::all_of(
            range.begin(), range.end(),
            [](const auto& value) {
                return !value.selected;
            });
    };
    Check(allClear(desktop) && allClear(dock) &&
            allClear(running) &&
            allClear(widgets) &&
            allClear(widgets[0].folderEntries) &&
            allClear(widgets[1].folderEntries),
        "one clear operation must cover desktop, dock, running-app, widget, and nested-entry slots");
    const std::uint64_t clearedRevision =
        controller.Revision();
    Check(!controller.ClearAll(
            desktop, dock, running, widgets) &&
            controller.Revision() == clearedRevision,
        "idempotent selection clears must not create false revisions");

    Check(controller.SelectDesktop(desktop, 1) &&
            desktop[1].selected &&
            controller.ToggleDesktop(desktop, 1) &&
            !desktop[1].selected,
        "desktop selection and toggle must share controller revision semantics");
    widgets[1].folderEntries[0].selected = true;
    Check(controller.SelectWidget(widgets, 1) &&
            !widgets[0].selected &&
            widgets[1].selected &&
            !widgets[1].folderEntries[0].selected,
        "selecting one widget must clear nested entry selection on every widget");
}

void TestRenameControllerKeepsTargetsExclusive()
{
    RenameController controller;
    controller.BeginWidget(4);
    Check(controller.IsWidget() &&
            controller.Index() == 4 &&
            controller.OwnerIndex() ==
                RenameController::InvalidIndex &&
            controller.BlocksScrolling(),
        "widget rename must expose one unambiguous target and lock scrolling");

    controller.BeginFolderEntry(2, 7);
    Check(controller.IsFolderEntry() &&
            !controller.IsWidget() &&
            controller.OwnerIndex() == 2 &&
            controller.Index() == 7,
        "starting a folder-entry rename must replace every prior target");
    controller.SetQuickNavigationPresentation(true);
    Check(controller.IsQuickNavigationPresentation() &&
            controller.BlocksScrolling(),
        "quick navigation rename presentation must keep scrolling locked");

    controller.BeginDockFolderEntry(3);
    Check(controller.IsDockFolderEntry() &&
            controller.OwnerIndex() ==
                RenameController::InvalidIndex &&
            !controller.IsQuickNavigationPresentation(),
        "dock popup entries must not retain stale owner or presentation state");

    controller.Reset();
    controller.SetQuickNavigationPresentation(true);
    Check(!controller.IsActive() &&
            !controller.IsQuickNavigationPresentation() &&
            !controller.BlocksScrolling(),
        "an inactive rename cannot acquire presentation state or lock scrolling");
}

void TestRenameControllerRejectsStaleFocusCommits()
{
    RenameController controller;
    Check(!controller.MatchesSession(0),
        "an inactive editor must reject a queued commit");

    controller.BeginDesktopItem(2);
    const auto firstSession = controller.SessionId();
    Check(controller.MatchesSession(firstSession),
        "the active editor must accept its own focus-loss notification");
    controller.Reset();
    Check(!controller.MatchesSession(firstSession),
        "a pointer commit must retire the old focus-loss notification");

    // Reopening even the same item must not consume its previous EDIT's
    // queued notification, regardless of native HWND reuse.
    controller.BeginDesktopItem(2);
    Check(!controller.MatchesSession(firstSession) &&
            controller.MatchesSession(controller.SessionId()),
        "reopening the same item must reject the previous editor's commit");
    const auto secondSession = controller.SessionId();
    controller.BeginDockFolderEntry(3);
    Check(!controller.MatchesSession(secondSession) &&
            controller.MatchesSession(controller.SessionId()),
        "switching rename surfaces must invalidate the previous session");
}

void TestPopupDwellControllerHandlesCandidateChanges()
{
    PopupDwellController controller;
    Check(controller.IsIdle(),
        "a new popup dwell controller must be idle");
    Check(controller.Track(4, 100) &&
            !controller.IsIdle() &&
            !controller.IsReady(149, 50) &&
            controller.IsReady(150, 50),
        "popup dwell must mature only after the configured delay");
    Check(!controller.Track(4, 140) &&
            controller.IsReady(150, 50),
        "repeated hover samples must not restart the same candidate timer");
    Check(controller.Track(7, 151) &&
            controller.Candidate() == 7 &&
            !controller.IsReady(199, 50),
        "changing popup candidate must atomically restart dwell timing");
    Check(!controller.CancelIfOccluded(false) &&
            controller.Candidate() == 7,
        "an unobscured dwell candidate must remain active");
    Check(controller.CancelIfOccluded(true) &&
            controller.Candidate() ==
                PopupDwellController::NoCandidate &&
            !controller.IsReady(1000, 0),
        "a foreground popup must cancel the opener tile hidden below it");
    controller.Track(9, 200);
    controller.Reset();
    Check(controller.Candidate() ==
            PopupDwellController::NoCandidate &&
            controller.IsIdle() &&
            !controller.IsReady(1000, 0),
        "reset popup dwell must remove both candidate and readiness");
}
}

int main()
{
    TestSlotCacheAndIdentity();
    TestHitRegionsUseContainerOrientation();
    TestExecuteDropDelegatesOnce();
    TestDragSessionRejectsInvalidatedSlots();
    TestDragSessionVisualVisibilityFollowsOleOwnership();
    TestListDragCanAnchorVisualAndLandingToPointer();
    TestDesktopPlacementPolicyMatchesSourceSemantics();
    TestEverySurfaceRetainsStableDragMetadata();
    TestEveryRegisteredSurfaceOriginLifecycle();
    TestDropActionModifiers();
    TestEveryDragSourceSurvivesPageTurnRebindMatrix();
    TestDragTargetResolutionUsesContractAndZOrder();
    TestDragDropControllerOwnsTransportTransitions();
    TestModelReloadDeferralCoversRetainedDragLifecycle();
    TestOwnedTransientDragTargetBoundsMemberWrappers();
    TestQueuedNativeDragMovesCoalesceAtOrderingBarriers();
    TestSelfOleReturnCancelsTransportBeforeNativeResume();
    TestOleAdapterOwnsComBoundary();
    TestTrayCallbackClassification();
    TestSelectionControllerCoversEveryRegisteredRange();
    TestRenameControllerKeepsTargetsExclusive();
    TestRenameControllerRejectsStaleFocusCommits();
    TestPopupDwellControllerHandlesCandidateChanges();
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
