#include "drag_session.h"

#include "container.h"
#include "item.h"

#include <algorithm>
#include <utility>

std::optional<snowdesktop::drag_target::SlotFeedbackKey>
snowdesktop::drag_target::SlotFeedbackKey::From(Slot* slot)
{
    if (!slot) return std::nullopt;
    const auto& identity = slot->GetFeedbackIdentity();
    if (identity.role == SlotFeedbackRole::None)
        return std::nullopt;
    return SlotFeedbackKey{
        identity.role,
        slot->GetBounds(),
        identity.scopeBounds,
        slot->GetIndex(),
        identity.revision,
        identity.variant,
        slot->GetItem() != nullptr,
    };
}

void DragSession::Begin(Container* source, std::vector<Item*> items,
    DragSourceList sourceList, POINT mouseDown, POINT current)
{
    active_ = true;
    visualVisible_ = true;
    hasContext_ = true;
    source_ = source;
    items_ = std::move(items);
    sourceList_ = std::move(sourceList);
    if (sourceList_.hasSearchResults)
    {
        items_.clear();
        for (auto& entry : sourceList_.entries)
        {
            if (entry.retainedItem) entry.item = entry.retainedItem.get();
            if (entry.item) items_.push_back(entry.item);
        }
    }
    mouseDownPoint_ = mouseDown;
    visualMouseDownPoint_ = mouseDown;
    currentPoint_ = current;
    visualItemBounds_.clear();
    primaryVisualIndex_ = static_cast<size_t>(-1);
    pointerAnchored_ = false;
    visualBoundsOffset_ = {};
    action_ = DropAction::Move;
    targetContainer_ = nullptr;
    targetSlot_ = nullptr;
    targetSlotGeneration_ = 0;
    targetSlotGenerationTracked_ = false;
    targetSlotFeedbackKey_ = {};
    targetRegion_ = HitRegion::None;
    presentationAnchorValid_ = false;
    presentationAnchorCell_ = {};
    InvalidateStaticScene();
}

bool DragSession::UpdateActionFromMods(int mods, DropAction defaultAction)
{
    DropAction next = DropActionFromMods(mods, defaultAction);
    if (next == action_) return false;
    action_ = next;
    InvalidateStaticScene();
    return true;
}

bool DragSession::UpdateTarget(Container* targetContainer, Slot* targetSlot,
    HitRegion targetRegion)
{
    const bool nextGenerationTracked =
        targetContainer && targetSlot &&
        !targetSlot->IsTransientDragTarget();
    const std::uint64_t nextGeneration =
        nextGenerationTracked
            ? targetContainer->GetSlotGeneration()
            : 0;
    const auto nextFeedbackKey =
        snowdesktop::drag_target::SlotFeedbackKey::From(targetSlot);
    const bool slotChanged =
        targetSlotFeedbackKey_ || nextFeedbackKey
            ? targetSlotFeedbackKey_ != nextFeedbackKey
            : targetSlot_ != targetSlot ||
                targetSlotGeneration_ != nextGeneration ||
                targetSlotGenerationTracked_ != nextGenerationTracked;
    const bool presentationChanged =
        targetContainer_ != targetContainer ||
        slotChanged ||
        targetRegion_ != targetRegion;
    targetContainer_ = targetContainer;
    targetSlot_ = targetSlot;
    targetSlotGeneration_ = nextGeneration;
    targetSlotGenerationTracked_ = nextGenerationTracked;
    targetSlotFeedbackKey_ = nextFeedbackKey;
    targetRegion_ = targetRegion;
    if (presentationChanged)
        InvalidatePresentation();
    return presentationChanged;
}

bool DragSession::UpdatePresentationAnchor(const GridCell& cell)
{
    if (cell.pageId.empty())
        return ClearPresentationAnchor();
    const bool changed =
        !presentationAnchorValid_ ||
        presentationAnchorCell_.pageId != cell.pageId ||
        presentationAnchorCell_.column != cell.column ||
        presentationAnchorCell_.row != cell.row;
    presentationAnchorValid_ = true;
    presentationAnchorCell_ = cell;
    if (changed)
        InvalidatePresentation();
    return changed;
}

