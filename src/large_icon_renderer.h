#pragma once
#include "large_icon_config.h"
#include "large_icon_render_rules.h"
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <string_view>

namespace snowdesktop::large_icon_renderer
{
struct View
{
    RECT frame{};
    float scale = 1, opacity = 1, hover = 0;
    bool original = true, animations = true, selected = false;
    unsigned neutral = 0x414751, accent = 0, edgeColor = 0, componentForeground = 0xffffff;
    bool hasEdgeColor = false, backgroundResolved = false;
    large_icon_render_rules::BackgroundStyle background;
    std::function<void(ID2D1RenderTarget*, RECT, float, float)> drawBackground;
    std::wstring_view name;
    ID2D1Bitmap* bitmap = nullptr;
    std::function<void(RECT, float)> placeholder;
};

// These are the production desktop draw paths, also usable with a WIC render
// target for deterministic visual checks without creating a desktop window.
void DrawFrame(ID2D1RenderTarget* target, IDWriteFactory* fonts, const LargeIconConfig& config, const View& view);
}
