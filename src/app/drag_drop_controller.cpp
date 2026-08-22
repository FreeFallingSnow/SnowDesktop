#include "drag_drop_controller.h"

#include <algorithm>
#include <utility>

void DragDropController::BeginSelfDrag()
{
    transport_ = Transport::SelfOle;
    selfDragReturned_ = false;
    selfDragNativeResumeRequested_ = false;
    externalSummary_ = {};
}

void DragDropController::MarkSelfDragReturned()
{
    if (transport_ == Transport::SelfOle)
        selfDragReturned_ = true;
}

void DragDropController::ClearSelfDragReturned()
{
    if (transport_ == Transport::SelfOle &&
        !selfDragNativeResumeRequested_)
    {
        selfDragReturned_ = false;
    }
}

void DragDropController::RequestSelfDragNativeResume()
{
    if (transport_ == Transport::SelfOle &&
        selfDragReturned_)
    {
        selfDragNativeResumeRequested_ = true;
    }
}

void DragDropController::EndSelfDrag()
{
    if (transport_ == Transport::SelfOle)
        transport_ = Transport::None;
}

bool DragDropController::IsSelfDragActive() const
{
    return transport_ == Transport::SelfOle;
}

bool DragDropController::SelfDragReturned() const
{
    return selfDragReturned_;
}

bool DragDropController::SelfDragNativeResumeRequested() const
{
    return selfDragNativeResumeRequested_;
}

void DragDropController::BeginExternalDrag(
    ExternalDragSummary summary)
{
    transport_ = Transport::ExternalOle;
    selfDragReturned_ = false;
    selfDragNativeResumeRequested_ = false;
    externalSummary_ = std::move(summary);
}

void DragDropController::ContinueExternalDrag()
{
    if (transport_ == Transport::ExternalOle)
        return;
    BeginExternalDrag({1, false, false});
}

void DragDropController::EndExternalDrag()
{
    if (transport_ == Transport::ExternalOle)
        transport_ = Transport::None;
    externalSummary_ = {};
}

bool DragDropController::IsExternalDragActive() const
{
    return transport_ == Transport::ExternalOle;
}

const ExternalDragSummary&
DragDropController::ExternalSummary() const
{
    return externalSummary_;
}

bool DragDropController::IsTransportActive() const
{
    return transport_ != Transport::None;
}

DragTargetResolution DragDropController::ResolveInternalTarget(
    const std::vector<std::unique_ptr<Container>>& containers,
    POINT point,
    const DragTargetResolver::CandidateFilter& filter)
{
    DragTargetResolution resolution =
        DragTargetResolver::ResolveInternal(
            containers, point, session_.SourceList(), filter);
    session_.UpdateTarget(
        resolution.container,
        resolution.slot,
        resolution.region);
    return resolution;
}

DragTargetResolution DragDropController::ResolveExternalTarget(
    const std::vector<std::unique_ptr<Container>>& containers,
    POINT point,
    const DragTargetResolver::CandidateFilter& filter)
{
    DragTargetResolution resolution =
        DragTargetResolver::ResolveExternal(
            containers, point, filter,
            static_cast<std::size_t>(std::max(
                1, externalSummary_.fileCount)));
    session_.UpdateTarget(
        resolution.container,
        resolution.slot,
        resolution.region);
    return resolution;
}
