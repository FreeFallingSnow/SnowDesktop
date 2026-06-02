#pragma once

#include "drop_model.h"

#include <cstdint>
#include <utility>

class DragSession
{
public:
    bool IsActive() const { return active_; }
    Container* Source() const { return source_; }
    const std::vector<Item*>& Items() const { return items_; }
    std::vector<Item*>& Items() { return items_; }
    const DragSourceList& SourceList() const { return sourceList_; }
    DragSourceList& SourceList() { return sourceList_; }
    POINT MouseDownPoint() const { return mouseDownPoint_; }
    POINT CurrentPoint() const { return currentPoint_; }
    DropAction Action() const { return action_; }
    bool IsMoveAction() const { return action_ == DropAction::Move; }
    Container* TargetContainer() const { return targetContainer_; }
    Slot* TargetSlot() const { return targetSlot_; }
    HitRegion TargetRegion() const { return targetRegion_; }
    std::uint64_t StaticSceneRevision() const { return staticSceneRevision_; }

    void Begin(Container* source, std::vector<Item*> items, DragSourceList sourceList,
        POINT mouseDown, POINT current)
    {
        active_ = true;
        source_ = source;
        items_ = std::move(items);
        sourceList_ = std::move(sourceList);
        mouseDownPoint_ = mouseDown;
        currentPoint_ = current;
        action_ = DropAction::Move;
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetRegion_ = HitRegion::None;
        InvalidateStaticScene();
    }

    void UpdatePoint(POINT current)
    {
        currentPoint_ = current;
    }

    bool UpdateActionFromMods(int mods, DropAction defaultAction = DropAction::Move)
    {
        DropAction next = DropActionFromMods(mods, defaultAction);
        if (next == action_) return false;
        action_ = next;
        InvalidateStaticScene();
        return true;
    }

    void UpdateTarget(Container* targetContainer, Slot* targetSlot, HitRegion targetRegion)
    {
        targetContainer_ = targetContainer;
        targetSlot_ = targetSlot;
        targetRegion_ = targetRegion;
    }

    void RebindSource(Container* source, std::vector<Item*> items, DragSourceList sourceList)
    {
        source_ = source;
        items_ = std::move(items);
        sourceList_ = std::move(sourceList);
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetRegion_ = HitRegion::None;
        InvalidateStaticScene();
    }

    void InvalidateStaticScene()
    {
        ++staticSceneRevision_;
        if (staticSceneRevision_ == 0)
            staticSceneRevision_ = 1;
    }

    void End()
    {
        active_ = false;
        source_ = nullptr;
        items_.clear();
        sourceList_ = {};
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetRegion_ = HitRegion::None;
        action_ = DropAction::Move;
        mouseDownPoint_ = {};
        currentPoint_ = {};
        InvalidateStaticScene();
    }

private:
    bool active_ = false;
    Container* source_ = nullptr;
    std::vector<Item*> items_;
    DragSourceList sourceList_;
    POINT mouseDownPoint_{};
    POINT currentPoint_{};
    DropAction action_ = DropAction::Move;
    Container* targetContainer_ = nullptr;
    Slot* targetSlot_ = nullptr;
    HitRegion targetRegion_ = HitRegion::None;
    std::uint64_t staticSceneRevision_ = 1;
};
