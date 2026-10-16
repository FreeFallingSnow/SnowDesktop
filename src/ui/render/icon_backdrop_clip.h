#pragma once

#include <d2d1.h>
#include <wrl/client.h>

#include <memory>
#include <optional>
#include <utility>

namespace snowdesktop
{

// D2D clips do not constrain a separate Windows Composition backdrop target.
// Capture the same logical-coordinate masks while collecting native icon glass.
struct IconBackdropGeometryClip
{
    Microsoft::WRL::ComPtr<ID2D1Geometry> geometry;
    std::shared_ptr<const IconBackdropGeometryClip> parent;
};

struct IconBackdropClip
{
    RECT bounds{};
    std::shared_ptr<const IconBackdropGeometryClip> geometry;
    POINT geometryOffset{};
};

inline bool SameIconBackdropClip(const std::optional<IconBackdropClip>& left,
    const std::optional<IconBackdropClip>& right)
{
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    if (!EqualRect(&left->bounds, &right->bounds)) return false;
    if (left->geometryOffset.x != right->geometryOffset.x ||
        left->geometryOffset.y != right->geometryOffset.y) return false;
    auto a = left->geometry;
    auto b = right->geometry;
    while (a != b)
    {
        if (!a || !b || a->geometry.Get() != b->geometry.Get()) return false;
        a = a->parent;
        b = b->parent;
    }
    return true;
}

class ScopedIconBackdropClip
{
public:
    ScopedIconBackdropClip(ID2D1RenderTarget* target, const RECT& bounds,
        ID2D1Geometry* geometry = nullptr)
        : target_(target), previous_(current_)
    {
        clip_ = Current(target).value_or(IconBackdropClip{bounds, {}});
        RECT intersection{};
        IntersectRect(&intersection, &clip_.bounds, &bounds);
        clip_.bounds = intersection;
        if (geometry)
        {
            auto mask = std::make_shared<IconBackdropGeometryClip>();
            mask->geometry = geometry;
            mask->parent = clip_.geometry;
            clip_.geometry = std::move(mask);
        }
        current_ = this;
    }

    ~ScopedIconBackdropClip() { current_ = previous_; }
    ScopedIconBackdropClip(const ScopedIconBackdropClip&) = delete;
    ScopedIconBackdropClip& operator=(const ScopedIconBackdropClip&) = delete;

    static std::optional<IconBackdropClip> Current(ID2D1RenderTarget* target)
    {
        for (auto* scope = current_; scope; scope = scope->previous_)
            if (scope->target_ == target) return scope->clip_;
        return std::nullopt;
    }

private:
    ID2D1RenderTarget* target_;
    IconBackdropClip clip_;
    ScopedIconBackdropClip* previous_;
    inline static thread_local ScopedIconBackdropClip* current_ = nullptr;
};

} // namespace snowdesktop
