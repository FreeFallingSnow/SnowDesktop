#pragma once

#include "app/render/desktop_backdrop_compositor.h"
#include "icons/icon_beautify.h"
#include "ui/render/icon_backdrop_clip.h"

#include <optional>
#include <vector>

namespace snowdesktop
{
struct CollectionPopupIconMaterials
{
    using Icons = std::vector<PanelBackdropIcon>;
    using PaintResult = std::optional<Icons>;
    Icons bitmap;
    Icons presented;

    // Images created in nested render targets (e.g. appearance caches) must
    // never register a desktop panel while the popup is being drawn.
    class DrawScope
    {
    public:
        DrawScope(ID2D1RenderTarget* target, Icons* icons)
            : target_(target), icons_(icons), previous_(current_) { current_ = this; }
        ~DrawScope() { current_ = previous_; }
        DrawScope(const DrawScope&) = delete;
        DrawScope& operator=(const DrawScope&) = delete;

        static bool Collect(ID2D1RenderTarget* target, RECT frame, float opacity,
            const IconBeautifySettings& style)
        {
            if (!current_) return false;
            if (current_->icons_ && current_->target_ == target && opacity > 0.f)
                current_->icons_->push_back({frame, style.shape, style.glassBlurRadius,
                    ScopedIconBackdropClip::Current(target)});
            return true;
        }
    private:
        ID2D1RenderTarget* target_;
        Icons* icons_;
        DrawScope* previous_;
        inline static thread_local DrawScope* current_ = nullptr;
    };
};
}
