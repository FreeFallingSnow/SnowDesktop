#include "drag_target_resolver.h"

namespace
{
template <typename Accepts>
DragTargetResolution Resolve(
    const std::vector<std::unique_ptr<Container>>& containers,
    POINT point,
    const DragTargetResolver::CandidateFilter& filter,
    Accepts&& accepts)
{
    for (auto iterator = containers.rbegin();
        iterator != containers.rend(); ++iterator)
    {
        Container* candidate = iterator->get();
        if (!candidate || (filter && !filter(*candidate)) ||
            !accepts(*candidate))
        {
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
        });
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
