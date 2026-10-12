#pragma once

#include <windows.h>
#include <cstdint>
#include <utility>

namespace snowdesktop
{

// DComp clips pixels to BeginDraw's update rectangle, while native backdrop
// registration is independent of that clip. Include both erased and newly
// visible tooltip content before publishing either composition tree.
class InlineTooltipPaintDamage
{
public:
    RECT IncludePrevious(RECT update, RECT client) const { return Include(update, previous_, client); }
    RECT IncludeCurrent(RECT update, RECT client) const { return Include(update, current_, client); }
    void BeginDraw() { current_ = {}; collecting_ = true; }
    void EndDraw() { collecting_ = false; }
    void Record(RECT bounds)
    {
        if (!collecting_ || IsRectEmpty(&bounds)) return;
        if (IsRectEmpty(&current_)) current_ = bounds;
        else UnionRect(&current_, &current_, &bounds);
    }
    void AcceptDraw() { previous_ = current_; }
    // The content HWND must expose exactly the tooltip pixels drawn in this
    // frame, including changes caused by popup dismissal without pointer input.
    void UpdateWindowRegionBounds(RECT& visible, bool& pending) const
    {
        if (EqualRect(&visible, &current_)) return;
        visible = current_;
        pending = true;
    }
private:
    static RECT Include(RECT update, RECT tooltip, RECT client)
    {
        RECT visible{};
        if (IntersectRect(&visible, &tooltip, &client))
            UnionRect(&update, &update, &visible);
        return update;
    }
    RECT previous_{}, current_{};
    bool collecting_ = false;
};

// A tooltip has one stable panel, regardless of its measured bounds or host.
// Cleanup visits live hosts instead of retaining a possibly destroyed target.
class InlineTooltipBackdrop
{
public:
    InlineTooltipBackdrop() = default;
    InlineTooltipBackdrop(const InlineTooltipBackdrop&) = delete;
    InlineTooltipBackdrop& operator=(const InlineTooltipBackdrop&) = delete;

    template<class Cleanup>
    class PaintScope
    {
    public:
        PaintScope(InlineTooltipBackdrop& panel, Cleanup cleanup)
            : panel_(panel), cleanup_(std::move(cleanup)) {}
        PaintScope(const PaintScope&) = delete;
        PaintScope& operator=(const PaintScope&) = delete;
        ~PaintScope() { if (!retained_) panel_.Clear(cleanup_); }
        void Keep(bool retained) { retained_ = retained; }
    private:
        InlineTooltipBackdrop& panel_;
        Cleanup cleanup_;
        bool retained_ = false;
    };

    template<class Cleanup>
    auto BeginPaint(Cleanup cleanup)
    {
        return PaintScope<Cleanup>(*this, std::move(cleanup));
    }

    template<class Cleanup>
    void Clear(Cleanup cleanup)
    {
        if (active_) cleanup(OwnerKey());
        active_ = false;
        surface_ = nullptr;
    }

    template<class Compositor, class Cleanup>
    bool Update(Compositor& compositor, RECT frame, float radius,
        float blur, Cleanup cleanup)
    {
        if (active_ && surface_ != &compositor) Clear(cleanup);
        if (IsRectEmpty(&frame) ||
            !compositor.AddPanel(frame, radius, blur, OwnerKey()))
        {
            // An unsuccessful update may have partially registered a panel.
            cleanup(OwnerKey());
            active_ = false;
            surface_ = nullptr;
            return false;
        }
        active_ = true;
        surface_ = &compositor;
        return true;
    }

private:
    std::uintptr_t OwnerKey() const { return reinterpret_cast<std::uintptr_t>(this); }
    bool active_ = false;
    const void* surface_ = nullptr;
};

} // namespace snowdesktop