bool DragSession::ClearPresentationAnchor()
{
    if (!presentationAnchorValid_)
        return false;
    presentationAnchorValid_ = false;
    presentationAnchorCell_ = {};
    InvalidatePresentation();
    return true;
}

void DragSession::RebindSource(Container* source, std::vector<Item*> items,
    DragSourceList sourceList)
{
    source_ = source;
    items_ = std::move(items);
    // A widget on the page being replaced can only recreate its logical
    // member wrappers after the turn; their new layout bounds are empty
    // because the source page is no longer visible.  Keep the drag-start
    // bounds on those transient wrappers so the existing visual snapshot
    // and hit-coordinate context remain usable until the drop completes.
    const size_t retainedBounds = std::min(
        items_.size(), visualItemBounds_.size());
    for (size_t i = 0; i < retainedBounds; ++i)
    {
        if (!items_[i])
            continue;
        const RECT currentBounds = items_[i]->GetBounds();
        const RECT retained = visualItemBounds_[i];
        const bool currentHasArea =
            currentBounds.right > currentBounds.left &&
            currentBounds.bottom > currentBounds.top;
        const bool retainedHasArea =
            retained.right > retained.left &&
            retained.bottom > retained.top;
        if (currentHasArea || !retainedHasArea)
        {
            continue;
        }
        items_[i]->SetBounds(retained);
    }
    sourceList_ = std::move(sourceList);
    targetContainer_ = nullptr;
    targetSlot_ = nullptr;
    targetSlotGeneration_ = 0;
    targetSlotGenerationTracked_ = false;
    targetSlotFeedbackKey_ = {};
    targetRegion_ = HitRegion::None;
    presentationAnchorValid_ = false;
    presentationAnchorCell_ = {};
    InvalidateStaticScene();
}

void DragSession::DetachRuntimeBindings()
{
    if (!active_) return;

    source_ = nullptr;
    items_.clear();
    sourceList_.origin = nullptr;
    for (auto& entry : sourceList_.entries)
        entry.item = nullptr;
    targetContainer_ = nullptr;
    targetSlot_ = nullptr;
    targetSlotGeneration_ = 0;
    targetSlotGenerationTracked_ = false;
    targetSlotFeedbackKey_ = {};
    targetRegion_ = HitRegion::None;
    presentationAnchorValid_ = false;
    presentationAnchorCell_ = {};
    InvalidateStaticScene();
}

void DragSession::DeactivateForDrop()
{
    if (!active_) return;
    active_ = false;
    visualVisible_ = false;
    InvalidateStaticScene();
}

bool DragSession::ResumeAfterDropDecision()
{
    if (!hasContext_) return false;
    active_ = true;
    visualVisible_ = false;
    InvalidateStaticScene();
    return true;
}

void DragSession::InvalidateStaticScene()
{
    ++staticSceneRevision_;
    if (staticSceneRevision_ == 0)
        staticSceneRevision_ = 1;
    InvalidatePresentation();
}

void DragSession::End()
{
    active_ = false;
    visualVisible_ = false;
    hasContext_ = false;
    source_ = nullptr;
    items_.clear();
    visualItemBounds_.clear();
    sourceList_ = {};
    primaryVisualIndex_ = static_cast<size_t>(-1);
    targetContainer_ = nullptr;
    targetSlot_ = nullptr;
    targetSlotGeneration_ = 0;
    targetSlotGenerationTracked_ = false;
    targetSlotFeedbackKey_ = {};
    targetRegion_ = HitRegion::None;
    presentationAnchorValid_ = false;
    presentationAnchorCell_ = {};
    action_ = DropAction::Move;
    mouseDownPoint_ = {};
    visualMouseDownPoint_ = {};
    currentPoint_ = {};
    pointerAnchored_ = false;
    visualBoundsOffset_ = {};
    InvalidateStaticScene();
}

void DragSession::InvalidatePresentation()
{
    ++presentationRevision_;
    if (presentationRevision_ == 0)
        presentationRevision_ = 1;
}

bool DragSession::TargetSlotGenerationIsCurrent() const
{
    if (!targetSlot_)
        return true;
    if (!targetSlotGenerationTracked_)
        return true;
    return targetContainer_ &&
        targetContainer_->GetSlotGeneration() ==
            targetSlotGeneration_;
}
