#pragma once
#include <string_view>
namespace snowdesktop::performance
{
// Finite semantic exceptions. All remaining Value observations are gauges;
// cumulative resource names are explicitly identified by the fixed SQL package.
inline bool IsEventMetric(std::string_view module, std::string_view name) noexcept
{
    if (module == "task.start" || module == "widget.view.layout_cache") return true;
    if (module == "widget.background.cache") return name == "hit" || name == "miss" || name == "bypass";
    if (module == "widget.text.layout") return name == "hits_per_draw" || name == "misses_per_draw" || name == "bypasses_per_draw";
    if (module == "widget.composition") return name == "surface_created" || name == "surface_reclaimed_expired_bytes" || name == "surface_reclaimed_budget_bytes";
    if (module == "render") return name == "backdrop.panel.transform" || name == "backdrop.panel.offset" ||
        name == "backdrop.panel.size" || name == "backdrop.panel.radius" || name == "backdrop.panel.clip";
    if (module == "desktop.input") return name == "hover.rejected" || name == "mouse.coalesced";
    if (module == "shell.refresh") return name == "animation_deferred" || name == "content_only";
    if (module == "shared.system") return name == "network.status.query" || name == "network.status.cached";
    if (module == "shared.audio") return name == "refresh.queued";
    if (module == "popup") return name == "backdrop.warmup.reused";
    if (module == "dock") return name == "running.discovery_deferred" || name == "floating.frame_ready";
    if (module == "desktop") return name == "dirty_pixels";
    return module == "profiler" && name == "widget_resources_error";
}
}
