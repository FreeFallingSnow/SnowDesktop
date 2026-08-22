#pragma once

#include "../core/drag_session.h"
#include "../core/drag_target_resolver.h"

#include <functional>
#include <memory>
#include <vector>

struct ExternalDragSummary
{
    int fileCount = 0;
    bool hasShortcut = false;
    bool foldersOnly = false;
};

/**
 * Owns application-level drag transport state and coordinates target updates.
 * Core source/target semantics remain in DragSession and DragTargetResolver;
 * Windows OLE callbacks remain in OleDragDropAdapter.
 */
class DragDropController
{
public:
    explicit DragDropController(DragSession& session)
        : session_(session)
    {
    }

    void BeginSelfDrag();
    void MarkSelfDragReturned();
    void ClearSelfDragReturned();
    void RequestSelfDragNativeResume();
    void EndSelfDrag();
    bool IsSelfDragActive() const;
    bool SelfDragReturned() const;
    bool SelfDragNativeResumeRequested() const;

    void BeginExternalDrag(ExternalDragSummary summary);
    void ContinueExternalDrag();
    void EndExternalDrag();
    bool IsExternalDragActive() const;
    const ExternalDragSummary& ExternalSummary() const;

    bool IsTransportActive() const;

    DragTargetResolution ResolveInternalTarget(
        const std::vector<std::unique_ptr<Container>>& containers,
        POINT point,
        const DragTargetResolver::CandidateFilter& filter = {});
    DragTargetResolution ResolveExternalTarget(
        const std::vector<std::unique_ptr<Container>>& containers,
        POINT point,
        const DragTargetResolver::CandidateFilter& filter = {});

private:
    enum class Transport
    {
        None,
        SelfOle,
        ExternalOle,
    };

    DragSession& session_;
    Transport transport_ = Transport::None;
    bool selfDragReturned_ = false;
    bool selfDragNativeResumeRequested_ = false;
    ExternalDragSummary externalSummary_;
};
