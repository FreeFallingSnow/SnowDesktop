#pragma once

namespace snowdesktop
{
constexpr bool CanFlushScrollLayout(bool reloading, bool dragging,
    bool transporting, bool widgetAction) noexcept
{
    return !reloading && !dragging && !transporting && !widgetAction;
}

constexpr bool ShouldFlushScrollLayoutForSession(bool pending, bool query,
    bool ending, bool reloading, bool dragging, bool transporting,
    bool widgetAction) noexcept
{
    return pending && (query || ending) &&
        CanFlushScrollLayout(reloading, dragging, transporting, widgetAction);
}
}
