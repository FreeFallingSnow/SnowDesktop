#pragma once

#include "icon_render_rules.h"

#include <d2d1_1.h>
#include <d2d1helper.h>
#include <wrl/client.h>

namespace snowdesktop::icon_bitmap_renderer
{
struct DrawPlan
{
    RECT bounds{};
    bool preserveAspectRatio = true;
};

// Every icon surface owns its logical target size. A small Shell bitmap must
// fill that size too; its pixel resolution only limits available detail.
inline DrawPlan Prepare(int sourceWidth, int sourceHeight, RECT frame,
    bool preserveAspectRatio = true)
{
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    if (sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0)
        return {};

    const auto fitted = preserveAspectRatio
        ? icon_render_rules::FitToBounds(sourceWidth, sourceHeight, width, height)
        : icon_render_rules::FittedSize{width, height};
    const int left = frame.left + (width - fitted.width) / 2;
    const int top = frame.top + (height - fitted.height) / 2;
    return {{left, top, left + fitted.width, top + fitted.height}, preserveAspectRatio};
}

inline void Draw(ID2D1RenderTarget* target, ID2D1Bitmap* bitmap,
    const DrawPlan& plan, float opacity = 1.0f, bool translucentMaterial = false)
{
    if (!target || !bitmap || IsRectEmpty(&plan.bounds))
        return;
    const auto& bounds = plan.bounds;
    const auto destination = D2D1::RectF(static_cast<float>(bounds.left),
        static_cast<float>(bounds.top), static_cast<float>(bounds.right),
        static_cast<float>(bounds.bottom));

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&context))) && context)
    {
        // Cubic ringing can leave RGB above alpha on translucent white rims.
        // Positive linear samples keep those pixels safe for later composition.
        const auto interpolation = !plan.preserveAspectRatio ? D2D1_INTERPOLATION_MODE_LINEAR
            : translucentMaterial ? D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR
                                  : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
        context->DrawBitmap(bitmap, &destination, opacity,
            interpolation, nullptr, nullptr);
    }
    else
        target->DrawBitmap(bitmap, destination, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}
} // namespace snowdesktop::icon_bitmap_renderer
