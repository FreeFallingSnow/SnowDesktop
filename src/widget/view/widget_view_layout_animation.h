#pragma once

#include "widget_view_tree.h"

namespace snowdesktop::widget_runtime::detail
{
float ResolveViewTransitionProgress(ViewTransitionEasing easing,
    float progress) noexcept;
std::uint32_t ScaleViewTransitionDuration(std::uint32_t duration,
    double scale) noexcept;

bool HasViewLayoutAnimation(const ViewNode& node) noexcept;

class ViewLayoutAnimation
{
public:
    void Commit(const ViewNode& previous, const ViewNode& current,
        double durationScale);
    const ViewNode& LastPainted(const ViewNode& committed) const noexcept;
    const ViewNode& Paint(const ViewNode& committed,
        ViewTransitionRuntime::TimePoint now);
    bool HasActive() const noexcept { return !entries_.empty(); }

private:
    struct Geometry
    {
        ViewRect layoutFrame;
        std::optional<ViewRect> clip;
        std::optional<ViewRect> scrollbar;
        float viewportExtent = 0.0f;
        float contentExtent = 0.0f;

        bool SameBounds(const Geometry& other) const noexcept
        {
            return layoutFrame == other.layoutFrame && clip == other.clip &&
                scrollbar == other.scrollbar;
        }
    };

    struct Entry
    {
        Geometry start;
        Geometry target;
        Geometry presented;
        ViewTransitionEasing easing;
        std::uint32_t duration;
        std::optional<ViewTransitionRuntime::TimePoint> started;
    };

    static Geometry Capture(const ViewNode& node) noexcept;
    static Geometry Interpolate(const Entry& entry, float progress) noexcept;
    void Apply(const ViewNode& committed, ViewNode& painted,
        float parentOffsetX, float parentOffsetY) const;

    std::unordered_map<std::string, Entry> entries_;
    std::optional<ViewNode> painted_;
};
}
