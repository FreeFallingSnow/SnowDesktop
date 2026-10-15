#pragma once

namespace snowdesktop
{
// Native messages can be dispatched by a modal loop inside a scheduled
// callback. Their feedback must remain synchronous even though the outer
// deadline snapshot is still active. This state is local to the UI thread.
class UiImmediatePresentationScope final
{
public:
    explicit UiImmediatePresentationScope(bool active = true) noexcept
        : active_(active) { if (active_) ++depth_; }
    ~UiImmediatePresentationScope() { if (active_) --depth_; }
    UiImmediatePresentationScope(const UiImmediatePresentationScope&) = delete;
    UiImmediatePresentationScope& operator=(const UiImmediatePresentationScope&) = delete;
    static bool IsActive() noexcept { return depth_ != 0; }

private:
    const bool active_;
    static inline thread_local unsigned depth_ = 0;
};
}
