#include "performance_animation.h"
#include "performance_trace.h"
#include "performance_trace_control.h"
#include "perfetto_categories.h"

namespace snowdesktop::performance
{
std::uint64_t AnimationBegin(std::uint64_t key, std::string_view type, std::string_view surface,
    double targetMs, double effectiveMs, bool observed)
{
    if (!Enabled()) return 0;
    if (!TRACE_EVENT_CATEGORY_ENABLED("snow.frame")) return 0;
    const auto identity = ReserveAnimationTrack();
    if (!identity) return 0;
    if (type.size() > 256) type = "compiler-callsite";
    const auto track = perfetto::Track(identity, perfetto::ProcessTrack::Current());
    TRACE_EVENT_BEGIN("snow.frame", "animation.lifetime", track,
        "animation", identity, "type", perfetto::DynamicString(type.data(), type.size()),
        "surface", perfetto::DynamicString(surface.data(), surface.size()), "observed", observed,
        "target_ms", targetMs, "effective_ms", effectiveMs, "callback_operation", OperationId(key));
    return identity;
}
void AnimationUpdate(std::uint64_t animation, std::uint64_t frame, double effectiveMs,
    double lateMs, std::uint64_t missed) noexcept
{
    if (!Enabled() || !animation) return;
    TRACE_EVENT_INSTANT("snow.frame", "animation.update", "animation", animation, "frame", frame,
        "effective_ms", effectiveMs, "late_ms", lateMs, "missed_deadlines", missed);
}
void AnimationEnd(std::uint64_t animation, bool canceled) noexcept
{
    if (!Enabled() || !animation) return;
    TRACE_EVENT_INSTANT("snow.frame", "animation.end", "animation", animation, "canceled", canceled);
    TRACE_EVENT_END("snow.frame", perfetto::Track(animation, perfetto::ProcessTrack::Current()));
}
void Presentation(std::uint64_t frame, std::uint64_t owner, bool shared) noexcept
{
    if (!Enabled()) return;
    TRACE_EVENT_INSTANT("snow.frame", "animation.presentation", "frame", frame, "presentation_owner", OperationId(owner), "shared", shared);
}
void CompositionCommit(std::uint64_t frame, std::uint64_t commit, double durationMs) noexcept
{
    if (!Enabled()) return;
    TRACE_EVENT_INSTANT("snow.frame", "animation.commit", "frame", frame, "commit", OperationId(commit), "duration_ms", durationMs);
}
}
