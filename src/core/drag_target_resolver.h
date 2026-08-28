#pragma once

#include "container.h"
#include "drop_model.h"
#include "slot.h"

#include <functional>
#include <memory>
#include <vector>

struct DragTargetResolution
{
    Container* container = nullptr;
    Slot* slot = nullptr;
    HitRegion region = HitRegion::None;

    explicit operator bool() const
    {
        return container && region != HitRegion::None;
    }
};

/** Resolves the topmost eligible slot target without UI or COM policy. */
class DragTargetResolver
{
public:
    using CandidateFilter =
        std::function<bool(const Container&)>;

    static bool AcceptsInternal(
        const Container& target,
        const DragSourceList& source);
    static bool AcceptsExternal(const Container& target,
        std::size_t itemCount = 1);

    static DragTargetResolution ResolveInternal(
        const std::vector<std::unique_ptr<Container>>& containers,
        POINT point,
        const DragSourceList& source,
        const CandidateFilter& filter = {});

    static DragTargetResolution ResolveExternal(
        const std::vector<std::unique_ptr<Container>>& containers,
        POINT point,
        const CandidateFilter& filter = {},
        std::size_t itemCount = 1);
};
