#include "drag_target_resolver.h"

namespace
{
template <typename Accepts>
DragTargetResolution Resolve(
    const std::vector<std::unique_ptr<Container>>& containers,
    POINT point,
    const DragTargetResolver::CandidateFilter& filter,
    Accepts&& accepts, bool blockRejected = false)
{
    for (auto iterator = containers.rbegin();
        iterator != containers.rend(); ++iterator)
    {
        Container* candidate = iterator->get();
        if (!candidate || (filter && !filter(*candidate)))
        {
            continue;
        }
        if (!accepts(*candidate))
        {
            if (blockRejected)
            {
                Slot* rejectedSlot = nullptr;
                if (candidate->HitTestDrag(point, rejectedSlot) != HitRegion::None)
                    return {candidate, nullptr, HitRegion::Blocked};
            }
            continue;
        }
        Slot* slot = nullptr;
        const HitRegion region = candidate->HitTestDrag(point, slot);
        if (region != HitRegion::None)
            return {candidate, slot, region};
    }
    return {};
}
}

bool DragTargetResolver::AcceptsInternal(
    const Container& target,
    const DragSourceList& source)
{
    namespace contract = snowdesktop::slot_contract;
    if (source.HasDesktopWidgetIcons())
    {
        const auto files = source.WithoutDesktopWidgetIcons();
        const auto targetSurface = target.GetSlotSurfaceKind();
        if (targetSurface != contract::SlotSurfaceKind::Desktop &&
            targetSurface != contract::SlotSurfaceKind::Dock)
            return AcceptsInternal(target, files);
        if (!files.Empty() && !AcceptsInternal(target, files)) return false;
        for (const auto& entry : source.entries)
        {
            if (!entry.desktopWidgetIcon) continue;
            DragSourceList single = source;
            single.entries = {entry};
            single.entries.front().desktopWidgetIcon = false;
            single.hasDesktopIcons = single.hasFolderEntries = single.hasExternalFiles = false;
            single.hasCollectionGroupEntries = single.hasFileGroupEntries = false;
            single.hasFileGroupSourceLabels = false;
            single.hasWidgets = true;
            if (!AcceptsInternal(target, single) ||
                !target.AcceptsDragPayload(single.SlotPayloadKind(), source.entries.size()))
                return false;
        }
        return true;
    }
    const auto payload = source.SlotPayloadKind();
    if (payload == contract::DragPayloadKind::Count)
        return false;
    const auto sourceSurface = source.SourceSurfaceKind();
    const auto targetSurface = target.GetSlotSurfaceKind();
    const auto relation = contract::ClassifyRelation(
        sourceSurface, targetSurface, source.origin == &target);
    return contract::AcceptsSlotDrop(
            sourceSurface, payload, targetSurface, relation) &&
        target.AcceptsDragPayload(payload, source.entries.size());
}

bool DragTargetResolver::AcceptsExternal(const Container& target,
    std::size_t itemCount)
{
    namespace contract = snowdesktop::slot_contract;
    return contract::AcceptsSlotDrop(
        contract::SlotSurfaceKind::External,
        contract::DragPayloadKind::ExternalFile,
        target.GetSlotSurfaceKind(),
        contract::DragRelation::ExternalIngress) &&
        target.AcceptsDragPayload(
            contract::DragPayloadKind::ExternalFile, itemCount);
}

DragTargetResolution DragTargetResolver::ResolveInternal(
    const std::vector<std::unique_ptr<Container>>& containers,
    POINT point,
    const DragSourceList& source,
    const CandidateFilter& filter)
{
    return Resolve(containers, point, filter,
        [&](const Container& candidate) {
            return AcceptsInternal(candidate, source);
        }, source.HasDesktopWidgetIcons());
}

DragTargetResolution DragTargetResolver::ResolveExternal(
    const std::vector<std::unique_ptr<Container>>& containers,
    POINT point,
    const CandidateFilter& filter,
    std::size_t itemCount)
{
    return Resolve(containers, point, filter,
        [itemCount](const Container& candidate) {
            return AcceptsExternal(candidate, itemCount);
        });
}
