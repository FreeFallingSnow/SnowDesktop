#pragma once
#include "large_icon_config.h"
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
    double launchWave = 0;
    bool original = true, animations = true, pressed = false, selected = false;
    unsigned neutral = 0x414751, accent = 0x414751;
    std::wstring_view name;
    ID2D1Bitmap* bitmap = nullptr;
    std::function<void(RECT, float)> placeholder;
};

// These are the production desktop draw paths, also usable with a WIC render
// target for deterministic visual checks without creating a desktop window.
void DrawFrame(ID2D1RenderTarget* target, IDWriteFactory* fonts, const LargeIconConfig& config, const View& view);
void DrawFloatingTitle(ID2D1RenderTarget* target, IDWriteFactory* fonts, const LargeIconConfig& config, const View& view, RECT workArea);
}
